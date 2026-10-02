#pragma once

// The first-time Bambu printer setup notice. A friendly checklist of what a Bambu Lab printer needs
// before it shows up and connects (account, firewall, LAN Only mode, access code, SD card, same
// network), with a live status next to the items that can be checked cheaply and without admin
// rights. Whether it pops up is decided by Utils/BambuSetupNotice (pure, unit-tested); this file
// gathers the inputs, draws the dialog and owns the app_config flag.
//
// It stays reachable from Help > Bambu printer setup... and from the Device page's "Can't find my
// devices?" area.

#include "GUI_Utils.hpp"
#include "slic3r/Utils/WinFirewall.hpp"

class Button;
class CheckBox;
class Label;

namespace Slic3r {
namespace GUI {

class BambuSetupNoticeDialog : public DPIDialog
{
public:
    explicit BambuSetupNoticeDialog(wxWindow* parent);

    // Shows the dialog modally, whatever the flag says (the menu entry and the Device page link).
    static void show_modal(wxWindow* parent);

    // The automatic triggers. Each is cheap and safe to call from anywhere on the UI thread; the
    // popup itself waits until the main window is up and no other modal dialog is open.
    // A Bambu Lab printer preset was picked by hand (a project load or a remote switch is not this).
    static void on_printer_preset_selected();
    // The setup wizard closed (the printer it left selected may be a Bambu Lab one).
    static void on_wizard_finished();
    // The Device tab was opened; fires if it still lists nothing a little later.
    static void on_device_tab_shown();

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    void refresh_status();
    void save_flag();
    void relayout();

    WinFirewall::Diagnosis m_diag;

    ::Label*    m_status_account { nullptr };
    ::Label*    m_status_firewall { nullptr };
    ::Label*    m_status_network { nullptr };
    ::Button*   m_btn_firewall { nullptr };
    ::CheckBox* m_cb_dont_show { nullptr };
    ::Button*   m_btn_ok { nullptr };
};

} // namespace GUI
} // namespace Slic3r
