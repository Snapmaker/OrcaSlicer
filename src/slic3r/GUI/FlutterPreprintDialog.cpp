#include "FlutterPreprintDialog.hpp"

#ifdef HAS_FLUTTER

#include "Flutter/FlutterPanel.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "Notebook.hpp"
#include "I18N.hpp"
#include <wx/sizer.h>
#include <boost/log/trivial.hpp>

BEGIN_EVENT_TABLE(FlutterPreprintDialog, wxDialog)
    EVT_CLOSE(FlutterPreprintDialog::OnClose)
END_EVENT_TABLE()

FlutterPreprintDialog::FlutterPreprintDialog(FlutterPanel* panel)
    : wxDialog((wxWindow*)wxGetApp().mainframe, wxID_ANY,
               _L("Print Preprocessing")),
      m_flt_panel(panel)
{
    SetBackgroundColour(*wxWHITE);
    SetMinSize(FromDIP(wxSize(714, 750)));
    SetSize(FromDIP(wxSize(714, 750)));
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
    Bind(wxEVT_SIZE, &FlutterPreprintDialog::OnSize, this);
}

FlutterPreprintDialog::~FlutterPreprintDialog()
{
    if (m_flt_panel && m_flt_panel->GetHandle() && m_orig_parent) {
        m_flt_panel->Reparent(m_orig_parent);
        m_flt_panel->Hide();
    }
}

void FlutterPreprintDialog::set_gcode_file_name(const std::string& filename)
{
    m_gcode_file_name = filename;
}

void FlutterPreprintDialog::set_display_file_name(const std::string& filename)
{
    m_display_file_name = filename;
}

bool FlutterPreprintDialog::run()
{
    if (!m_flt_panel || !m_flt_panel->GetHandle()) {
        BOOST_LOG_TRIVIAL(error) << "[FlutterPrint] no FlutterPanel/view";
        return false;
    }

    m_orig_parent = m_flt_panel->GetParent();
    m_flt_panel->Reparent(this);
    m_flt_panel->Show();

    wxSize sz = GetClientSize();
    if (sz.x > 1 && sz.y > 1)
        m_flt_panel->resizeView(sz.x, sz.y);

    m_flt_panel->channel().invoke("switchPage", "print");

    wxGetApp().mainframe->m_active_flt_dialog = this;
    ShowModal();
    wxGetApp().mainframe->m_active_flt_dialog = nullptr;

    if (m_flt_panel && m_flt_panel->GetHandle() && m_orig_parent) {
        m_flt_panel->Reparent(m_orig_parent);
        m_orig_parent = nullptr;
    }
    m_flt_panel->Hide();
    return m_print_confirmed;
}

void FlutterPreprintDialog::OnSize(wxSizeEvent& evt)
{
    if (m_flt_panel && m_flt_panel->GetHandle()) {
        wxSize sz = GetClientSize();
        if (sz.x > 1 && sz.y > 1)
            m_flt_panel->resizeView(sz.x, sz.y);
    }
    evt.Skip();
}

void FlutterPreprintDialog::OnClose(wxCloseEvent& evt)
{
    if (m_flt_panel && m_flt_panel->GetHandle() && m_orig_parent) {
        m_flt_panel->Reparent(m_orig_parent);
        m_orig_parent = nullptr;
    }
    m_flt_panel->Hide();
    EndModal(wxID_CANCEL);
}

#endif // HAS_FLUTTER
