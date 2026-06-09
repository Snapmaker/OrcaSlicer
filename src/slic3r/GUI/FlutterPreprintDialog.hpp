#ifndef slic3r_FlutterPreprintDialog_hpp_
#define slic3r_FlutterPreprintDialog_hpp_

#include <wx/dialog.h>
#include <string>

#ifdef HAS_FLUTTER

class FlutterPanel;

namespace Slic3r { namespace GUI {

class FlutterPreprintDialog : public wxDialog
{
public:
    FlutterPreprintDialog(FlutterPanel* fltPanel);
    virtual ~FlutterPreprintDialog();
    bool run();

    void set_gcode_file_name(const std::string& filename);
    void set_display_file_name(const std::string& filename);

    bool m_print_confirmed = false;
    std::string   m_gcode_file_name;
    std::string   m_display_file_name;

private:
    void OnClose(wxCloseEvent& evt);
    void OnSize(wxSizeEvent& evt);

    FlutterPanel* m_flt_panel;
    wxWindow*     m_orig_parent = nullptr;

    DECLARE_EVENT_TABLE()
};

}} // namespace Slic3r::GUI
#endif // HAS_FLUTTER
#endif
