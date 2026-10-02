#ifndef slic3r_GUI_FreeCADBridge_hpp_
#define slic3r_GUI_FreeCADBridge_hpp_

// "Edit in FreeCAD": opens one part in FreeCAD and swaps the edited part back into it.
//
// Exchange formats (in the part's own mesh coordinates, millimetres, so the part keeps its
// placement):
//  * to FreeCAD: STEP with the exact B-rep when the part has one - an attached CAD body (CAD
//    fillet / chamfer / shell, or an earlier round trip) or an unedited STEP import, the same
//    order File > Export > Export as STEP uses - otherwise the mesh as binary STL, which FreeCAD
//    opens as a mesh object;
//  * back: STEP (edited.step) when every visible result in FreeCAD is a solid or shape, tessellated
//    the way a normal STEP import is, with the solid attached to the part as its CAD body (so it
//    stays exact for STEP export, CAD fillets and the next round trip); STL (edited.stl) when a
//    result is still a mesh.
//
// EdgeSlicer writes the part, a session.json describing the session and a small macro
// (edgeslicer_edit.FCMacro) into the session folder, and starts FreeCAD on the macro. The macro
// loads resources/freecad/EdgeSlicerBridge/edgeslicer_bridge.py, which opens the part, saves the
// document next to it and sends the part back whenever the document is saved or "Update
// EdgeSlicer" is pressed. "Edit in FreeCAD" again on the same part reopens that document, so the
// FreeCAD feature history survives while EdgeSlicer is open.

#include <string>
#include <vector>

#include "ExternalEditorBridge.hpp"

class wxWindow;

namespace Slic3r {
namespace GUI {

class FreeCADBridge : public ExternalEditorBridge
{
public:
    explicit FreeCADBridge(Plater *plater) : ExternalEditorBridge(plater) {}

    // One part selected, or one object made of a single part (ExternalEditorBridge::selected_volume).
    static bool can_edit(const Selection &selection) { return ExternalEditorBridge::can_edit(selection); }

    // Preferences > "Install FreeCAD add-on": copies resources/freecad/EdgeSlicerBridge into
    // FreeCAD's user Mod folders and reports the result.
    static void install_addon(wxWindow *parent);

protected:
    bool        find_editor() override;
    std::string folder_name() const override { return "freecad_bridge"; }
    std::string log_tag() const override { return "FreeCADBridge"; }
    bool        start(const ModelVolume &volume, Session &session) override;
    bool        reopen(const ModelVolume &volume, Session &session) override;

    std::string opened_message(const std::string &name) const override;
    std::string snapshot_name(const std::string &name) const override;
    std::string updated_message(const std::string &name) const override;
    std::string read_failed_message(const std::string &name) const override;

private:
    bool launch_session(const Session &session);

    std::vector<std::string> m_freecad;
    std::string              m_bridge_dir;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_FreeCADBridge_hpp_
