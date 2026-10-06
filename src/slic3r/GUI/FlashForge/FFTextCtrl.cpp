#include "FFTextCtrl.hpp"
#include <wx/clipbrd.h>
#include <wx/dcclient.h>
#include <wx/dcmemory.h>
#include <wx/sizer.h>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>

namespace Slic3r { namespace GUI {

FFTextCtrl::FFTextCtrl(wxWindow* parent, wxString text, wxSize size, int style, wxString hint) : 
    wxTextCtrl(parent, wxID_ANY, text, wxDefaultPosition, size, style)
{
    SetDoubleBuffered(true);
    SetTextHint(hint);
    Bind(wxEVT_PAINT, &FFTextCtrl::OnPaint, this);
    m_length_label = new Label(this, "0/150");
    m_length_label->SetFont(Label::Body_10);
    m_length_label->SetForegroundColour("#999999");
    m_length_label->SetBackgroundColour(*wxWHITE);
    auto sizer = new wxBoxSizer(wxVERTICAL);
    sizer->AddStretchSpacer();
    sizer->Add(m_length_label, 0, wxALIGN_RIGHT | wxRIGHT, FromDIP(16));
    sizer->AddSpacer(FromDIP(16));
    SetSizer(sizer);
    sizer->Fit(this);
    Layout();
    Bind(wxEVT_TEXT, [=](wxCommandEvent& event) { 
        event.Skip();
        FFTextCtrl* textCtrl = dynamic_cast<FFTextCtrl*>(event.GetEventObject());
        if (!textCtrl) {
            return;
        }
        auto text = textCtrl->GetValue();
        if (text.Length() > m_max_length) {
            textCtrl->ChangeValue(m_old_text.Mid(0, wxMin(m_max_length, m_old_text.Length())));
            textCtrl->SetInsertionPointEnd();
            return;
        }
        auto str  = wxString::Format(wxT("%d/%d"), text.Length(), m_max_length);
        m_length_label->SetLabel(str);
        m_old_text = textCtrl->GetValue();
        Refresh();
    });
    Bind(wxEVT_TEXT_PASTE, [=](wxCommandEvent& event) {
        wxTextCtrl* textCtrl = dynamic_cast<wxTextCtrl*>(event.GetEventObject());
        if (!textCtrl) {
            return;
        }

        if (wxTheClipboard->Open()) {
            wxTextDataObject data;
            if (wxTheClipboard->GetData(data)) {
                wxString pastedText = data.GetText();
                auto     old_size   = textCtrl->GetValue().Length();
                auto     ins        = pastedText.Mid(0, wxMin(pastedText.size(), m_max_length - old_size));
                int pos             = textCtrl->GetInsertionPoint();
                textCtrl->WriteText(ins);
                textCtrl->SetInsertionPoint(pos + ins.Length());
            }
            wxTheClipboard->Close();
        }
        event.Skip(false);
    });
}

void FFTextCtrl::SetTextHint(const wxString& hint) 
{ 
    m_hint = hint;
    Refresh();
}

void FFTextCtrl::SetMaxLength(int max_length) {
    m_max_length = max_length; 
    auto str     = wxString::Format(wxT("%d/%d"), GetValue().ToStdString().size(),
        m_max_length);
    m_length_label->SetLabel(str);
}

void FFTextCtrl::OnPaint(wxPaintEvent& event) 
{
    wxPaintDC dc(this);
    auto      size = GetClientSize();
    if (GetValue().IsEmpty()) {
        wxBitmap   bitmap(size);
        wxMemoryDC memDC;
        memDC.SelectObject(bitmap);
        memDC.SetFont(GetFont());
        wxString sstr;
        const int hint_sper = FromDIP(5);
        Label::split_lines(memDC, size.x - hint_sper * 2, m_hint, sstr);
        boost::algorithm::split(m_vs, sstr.utf8_string(), boost::is_any_of("\n"));
        memDC.SelectObject(wxNullBitmap);
        dc.SetTextForeground(wxColour(150, 150, 150));
        dc.SetFont(GetFont());
        for (int i = 0; i < m_vs.size(); i++) {
            auto text_size = dc.GetTextExtent(wxString::FromUTF8(m_vs[i]));
            dc.DrawText(wxString::FromUTF8(m_vs[i]), hint_sper, i * text_size.y);
        }
        return;
    }
    event.Skip();
}

}} // namespace Slic3r::GUI
