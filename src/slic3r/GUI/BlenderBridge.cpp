#include "BlenderBridge.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"

#include "slic3r/Utils/BlenderLauncher.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"
#include "format.hpp"

namespace fs = boost::filesystem;

namespace Slic3r {
namespace GUI {

bool BlenderBridge::find_editor()
{
    m_blender = BlenderLauncher::find_blender(wxGetApp().app_config->get("blender_path"));
    if (m_blender.empty()) {
        MessageDialog dlg(m_plater,
                          _L("Blender was not found. Install Blender, or set its location in Preferences under \"Blender path\"."),
                          _L("Edit in Blender"), wxOK | wxICON_INFORMATION);
        dlg.ShowModal();
        return false;
    }

    m_script = resources_dir() + "/blender/edgeslicer_bridge.py";
    if (!fs::exists(to_path(m_script))) {
        BOOST_LOG_TRIVIAL(error) << "BlenderBridge: missing " << m_script;
        show_error(m_plater, _L("The Blender bridge script is missing from this installation."));
        return false;
    }
    return true;
}

bool BlenderBridge::start(const ModelVolume &volume, Session &session)
{
    session.outputs = { session.dir / "edited.stl" };
    const fs::path input = session.dir / to_path(safe_file_stem(session.name) + ".stl");

    boost::system::error_code ec;
    fs::create_directories(session.dir, ec);
    if (ec || !its_write_stl_binary(u8(input).c_str(), session.name.c_str(), volume.mesh().its)) {
        show_error(m_plater, format_wxstr(_L("Could not write the part for Blender to %1%."), u8(session.dir)));
        return false;
    }

    BlenderLauncher::EditSession args;
    args.script         = m_script;
    args.input          = u8(input);
    args.output         = u8(session.outputs.front());
    args.name           = session.name;
    args.edgeslicer_exe = own_executable();
    if (!BlenderLauncher::launch(BlenderLauncher::edit_session_args(m_blender, args))) {
        show_error(m_plater, format_wxstr(_L("Could not start Blender (%1%)."), m_blender.front()));
        return false;
    }
    return true;
}

std::string BlenderBridge::opened_message(const std::string &name) const
{
    return format(_u8L("Opened \"%1%\" in Blender. Save in Blender (Ctrl+S) to update the part here."), name);
}

std::string BlenderBridge::snapshot_name(const std::string &name) const { return format(_u8L("Edit in Blender: %1%"), name); }

std::string BlenderBridge::updated_message(const std::string &name) const { return format(_u8L("Updated \"%1%\" from Blender."), name); }

std::string BlenderBridge::read_failed_message(const std::string &name) const
{
    return format(_u8L("Could not read the part sent back from Blender for \"%1%\"."), name);
}

} // namespace GUI
} // namespace Slic3r
