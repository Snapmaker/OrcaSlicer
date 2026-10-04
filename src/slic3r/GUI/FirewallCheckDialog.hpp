#pragma once

// Help > Check Windows Firewall: what Windows Firewall does to this copy of EdgeSlicer (Bambu
// printer discovery, the phone hub, phone camera video), in plain words, and a "Fix firewall rules"
// button that runs the elevated helper (one UAC prompt). The logic is slic3r/Utils/WinFirewall.

#include "GUI_Utils.hpp"
#include "slic3r/Utils/WinFirewall.hpp"

#include <memory>

class wxBoxSizer;
class wxPanel;
class wxStaticText;
class Button;
class CheckBox;

namespace Slic3r {
namespace GUI {

class FirewallCheckDialog : public DPIDialog
{
public:
    explicit FirewallCheckDialog(wxWindow* parent);
    ~FirewallCheckDialog() override;

    // Windows: shows the dialog modally. Elsewhere: a short note that there is nothing to check.
    static void show_modal(wxWindow* parent);
    // Whether the menu entries make sense on this platform.
    static bool supported();

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    enum class Severity { Ok, Note, Problem };

    void run_check();
    void rebuild_lines();
    void add_line(Severity sev, const wxString& text);
    void update_public_hint();
    void start_fix();
    void on_fix_done(const WinFirewall::ElevatedOutcome& outcome, bool allow_public);
    void open_network_settings();
    void relayout();

    WinFirewall::Diagnosis m_diag;
    bool                   m_fixing { false };
    std::shared_ptr<bool>  m_alive;

    wxStaticText* m_summary { nullptr };
    wxPanel*      m_lines_panel { nullptr };
    wxBoxSizer*   m_lines_sizer { nullptr };
    ::CheckBox*   m_cb_public { nullptr };
    wxStaticText* m_public_label { nullptr };
    wxStaticText* m_public_explain { nullptr };
    wxPanel*      m_public_hint { nullptr };
    wxStaticText* m_public_hint_text { nullptr };
    ::Button*     m_btn_settings { nullptr };
    wxStaticText* m_result { nullptr };
    ::Button*     m_btn_fix { nullptr };
    ::Button*     m_btn_recheck { nullptr };
    ::Button*     m_btn_close { nullptr };
};

} // namespace GUI
} // namespace Slic3r
