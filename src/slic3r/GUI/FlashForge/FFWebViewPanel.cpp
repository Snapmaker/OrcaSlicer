#include "FFWebViewPanel.hpp"
#include <algorithm>
#include <chrono>
#include <map>
#include <boost/filesystem.hpp>
#include <boost/json/src.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <wx/base64.h>
#include <wx/file.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/object.h>
#include <wx/sizer.h>
#include <wx/url.h>
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/FFUtils.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/FlashForge/MultiComHelper.hpp"
#include "slic3r/GUI/FlashForge/MultiComMgr.hpp"

namespace Slic3r { namespace GUI {

wxDEFINE_EVENT(NAV_MORE_MENU_EVENT, wxCommandEvent);
wxDEFINE_EVENT(REPORT_ITEM_SELECTED_EVENT, wxCommandEvent);
wxDEFINE_EVENT(REPORT_BUTTON_EVENT, wxCommandEvent);
wxDEFINE_EVENT(VIEW_NOW_BUTTON_EVENT, wxCommandEvent);
wxDEFINE_EVENT(FIND_DOWNLOAD_URL_EVENT, FindDownloadUrlEvent);
wxDEFINE_EVENT(OPEN_BASE64_MODEL_EVENT, wxCommandEvent);

NavMoreMenu::NavMoreMenu(wxWindow *parent)
    : FFTransientWindow(parent)
    , m_hoverItemIndex(-1)
{
    SetFont(Label::Body_14);
    Bind(wxEVT_PAINT, &NavMoreMenu::OnPaint, this);
    Bind(wxEVT_LEFT_UP, &NavMoreMenu::OnLeftUp, this);
    Bind(wxEVT_MOTION, &NavMoreMenu::OnMotion, this);
}

void NavMoreMenu::AddItem(const std::string &icon, int iconHeight, const wxString &text)
{
    int iconWidth;
    if (icon.empty()) {
        m_iconBmps.emplace_back(nullptr);
        iconWidth = 0;
    } else {
        m_iconBmps.emplace_back(std::make_unique<ScalableBitmap>(this, icon, iconHeight));
        iconWidth = m_iconBmps.back()->GetBmpWidth() + FromDIP(IconSpace);
    }
    m_texts.emplace_back(text);

    wxScreenDC dc;
    dc.SetFont(GetFont());
    m_textSizes.emplace_back(dc.GetTextExtent(text));
    
    int width = std::max(FromDIP(160), m_textSizes.back().x + iconWidth + FromDIP(16));
    int height = FromDIP(ItemHeight) * m_texts.size() + 2;
    SetSize(wxSize(width, height));
    SetMinSize(wxSize(width, height));
    SetMaxSize(wxSize(width, height));
}

void NavMoreMenu::OnPaint(wxPaintEvent &evt)
{
    wxPaintDC dc(this);
    int width = GetSize().x;
    for (size_t i = 0; i < m_iconBmps.size(); ++i) {
        int itemHeightDIP = FromDIP(ItemHeight);
        int roundedHeight = m_radius * 2 + 1;
        int itemY = i * itemHeightDIP + 1;
        if (i == m_hoverItemIndex) {
            if (m_iconBmps.size() == 1) {
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.SetBrush(wxColour("#d9eaff"));
                dc.DrawRoundedRectangle(1, itemY, width - 2, itemHeightDIP, m_radius);
            } else if (i == 0) {
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.SetBrush(wxColour("#d9eaff"));
                dc.DrawRoundedRectangle(1, itemY, width - 2, roundedHeight, m_radius);
                dc.DrawRectangle(1, itemY + m_radius, width - 2, itemHeightDIP - m_radius);
            } else if (i == m_iconBmps.size() - 1) {
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.SetBrush(wxColour("#d9eaff"));
                dc.DrawRectangle(1, itemY, width - 2, itemHeightDIP - m_radius);
                dc.DrawRoundedRectangle(1, itemY + itemHeightDIP - roundedHeight, width - 2, roundedHeight, m_radius);
            } else {
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.SetBrush(wxColour("#d9eaff"));
                dc.DrawRectangle(1, itemY, width - 2, itemHeightDIP);
            }
        }
        if (m_iconBmps[i].get() == nullptr) {
            int x = (width - m_textSizes[i].x) / 2;
            int y = (itemHeightDIP - m_textSizes[i].y) / 2 + itemY;
            dc.DrawText(m_texts[i], x, y);
        } else {
            int contentWidth = m_iconBmps[i]->GetBmpWidth() + FromDIP(IconSpace) + m_textSizes[i].GetWidth();
            int iconX = (width - contentWidth) / 2;
            int iconY = (itemHeightDIP - m_iconBmps[i]->GetBmpHeight()) / 2 + itemY;
            int textX = iconX + m_iconBmps[i]->GetBmpWidth() + FromDIP(IconSpace);
            int textY = (itemHeightDIP - m_textSizes[i].y) / 2 + itemY;
            dc.DrawBitmap(m_iconBmps[i]->bmp(), iconX, iconY);
            dc.DrawText(m_texts[i], textX, textY);
        }
    }
    dc.SetPen(wxColour("#c1c1c1"));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawRoundedRectangle(GetRect(), m_radius);
}

void NavMoreMenu::OnLeftUp(wxMouseEvent &evt)
{
    evt.Skip();
    if (m_hoverItemIndex == -1) {
        return;
    }
    wxCommandEvent event(NAV_MORE_MENU_EVENT);
    event.SetEventObject(this);
    event.SetId(GetId());
    event.SetInt(m_hoverItemIndex);
    wxPostEvent(this, event);
    Show(false);
}

void NavMoreMenu::OnMotion(wxMouseEvent &evt)
{
    evt.Skip();
    int hoverItemIndex;
    wxPoint pos = evt.GetPosition();
    if (HitTest(pos) == wxHT_WINDOW_OUTSIDE) {
        hoverItemIndex = -1;
    } else {
        hoverItemIndex = (pos.y - 1) / FromDIP(ItemHeight);
    }
    if (hoverItemIndex != m_hoverItemIndex) {
        m_hoverItemIndex = hoverItemIndex;
        Refresh();
    }
}

ReportOptionItem::ReportOptionItem(wxWindow *parent, const wxString &text, int id)
    : wxPanel(parent)
    , m_selectedBmp(this, "report_item_selected", 13)
    , m_text(text)
    , m_id(id)
    , m_isHover(false)
    , m_isSelected(false)
{
    wxScreenDC dc;
    dc.SetFont(GetFont());
    m_textSize = dc.GetTextExtent(text);
    int contentWidth = m_textSize.x + FromDIP(Spacing) * 3 + FromDIP(IconSize);
    int minWidth = std::clamp(contentWidth, FromDIP(300), FromDIP(500));

    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetDoubleBuffered(true);
    SetFont(Label::Body_12);
    SetSize(wxSize(minWidth, FromDIP(Height)));
    SetMinSize(wxSize(minWidth, FromDIP(Height)));
    SetMaxSize(wxSize(-1, FromDIP(Height)));

    Bind(wxEVT_PAINT, &ReportOptionItem::OnPaint, this);
    Bind(wxEVT_LEFT_DOWN, &ReportOptionItem::OnLeftDown, this);
    Bind(wxEVT_ENTER_WINDOW, &ReportOptionItem::OnEnterWindow, this);
    Bind(wxEVT_LEAVE_WINDOW, &ReportOptionItem::OnLeaveWindow, this);
}

void ReportOptionItem::SetSelected(bool isSelected)
{
    m_isSelected = isSelected;
    Refresh();
}

void ReportOptionItem::OnPaint(wxPaintEvent &evt)
{
    wxPaintDC dc(this);
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (gc == nullptr) {
        return;
    }
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->SetBrush(*wxWHITE);
    gc->DrawRectangle(0, 0, GetSize().x, GetSize().y);
    if (m_isHover || m_isSelected) {
        gc->SetBrush(wxColour("#328DFB"));
        dc.SetTextForeground(*wxWHITE);
    } else {
        gc->SetBrush(wxColour("#F5F5F5"));
        dc.SetTextForeground(wxColour("#3333333"));
    }
    gc->DrawRoundedRectangle(0, 0, GetSize().x, GetSize().y, FromDIP(6));
    if (m_isSelected) {
        int selectedWidth = m_selectedBmp.GetBmpWidth();
        int selectedHeight = m_selectedBmp.GetBmpHeight();
        int selectedX = GetSize().x - Spacing - selectedWidth;
        int selectedY = (GetSize().y - selectedHeight) / 2;
        gc->DrawBitmap(m_selectedBmp.bmp(), selectedX, selectedY, selectedWidth, selectedHeight);
    }
    dc.DrawText(m_text, FromDIP(Spacing), (FromDIP(Height) - m_textSize.y) / 2);
}

void ReportOptionItem::OnLeftDown(wxMouseEvent &evt)
{
    m_isSelected = true;
    wxCommandEvent event(REPORT_ITEM_SELECTED_EVENT);
    event.SetEventObject(this);
    event.SetId(GetId());
    event.SetInt(m_id);
    wxPostEvent(this, event);
    Refresh();
}

void ReportOptionItem::OnEnterWindow(wxMouseEvent &evt)
{
    m_isHover = true;
    Refresh();
}

void ReportOptionItem::OnLeaveWindow(wxMouseEvent &evt)
{
    m_isHover = false;
    Refresh();
}

ReportWindow::ReportWindow(wxWindow *parent, const nlohmann::json &data)
    : wxDialog(parent, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxFRAME_SHAPED | wxBORDER_NONE)
    , m_radius(FromDIP(6))
{
    try {
        Initialize(data);
        m_isOk = true;
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "ReportWindow parse json Error, "
            << e.what() << ", " << nlohmann::to_string(data);
        m_isOk = false;
    }
}

void ReportWindow::Initialize(const nlohmann::json &data)
{
    SetBackgroundColour(*wxWHITE);
    wxString windowTitle = wxString::FromUTF8((std::string)data.at("name"));
    m_titleBar = new TitleBar(this, windowTitle, wxColour("#E1E2E6"), m_radius);

    wxString reportTitle = wxString::FromUTF8((std::string)data.at("title"));
    wxFont font = Label::Body_14;
    font.SetWeight(wxFONTWEIGHT_MEDIUM);
    m_reportTitleLbl = new wxStaticText(this, wxID_ANY, reportTitle);
    m_reportTitleLbl->SetForegroundColour(wxColour("#333333"));
    m_reportTitleLbl->SetFont(font);

    const nlohmann::json &itemArr = data.at("options");
    for (size_t i = 0; i < itemArr.size(); ++i) {
        wxString optionText = wxString::FromUTF8((std::string)itemArr[i]["option"]);
        m_optionItems.emplace_back(new ReportOptionItem(this, optionText, itemArr[i]["id"]));
        m_optionItems[i]->Bind(REPORT_ITEM_SELECTED_EVENT, &ReportWindow::OnItemSelected, this);
    }

    m_textCtrl = new FFTextCtrl(this, "", wxDefaultSize, wxBORDER_NONE | wxTE_MULTILINE);
    m_textCtrl->SetBackgroundColour(*wxWHITE);
    m_textCtrl->SetFont(Label::Body_12);
    m_textCtrl->SetSize(wxSize(-1, FromDIP(128)));
    m_textCtrl->SetMinSize(wxSize(-1, FromDIP(128)));
    m_textCtrl->SetMaxSize(wxSize(-1, FromDIP(128)));
    m_textCtrl->SetTextHint(wxString::FromUTF8((std::string)data.at("otherTips")));
    m_textCtrl->SetMaxLength(data.at("otherTipsMaxNum"));
    m_textCtrl->Hide();
    m_textCtrl->Bind(wxEVT_TEXT, &ReportWindow::OnTextChanged, this);

    m_textCtrlDummyPnl = new wxPanel(this);
    m_textCtrlDummyPnl->SetBackgroundColour(*wxWHITE);
    m_textCtrlDummyPnl->SetSize(wxSize(-1, FromDIP(128)));
    m_textCtrlDummyPnl->SetMinSize(wxSize(-1, FromDIP(128)));
    m_textCtrlDummyPnl->SetMaxSize(wxSize(-1, FromDIP(128)));

    wxFont reportBtnFont = Label::Body_14;
    reportBtnFont.SetWeight(wxFONTWEIGHT_MEDIUM);
    m_reportBtn = new FFButton(this, wxID_ANY, "", FromDIP(16));
    m_reportBtn->SetBackgroundColour(*wxWHITE);
    m_reportBtn->SetDoubleBuffered(true);
    m_reportBtn->SetFont(reportBtnFont);
    m_reportBtn->SetLabel(_CTX("Submit", "Flashforge"), FromDIP(96), FromDIP(20), FromDIP(32), FromDIP(6));
    m_reportBtn->SetFontUniformColor(*wxWHITE);
    m_reportBtn->SetBorderWidth(0);
    m_reportBtn->SetBGColor(wxColour("#328DFB"));
    m_reportBtn->SetBGHoverColor(wxColour("#48AAFE"));
    m_reportBtn->SetBGPressColor(wxColour("#328DFB"));
    m_reportBtn->SetBGDisableColor(wxColour("#E5E5E5"));
    m_reportBtn->Enable(false);
    m_reportBtn->Bind(wxEVT_BUTTON, &ReportWindow::OnReportButton, this);

    wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);
    sizer->AddSpacer(1);
    sizer->Add(m_titleBar, 0, wxEXPAND | wxLEFT | wxRIGHT, 1);
    sizer->AddSpacer(FromDIP(16));
    sizer->Add(m_reportTitleLbl, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(24));
    sizer->AddSpacer(FromDIP(15));
    for (size_t i = 0; i < m_optionItems.size(); ++i) {
        sizer->Add(m_optionItems[i], 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(24));
        sizer->AddSpacer(FromDIP(11));
    }
    sizer->AddSpacer(FromDIP(TextCtrlSpacing));
    sizer->Add(m_textCtrl, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(24) + FromDIP(TextCtrlSpacing));
    sizer->Add(m_textCtrlDummyPnl, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(24) + FromDIP(TextCtrlSpacing));
    sizer->AddSpacer(FromDIP(12) + FromDIP(TextCtrlSpacing));
    sizer->Add(m_reportBtn, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, FromDIP(24));
    sizer->AddSpacer(FromDIP(12));
    SetSizer(sizer);

    Bind(wxEVT_PAINT, &ReportWindow::OnPaint, this);
    Bind(wxEVT_SIZE, &ReportWindow::OnSize, this);
    Layout();
    Fit();
    CenterOnParent();
}

void ReportWindow::OnPaint(wxPaintEvent &evt)
{
    wxPaintDC dc(this);
    wxSize size = GetSize();
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxColour("#c1c1c1"));
    dc.DrawRectangle(0, 0, size.x, size.y);

    dc.SetBrush(*wxWHITE);
    dc.DrawRoundedRectangle(1, 1, size.x - 2, size.y - 2, m_radius);

    if (m_textCtrl->IsShown()) {
        wxRect textCtrlRect = m_textCtrl->GetRect();
        int textCtrlSpacingDIP = FromDIP(TextCtrlSpacing);
        int textCtrlX = textCtrlRect.x - textCtrlSpacingDIP;
        int textCtrlY = textCtrlRect.y - textCtrlSpacingDIP;
        int textCtrlWidth = textCtrlRect.width + 2 * textCtrlSpacingDIP;
        int textCtrlHeight = textCtrlRect.height + 2 * textCtrlSpacingDIP;
        dc.SetPen(wxColour("#c1c1c1"));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRoundedRectangle(textCtrlX, textCtrlY, textCtrlWidth, textCtrlHeight, m_radius);
    }
}

void ReportWindow::OnSize(wxSizeEvent &evt)
{
    evt.Skip();
    wxEventBlocker evtBlocker(this, wxEVT_SIZE);
    wxGraphicsPath path = wxGraphicsRenderer::GetDefaultRenderer()->CreatePath();
    path.AddRoundedRectangle(0, 0, GetSize().x, GetSize().y, m_radius);
    SetShape(path);
}

void ReportWindow::OnItemSelected(wxCommandEvent &evt)
{
    for (size_t i = 0; i < m_optionItems.size(); ++i) {
        if (m_optionItems[i]->GetId() != evt.GetInt()) {
            m_optionItems[i]->SetSelected(false);
        }
    }
    bool showTextCtrl = evt.GetId() == 0;
    if (showTextCtrl != m_textCtrl->IsShown()) {
        m_textCtrl->Show(showTextCtrl);
        m_textCtrlDummyPnl->Show(!showTextCtrl);
        Layout();
        Refresh();
    }
    m_reportBtn->Enable(!showTextCtrl || !m_textCtrl->GetValue().empty());
}

void ReportWindow::OnTextChanged(wxCommandEvent &evt)
{
    evt.Skip();
    m_reportBtn->Enable(!m_textCtrl->IsShown() || !m_textCtrl->GetValue().empty());
}

void ReportWindow::OnReportButton(wxCommandEvent &evt)
{
    int selectionOptionId = 0;
    for (size_t i = 0; i < m_optionItems.size(); ++i) {
        if (m_optionItems[i]->IsSelected()) {
            selectionOptionId = m_optionItems[i]->GetId();
            break;
        }
    }
    wxCommandEvent reportModelEvent(REPORT_BUTTON_EVENT);
    reportModelEvent.SetInt(selectionOptionId);
    reportModelEvent.SetString(m_textCtrl->GetValue());
    wxPostEvent(this, reportModelEvent);
    EndModal(wxID_CANCEL);
}

PrintListTipWindow::PrintListTipWindow(wxWindow *parent)
    : FFRoundedWindow(parent)
    , m_timer(this)
{
    SetBackgroundColour(*wxWHITE);
    SetSize(wxSize(-1, FromDIP(38)));
    SetMaxSize(wxSize(-1, FromDIP(52)));

    m_printListTipLbl = new wxStaticText(this, wxID_ANY, "print_list_tip");
    m_printListTipLbl->SetForegroundColour(wxColour("#333333"));
    m_printListTipLbl->SetFont(Label::Body_13);

    wxFont viewNowFont = Label::Body_13;
    viewNowFont.SetWeight(wxFONTWEIGHT_MEDIUM);
    m_button = new FFButton(this, wxID_ANY, "", FromDIP(13));
    m_button->SetBackgroundColour(*wxWHITE);
    m_button->SetDoubleBuffered(true);
    m_button->SetFont(viewNowFont);
    m_button->SetLabel(_L("View Now"), FromDIP(52), FromDIP(12), FromDIP(26), FromDIP(4));
    m_button->SetFontUniformColor(*wxWHITE);
    m_button->SetBorderWidth(0);
    m_button->SetBGColor(wxColour("#328DFB"));
    m_button->SetBGHoverColor(wxColour("#48AAFE"));
    m_button->SetBGPressColor(wxColour("#328DFB"));

    Bind(wxEVT_TIMER, [this](wxTimerEvent &) { Hide(); });
    m_button->Bind(wxEVT_BUTTON, &PrintListTipWindow::OnButton, this);

    wxBoxSizer *sizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add(m_printListTipLbl, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(20));
    sizer->AddStretchSpacer(1);
    sizer->Add(m_button, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(20));
    sizer->AddSpacer(FromDIP(20));
    SetSizer(sizer);
    Layout();
    Fit();
}

void PrintListTipWindow::ShowAutoClose(int msTime)
{
    if (msTime <= 0) {
        return;
    }
    Show();
    m_timer.StartOnce(msTime);
}

void PrintListTipWindow::Setup(const wxString &text, bool showButton)
{
    if (IsShown()) {
        Hide();
        m_timer.Stop();
    }
    if (showButton) {
        SetMinSize(wxSize(FromDIP(256), FromDIP(52)));
    } else {
        SetMinSize(wxSize(FromDIP(128), FromDIP(52)));
    }
    m_printListTipLbl->SetLabelText(text);
    m_button->Show(showButton);
    Layout();
    Fit();
}

bool PrintListTipWindow::IsAutoCloseTimerRunning()
{
    return m_timer.IsRunning();
}

void PrintListTipWindow::OnButton(wxCommandEvent &evt)
{
    wxCommandEvent event(VIEW_NOW_BUTTON_EVENT);
    event.SetEventObject(this);
    event.SetId(GetId());
    wxPostEvent(this, event);
    Hide();
}

ComThreadPool *CheckDownloadUrl::s_threadPool = new ComThreadPool(10, 60000);

void CheckDownloadUrl::AddUrl(const wxString &url, const std::string &userAgent)
{
    s_threadPool->post([weakSelf = weak_from_this(), url, userAgent]() {
        try {
            std::vector<std::string> keys = { "Content-Disposition:", "Content-Type:" };
            std::map<std::string, std::string> headerMap;
            FFUtils::getHttpHeaders(url.ToStdString(), keys, userAgent, headerMap, ComTimeoutWanA);
            std::string contentDispositionHeader;
            auto contentDispositionHeaderIt = headerMap.find(keys[0]);
            if (contentDispositionHeaderIt != headerMap.end()) {
                contentDispositionHeader = contentDispositionHeaderIt->second;
            }
            std::string contentTypeHeader;
            auto contentTypeHeaderIt = headerMap.find(keys[1]);
            if (contentTypeHeaderIt != headerMap.end()) {
                contentTypeHeader = contentTypeHeaderIt->second;
            }
            std::shared_ptr<CheckDownloadUrl> self = weakSelf.lock();
            if (self.get() == nullptr) {
                return;
            }
            wxString fileName;
            bool isSupportedFormat = false;
            if (self->IsDownloadUrl(url, contentDispositionHeader, contentTypeHeader, fileName, isSupportedFormat)) {
                FindDownloadUrlEvent *event = new FindDownloadUrlEvent(
                    FIND_DOWNLOAD_URL_EVENT, url, fileName, isSupportedFormat);
                self->QueueEvent(event);
            }
        } catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(error) << "CheckDownloadUrl::AddUrl exception," << e.what();
        }
    });
}

bool CheckDownloadUrl::IsDownloadUrl(const wxString &url, const std::string &contentDispositionHeader,
    const std::string &contentTypeHeader, wxString &fileName, bool &isSupportedFormat)
{
    fileName = GetFileName(contentDispositionHeader);
    if (fileName.IsEmpty()) {
        wxURL wxUrl(url);
        fileName = wxFileNameFromPath(wxString::FromUTF8(FFUtils::urlUnescape(wxUrl.GetPath().ToStdString())));
    }
    const std::regex patternSuffix(".*[.](stp|step|stl|oltp|obj|amf|3mf|svg|zip|gcode|g)$", std::regex::icase);
    isSupportedFormat = std::regex_match(fileName.utf8_string(), patternSuffix);

    std::regex patternDisposition(R"(Content-Disposition:\s*(attachment|inline|form-data)\b)", std::regex::icase);
    std::smatch matchesDisposition;
    if (std::regex_search(contentDispositionHeader, matchesDisposition, patternDisposition)
     && matchesDisposition.size() > 1) {
        std::string type = matchesDisposition[1].str();
        for (auto &ch : type) {
            ch = tolower(ch);
        }
        if (type == "attachment") {
            return true;
        }
    }
    std::regex patternTypeBin(
        R"(^Content-Type:\s*)"
        R"((?:application/(octet-stream)|)"
        R"(binary/(octet-stream)))"
        R"(\s*(?:;.*)?)",
        std::regex::icase);
    std::smatch matchesTypeBin;
    if (std::regex_search(contentTypeHeader, matchesTypeBin, patternTypeBin) && matchesTypeBin.size() > 1) {
        return true;
    }
    if (!isSupportedFormat) {
        return false;
    }
    std::regex patternTypeSupported(
        R"(^Content-Type:\s*)"
        R"((?:application/(sla|stl|x-stl|step|x-step|oltp|obj|amf|3mf|svg\+xml|zip|x-zip-compressed|gcode)|)"
        R"(model/(stl|x-stl|step|obj|amf|3mf)|)"
        R"(text/(plain|gcode)|)"
        R"(image/(svg\+xml)))"
        R"(\s*(?:;.*)?)",
        std::regex::icase);
    std::smatch matchesTypeSupported;
    if (std::regex_search(contentTypeHeader, matchesTypeSupported, patternTypeSupported)
     && matchesTypeSupported.size() > 1) {
        return true;
    }
    return false;
}

wxString CheckDownloadUrl::GetFileName(const std::string &contentDispositionHeader)
{
    struct file_name_parse_data_t {
        std::regex regex;
        bool isRFC5987;
        bool isUtf8;
    };
    std::vector<file_name_parse_data_t> patterns = {
        // filename*=utf-8''encoded_value (RFC 5987)
        { std::regex(R"(filename\*\s*=\s*utf-8''([^;]+))", std::regex::icase), true, true },

        // filename*=ISO-8859-1''encoded_value
        { std::regex(R"(filename\*\s*=\s*[^']*''([^;]+))", std::regex::icase), true, false },

        // filename="value"
        { std::regex("filename\\s*=\\s*\"([^\"]*)\"", std::regex::icase), false, true },

        // filename=value
        { std::regex(R"(filename\s*=\s*([^";\s]+(?:\s+[^";\s]+)*))", std::regex::icase), false, true },
    };
    for (const auto &pattern : patterns) {
        std::smatch matches;
        if (std::regex_search(contentDispositionHeader, matches, pattern.regex) && matches.size() > 1) {
            std::string fileName = matches[1].str();
            fileName.erase(0, fileName.find_first_not_of(" \t\r\n"));
            fileName.erase(fileName.find_last_not_of(" \t\r\n") + 1);
            if (!fileName.empty()) {
                if (pattern.isRFC5987 && pattern.isUtf8) {
                    fileName = FFUtils::urlUnescape(fileName);
                }
                return wxString::FromUTF8(fileName);
            }
        }
    }
    return "";
}

ComThreadPool *OpenBase64Model::s_threadPool = new ComThreadPool(1, 60000);

void OpenBase64Model::open(std::string &str)
{
    if (wxGetApp().app_config == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "app_config is nullptr";
        return;
    }
    std::string destFolder = wxGetApp().app_config->get("download_path");
    if (destFolder.empty() || !boost::filesystem::is_directory(destFolder)) {
        std::string msg = _u8L("Could not start URL download. Destination folder is not set. Please choose destination folder in Configuration Wizard.");
        BOOST_LOG_TRIVIAL(error) << msg;
        show_error(nullptr, msg);
        return;
    }
    s_threadPool->post([weakSelf = weak_from_this(), _str = std::move(str), destFolder]() mutable {
        try {
            // Wait for the memory release of wxCommandEvent to reduce memory usage peaks.
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            boost::json::value root = boost::json::parse(_str);
            std::string().swap(_str);
            if (root.at("command").as_string() != "download_captured") {
                BOOST_LOG_TRIVIAL(error) << "download json invalid command";
                return;
            }
            const boost::json::object &data = root.at("data").as_object();
            std::string fileName = data.at("file_name").as_string().c_str();
            if (fileName.empty()) {
                BOOST_LOG_TRIVIAL(error) << "download json filename is empty";
                return;
            }
            const boost::json::string &fileData = data.at("file_data").as_string();
            wxString filePath;
            wxMemoryBuffer buf = wxBase64Decode(fileData.c_str(), fileData.size());
            if (!writeFile(destFolder, fileName, buf, filePath)) {
                return;
            }
            std::shared_ptr<OpenBase64Model> self = weakSelf.lock();
            if (self.get() == nullptr) {
                return;
            }
            wxCommandEvent *event = new wxCommandEvent(OPEN_BASE64_MODEL_EVENT);
            event->SetString(filePath);
            self->QueueEvent(event);
        } catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(error) << "process download json error, " << e.what();
        }
    });
}

bool OpenBase64Model::writeFile(const std::string &destFolder, const std::string &fileName,
    const wxMemoryBuffer &buf, wxString &filePath)
{
    wxString tmpFilePath = getSaveFilePath(destFolder, fileName, true);
    wxLogNull logNo;
    wxFile file;
    if (!file.Open(tmpFilePath, wxFile::write)) {
        BOOST_LOG_TRIVIAL(error) << "open file error, " << tmpFilePath.utf8_string()
            << ", " << file.GetLastError();
        return false;
    }
    if (file.Write(buf.GetData(), buf.GetDataLen()) != buf.GetDataLen()) {
        BOOST_LOG_TRIVIAL(error) << "write file error, " << tmpFilePath.utf8_string()
            << ", " << file.GetLastError();
        return false;
    }
    file.Close();
    filePath = getSaveFilePath(destFolder, fileName, false);
    if (!wxRenameFile(tmpFilePath, filePath, false)) {
        BOOST_LOG_TRIVIAL(error) << "rename file error, " << tmpFilePath << ", " << filePath;
        return false;
    }
    return true;
}

wxString OpenBase64Model::getSaveFilePath(const std::string &destFolder, const std::string &fileName, bool isTmp)
{
    wxString tmpFileName;
    wxString forbiddenChars = wxFileName::GetForbiddenChars();
    for (auto ch : wxString::FromUTF8(fileName)) {
        if (forbiddenChars.find(ch) == wxNOT_FOUND) {
            tmpFileName.append(ch);
        }
    }
    wxString baseName;
    wxString extension;
    wxFileName::SplitPath(tmpFileName, nullptr, &baseName, &extension);
    wxString saveName;
    if (!destFolder.empty() && (destFolder.back() == '/' || destFolder.back() == '\\')) {
        saveName = wxString::FromUTF8(destFolder) + tmpFileName;
    } else {
        saveName = wxString::FromUTF8(destFolder) + "/" + tmpFileName;
    }
    if (isTmp) {
        saveName += ".ffdownload";
    }
    for (int i = 1; true; ++i) {
        if (!saveName.empty() && !wxFileExists(saveName)) {
            break;
        }
        saveName = wxString::Format("%s/%s(%d).%s", destFolder, baseName, i, extension);
        if (isTmp) {
            saveName += ".ffdownload";
        }
    }
    return saveName;
}

FFWebViewPanel::FFWebViewPanel(wxWindow *parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
    , m_navMoreMenu(nullptr)
    , m_printListAdded(false)
    , m_showWebviewBackButton(false)
    , m_autoOpenDownloadLink(false)
    , m_modelPersonalizedRecEnabled(false)
    , m_getSystemI18nConfigTryCnt(0)
    , m_getSystemI18nConfigReqId(MultiComHelper::InvalidRequestId)
    , m_getOnlineConfigTryCnt(0)
    , m_getOnlineConfigReqId(MultiComHelper::InvalidRequestId)
    , m_getDownloadScriptTryCnt(0)
    , m_getDownloadScriptReqId(MultiComHelper::InvalidRequestId)
    , m_printListReqId(MultiComHelper::InvalidRequestId)
    , m_reportReqId(MultiComHelper::InvalidRequestId)
    , m_modelUserAgent("User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/107.0.0.0 Safari/537.36 Edg/107.0.1418.52")
    , m_checkDownloadUrl(std::make_shared<CheckDownloadUrl>())
    , m_openBase64Model(std::make_shared<OpenBase64Model>())
{
    if (!InitBrowser()) {
        return;
    }
    InitModelNav();
    SetMainLayout();

    m_checkDownloadUrl->Bind(FIND_DOWNLOAD_URL_EVENT, &FFWebViewPanel::OnFindDownloadUrl, this);
    m_openBase64Model->Bind(OPEN_BASE64_MODEL_EVENT, &FFWebViewPanel::OnOpenBase64Model, this);
    MultiComMgr::inst()->Bind(COM_WAN_DEV_MAINTAIN_EVENT, &FFWebViewPanel::OnComMaintain, this);
    MultiComMgr::inst()->Bind(COM_GET_USER_PROFILE_EVENT, &FFWebViewPanel::OnComGetUserProfile, this);
    MultiComHelper::inst()->Bind(COM_ADD_PRINT_LIST_MODEL_EVENT, &FFWebViewPanel::OnComAddPrintListModel, this);
    MultiComHelper::inst()->Bind(COM_REMOVE_PRINT_LIST_MODEL_EVENT, &FFWebViewPanel::OnComRemovePrintListModel, this);
    MultiComHelper::inst()->Bind(COM_LIKE_MODEL_EVENT, &FFWebViewPanel::OnComLikeModel, this);
    MultiComHelper::inst()->Bind(COM_REPORT_MODEL_EVENT, &FFWebViewPanel::OnComReportModel, this);
    CallAfter([this]() {
        wxGetApp().mainframe->Bind(wxEVT_ICONIZE, &FFWebViewPanel::OnMainFrameIconize, this);
        wxGetApp().mainframe->Bind(wxEVT_MOVE, &FFWebViewPanel::OnMainFrameMove, this);
        wxGetApp().mainframe->Bind(wxEVT_SIZE, &FFWebViewPanel::OnMainFrameSize, this);
        CheckGetSystemI18nConfig();
        CheckGetOnlineConfig();
        CheckGetDownloadScript();
    });
}

void FFWebViewPanel::RunScript(const wxString &jsStr)
{
    if (m_mainBrowser == nullptr) {
        return;
    }
    WebView::RunScript(m_mainBrowser, jsStr);
}

void FFWebViewPanel::SendRecentList(int images)
{
    if (m_mainBrowser == nullptr) {
        return;
    }
    boost::property_tree::wptree data;
    wxGetApp().mainframe->get_recent_projects(data, images);

    boost::property_tree::wptree req;
    req.put(L"sequence_id", "");
    req.put(L"command", L"get_recent_projects");
    req.put_child(L"response", data);

    std::wostringstream oss;
    boost::property_tree::write_json(oss, req, false);
    RunScript(wxString::Format("window.postMessage(%s)", oss.str()));
}

void FFWebViewPanel::ShowModelDetail(const std::string &data)
{
    try {
        auto getStringIf = [](const nlohmann::json &obj, const char *key) {
            if (obj.contains(key) && obj.at(key).is_string()) {
                return (std::string)obj.at(key);
            }
            return std::string();
        };
        nlohmann::json json = nlohmann::json::parse(data);
        nlohmann::json &modelDetail = json.at("model_detail");
        m_did = getStringIf(json, "did");
        m_sid = getStringIf(json, "sid");
        m_modelId = modelDetail.at("modelId");
        m_printListAdded = modelDetail.at("printAdded");
        m_modelLike                 = modelDetail.at("liked");
        m_modelLikeCount            = modelDetail.at("likeCount");
        m_modelReqId = getStringIf(modelDetail, "requestId");
        m_modelExpIds = getStringIf(modelDetail, "expIds");
        m_modelSearchKeyword = getStringIf(json, "searchKeyword");
        m_modelDownloadJsId = getStringIf(modelDetail, "downloadJsId");
        m_modelDownloadType = getStringIf(modelDetail, "downloadType");
        m_modelLoadingUrl           = wxString::FromUTF8(modelDetail.at("modelUrl").get<std::string>().c_str());
        m_modelBackUrls = { std::make_pair(m_modelLoadingUrl, GetModelUrlId(m_modelLoadingUrl)) };

#ifdef __APPLE__
        if (m_modelDownloadType == "disable") {
            wxLaunchDefaultBrowser(m_modelLoadingUrl);
            return;
        }
#endif
        CheckGetSystemI18nConfig();
        CheckGetOnlineConfig();
        CheckGetDownloadScript();
        SetupDownloadScript();
        SetupPrintListButton(m_printListAdded);
        SetupBackButton();
        SetupLikeButton();
        m_mainBrowser->Hide();
        m_modelPnl->Show();
        m_modelBrowser->LoadURL(m_modelLoadingUrl);
        Layout();
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "FFWebViewPanel::ShowModelDeatil error, " << e.what() << ", " << data;
    }
}

bool FFWebViewPanel::ProcComBusGetRequest(const ComBusGetRequestEvent &evt)
{
    if (evt.requestId == m_getSystemI18nConfigReqId) {
        ProcessGetSystemI18nConfig(evt);
        return true;
    } else if (evt.requestId == m_getOnlineConfigReqId) {
        ProcessGetOnlineConfig(evt);
        return true;
    } else if (evt.requestId == m_getDownloadScriptReqId) {
        ProcessGetDownloadScript(evt);
        return true;
    }
    return false;
}

bool FFWebViewPanel::ProcComBusPostRequest(const ComBusPostRequestEvent &evt)
{
    if (m_setUserConfigReqIds.find(evt.requestId) == m_setUserConfigReqIds.end()) {
        return false;
    }
    if (evt.ret != COM_OK) {
        wxString text = wxString::Format("%s (%s)", _L("Network Error"), m_modelPersonalizedRecText);
        MessageDialog dlg(wxGetApp().mainframe, text, _L("Error"));
        dlg.ShowModal();
    }
    m_setUserConfigReqIds.erase(evt.requestId);
    return true;
}

bool FFWebViewPanel::GetUserConfigData(web_veiw_user_config_data_t &configData)
{
    if (!IsUserConfigOk() || !m_systemI18nConfig.is_object()) {
        CheckGetSystemI18nConfig();
        CheckGetOnlineConfig();
        return false;
    }
    configData.modelPersonalizedRecEnabled = m_modelPersonalizedRecEnabled;
    configData.modelPersonalizedRecText = m_modelPersonalizedRecText;
    return true;
}

void FFWebViewPanel::SetUserConfig(bool modelPersonalizedRecEnabled)
{
    if (modelPersonalizedRecEnabled == m_modelPersonalizedRecEnabled) {
        return;
    }
    m_modelPersonalizedRecEnabled = modelPersonalizedRecEnabled;
    m_userConfig["recommendForYourSwitch"] = modelPersonalizedRecEnabled;
    SyncUserConfig();

    nlohmann::json json;
    json["recommendForYourSwitch"] = modelPersonalizedRecEnabled;
    std::string target = "/api/v3/model/user/system/config";
    std::string language = wxGetApp().current_language_code_safe().BeforeFirst('_').ToStdString();
    int64_t requretId = MultiComHelper::inst()->doBusPostRequest(target, language, json.dump(), ComTimeoutWanB);
    m_setUserConfigReqIds.emplace(requretId);
}

void FFWebViewPanel::GoHome() 
{
    if (m_homePageUrl.empty() || m_mainBrowser->GetCurrentURL().Contains(m_homePageUrl)) {
        return;
    }
    wxString language = wxGetApp().current_language_code_safe().BeforeFirst('_');
    m_mainBrowser->LoadURL(wxString::Format("%s?lang=%s", m_homePageUrl, language));
}

void FFWebViewPanel::Rescale()
{
    if (GetSizer() == nullptr) {
        return;
    }
    GetSizer()->Clear(false);
    SetSizer(nullptr);
    delete m_modelNavPnl;
    delete m_viewNowWindow;
    delete m_spacerLinePnl;
    m_modelNavPnl = nullptr;
    m_viewNowWindow = nullptr;
    m_spacerLinePnl = nullptr;
    InitModelNav();
    SetMainLayout();
    SetupBackButton();
    SetupLikeButton();
    SetupPrintListButton(m_printListAdded);
    SetupSystemI18n();
}

bool FFWebViewPanel::InitBrowser()
{
    m_homePageUrl     = wxGetApp().get_homepage_url();
    wxString language = wxGetApp().current_language_code_safe().BeforeFirst('_');
    m_mainBrowser = WebView::CreateWebView(this, wxString::Format("%s?lang=%s", m_homePageUrl, language));
    if (m_mainBrowser == nullptr) {
        return false;
    }
    std::string homePageEnableDebug = wxGetApp().app_config->get("home_page_enable_debug");
    m_mainBrowser->EnableAccessToDevTools(homePageEnableDebug == "true" || homePageEnableDebug == "1");
    m_mainBrowser->Bind(wxEVT_WEBVIEW_NAVIGATED, &FFWebViewPanel::OnMainNavigated, this);

    m_modelPnl = new wxPanel(this);
    m_modelPnl->Hide();
    m_modelBrowser = FFUtils::CreateWebView(m_modelPnl);
    if (m_modelBrowser == nullptr) {
        return false;
    }
    m_modelBrowser->EnableAccessToDevTools(homePageEnableDebug == "true" || homePageEnableDebug == "1");
    //m_modelBrowser->SetUserAgent(m_modelUserAgent);

    Bind(wxEVT_WEBVIEW_NEWWINDOW, &FFWebViewPanel::OnMainNewWindow, this);
    Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &FFWebViewPanel::OnMainScriptMessageReceived, this);
    m_modelPnl->Bind(wxEVT_WEBVIEW_NAVIGATING, &FFWebViewPanel::OnModelNavigating, this);
    m_modelPnl->Bind(wxEVT_WEBVIEW_NAVIGATED, &FFWebViewPanel::OnModelNavigated, this);
    m_modelPnl->Bind(wxEVT_WEBVIEW_LOADED, &FFWebViewPanel::OnModelLoaded, this);
    m_modelPnl->Bind(wxEVT_WEBVIEW_ERROR, &FFWebViewPanel::OnModelError, this);
    m_modelPnl->Bind(wxEVT_WEBVIEW_NEWWINDOW, &FFWebViewPanel::OnModelNewWindow, this);
    m_modelPnl->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &FFWebViewPanel::OnModelScriptMessageReceived, this);
    return true;
}

void FFWebViewPanel::InitModelNav()
{
    m_modelNavPnl = new wxPanel(m_modelPnl);
    m_modelNavPnl->SetBackgroundColour(*wxWHITE);
    m_modelNavPnl->SetSize(wxSize(-1, FromDIP(52)));
    m_modelNavPnl->SetMinSize(wxSize(-1, FromDIP(52)));
    m_modelNavPnl->SetMaxSize(wxSize(-1, FromDIP(52)));

    m_navHideBtn = new FFPushButton(m_modelNavPnl, wxID_ANY, "model_nav_close", "model_nav_close", "model_nav_close", "model_nav_close", 16);
    m_navHideBtn->SetBackgroundColour(*wxWHITE);
    m_navHideBtn->SetSize(wxSize(FromDIP(16), FromDIP(16)));
    m_navHideBtn->SetMinSize(wxSize(FromDIP(16), FromDIP(16)));
    m_navHideBtn->SetMaxSize(wxSize(FromDIP(16), FromDIP(16)));
    m_navHideBtn->Bind(wxEVT_BUTTON, &FFWebViewPanel::OnHideButton, this);

    wxFont navDetailFont = Label::Head_18;
    navDetailFont.SetWeight(wxFONTWEIGHT_MEDIUM);
    m_navDetailLbl = new wxStaticText(m_modelNavPnl, wxID_ANY, "");
    m_navDetailLbl->SetForegroundColour(wxColour("#333333"));
    m_navDetailLbl->SetFont(navDetailFont);
    m_navDetailLbl->Hide();

    m_navBackBtn = new FFPushButton(m_modelNavPnl, wxID_ANY, "model_nav_back", "model_nav_back", "model_nav_back", "model_nav_back", 16);
    m_navBackBtn->SetBackgroundColour(*wxWHITE);
    m_navBackBtn->SetSize(wxSize(FromDIP(16), FromDIP(16)));
    m_navBackBtn->SetMinSize(wxSize(FromDIP(16), FromDIP(16)));
    m_navBackBtn->SetMaxSize(wxSize(FromDIP(16), FromDIP(16)));
    m_navBackBtn->Bind(wxEVT_BUTTON, &FFWebViewPanel::OnBackButton, this);
    m_navBackBtn->Hide();

    m_modelLike  = false;
    m_navLikeBtn = new FFButton(m_modelNavPnl, wxID_ANY, "123");
    m_navLikeBtn->SetDoubleBuffered(true);
    m_navLikeBtn->SetBackgroundColour(*wxWHITE);
    m_navLikeBtn->SetFontUniformColor(*wxBLACK);
    m_navLikeBtn->SetSize(wxSize(FromDIP(83), FromDIP(35)));
    m_navLikeBtn->SetMinSize(wxSize(FromDIP(83), FromDIP(35)));
    m_navLikeBtn->SetMaxSize(wxSize(FromDIP(83), FromDIP(35)));
    ScalableBitmap unlike_bmp(this, "model_nav_unlike", 16);
    m_navLikeBtn->SetUniformIcon(unlike_bmp);
    m_navLikeBtn->SetFont(Label::Body_16);
    m_navLikeBtn->SetBorderUniformColor(wxColour("#DCDFE6"));
    m_navLikeBtn->Bind(wxEVT_BUTTON, &FFWebViewPanel::OnLikeButton, this);

    m_navMoreBtn = new FFPushButton(m_modelNavPnl, wxID_ANY, "model_nav_more", "model_nav_more", "model_nav_more", "model_nav_more", 26);
    m_navMoreBtn->SetBackgroundColour(*wxWHITE);
    m_navMoreBtn->SetSize(wxSize(FromDIP(26), FromDIP(26)));
    m_navMoreBtn->SetMinSize(wxSize(FromDIP(26), FromDIP(26)));
    m_navMoreBtn->SetMaxSize(wxSize(FromDIP(26), FromDIP(26)));
    m_navMoreBtn->Bind(wxEVT_BUTTON, &FFWebViewPanel::OnMoreButton, this);
    m_navMoreBtn->Hide();



    wxFont navPrintListFont = Label::Body_16;
    navPrintListFont.SetWeight(wxFONTWEIGHT_MEDIUM);
    m_navPrintListBtn = new FFButton(m_modelNavPnl, wxID_ANY, "");
    m_navPrintListBtn->SetBackgroundColour(*wxWHITE);
    m_navPrintListBtn->SetDoubleBuffered(true);
    ScalableBitmap print_list_bmp(this, "model_nav_print_list", 16);
    m_navPrintListBtn->SetUniformIcon(print_list_bmp);
    m_navPrintListBtn->SetFont(navPrintListFont);
    m_navPrintListBtn->Bind(wxEVT_BUTTON, &FFWebViewPanel::OnPrintListButton, this);
    m_navPrintListBtn->Hide();

    wxBoxSizer *modelNavSizer = new wxBoxSizer(wxHORIZONTAL);
    modelNavSizer->Add(m_navBackBtn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(32));
    modelNavSizer->Add(m_navDetailLbl, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(20));
    modelNavSizer->AddStretchSpacer(1);
    modelNavSizer->Add(m_navMoreBtn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    modelNavSizer->Add(m_navLikeBtn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    modelNavSizer->Add(m_navPrintListBtn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(147));
    modelNavSizer->Add(m_navHideBtn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(24));
    m_modelNavPnl->SetSizer(modelNavSizer);
    m_modelNavPnl->Layout();
}

void FFWebViewPanel::SetMainLayout()
{
    m_viewNowWindow = new PrintListTipWindow(this);
    m_viewNowWindow->Bind(VIEW_NOW_BUTTON_EVENT, &FFWebViewPanel::OnViewNow, this);

    m_spacerLinePnl = new wxPanel(m_modelPnl, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
    m_spacerLinePnl->SetForegroundColour(wxColour("#dddddd"));
    m_spacerLinePnl->SetBackgroundColour(wxColour("#dddddd"));

    wxBoxSizer *modelSizer = new wxBoxSizer(wxVERTICAL);
    modelSizer->Add(m_modelNavPnl, 1, wxEXPAND);
    modelSizer->Add(m_spacerLinePnl, 0, wxEXPAND);
    modelSizer->Add(m_modelBrowser, 1, wxEXPAND);
    m_modelPnl->SetSizer(modelSizer);
    m_modelPnl->Layout();

    wxBoxSizer *sizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add(m_mainBrowser, 1, wxEXPAND);
    sizer->Add(m_modelPnl, 1, wxEXPAND);
    SetSizer(sizer);
    Layout();
}

void FFWebViewPanel::CheckGetSystemI18nConfig()
{
    if (m_systemI18nConfig.is_object()) {
        return;
    }
    if (m_getSystemI18nConfigReqId != MultiComHelper::InvalidRequestId) {
        return;
    }
    m_getSystemI18nConfigTryCnt = 1;
    PostGetSystemI18nConfig();
}

void FFWebViewPanel::PostGetSystemI18nConfig()
{
    std::string target = "/api/v3/model/sys_i18n/list?keys="
        "all_thirdparty_model_page_title,all_thirdparty_model_page_more_report_btn,"
        "all_thirdparty_model_page_add_btn,all_thirdparty_model_page_remove_btn,"
        "all_model_printlist_add_success_tips,all_printlist_page_remove_model_tips,"
        "orca_setting_page_rec_switch";
    std::string language = wxGetApp().current_language_code_safe().BeforeFirst('_').ToStdString();
    m_getSystemI18nConfigReqId = MultiComHelper::inst()->doBusGetRequestSystem(target, language, ComTimeoutWanB);
}

void FFWebViewPanel::ProcessGetSystemI18nConfig(const ComBusGetRequestEvent &evt)
{
    if (evt.ret != COM_OK) {
        if (m_getSystemI18nConfigTryCnt < 3) {
            PostGetSystemI18nConfig();
            m_getSystemI18nConfigTryCnt++;
        } else {
            m_getSystemI18nConfigReqId = MultiComHelper::InvalidRequestId;
        }
        BOOST_LOG_TRIVIAL(error) << "FFWebViewPanel::ProcessGetSystemI18nConfig error, " << evt.ret << ", " << evt.responseData;
        return;
    }
    try {
        std::map<std::string, std::string> i18nMap;
        std::string language = wxGetApp().current_language_code_safe().BeforeFirst('_').ToStdString();
        nlohmann::json json = nlohmann::json::parse(evt.responseData);
        for (auto &item : json.at("items")) {
            if (item.contains("key") && item.contains("value")) {
                const nlohmann::json &itemValue = item.at("value");
                if (itemValue.contains(language)) {
                    i18nMap.emplace(item.at("key"), itemValue.at(language));
                } else if (itemValue.contains("en")) {
                    i18nMap.emplace(item.at("key"), itemValue.at("en"));
                }
            }
        }
        if (i18nMap.find("all_thirdparty_model_page_title") != i18nMap.end()) {
            m_navDetailText = wxString::FromUTF8(i18nMap.at("all_thirdparty_model_page_title"));
        } else {
            BOOST_LOG_TRIVIAL(error) << "FFWebViewPanel i18n error, all_thirdparty_model_page_title";
        }
        if (i18nMap.find("all_thirdparty_model_page_more_report_btn") != i18nMap.end()) {
            m_reportMenuText = wxString::FromUTF8(i18nMap.at("all_thirdparty_model_page_more_report_btn"));
        } else {
            BOOST_LOG_TRIVIAL(error) << "FFWebViewPanel i18n error, all_thirdparty_model_page_more_report_btn";
        }
        if (i18nMap.find("all_thirdparty_model_page_add_btn") != i18nMap.end()) {
            m_addPrintListText = wxString::FromUTF8(i18nMap.at("all_thirdparty_model_page_add_btn"));
        } else {
            BOOST_LOG_TRIVIAL(error) << "FFWebViewPanel i18n error, all_thirdparty_model_page_add_btn";
        }
        if (i18nMap.find("all_thirdparty_model_page_remove_btn") != i18nMap.end()) {
            m_removePrintListText = wxString::FromUTF8(i18nMap.at("all_thirdparty_model_page_remove_btn"));
        } else {
            BOOST_LOG_TRIVIAL(error) << "FFWebViewPanel i18n error, all_thirdparty_model_page_remove_btn";
        }
        if (i18nMap.find("all_model_printlist_add_success_tips") != i18nMap.end()) {
            m_addPrintListTipText = wxString::FromUTF8(i18nMap.at("all_model_printlist_add_success_tips"));
        } else {
            BOOST_LOG_TRIVIAL(error) << "FFWebViewPanel i18n error, all_model_printlist_add_success_tips";
        }
        if (i18nMap.find("all_printlist_page_remove_model_tips") != i18nMap.end()) {
            m_removePrintListTipText = wxString::FromUTF8(i18nMap.at("all_printlist_page_remove_model_tips"));
        } else {
            BOOST_LOG_TRIVIAL(error) << "FFWebViewPanel i18n error, all_printlist_page_remove_model_tips";
        }
        if (i18nMap.find("orca_setting_page_rec_switch") != i18nMap.end()) {
            m_modelPersonalizedRecText = wxString::FromUTF8(i18nMap.at("orca_setting_page_rec_switch"));
        } else {
            BOOST_LOG_TRIVIAL(error) << "FFWebViewPanel i18n error, orca_setting_page_rec_switch";
        }
        SetupSystemI18n();
        m_systemI18nConfig = json;
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "FFWebViewPanel::ProcessGetSystemI18nConfig error, " << e.what() << ", " << evt.responseData;
    }
    m_getSystemI18nConfigReqId = MultiComHelper::InvalidRequestId;
}

void FFWebViewPanel::CheckGetOnlineConfig()
{
    if (m_reportConfig.is_object() && IsUserConfigOk()) {
        return;
    }
    if (m_getOnlineConfigReqId != MultiComHelper::InvalidRequestId) {
        return;
    }
    m_getOnlineConfigTryCnt = 1;
    PostGetOnlineConfig();
}

void FFWebViewPanel::PostGetOnlineConfig()
{
    std::string target = "/api/v3/model/user/system/config";
    std::string language = wxGetApp().current_language_code_safe().BeforeFirst('_').ToStdString();
    m_getOnlineConfigReqId = MultiComHelper::inst()->doBusGetRequest(target, language, ComTimeoutWanB);
}

void FFWebViewPanel::ProcessGetOnlineConfig(const ComBusGetRequestEvent &evt)
{
    if (evt.ret != COM_OK) {
        if (m_getOnlineConfigTryCnt < 3) {
            PostGetOnlineConfig();
            m_getOnlineConfigTryCnt++;
        } else {
            m_getOnlineConfigReqId = MultiComHelper::InvalidRequestId;
        }
        BOOST_LOG_TRIVIAL(error) << "FFWebViewPanel::ProcessGetOnlineConfig error, "
            << evt.ret << ", " << evt.responseData;
        return;
    }
    try {
        auto getBoolIf = [](const nlohmann::json &obj, const char *key) {
            if (obj.contains(key) && obj.at(key).is_boolean()) {
                return (bool)obj.at(key);
            }
            return false;
        };
        nlohmann::json json = nlohmann::json::parse(evt.responseData);
        const nlohmann::json &system = json.at("system");
        const nlohmann::json &user = json.at("user");
        m_autoOpenDownloadLink = getBoolIf(system, "orcaAutoOpenDownloadLink");
        m_showWebviewBackButton = getBoolIf(system, "orcaShowWebviewBackButton");
        m_modelPersonalizedRecEnabled = getBoolIf(user, "recommendForYourSwitch");
        m_reportConfig = system.at("reportConfig");
        m_userConfig = user;
        SyncUserConfig();
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "FFWebViewPanel::ProcessGetOnlineConfig error, "
            << e.what() << ", " << evt.responseData;
    }
    m_getOnlineConfigReqId = MultiComHelper::InvalidRequestId;
}

void FFWebViewPanel::CheckGetDownloadScript()
{
#ifdef __APPLE__
    if (m_downloadJsConfig.is_object()) {
        return;
    }
    if (m_getDownloadScriptReqId != MultiComHelper::InvalidRequestId) {
        return;
    }
    m_getDownloadScriptTryCnt = 1;
    PostGetDownloadScript();
#endif
}

void FFWebViewPanel::PostGetDownloadScript()
{
    std::string target = "/api/v3/model/download/js/config";
    std::string language = wxGetApp().current_language_code_safe().BeforeFirst('_').ToStdString();
    m_getDownloadScriptReqId = MultiComHelper::inst()->doBusGetRequest(target, language, 60000);
}

void FFWebViewPanel::ProcessGetDownloadScript(const ComBusGetRequestEvent &evt)
{
    if (evt.ret != COM_OK) {
        if (m_getDownloadScriptTryCnt < 3) {
            PostGetDownloadScript();
            m_getDownloadScriptTryCnt++;
        } else {
            m_getDownloadScriptReqId = MultiComHelper::InvalidRequestId;
        }
        BOOST_LOG_TRIVIAL(error) << "FFWebViewPanel::ProcessGetDownloadScript error, "
            << evt.ret << ", " << evt.responseData;
        return;
    }
    try {
        nlohmann::json json = nlohmann::json::parse(evt.responseData);
        m_downloadJsMap.clear();
        for (auto &item : json.at("items")) {
            if (item.contains("jsId") && item.contains("value")) {
                m_downloadJsMap.emplace(item.at("jsId"), item.at("value"));
            }
        }
        m_downloadJsConfig = json;
        SetupDownloadScript();
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "FFWebViewPanel::ProcessGetDownloadScript error, "
            << e.what() << ", " << evt.responseData;
    }
    m_getDownloadScriptReqId = MultiComHelper::InvalidRequestId;
}

bool FFWebViewPanel::IsUserConfigOk()
{
    return m_userConfig.is_object() && m_userConfig.contains("recommendForYourSwitch"); 
}

void FFWebViewPanel::SetupLikeButton() 
{
    m_navLikeBtn->SetLabel(wxString::Format("%d", m_modelLikeCount), FromDIP(83), FromDIP(35));
    if (m_modelLike) {
        ScalableBitmap like_bmp(this, "model_nav_like", 16);
        m_navLikeBtn->SetUniformIcon(like_bmp);
        m_navLikeBtn->SetFontUniformColor(wxColour("#FF4D4D"));
    } else {
        ScalableBitmap unlike_bmp(this, "model_nav_unlike", 16);
        m_navLikeBtn->SetUniformIcon(unlike_bmp);
        m_navLikeBtn->SetFontUniformColor(*wxBLACK);
    }
    Layout();
}

void FFWebViewPanel::SetupBackButton()
{
    m_navBackBtn->Show(m_modelBackUrls.size() > 1);
    Layout();
}

void FFWebViewPanel::SetupPrintListButton(bool printListAdded)
{
    if (!printListAdded) {
        m_navPrintListBtn->SetLabel(m_addPrintListText, FromDIP(96), FromDIP(20), FromDIP(36), FromDIP(6));
        m_navPrintListBtn->SetFontUniformColor(*wxWHITE);
        ScalableBitmap bmp(this, "model_nav_print_list", 16);
        m_navPrintListBtn->SetUniformIcon(bmp);
        m_navPrintListBtn->SetBorderWidth(0);
        m_navPrintListBtn->SetBGColor(wxColour("#328DFB"));
        m_navPrintListBtn->SetBGHoverColor(wxColour("#48AAFE"));
        m_navPrintListBtn->SetBGPressColor(wxColour("#328DFB"));
        m_navPrintListBtn->SetBGDisableColor(wxColour("#328DFB"));
    } else {
        m_navPrintListBtn->SetLabel(m_removePrintListText, FromDIP(96), FromDIP(20), FromDIP(36), FromDIP(6));
        m_navPrintListBtn->SetFontColor(wxColour("#328DFB"));
        m_navPrintListBtn->SetFontHoverColor(wxColour("#48AAFE"));
        m_navPrintListBtn->SetFontPressColor(wxColour("#328DFB"));
        m_navPrintListBtn->SetFontDisableColor(wxColour("#328DFB"));
        m_navPrintListBtn->SetUniformIcon(ScalableBitmap());
        m_navPrintListBtn->SetBorderWidth(2);
        m_navPrintListBtn->SetBorderColor(wxColour("#328DFB"));
        m_navPrintListBtn->SetBorderHoverColor(wxColour("#48AAFE"));
        m_navPrintListBtn->SetBorderPressColor(wxColour("#328DFB"));
        m_navPrintListBtn->SetBorderDisableColor(wxColour("#328DFB"));
        m_navPrintListBtn->SetBGUniformColor(*wxWHITE);
    }
    Layout();
}

void FFWebViewPanel::SetupSystemI18n()
{
    m_navDetailLbl->SetLabelText(m_navDetailText);
    const wxString &printListBtnText = !m_printListAdded ? m_addPrintListText : m_removePrintListText;
    m_navPrintListBtn->SetLabel(printListBtnText, FromDIP(96), FromDIP(20), FromDIP(36), FromDIP(6));
    ScalableBitmap bmp(this, "model_nav_print_list", 16);
    m_navPrintListBtn->SetUniformIcon(!m_printListAdded ? bmp : ScalableBitmap());
    m_navDetailLbl->Show();
    m_navBackBtn->Show(m_modelBackUrls.size() > 1);
    m_navMoreBtn->Show();
    m_navPrintListBtn->Show();
    Layout();
}

void FFWebViewPanel::SetupDownloadScript()
{
#ifdef __APPLE__
    if (m_modelBrowser == nullptr || m_modelDownloadType != "js") {
        return;
    }
    auto it = m_downloadJsMap.find(m_modelDownloadJsId);
    if (it == m_downloadJsMap.end()) {
        return;
    }
    m_modelBrowser->RemoveScriptMessageHandler("wx");
    m_modelBrowser->RemoveAllUserScripts();
    m_modelBrowser->AddScriptMessageHandler("wx");
    m_modelBrowser->AddUserScript(wxString::FromUTF8(it->second));
#endif
}

void FFWebViewPanel::MoveViewNowWindow()
{
    int x = m_modelNavPnl->GetRect().GetRight() - m_viewNowWindow->GetSize().x - FromDIP(16);
    int y = m_modelNavPnl->GetRect().GetBottom() + FromDIP(20);
    m_viewNowWindow->Move(ClientToScreen(wxPoint(x, y)));
}

void FFWebViewPanel::ReportTrackingData(const std::string &eventType, const std::string &eventName)
{
    std::string uuid = boost::uuids::to_string(boost::uuids::random_generator()());
    uuid.erase(std::remove(uuid.begin(), uuid.end(), '-'), uuid.end());
    std::string timestamp = FFUtils::getTimestampMsStr();

    com_tracking_common_data_t commonData;
    commonData.uid = m_uid;
    commonData.did = m_did;
    commonData.sid = m_sid;
    
    com_tracking_event_data_t eventData;
    eventData.eventType = eventType;
    eventData.eventId = (boost::format("%s_%s_%s") % eventName % timestamp % uuid).str();
    eventData.eventName = eventName;
    eventData.pageId = "mdel_detail";
    eventData.moduleId = "model_detail";
    eventData.reqId = m_modelReqId;
    eventData.expIds = m_modelExpIds;
    eventData.objectType = "model";
    eventData.objectId = m_modelId;
    json j;
    j["keyword"]        = m_modelSearchKeyword;
    eventData.extend = j.dump();
    eventData.timestamp = timestamp;

    MultiComHelper::inst()->reportTrackingData(commonData, eventData, ComTimeoutWanB);
}

void FFWebViewPanel::SyncModelAction(const std::string &action)
{
    nlohmann::json json;
    json["command"] = "sync_model_action";
    json["action"] = action;
    json["model_id"] = m_modelId;
    json["sequence_id"] = FFUtils::getTimestampMsStr();

    std::string jsonStr = json.dump();
    wxString jsStr = wxString::Format("window.postMessage(%s)", wxString::FromUTF8(jsonStr));
    RunScript(jsStr);
}

void FFWebViewPanel::SyncUserConfig()
{
    nlohmann::json json;
    json["command"] = "sync_user_config";
    json["recommendForYourSwitch"] = m_modelPersonalizedRecEnabled;
    json["sequence_id"] = FFUtils::getTimestampMsStr();

    std::string jsonStr = json.dump();
    wxString jsStr = wxString::Format("window.postMessage(%s)", wxString::FromUTF8(jsonStr));
    RunScript(jsStr);
}

void FFWebViewPanel::TryPushBackUrl(const wxString &url)
{
    if (m_modelBackUrls.empty()) {
        return;
    }
    wxString urlId = GetModelUrlId(url);
    if (m_modelBackUrls.back().first.empty()) {
        m_modelBackUrls.back().first = url;
        m_modelBackUrls.back().second = urlId;
    } else {
        if (urlId != m_modelBackUrls.back().second) {
            m_modelBackUrls.emplace_back(url, urlId);
            SetupBackButton();
        }
    }
}

void FFWebViewPanel::OnHideButton(wxCommandEvent &evt)
{
    m_modelPnl->Hide();
    m_mainBrowser->Show();
    m_modelBrowser->LoadURL("about:blank");
    if (m_viewNowWindow->IsShownOnScreen()) {
        m_viewNowWindow->Hide();
    }
    Layout();
}

void FFWebViewPanel::OnBackButton(wxCommandEvent &evt)
{
    if (m_modelBackUrls.size() > 1) {
        m_modelBackUrls.pop_back();
        SetupBackButton();
        m_modelLoadingUrl = m_modelBackUrls.back().first;
        m_modelBrowser->LoadURL(m_modelLoadingUrl);
    }
}

void FFWebViewPanel::OnMoreButton(wxCommandEvent &evt)
{
    delete m_navMoreMenu;
    m_navMoreMenu = new NavMoreMenu(this);
    m_navMoreMenu->AddItem("model_nav_report", 20, m_reportMenuText);
    m_navMoreMenu->Bind(NAV_MORE_MENU_EVENT, &FFWebViewPanel::OnMoreMenu, this);

    int x = m_navMoreBtn->GetRect().x + m_navMoreBtn->GetSize().x / 2 - m_navMoreMenu->GetSize().x / 2;
    int y = m_modelNavPnl->GetRect().height - FromDIP(5);
    m_navMoreMenu->Move(ClientToScreen(wxPoint(x, y)));
    m_navMoreMenu->Show();
}

void FFWebViewPanel::OnLikeButton(wxCommandEvent& evt) 
{
    if (!wxGetApp().is_flashforge_login()) {
        wxGetApp().ShowUserLogin();
        return;
    }
    MultiComHelper::inst()->likeModel(m_modelId, !m_modelLike, ComTimeoutWanB);
    evt.Skip();
}

void FFWebViewPanel::OnPrintListButton(wxCommandEvent &evt)
{
    if (!wxGetApp().is_flashforge_login()) {
        wxGetApp().ShowUserLogin();
        return;
    }
    CheckGetOnlineConfig();
    if (m_printListReqId != MultiComHelper::InvalidRequestId) {
        return;
    }
    if (m_printListAdded) {
        m_printListReqId = MultiComHelper::inst()->removePrintListModel(m_modelId, ComTimeoutWanB);
        ReportTrackingData("action", "delprint");
    } else {
        std::string language = wxGetApp().current_language_code_safe().BeforeFirst('_').ToStdString();
        m_printListReqId = MultiComHelper::inst()->addPrintListModel(m_modelId, language, ComTimeoutWanB);
        ReportTrackingData("action", "addprint");
    }
}

void FFWebViewPanel::OnMoreMenu(wxCommandEvent &evt)
{
    if (!wxGetApp().is_flashforge_login()) {
        wxGetApp().ShowUserLogin();
        return;
    }
    CheckGetOnlineConfig();
    /*WebDialog reportWnd(wxGetApp().mainframe, _L("Report Model"), wxString::Format("http://localhost:3001/report?modelId=%s", m_modelId),
        6, FromDIP(wxSize(618, 700)), false);
    reportWnd.ShowModal();*/
    ReportWindow reportWnd(wxGetApp().mainframe, m_reportConfig);
    reportWnd.Bind(REPORT_BUTTON_EVENT, &FFWebViewPanel::OnReportButton, this);
    if (reportWnd.isOk()) {
        m_reportWndTitle = reportWnd.GetWindowTitle();
        reportWnd.ShowModal();
    }
}

void FFWebViewPanel::OnReportButton(wxCommandEvent &evt)
{
    if (m_reportReqId != MultiComHelper::InvalidRequestId) {
        return;
    }
    m_reportReqId = MultiComHelper::inst()->reportModel(
        evt.GetInt(), m_modelId, evt.GetString().utf8_string(), ComTimeoutWanB);
    ReportTrackingData("action", "report");
}

void FFWebViewPanel::OnViewNow(wxCommandEvent &evt)
{
    if (m_mainBrowser == nullptr) {
        return;
    }
    wxString language = wxGetApp().current_language_code_safe().BeforeFirst('_');
    m_modelPnl->Hide();
    m_mainBrowser->Show();
    m_mainBrowser->LoadURL(wxString::Format("%s/print_list?lang=%s", m_homePageUrl, language));
    Layout();
}

void FFWebViewPanel::OnMainNavigated(wxWebViewEvent &evt) 
{
    wxString currentURL = m_mainBrowser->GetCurrentURL();
    wxString targetDomain = "google.com";

    if (currentURL.Contains(targetDomain)) {
        wxString jsCode = R"(
            setInterval(() => {
                const el = document.getElementById('headingSubtext');
                if (el) el.style.display = 'none';
            }, 100)
            console.log("googleJs injure success")
        )";
        CallAfter([=]() { m_mainBrowser->RunScript(jsCode); });
    }
}

void FFWebViewPanel::OnMainNewWindow(wxWebViewEvent &evt)
{
    if (m_mainBrowser == nullptr) {
        return;
    }
    if (evt.GetURL().Contains("auth.flashforge.com") || evt.GetURL().Contains("desktop.voxelshare.com")) {
        wxLaunchDefaultBrowser(evt.GetURL(), wxBROWSER_NEW_WINDOW);
    } else {
        m_mainBrowser->LoadURL(evt.GetURL());
    }
}

void FFWebViewPanel::OnMainScriptMessageReceived(wxWebViewEvent &evt)
{
    if (m_mainBrowser == nullptr) {
        return;
    }
    std::string response = wxGetApp().handle_web_request(evt.GetString().ToUTF8().data());
    response.erase(std::remove(response.begin(), response.end(), '\n'), response.end());
    if (response.empty()) {
        return;
    }
    RunScript(wxString::Format("window.postMessage('%s')", response));
}

void FFWebViewPanel::OnModelNavigating(wxWebViewEvent &evt)
{
    if (m_modelBrowser == nullptr) {
        return;
    }
#ifndef __APPLE__
    if (!m_autoOpenDownloadLink) {
        m_modelLoadingUrl = evt.GetURL();
        return;
    }
    fs::path path(into_path(evt.GetURL()));
    const std::regex pattern(R"(^https?:\/\/.*\.(stp|step|stl|oltp|obj|amf|3mf|svg|zip|gcode|g)$)", std::regex::icase);
    if (std::regex_match(path.string(), pattern)) {
        wxGetApp().start_download("orcaflashforge://open/?file=" + path.string());
        evt.Veto();
    } else {
        m_modelLoadingUrl = evt.GetURL();
    }
#else
    if (m_modelDownloadType == "url") {
        m_checkDownloadUrl->AddUrl(evt.GetURL(), m_modelUserAgent);
    }
    m_modelLoadingUrl = evt.GetURL();
#endif
}

void FFWebViewPanel::OnModelNavigated(wxWebViewEvent &evt)
{
    if (m_modelBrowser == nullptr) {
        return;
    }
    if (evt.GetURL() == m_modelLoadingUrl) {
        m_modelLoadingUrl.clear();
    } else {
        return;
    }
    TryPushBackUrl(evt.GetURL());
}

void FFWebViewPanel::OnModelLoaded(wxWebViewEvent &evt)
{
    if (m_modelBrowser == nullptr) {
        return;
    }
    if (evt.GetURL() == m_modelLoadingUrl) {
        m_modelLoadingUrl.clear();
    } else {
        return;
    }
    TryPushBackUrl(evt.GetURL());
}

void FFWebViewPanel::OnModelError(wxWebViewEvent &evt)
{
    if (m_modelBrowser == nullptr || m_modelBackUrls.empty()) {
        return;
    }
    if (evt.GetURL() != m_modelLoadingUrl) {
        return;
    }
    if (!m_modelBackUrls.back().first.empty() && m_modelBackUrls.size() == 1) {
        m_modelBackUrls.emplace_back("", "");
        SetupBackButton();
    }
}

void FFWebViewPanel::OnModelNewWindow(wxWebViewEvent &evt)
{
    if (m_modelBrowser == nullptr) {
        return;
    }
    m_modelLoadingUrl = evt.GetURL();
    m_modelBrowser->LoadURL(m_modelLoadingUrl);
}

void FFWebViewPanel::OnModelScriptMessageReceived(wxWebViewEvent &evt)
{
    std::string msg = evt.GetString().utf8_string();
    if (msg.size() < 64 * 1024) {
        try {
            nlohmann::json json = nlohmann::json::parse(msg);
            if ((std::string)json["command"] != "download_captured") {
                BOOST_LOG_TRIVIAL(error) << "download json invalid command";
                return;
            }
            const nlohmann::json &data = json.at("data");
            std::string downloadType = data["download_type"];
            std::string fileName = data["file_name"];
            if (downloadType == "base64_data") {
                m_openBase64Model->open(msg);
            } else if (downloadType == "url") {
                std::string url = data["file_url"];
                wxGetApp().start_download("orcaflashforge://open/?file=" + url, fileName, true);
            }
        } catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(error) << "FFWebViewPanel::OnModelScriptMessageReceived error, "
                << e.what() << ", " << msg;
        }
    } else {
        m_openBase64Model->open(msg);
    }
}

void FFWebViewPanel::OnFindDownloadUrl(FindDownloadUrlEvent &evt)
{
    if (m_modelBrowser == nullptr) {
        return;
    }
    wxGetApp().start_download("orcaflashforge://open/?file=" + evt.url.utf8_string(), evt.fileName.utf8_string(), true);
}

void FFWebViewPanel::OnOpenBase64Model(wxCommandEvent &evt)
{
    if (m_modelBrowser == nullptr) {
        return;
    }
    wxArrayString paths;
    paths.Add(evt.GetString());
    wxGetApp().plater()->load_files(paths);
}

void FFWebViewPanel::OnMainFrameIconize(wxIconizeEvent &evt)
{
    evt.Skip();
    CallAfter([this, isIconized = evt.IsIconized()]() {
        if (m_viewNowWindow->IsAutoCloseTimerRunning()) {
            if (isIconized) {
                m_viewNowWindow->Hide();
            } else {
                MoveViewNowWindow();
                m_viewNowWindow->Show();
            }
        }
    });
}

void FFWebViewPanel::OnMainFrameMove(wxMoveEvent &evt)
{
    evt.Skip();
    CallAfter([this]() {
        if (m_viewNowWindow->IsShownOnScreen()) {
            MoveViewNowWindow();
        }
    });
}

void FFWebViewPanel::OnMainFrameSize(wxSizeEvent &evt)
{
    evt.Skip();
    CallAfter([this]() {
        if (m_viewNowWindow->IsShownOnScreen()) {
            MoveViewNowWindow();
        }
    });
}

void FFWebViewPanel::OnComMaintain(ComWanDevMaintainEvent &evt)
{
    evt.Skip();
    if (evt.login) {
        CheckGetOnlineConfig();
    } else {
        m_uid.clear();
        m_userConfig = nlohmann::json();
    }
}

void FFWebViewPanel::OnComGetUserProfile(ComGetUserProfileEvent &evt)
{
    evt.Skip();
    m_uid = evt.userProfile.uid;
}

void FFWebViewPanel::OnComAddPrintListModel(ComBusRequestEvent &evt)
{
    evt.Skip();
    if (evt.requestId != m_printListReqId) {
        return;
    }
    if (evt.ret == COM_OK) {
        m_printListAdded = true;
        SetupPrintListButton(m_printListAdded);
        m_viewNowWindow->Setup(m_addPrintListTipText, true);
        MoveViewNowWindow();
        m_viewNowWindow->ShowAutoClose(3000);
        SyncModelAction("add_print_list_model");
    } else if (evt.ret == COM_PRINT_LIST_MODEL_COUNT_EXCEEDED) {
        MessageDialog dlg(wxGetApp().mainframe, wxString::FromUTF8(evt.message), _L("Information"));
        dlg.ShowModal();
    } else {
        wxString text = wxString::Format("%s (%s)", _L("Network Error"), m_navPrintListBtn->GetLabel());
        MessageDialog dlg(wxGetApp().mainframe, text, _L("Error"));
        dlg.ShowModal();
    }
    m_printListReqId = MultiComHelper::InvalidRequestId;
}

void FFWebViewPanel::OnComRemovePrintListModel(ComBusRequestEvent &evt)
{
    evt.Skip();
    if (evt.requestId != m_printListReqId) {
        return;
    }
    if (evt.ret == COM_OK) {
        m_printListAdded = false;
        SetupPrintListButton(m_printListAdded);
        m_viewNowWindow->Setup(m_removePrintListTipText, false);
        MoveViewNowWindow();
        m_viewNowWindow->ShowAutoClose(3000);
        SyncModelAction("remove_print_list_model");
    } else {
        wxString text = wxString::Format("%s (%s)", _L("Network Error"), m_navPrintListBtn->GetLabel());
        MessageDialog dlg(wxGetApp().mainframe, text, _L("Error"));
        dlg.ShowModal();
    }
    m_printListReqId = MultiComHelper::InvalidRequestId;
}

void FFWebViewPanel::OnComLikeModel(ComBusRequestEvent& evt) 
{
    evt.Skip();
    if (evt.ret == COM_OK) {
        m_modelLike = !m_modelLike;
        m_modelLikeCount += m_modelLike ? 1 : -1;
        SetupLikeButton();
        SyncModelAction(m_modelLike ? "like_model" : "unlike_model");
    } else {
        wxString      text = wxString::Format("%s (%s)", _L("Network Error"), "likeModel");
        MessageDialog dlg(wxGetApp().mainframe, text, _L("Error"));
        dlg.ShowModal();
    }
}

void FFWebViewPanel::OnComReportModel(ComBusRequestEvent &evt)
{
    evt.Skip();
    if (evt.requestId != m_reportReqId) {
        return;
    }
    if (evt.ret == COM_OK) {
        SyncModelAction("report_model");
        MessageDialog dlg(wxGetApp().mainframe, _L("Report submitted"), _L("Information"));
        dlg.ShowModal();
    } else {
        wxString text = wxString::Format("%s (%s)", _L("Network Error"), m_reportWndTitle);
        MessageDialog dlg(wxGetApp().mainframe, text, _L("Error"));
        dlg.ShowModal();
    }
    m_reportReqId = MultiComHelper::InvalidRequestId;
}

wxString FFWebViewPanel::GetModelUrlId(const wxString &url)
{
    wxURL wxUrl(url);
    wxString server = wxUrl.GetServer();
    wxString port = wxUrl.GetPort();
    wxString path = wxUrl.GetPath();
    size_t pos = path.find_last_of('@');
    if (pos != wxString::npos) {
        path.resize(pos);
    }
    if (path.EndsWith('/')) {
        path.RemoveLast();
    }
    return server + port + path;
}

}} // namespace Slic3r::GUI
