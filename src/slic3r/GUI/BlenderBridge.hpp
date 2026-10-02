#ifndef slic3r_GUI_BlenderBridge_hpp_
#define slic3r_GUI_BlenderBridge_hpp_

// "Edit in Blender": opens one part in Blender and swaps the edited mesh back into it.
//
// The part's mesh is written in its own coordinates (before the part's position, rotation and
// scale), so whatever comes back replaces the mesh alone and the part keeps its placement,
// modifiers and settings. resources/blender/edgeslicer_bridge.py writes the edited mesh to the
// session's output file whenever the user saves in Blender; ExternalEditorBridge polls for that
// file and applies each new version as one undoable step.

#include <string>
#include <vector>

#include "ExternalEditorBridge.hpp"

namespace Slic3r {
namespace GUI {

class BlenderBridge : public ExternalEditorBridge
{
public:
    explicit BlenderBridge(Plater *plater) : ExternalEditorBridge(plater) {}

    // One part selected, or one object made of a single part (ExternalEditorBridge::selected_volume).
    static bool can_edit(const Selection &selection) { return ExternalEditorBridge::can_edit(selection); }

protected:
    bool        find_editor() override;
    std::string folder_name() const override { return "blender_bridge"; }
    std::string log_tag() const override { return "BlenderBridge"; }
    bool        start(const ModelVolume &volume, Session &session) override;

    std::string opened_message(const std::string &name) const override;
    std::string snapshot_name(const std::string &name) const override;
    std::string updated_message(const std::string &name) const override;
    std::string read_failed_message(const std::string &name) const override;

private:
    std::vector<std::string> m_blender;
    std::string              m_script;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_BlenderBridge_hpp_
