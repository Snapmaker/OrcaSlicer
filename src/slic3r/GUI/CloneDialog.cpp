#include "CloneDialog.hpp"
#include "GLCanvas3D.hpp"   // H2 3.1：真实供给头（Plater.hpp 传递链清算）
#include "app/ArrangementService.hpp"   // H2 3.1：真实供给头
#include "app/LayoutManager.hpp"   // H2 3.1：真实供给头（Plater.hpp 传递链清算）
#include "Plater.hpp"   // H2 3.1 白名单：Plater 完整型解引用（偏离登记）

#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "app/ArrangeRequest.hpp"   // F3：Request 显式传递

namespace Slic3r { namespace GUI {

CloneDialog::CloneDialog(wxWindow *parent)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe), wxID_ANY, _L("Clone"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
{
    SetBackgroundColour(*wxWHITE);
    SetFont(Label::Body_14);

    m_plater = wxGetApp().plater();
    m_config = wxGetApp().app_config;
    m_cancel_process = false;

    auto v_sizer = new wxBoxSizer(wxVERTICAL);
    auto f_sizer = new wxFlexGridSizer(2, 2, FromDIP(4) , FromDIP(20));

    auto count_label = new wxStaticText(this, wxID_ANY, _L("Number of copies:"), wxDefaultPosition, wxDefaultSize, 0);
    m_count_spin = new SpinInput(this, wxEmptyString, "", wxDefaultPosition, wxSize(FromDIP(120), -1), wxSP_ARROW_KEYS, 1, 1000, 1);
    m_count_spin->GetTextCtrl()->SetFocus();
    f_sizer->Add(count_label  , 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(m_count_spin, 0, wxALIGN_CENTER_VERTICAL);

    auto arrange_label = new wxStaticText(this, wxID_ANY, _L("Auto arrange plate after cloning") + ":", wxDefaultPosition, wxDefaultSize, 0);
    arrange_label->Wrap(FromDIP(300));
    m_arrange_cb = new ::CheckBox(this);
    m_arrange_cb->SetValue(m_config->get("auto_arrange") == "true");
    f_sizer->Add(arrange_label, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(m_arrange_cb , 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, FromDIP(5));

    v_sizer->Add(f_sizer, 1, wxEXPAND | wxALL, FromDIP(10));

    auto bottom_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_progress = new ProgressBar(this, wxID_ANY, 100);
    m_progress->SetHeight(FromDIP(8));
    m_progress->SetMaxSize(wxSize(-1, FromDIP(8)));
    m_progress->SetProgressForedColour(StateColor::darkModeColorFor(wxColour("#DFDFDF")));
    m_progress->SetDoubleBuffered(true);
    m_progress->Hide();
    bottom_sizer->Add(m_progress, 2, wxEXPAND | wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(10));

    // used next button to get automatic left alignment
    // will add a left_align_first_n parameter to DialogButtons. current method not good
    auto dlg_btns = new DialogButtons(this, {"Next", "OK", "Cancel"});

    dlg_btns->GetNEXT()->SetLabel(_L("Fill"));
    dlg_btns->GetNEXT()->SetToolTip(_L("Fill bed with copies"));
    dlg_btns->GetNEXT()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) {
        wxGetApp().arrange_svc().fill_bed_with_instances();
        EndModal(wxID_OK);
    });

    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, [this, dlg_btns, v_sizer](wxCommandEvent &e) {

        m_count_spin->Disable(); // also ensures input box value applied with wxEVT_KILL_FOCUS
        m_arrange_cb->Disable();

        m_count = m_count_spin->GetValue();

        m_progress->Show();

        dlg_btns->GetOK()->Hide();
        dlg_btns->GetNEXT()->Hide();

        this->Layout();
        v_sizer->Fit(this);
        Refresh();

        Selection& sel = wxGetApp().layout().get_current_canvas3D()->get_selection();
        wxGetApp().snapshots().take_snapshot(std::string("Selection-clone"));
        m_plater->Freeze(); // Better to stop rendering canvas while processing
        sel.copy_to_clipboard();
        for (int i = 0; i < m_count; i++) { // same method with Selection::clone()
            m_progress->SetValue(static_cast<int>(static_cast<double>(i) / m_count * 100)); // pass 0 / 100
            sel.paste_from_clipboard();
            if(m_cancel_process){
                wxGetApp().snapshots().undo();
                return;
            }
            wxYield(); // Allow event loop to process updates
        }

        if(!m_cancel_process){
            if (m_arrange_cb->GetValue()){
                wxGetApp().arrange_svc().arrange(app::ArrangeRequest::current_plate());   // F3：原写点 MENU + 无参 arrange 改随调用 Request
            }
            m_plater->Thaw();
            EndModal(wxID_OK);
        }
    });

    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) {
        m_cancel_process = true;
        if(m_plater->IsFrozen())
            m_plater->Thaw();
        EndModal(wxID_CANCEL);
    });

    bottom_sizer->Add(dlg_btns, 1, wxEXPAND);

    v_sizer->Add(bottom_sizer, 0, wxEXPAND);

    this->SetSizer(v_sizer);
    this->Layout();
    v_sizer->Fit(this);
    wxGetApp().UpdateDlgDarkUI(this);
}

CloneDialog::~CloneDialog() {}

}} // namespace Slic3r::GUI