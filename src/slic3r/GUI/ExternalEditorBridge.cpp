#include "ExternalEditorBridge.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>

#include <wx/stdpaths.h>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/BRep/CadBody.hpp"
#include "libslic3r/Format/STEP.hpp"
#include "libslic3r/Format/STEPExport.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PartMeshReplace.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"

#include "GLCanvas3D.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "I18N.hpp"
#include "NotificationManager.hpp"
#include "Plater.hpp"
#include "Selection.hpp"
#include "format.hpp"
#include "Gizmos/GLGizmosManager.hpp"

namespace fs = boost::filesystem;

namespace Slic3r {
namespace GUI {

namespace {

// How often the files written back are checked for a new version.
constexpr int POLL_INTERVAL_MS = 1000;
// Session folders left behind (EdgeSlicer closed while the other program was open) are removed
// after this.
constexpr auto STALE_SESSION_AGE = std::chrono::hours(24 * 7);

} // namespace

ExternalEditorBridge::ExternalEditorBridge(Plater *plater) : m_plater(plater), m_timer(this)
{
    Bind(wxEVT_TIMER, &ExternalEditorBridge::on_timer, this);
}

ExternalEditorBridge::~ExternalEditorBridge() { m_timer.Stop(); }

// Paths travel as UTF-8 (the STL reader/writer and the other programs' command lines expect it);
// Windows needs them widened for boost::filesystem.
fs::path ExternalEditorBridge::to_path(const std::string &utf8)
{
#ifdef _WIN32
    return fs::path(boost::nowide::widen(utf8));
#else
    return fs::path(utf8);
#endif
}

std::string ExternalEditorBridge::u8(const fs::path &path)
{
#ifdef _WIN32
    return boost::nowide::narrow(path.wstring());
#else
    return path.string();
#endif
}

std::string ExternalEditorBridge::own_executable()
{
#ifdef __linux__
    // Inside an AppImage the executable lives in a temporary mount; the AppImage itself is stable.
    if (const char *appimage = std::getenv("APPIMAGE"); appimage && *appimage)
        return appimage;
#endif
    fs::path exe = into_path(wxStandardPaths::Get().GetExecutablePath());
#ifdef __APPLE__
    // .../EdgeSlicer.app/Contents/MacOS/EdgeSlicer: the add-ons open the bundle with `open -a`.
    fs::path bundle = exe.parent_path().parent_path().parent_path();
    if (bundle.extension() == ".app")
        return u8(bundle);
#endif
    return u8(exe);
}

std::string ExternalEditorBridge::safe_file_stem(const std::string &name)
{
    std::string out;
    for (char c : name)
        out += (std::string("<>:\"/\\|?*").find(c) != std::string::npos || static_cast<unsigned char>(c) < 32) ? '_' : c;
    return out.empty() ? std::string("part") : out;
}

fs::path ExternalEditorBridge::bridge_root() const { return to_path(data_dir()) / folder_name(); }

void ExternalEditorBridge::remove_stale_sessions() const
{
    boost::system::error_code ec;
    const std::time_t now = std::time(nullptr);
    const std::time_t max_age = std::chrono::duration_cast<std::chrono::seconds>(STALE_SESSION_AGE).count();
    for (fs::directory_iterator it(bridge_root(), ec), end; !ec && it != end; it.increment(ec)) {
        std::time_t written = fs::last_write_time(it->path(), ec);
        if (!ec && now - written > max_age)
            fs::remove_all(it->path(), ec);
        ec.clear();
    }
}

ModelVolume *ExternalEditorBridge::selected_volume(const Selection &selection, Model &model, int &object_idx)
{
    if (selection.is_empty() || selection.is_wipe_tower())
        return nullptr;
    int volume_idx = -1;
    if (selection.is_single_volume_or_modifier()) {
        const GLVolume *v = selection.get_first_volume();
        object_idx = v->object_idx();
        volume_idx = v->volume_idx();
    } else if (selection.is_single_full_object() || selection.is_single_full_instance()) {
        object_idx = selection.get_object_idx();
        if (0 <= object_idx && object_idx < int(model.objects.size()) && model.objects[object_idx]->volumes.size() == 1)
            volume_idx = 0;
    }
    if (object_idx < 0 || object_idx >= int(model.objects.size()) || volume_idx < 0 ||
        volume_idx >= int(model.objects[object_idx]->volumes.size()))
        return nullptr;
    ModelVolume *volume = model.objects[object_idx]->volumes[volume_idx];
    if (volume->is_text() || volume->is_svg())
        return nullptr;
    return volume;
}

bool ExternalEditorBridge::can_edit(const Selection &selection)
{
    int object_idx = -1;
    return selected_volume(selection, wxGetApp().model(), object_idx) != nullptr;
}

void ExternalEditorBridge::edit_selection()
{
    int          object_idx = -1;
    ModelVolume *volume     = selected_volume(m_plater->canvas3D()->get_selection(), m_plater->model(), object_idx);
    if (volume == nullptr)
        return;

    if (!find_editor())
        return;

    remove_stale_sessions();
    if (Session *existing = find_session_for(*volume); existing != nullptr && reopen(*volume, *existing)) {
        BOOST_LOG_TRIVIAL(info) << log_tag() << ": reopened \"" << existing->name << "\" from " << u8(existing->dir);
        m_plater->get_notification_manager()->push_notification(
            NotificationType::CustomNotification, NotificationManager::NotificationLevel::RegularNotificationLevel,
            opened_message(existing->name));
        return;
    }
    // A second "Edit in ..." on the same part starts over; the older window's sends then report
    // that EdgeSlicer is no longer waiting, because its folder is gone.
    remove_session_for(*volume);

    const std::string name = volume->name.empty() ? volume->get_object()->name : volume->name;
    Session session;
    session.volume_ids.push_back(volume->id());
    session.name = name;
    session.dir  = bridge_root() / (std::to_string(std::time(nullptr)) + "-" + std::to_string(volume->id().id));
    if (!start(*volume, session)) {
        boost::system::error_code ec;
        fs::remove_all(session.dir, ec);
        return;
    }

    BOOST_LOG_TRIVIAL(info) << log_tag() << ": editing \"" << name << "\" in " << u8(session.dir);
    m_sessions.push_back(std::move(session));
    if (!m_timer.IsRunning())
        m_timer.Start(POLL_INTERVAL_MS);
    m_plater->get_notification_manager()->push_notification(
        NotificationType::CustomNotification, NotificationManager::NotificationLevel::RegularNotificationLevel,
        opened_message(name));
}

ModelVolume *ExternalEditorBridge::find_volume(const Session &session, int &object_idx) const
{
    const Model &model = m_plater->model();
    for (int i = 0; i < int(model.objects.size()); ++i)
        for (ModelVolume *volume : model.objects[i]->volumes)
            for (const ObjectID &id : session.volume_ids)
                if (volume->id() == id) {
                    object_idx = i;
                    return volume;
                }
    return nullptr;
}

ExternalEditorBridge::Session *ExternalEditorBridge::find_session_for(const ModelVolume &volume)
{
    for (Session &session : m_sessions)
        if (std::find(session.volume_ids.begin(), session.volume_ids.end(), volume.id()) != session.volume_ids.end())
            return &session;
    return nullptr;
}

void ExternalEditorBridge::remove_session_for(const ModelVolume &volume)
{
    for (auto it = m_sessions.begin(); it != m_sessions.end();) {
        if (std::find(it->volume_ids.begin(), it->volume_ids.end(), volume.id()) != it->volume_ids.end()) {
            boost::system::error_code ec;
            fs::remove_all(it->dir, ec);
            it = m_sessions.erase(it);
        } else
            ++it;
    }
}

void ExternalEditorBridge::on_timer(wxTimerEvent &)
{
    for (auto it = m_sessions.begin(); it != m_sessions.end();) {
        if (poll(*it))
            ++it;
        else {
            BOOST_LOG_TRIVIAL(info) << log_tag() << ": \"" << it->name << "\" is gone, no longer watching " << u8(it->dir);
            boost::system::error_code ec;
            fs::remove_all(it->dir, ec);
            it = m_sessions.erase(it);
        }
    }
    if (m_sessions.empty())
        m_timer.Stop();
}

bool ExternalEditorBridge::read_output(const fs::path &file, TriangleMesh &mesh, std::shared_ptr<const BRep::CadBody> &cad_body) const
{
    cad_body.reset();
    const std::string path = u8(file);
    if (boost::iends_with(path, ".step") || boost::iends_with(path, ".stp")) {
        // The same tessellation a normal STEP import uses.
        double linear = string_to_double_decimal_point(wxGetApp().app_config->get("linear_defletion"));
        if (linear <= 0)
            linear = 0.003;
        double angle = string_to_double_decimal_point(wxGetApp().app_config->get("angle_defletion"));
        if (angle <= 0)
            angle = 0.5;
        // The shapes with their exact CAD body; failing that, the plain STEP import tessellation.
        std::string          error;
        indexed_triangle_set its;
        if (load_step_part(path, linear, angle, its, cad_body, &error) && !its.indices.empty()) {
            mesh = TriangleMesh(std::move(its));
            BOOST_LOG_TRIVIAL(info) << log_tag() << ": " << path << (cad_body ? " read with its CAD body" : " read as a mesh (no solid)");
            return !mesh.empty();
        }
        BOOST_LOG_TRIVIAL(warning) << log_tag() << ": " << path << ": " << error << "; reading it as a plain STEP import";
        cad_body.reset();
        if (!load_step_mesh(path.c_str(), mesh, linear, angle, &error)) {
            BOOST_LOG_TRIVIAL(error) << log_tag() << ": " << path << ": " << error;
            return false;
        }
        return !mesh.empty();
    }
    return mesh.ReadSTLFile(path.c_str(), true) && !mesh.empty();
}

bool ExternalEditorBridge::poll(Session &session)
{
    int          object_idx = -1;
    ModelVolume *volume     = find_volume(session, object_idx);
    if (volume == nullptr)
        return false;

    // The most recently written of the files the program sends back.
    boost::system::error_code ec;
    fs::path    output;
    std::time_t written = 0;
    uintmax_t   size    = 0;
    for (const fs::path &candidate : session.outputs) {
        const std::time_t t = fs::last_write_time(candidate, ec);
        if (ec) {
            ec.clear();
            continue; // nothing sent back in this format yet
        }
        if (output.empty() || t > written) {
            const uintmax_t s = fs::file_size(candidate, ec);
            if (ec) {
                ec.clear();
                continue;
            }
            output  = candidate;
            written = t;
            size    = s;
        }
    }
    if (output.empty() || (output == session.last_output && written == session.last_write && size == session.last_size))
        return true;

    // Changing the mesh under an open gizmo (painting, cutting, ...) would leave the gizmo working
    // on stale data; try again once it is closed.
    if (m_plater->get_view3D_canvas3D()->get_gizmos_manager().get_current_type() != GLGizmosManager::Undefined)
        return true;

    session.last_output = output;
    session.last_write  = written;
    session.last_size   = size;

    TriangleMesh                         mesh;
    std::shared_ptr<const BRep::CadBody> cad_body;
    if (!read_output(output, mesh, cad_body)) {
        BOOST_LOG_TRIVIAL(error) << log_tag() << ": could not read " << u8(output);
        m_plater->get_notification_manager()->push_notification(
            NotificationType::CustomNotification, NotificationManager::NotificationLevel::WarningNotificationLevel,
            read_failed_message(session.name));
        return true;
    }

    m_plater->take_snapshot(snapshot_name(session.name));
    const bool had_paint = replace_part_mesh(*volume, std::move(mesh), std::move(cad_body));
    session.volume_ids.push_back(volume->id());

    // Fixes hollowing and SLA points, refreshes the scene and restarts slicing.
    m_plater->changed_mesh(object_idx);
    wxGetApp().obj_list()->update_item_error_icon(object_idx, -1);

    std::string message = updated_message(session.name);
    if (had_paint)
        message += " " + _u8L("Its painted supports, seams and colors were removed because the mesh changed.");
    m_plater->get_notification_manager()->push_notification(
        NotificationType::CustomNotification,
        had_paint ? NotificationManager::NotificationLevel::WarningNotificationLevel : NotificationManager::NotificationLevel::RegularNotificationLevel,
        message);
    return true;
}

} // namespace GUI
} // namespace Slic3r
