#include "FreeCADBridge.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <wx/utils.h>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Format/STEPExport.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"

#include "slic3r/Utils/FreeCADLauncher.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"
#include "format.hpp"

namespace fs = boost::filesystem;

namespace Slic3r {
namespace GUI {

namespace {

constexpr const char *SESSION_JSON  = "session.json";
constexpr const char *SESSION_MACRO = "edgeslicer_edit.FCMacro";

bool write_text(const fs::path &path, const std::string &text)
{
#ifdef _WIN32
    boost::nowide::ofstream out(boost::nowide::narrow(path.wstring()), std::ios::binary | std::ios::trunc);
#else
    boost::nowide::ofstream out(path.string(), std::ios::binary | std::ios::trunc);
#endif
    out << text;
    return bool(out);
}

} // namespace

bool FreeCADBridge::find_editor()
{
    m_freecad = FreeCADLauncher::find_freecad(wxGetApp().app_config->get("freecad_path"));
    if (m_freecad.empty()) {
        MessageDialog dlg(m_plater,
                          _L("FreeCAD was not found. Install FreeCAD, or set its location in Preferences under \"FreeCAD path\"."),
                          _L("Edit in FreeCAD"), wxOK | wxICON_INFORMATION);
        dlg.ShowModal();
        return false;
    }

    m_bridge_dir = resources_dir() + "/freecad/" + FreeCADLauncher::ADDON_FOLDER;
    if (!fs::exists(to_path(m_bridge_dir + "/edgeslicer_bridge.py"))) {
        BOOST_LOG_TRIVIAL(error) << "FreeCADBridge: missing " << m_bridge_dir;
        show_error(m_plater, _L("The FreeCAD bridge add-on is missing from this installation."));
        return false;
    }
    return true;
}

bool FreeCADBridge::start(const ModelVolume &volume, Session &session)
{
    const fs::path output_step = session.dir / "edited.step";
    const fs::path output_stl  = session.dir / "edited.stl";
    session.outputs            = { output_step, output_stl };
    const std::string stem     = safe_file_stem(session.name);

    boost::system::error_code ec;
    fs::create_directories(session.dir, ec);
    if (ec) {
        show_error(m_plater, format_wxstr(_L("Could not write the part for FreeCAD to %1%."), u8(session.dir)));
        return false;
    }

    // A part with an exact B-rep (attached CAD body, or an unedited STEP import) goes over
    // exactly; anything else as its mesh.
    std::string input_format = "stl";
    fs::path    input        = session.dir / to_path(stem + ".stl");
    {
        wxBusyCursor     busy;
        const fs::path   step = session.dir / to_path(stem + ".step");
        StepExportReport report;
        if (store_step_part(u8(step), volume, {}, report, true)) {
            input_format = "step";
            input        = step;
        } else
            BOOST_LOG_TRIVIAL(info) << "FreeCADBridge: \"" << session.name << "\" goes over as a mesh: " << report.error;
    }
    if (input_format == "stl" && !its_write_stl_binary(u8(input).c_str(), session.name.c_str(), volume.mesh().its)) {
        show_error(m_plater, format_wxstr(_L("Could not write the part for FreeCAD to %1%."), u8(session.dir)));
        return false;
    }

    nlohmann::json json;
    json["version"]        = 1;
    json["name"]           = session.name;
    json["input"]          = u8(input);
    json["input_format"]   = input_format;
    json["document"]       = u8(session.dir / to_path(stem + ".FCStd"));
    json["output_step"]    = u8(output_step);
    json["output_stl"]     = u8(output_stl);
    json["edgeslicer_exe"] = own_executable();
    const fs::path json_path = session.dir / SESSION_JSON;
    if (!write_text(json_path, json.dump(2)) ||
        !write_text(session.dir / SESSION_MACRO, FreeCADLauncher::session_macro(m_bridge_dir, u8(json_path)))) {
        show_error(m_plater, format_wxstr(_L("Could not write the part for FreeCAD to %1%."), u8(session.dir)));
        return false;
    }
    BOOST_LOG_TRIVIAL(info) << "FreeCADBridge: \"" << session.name << "\" exported as " << input_format;
    return launch_session(session);
}

bool FreeCADBridge::reopen(const ModelVolume &, Session &session)
{
    // Only while the document FreeCAD saved is still there; otherwise start over from the part.
    boost::system::error_code ec;
    bool has_document = false;
    for (fs::directory_iterator it(session.dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == ".FCStd")
            has_document = true;
    return has_document && fs::exists(session.dir / SESSION_MACRO, ec) && launch_session(session);
}

bool FreeCADBridge::launch_session(const Session &session)
{
    if (!FreeCADLauncher::launch(FreeCADLauncher::edit_session_args(m_freecad, u8(session.dir / SESSION_MACRO)))) {
        show_error(m_plater, format_wxstr(_L("Could not start FreeCAD (%1%)."), m_freecad.front()));
        return false;
    }
    return true;
}

std::string FreeCADBridge::opened_message(const std::string &name) const
{
    return format(_u8L("Opened \"%1%\" in FreeCAD. Save in FreeCAD (Ctrl+S) or press \"Update EdgeSlicer\" to update the part here."), name);
}

std::string FreeCADBridge::snapshot_name(const std::string &name) const { return format(_u8L("Edit in FreeCAD: %1%"), name); }

std::string FreeCADBridge::updated_message(const std::string &name) const { return format(_u8L("Updated \"%1%\" from FreeCAD."), name); }

std::string FreeCADBridge::read_failed_message(const std::string &name) const
{
    return format(_u8L("Could not read the part sent back from FreeCAD for \"%1%\"."), name);
}

void FreeCADBridge::install_addon(wxWindow *parent)
{
    const std::string                 source = resources_dir() + "/freecad/" + FreeCADLauncher::ADDON_FOLDER;
    const std::vector<std::string>    dirs   = FreeCADLauncher::find_addon_mod_dirs();
    FreeCADLauncher::InstallReport    report;
    const bool ok = !dirs.empty() && FreeCADLauncher::install_addon(source, dirs, own_executable(), report);
    for (const std::string &dir : report.installed)
        BOOST_LOG_TRIVIAL(info) << "FreeCADBridge: add-on installed in " << dir;
    for (const std::string &error : report.errors)
        BOOST_LOG_TRIVIAL(error) << "FreeCADBridge: add-on install: " << error;

    wxString message;
    if (ok) {
        message = _L("The EdgeSlicer add-on was installed for FreeCAD in:") + "\n";
        for (const std::string &dir : report.installed)
            message += "\n" + from_u8(dir);
        message += "\n\n" + _L("Restart FreeCAD to get the EdgeSlicer toolbar (Send to EdgeSlicer, Update EdgeSlicer).");
    } else {
        message = _L("The EdgeSlicer add-on could not be installed for FreeCAD.");
        if (dirs.empty())
            message += "\n\n" + _L("FreeCAD's user folder could not be determined.");
        for (const std::string &error : report.errors)
            message += "\n\n" + from_u8(error);
        if (!report.installed.empty()) {
            message += "\n\n" + _L("Installed in:");
            for (const std::string &dir : report.installed)
                message += "\n" + from_u8(dir);
        }
    }
    MessageDialog dlg(parent, message, _L("Install FreeCAD add-on"), wxOK | (ok ? wxICON_INFORMATION : wxICON_WARNING));
    dlg.ShowModal();
}

} // namespace GUI
} // namespace Slic3r
