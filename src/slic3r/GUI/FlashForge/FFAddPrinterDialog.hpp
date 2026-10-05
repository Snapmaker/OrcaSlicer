#ifndef slic3r_GUI_FFAddPrinterDialog_hpp_
#define slic3r_GUI_FFAddPrinterDialog_hpp_

#include <string>
#include <vector>

#include <wx/dialog.h>
#include <wx/string.h>

class wxButton;
class wxChoice;
class wxStaticText;
// Not in a namespace, unlike most of our widgets.
class TextInput;

namespace Slic3r { namespace GUI {

// "Add printer" on the FlashForge Device tab: a FlashForge printer by serial number, IP address and
// check code (Settings > Network > Network Mode on the printer), the same three things the Physical
// Printer dialog asks for with Host Type = Flashforge.
//
// The Device tab had no way to put a printer in its list at all. It listed what a LAN scan found
// and what the FlashForge cloud account owned, but nothing ran the scan and the cloud sign-in was
// never mounted, so a Creator 5 added by IP in the printer dialog never showed up on the tab.
//
// Adding asks the printer for its description first (the same read-only call as Test connection),
// so a wrong serial or check code is refused here with the reason, not saved as a dead tile. The
// check code goes into the app config next to the other FlashForge check codes and nowhere else.
class FFAddPrinterDialog : public wxDialog
{
public:
    explicit FFAddPrinterDialog(wxWindow* parent);

    // True when a printer was added (the connection is being opened; its tile appears when it is up).
    bool added() const { return m_added; }

private:
    void on_search(wxCommandEvent& event);
    void on_pick_saved(wxCommandEvent& event);
    void on_add(wxCommandEvent& event);
    void set_status(const wxString& text, bool ok);

    struct SavedPrinter
    {
        wxString    label;
        std::string serial;
        std::string ip;
        std::string check_code;
    };

    // ::TextInput deliberately: see FFDiagnosticsDialog.
    ::TextInput*  m_serial { nullptr };
    ::TextInput*  m_ip { nullptr };
    ::TextInput*  m_code { nullptr };
    wxChoice*     m_saved_choice { nullptr };
    wxButton*     m_search_btn { nullptr };
    wxButton*     m_add_btn { nullptr };
    wxStaticText* m_status { nullptr };

    std::vector<SavedPrinter> m_saved;
    bool                      m_added { false };
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_FFAddPrinterDialog_hpp_
