#ifndef slic3r_GUI_ExternalEditorBridge_hpp_
#define slic3r_GUI_ExternalEditorBridge_hpp_

// What "Edit in Blender" and "Edit in FreeCAD" share: one part is exported into a session folder,
// another program is started on it, and every new version of the file that program writes back
// replaces the part's mesh as one undoable step.
//
// The part is exported in its own mesh coordinates (before the part's position, rotation and
// scale, and before the instance's), so whatever comes back replaces the mesh alone and the part
// keeps its placement, name, modifiers, settings and source (libslic3r/PartMeshReplace.hpp).
//
// A derived bridge finds its program, writes the part and starts the program (start()), and says
// how its messages read; this class owns the session folders, polls the files written back and
// swaps the mesh in.

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>
#include <wx/event.h>
#include <wx/timer.h>

#include "libslic3r/ObjectID.hpp"

namespace Slic3r {

class Model;
class ModelVolume;
class TriangleMesh;

namespace GUI {

class Plater;
class Selection;

class ExternalEditorBridge : public wxEvtHandler
{
public:
    explicit ExternalEditorBridge(Plater *plater);
    ~ExternalEditorBridge() override;

    // The model volume `selection` stands for: one part selected, or an object made of a single
    // part. Text and SVG parts are left out: their shape is regenerated from the text or the SVG,
    // so an edited mesh would be lost.
    static ModelVolume *selected_volume(const Selection &selection, Model &model, int &object_idx);
    static bool         can_edit(const Selection &selection);

    // Exports the selected part and starts the program on it. Shows a message when the program
    // cannot be found or started.
    void edit_selection();

protected:
    struct Session
    {
        // Every id the part has had while this session ran: applying an edit gives the part a new
        // id, and undo brings back an older one.
        std::vector<ObjectID>                volume_ids;
        std::string                          name;
        boost::filesystem::path              dir;
        // The files the program writes back; the most recently written one is applied.
        std::vector<boost::filesystem::path> outputs;
        boost::filesystem::path              last_output;
        std::time_t                          last_write = 0;
        uintmax_t                            last_size  = 0;
    };

    // Looks for the program. False (after telling the user) when it cannot be used.
    virtual bool        find_editor() = 0;
    // Folder inside data_dir() that holds this bridge's session folders.
    virtual std::string folder_name() const = 0;
    // Prefix of this bridge's log lines.
    virtual std::string log_tag() const = 0;
    // Writes the part into session.dir (not created yet), fills session.outputs and starts the
    // program. False (after telling the user) on failure; the folder is removed then.
    virtual bool        start(const ModelVolume &volume, Session &session) = 0;
    // "Edit in ..." again on a part that already has a session: true when that session was
    // brought back instead (the program reopened on its own file); false starts over.
    virtual bool        reopen(const ModelVolume &volume, Session &session) { return false; }

    virtual std::string opened_message(const std::string &name) const      = 0;
    virtual std::string snapshot_name(const std::string &name) const       = 0;
    virtual std::string updated_message(const std::string &name) const     = 0;
    virtual std::string read_failed_message(const std::string &name) const = 0;

    // Reads a file the program sent back: STL, or STEP tessellated like a normal STEP import.
    virtual bool read_output(const boost::filesystem::path &file, TriangleMesh &mesh) const;

    // Helpers for the derived bridges.
    static boost::filesystem::path to_path(const std::string &utf8);
    static std::string             u8(const boost::filesystem::path &path);
    // This install, as the other program's "Send to EdgeSlicer" should start it.
    static std::string             own_executable();
    static std::string             safe_file_stem(const std::string &name);
    boost::filesystem::path        bridge_root() const;

    Plater *m_plater;

private:
    void         on_timer(wxTimerEvent &);
    // False when the part is gone and the session should end.
    bool         poll(Session &session);
    ModelVolume *find_volume(const Session &session, int &object_idx) const;
    Session     *find_session_for(const ModelVolume &volume);
    void         remove_session_for(const ModelVolume &volume);
    void         remove_stale_sessions() const;

    wxTimer              m_timer;
    std::vector<Session> m_sessions;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_ExternalEditorBridge_hpp_
