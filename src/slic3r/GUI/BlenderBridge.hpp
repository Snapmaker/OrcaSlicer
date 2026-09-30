#ifndef slic3r_GUI_BlenderBridge_hpp_
#define slic3r_GUI_BlenderBridge_hpp_

// "Edit in Blender": opens one part in Blender and swaps the edited mesh back into it.
//
// The part's mesh is written in its own coordinates (before the part's position, rotation and
// scale), so whatever comes back replaces the mesh alone and the part keeps its placement,
// modifiers and settings. resources/blender/edgeslicer_bridge.py writes the edited mesh to the
// session's output file whenever the user saves in Blender; this class polls for that file and
// applies each new version as one undoable step.

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>
#include <wx/event.h>
#include <wx/timer.h>

#include "libslic3r/ObjectID.hpp"

namespace Slic3r {

class ModelVolume;

namespace GUI {

class Plater;
class Selection;

class BlenderBridge : public wxEvtHandler
{
public:
    explicit BlenderBridge(Plater *plater);
    ~BlenderBridge() override;

    // One part selected, or one object made of a single part. Text and SVG parts are left out:
    // their shape is regenerated from the text or the SVG, so an edited mesh would be lost.
    static bool can_edit(const Selection &selection);

    // Exports the selected part and starts Blender on it. Shows a message when Blender cannot be
    // found or started.
    void edit_selection();

private:
    struct Session
    {
        // Every id the part has had while this session ran: applying an edit gives the part a new
        // id, and undo brings back an older one.
        std::vector<ObjectID>   volume_ids;
        std::string             name;
        boost::filesystem::path dir;
        boost::filesystem::path output;
        std::time_t             last_write = 0;
        uintmax_t               last_size  = 0;
    };

    void         on_timer(wxTimerEvent &);
    // False when the part is gone and the session should end.
    bool         poll(Session &session);
    ModelVolume *find_volume(const Session &session, int &object_idx) const;
    void         remove_session_for(const ModelVolume &volume);

    Plater              *m_plater;
    wxTimer              m_timer;
    std::vector<Session> m_sessions;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_BlenderBridge_hpp_
