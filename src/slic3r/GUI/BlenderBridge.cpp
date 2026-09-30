#include "BlenderBridge.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>

#include <wx/stdpaths.h>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"

#include "slic3r/Utils/BlenderLauncher.hpp"

#include "GLCanvas3D.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "NotificationManager.hpp"
#include "Plater.hpp"
#include "Selection.hpp"
#include "format.hpp"
#include "Gizmos/GLGizmosManager.hpp"

namespace fs = boost::filesystem;

namespace Slic3r {
namespace GUI {

namespace {

// How often Blender's output file is checked for a new version.
constexpr int POLL_INTERVAL_MS = 1000;
// Session folders left behind (EdgeSlicer closed while Blender was open) are removed after this.
constexpr auto STALE_SESSION_AGE = std::chrono::hours(24 * 7);

// Paths travel as UTF-8 (the STL reader/writer and Blender's command line expect it); Windows
// needs them widened for boost::filesystem.
fs::path to_path(const std::string &utf8)
{
#ifdef _WIN32
    return fs::path(boost::nowide::widen(utf8));
#else
    return fs::path(utf8);
#endif
}

std::string u8(const fs::path &path)
{
#ifdef _WIN32
    return boost::nowide::narrow(path.wstring());
#else
    return path.string();
#endif
}

fs::path bridge_root() { return to_path(data_dir()) / "blender_bridge"; }

void remove_stale_sessions()
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

// The model volume `selection` stands for: a single part, or an object that has only one part.
ModelVolume *selected_volume(const Selection &selection, Model &model, int &object_idx)
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

// This install, as Blender's "Send to EdgeSlicer" should start it.
std::string own_executable()
{
#ifdef __linux__
    // Inside an AppImage the executable lives in a temporary mount; the AppImage itself is stable.
    if (const char *appimage = std::getenv("APPIMAGE"); appimage && *appimage)
        return appimage;
#endif
    fs::path exe = into_path(wxStandardPaths::Get().GetExecutablePath());
#ifdef __APPLE__
    // .../EdgeSlicer.app/Contents/MacOS/EdgeSlicer: the add-on opens the bundle with `open -a`.
    fs::path bundle = exe.parent_path().parent_path().parent_path();
    if (bundle.extension() == ".app")
        return u8(bundle);
#endif
    return u8(exe);
}

std::string safe_file_stem(const std::string &name)
{
    std::string out;
    for (char c : name)
        out += (std::string("<>:\"/\\|?*").find(c) != std::string::npos || static_cast<unsigned char>(c) < 32) ? '_' : c;
    return out.empty() ? std::string("part") : out;
}

} // namespace

BlenderBridge::BlenderBridge(Plater *plater) : m_plater(plater), m_timer(this)
{
    Bind(wxEVT_TIMER, &BlenderBridge::on_timer, this);
}

BlenderBridge::~BlenderBridge() { m_timer.Stop(); }

bool BlenderBridge::can_edit(const Selection &selection)
{
    int object_idx = -1;
    return selected_volume(selection, wxGetApp().model(), object_idx) != nullptr;
}

void BlenderBridge::edit_selection()
{
    int          object_idx = -1;
    ModelVolume *volume     = selected_volume(m_plater->canvas3D()->get_selection(), m_plater->model(), object_idx);
    if (volume == nullptr)
        return;

    std::vector<std::string> blender = BlenderLauncher::find_blender(wxGetApp().app_config->get("blender_path"));
    if (blender.empty()) {
        MessageDialog dlg(m_plater,
                          _L("Blender was not found. Install Blender, or set its location in Preferences under \"Blender path\"."),
                          _L("Edit in Blender"), wxOK | wxICON_INFORMATION);
        dlg.ShowModal();
        return;
    }

    const std::string script = resources_dir() + "/blender/edgeslicer_bridge.py";
    if (!fs::exists(to_path(script))) {
        BOOST_LOG_TRIVIAL(error) << "BlenderBridge: missing " << script;
        show_error(m_plater, _L("The Blender bridge script is missing from this installation."));
        return;
    }

    remove_stale_sessions();
    // A second "Edit in Blender" on the same part starts over; the older Blender window's sends
    // then report that EdgeSlicer is no longer waiting, because its folder is gone.
    remove_session_for(*volume);

    const std::string name = volume->name.empty() ? volume->get_object()->name : volume->name;
    Session session;
    session.volume_ids.push_back(volume->id());
    session.name   = name;
    session.dir    = bridge_root() / (std::to_string(std::time(nullptr)) + "-" + std::to_string(volume->id().id));
    session.output = session.dir / "edited.stl";
    const fs::path input = session.dir / to_path(safe_file_stem(name) + ".stl");

    boost::system::error_code ec;
    fs::create_directories(session.dir, ec);
    if (ec || !its_write_stl_binary(u8(input).c_str(), name.c_str(), volume->mesh().its)) {
        show_error(m_plater, format_wxstr(_L("Could not write the part for Blender to %1%."), u8(session.dir)));
        return;
    }

    BlenderLauncher::EditSession args;
    args.script         = script;
    args.input          = u8(input);
    args.output         = u8(session.output);
    args.name           = name;
    args.edgeslicer_exe = own_executable();
    if (!BlenderLauncher::launch(BlenderLauncher::edit_session_args(blender, args))) {
        fs::remove_all(session.dir, ec);
        show_error(m_plater, format_wxstr(_L("Could not start Blender (%1%)."), blender.front()));
        return;
    }

    BOOST_LOG_TRIVIAL(info) << "BlenderBridge: editing \"" << name << "\" in " << u8(session.dir);
    m_sessions.push_back(std::move(session));
    if (!m_timer.IsRunning())
        m_timer.Start(POLL_INTERVAL_MS);
    m_plater->get_notification_manager()->push_notification(
        NotificationType::CustomNotification, NotificationManager::NotificationLevel::RegularNotificationLevel,
        format(_u8L("Opened \"%1%\" in Blender. Save in Blender (Ctrl+S) to update the part here."), name));
}

ModelVolume *BlenderBridge::find_volume(const Session &session, int &object_idx) const
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

void BlenderBridge::remove_session_for(const ModelVolume &volume)
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

void BlenderBridge::on_timer(wxTimerEvent &)
{
    for (auto it = m_sessions.begin(); it != m_sessions.end();) {
        if (poll(*it))
            ++it;
        else {
            BOOST_LOG_TRIVIAL(info) << "BlenderBridge: \"" << it->name << "\" is gone, no longer watching " << u8(it->dir);
            boost::system::error_code ec;
            fs::remove_all(it->dir, ec);
            it = m_sessions.erase(it);
        }
    }
    if (m_sessions.empty())
        m_timer.Stop();
}

bool BlenderBridge::poll(Session &session)
{
    int          object_idx = -1;
    ModelVolume *volume     = find_volume(session, object_idx);
    if (volume == nullptr)
        return false;

    boost::system::error_code ec;
    const std::time_t written = fs::last_write_time(session.output, ec);
    if (ec)
        return true; // nothing sent back yet
    const uintmax_t size = fs::file_size(session.output, ec);
    if (ec || (written == session.last_write && size == session.last_size))
        return true;

    // Changing the mesh under an open gizmo (painting, cutting, ...) would leave the gizmo working
    // on stale data; try again once it is closed.
    if (m_plater->get_view3D_canvas3D()->get_gizmos_manager().get_current_type() != GLGizmosManager::Undefined)
        return true;

    session.last_write = written;
    session.last_size  = size;

    TriangleMesh mesh;
    if (!mesh.ReadSTLFile(u8(session.output).c_str(), true) || mesh.empty()) {
        BOOST_LOG_TRIVIAL(error) << "BlenderBridge: could not read " << u8(session.output);
        m_plater->get_notification_manager()->push_notification(
            NotificationType::CustomNotification, NotificationManager::NotificationLevel::WarningNotificationLevel,
            format(_u8L("Could not read the part sent back from Blender for \"%1%\"."), session.name));
        return true;
    }

    m_plater->take_snapshot(format(_u8L("Edit in Blender: %1%"), session.name));

    ModelObject *object  = volume->get_object();
    const bool   sinking = object->min_z() < SINKING_Z_THRESHOLD;

    // Painted supports, seams, colours and fuzzy skin are stored per triangle of the old mesh and
    // mean nothing on the new one.
    const bool had_paint = !volume->supported_facets.empty() || !volume->seam_facets.empty() ||
                           !volume->mmu_segmentation_facets.empty() || !volume->fuzzy_skin_facets.empty();
    volume->supported_facets.reset();
    volume->seam_facets.reset();
    volume->mmu_segmentation_facets.reset();
    volume->fuzzy_skin_facets.reset();

    volume->set_mesh(std::move(mesh));
    volume->calculate_convex_hull();
    volume->invalidate_convex_hull_2d();
    volume->set_new_unique_id();
    session.volume_ids.push_back(volume->id());
    object->invalidate_bounding_box();
    if (!sinking)
        object->ensure_on_bed();

    // Fixes hollowing and SLA points, refreshes the scene and restarts slicing.
    m_plater->changed_mesh(object_idx);
    wxGetApp().obj_list()->update_item_error_icon(object_idx, -1);

    std::string message = format(_u8L("Updated \"%1%\" from Blender."), session.name);
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
