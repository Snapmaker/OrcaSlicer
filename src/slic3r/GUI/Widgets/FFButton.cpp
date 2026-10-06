#include "FFButton.hpp"
#include <memory>
#include <wx/dcgraph.h>
#include "slic3r/GUI/wxExtensions.hpp"


FFButton::FFButton(wxWindow* parent, wxWindowID id/*= wxID_ANY*/, const wxString& label/*= ""*/,
	int borderRadius/*=4*/, bool borderFlag/* = true*/)
	: wxWindow(parent, id, wxDefaultPosition, wxDefaultSize, wxNO_BORDER)
	, m_hoverFlag(false)
	, m_pressFlag(false)
	, m_borderFlag(borderFlag)
    , m_enable(true)
	, m_borderRadius(borderRadius)
	, m_borderWidth(2)
    , m_iconSpacing(8)
	, m_fontColor("#333333")
	, m_fontHoverColor("#65A79E")
	, m_fontPressColor("#419488")
	, m_fontDisableColor("#333333")
	, m_borderColor("#333333")
	, m_borderHoverColor("#65A79E")
	, m_borderPressColor("#419488")
	, m_borderDisableColor("#dddddd")
	, m_bgColor("#ffffff")
	, m_bgHoverColor("#ffffff")
	, m_bgPressColor("#ffffff")
	, m_bgDisableColor("#dddddd")
{
	if (parent) {
		SetBackgroundColour(parent->GetBackgroundColour());	
	}
	SetLabel(label);
	Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& e) { m_hoverFlag = true; Refresh(); e.Skip(); });
	Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) { m_hoverFlag = false; m_pressFlag = false; Refresh(); e.Skip(); });
	Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) { m_pressFlag = true; Refresh(); e.Skip(); });
	Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& e) { m_pressFlag = false; Refresh(); sendEvent(); e.Skip(); });
	Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent &e) { Refresh(); e.Skip(); });
	Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent &e) { Refresh(); e.Skip(); });
	Bind(wxEVT_PAINT, &FFButton::OnPaint, this);
	Bind(wxEVT_ERASE_BACKGROUND, [=](auto& e) {
		e.Skip();
	});
	Layout();
	Fit();
}

bool FFButton::Enable(bool enable/* = true*/)
{
	bool ret = wxWindow::Enable(enable);
	if (ret) {
		Refresh();
	}
	return ret;
}


void FFButton::SetEnable(bool enable) 
{
	m_enable = enable; 
	Refresh();
}

void FFButton::SetLabel(const wxString & label)
{
	wxWindow::SetLabel(label);

	wxScreenDC dc;
	dc.SetFont(GetFont());
	wxSize textSize = dc.GetTextExtent(label);
	SetMinSize(textSize + wxSize(FromDIP(32), FromDIP(12)));
	Refresh();
}

void FFButton::SetLabel(const wxString& label, int minWidth, int minHeight)
{
    wxWindow::SetLabel(label);

    wxScreenDC dc;
    dc.SetFont(GetFont());
	wxSize textSize = dc.GetTextExtent(label);
	int width = std::max(textSize.GetWidth() + FromDIP(32), minWidth);
	int height = std::max(textSize.GetHeight() + FromDIP(12), minHeight);
    SetMinSize(wxSize(width, height));
    Refresh();
}

void FFButton::SetLabel(const wxString& label, int minWidth, int paddingX, int minHeight, int paddingY)
{
    wxWindow::SetLabel(label);

    wxScreenDC dc;
    dc.SetFont(GetFont());
    wxSize textSize = dc.GetTextExtent(label);
    int width = std::max(textSize.GetWidth() + paddingX * 2, minWidth);
    int height = std::max(textSize.GetHeight() + paddingY * 2, minHeight);
    SetMinSize(wxSize(width, height));
    Refresh();
}

void FFButton::SetFontColor(const wxColour& color)
{
	m_fontColor = color;
	Refresh();
}

void FFButton::SetFontHoverColor(const wxColour& color)
{
	m_fontHoverColor = color;
	Refresh();
}

void FFButton::SetFontPressColor(const wxColour& color)
{
	m_fontPressColor = color;
	Refresh();
}

void FFButton::SetFontDisableColor(const wxColour& color)
{
	m_fontDisableColor = color;
	Refresh();
}

void FFButton::SetFontUniformColor(const wxColour& color)
{
	m_fontColor = color;
	m_fontHoverColor = color;
	m_fontPressColor = color;
	m_fontDisableColor = color;
	Refresh();
}

void FFButton::SetBorderColor(const wxColour& color)
{
	m_borderColor = color;
	Refresh();
}

void FFButton::SetBorderHoverColor(const wxColour& color)
{
	m_borderHoverColor = color;
	Refresh();
}

void FFButton::SetBorderPressColor(const wxColour& color)
{
	m_borderPressColor = color;
	Refresh();
}

void FFButton::SetBorderDisableColor(const wxColour& color)
{
	m_borderDisableColor = color;
	Refresh();
}

void FFButton::SetBorderUniformColor(const wxColour& color)
{
	m_borderColor = color;
	m_borderHoverColor = color;
	m_borderPressColor = color;
	m_borderDisableColor = color;
	Refresh();
}

void FFButton::SetBorderWidth(int width)
{
    m_borderWidth = width;
	Refresh();
}

void FFButton::SetBGColor(const wxColour& color)
{
	m_bgColor = color;
	Refresh();
}

void FFButton::SetBGHoverColor(const wxColour& color)
{
	m_bgHoverColor = color;
	Refresh();
}

void FFButton::SetBGPressColor(const wxColour& color)
{
	m_bgPressColor = color;
	Refresh();
}

void FFButton::SetBGDisableColor(const wxColour& color)
{
	m_bgDisableColor = color;
	Refresh();
}

void FFButton::SetBGUniformColor(const wxColour& color)
{
	m_bgColor = color;
	m_bgHoverColor = color;
	m_bgPressColor = color;
	m_bgDisableColor = color;
	Refresh();
}

void FFButton::SetIcon(const ScalableBitmap& bmp)
{
    m_bitmap = bmp;
    Refresh();
}

void FFButton::SetHoverIcon(const ScalableBitmap& bmp)
{
    m_hoverBitmap = bmp;
    Refresh();
}

void FFButton::SetPressIcon(const ScalableBitmap& bmp)
{
    m_pressBitmap = bmp;
    Refresh();
}

void FFButton::SetDisableIcon(const ScalableBitmap& bmp)
{
    m_disableBitmap = bmp;
    Refresh();
}

void FFButton::SetUniformIcon(const ScalableBitmap& bmp)
{
    m_bitmap        = bmp;
    m_hoverBitmap   = bmp;
    m_pressBitmap   = bmp;
    m_disableBitmap = bmp;
    Refresh();
}

void FFButton::SetIconSpacing(int spacing) 
{ 
	m_iconSpacing = spacing;
    Refresh();
}

void FFButton::OnPaint(wxPaintEvent& event)
{
	wxPaintDC dc(this);
    render(dc);
    event.Skip();
}

void FFButton::render(wxPaintDC &dc)
{
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (gc == nullptr) {
        return;
    }

	wxPen pen;
    if (!IsEnabled() || !m_enable) {
		pen.SetColour(m_borderDisableColor);
		gc->SetBrush(m_bgDisableColor);
	} else if (m_pressFlag) {
		pen.SetColour(m_borderPressColor);
		gc->SetBrush(m_bgPressColor);
	} else if (m_hoverFlag) {		
		pen.SetColour(m_borderHoverColor);
		gc->SetBrush(m_bgHoverColor);
	} else {
		pen.SetColour(m_borderColor);
		gc->SetBrush(m_bgColor);
	}
	wxSize size = GetSize();
	if (!m_borderFlag || m_borderWidth == 0) {
		gc->SetPen(*wxTRANSPARENT_PEN);
	} else {
		pen.SetWidth(m_borderWidth);
		gc->SetPen(pen);
	}

	double bordeWidth = m_borderFlag ? m_borderWidth : 0;
    double x          = bordeWidth * 0.5;
    double y          = bordeWidth * 0.5;
    double width      = size.x - bordeWidth;
    double height     = size.y - bordeWidth;
    if (m_borderRadius == 0) {
        gc->DrawRectangle(x, y, width, height);
    } else {
        gc->DrawRoundedRectangle(x, y, width, height, m_borderRadius);
    }

	wxString       text = GetLabel();
    ScalableBitmap bmp;
    if (!text.IsEmpty()) {
        if (!IsEnabled() || !m_enable) {
            dc.SetTextForeground(m_fontDisableColor);
            bmp = m_disableBitmap;
        } else if (m_pressFlag) {
            dc.SetTextForeground(m_fontPressColor);
            bmp = m_pressBitmap;
        } else if (m_hoverFlag) {
            dc.SetTextForeground(m_fontHoverColor);
            bmp = m_hoverBitmap;
        } else {
            dc.SetTextForeground(m_fontColor);
            bmp = m_bitmap;
        }
        // For Text: Just align-center
        dc.SetFont(GetFont());
        wxSize size         = GetSize();
        wxSize textSize     = dc.GetMultiLineTextExtent(text);
        int    contentWidth = textSize.x;

        if (bmp.bmp().IsOk()) {
            contentWidth += bmp.GetBmpWidth() + FromDIP(m_iconSpacing);
        }

        int pt = (size.x - contentWidth) / 2;
        if (bmp.bmp().IsOk()) {
            gc->DrawBitmap(bmp.bmp(), pt, (size.y - bmp.GetBmpHeight()) / 2, bmp.GetBmpWidth(), bmp.GetBmpHeight());
            pt += bmp.GetBmpWidth() + FromDIP(m_iconSpacing);
        }
        dc.DrawText(text, pt, (size.y - textSize.y) / 2);
    } else {
        if (!IsEnabled() || !m_enable) {
            bmp = m_disableBitmap;
        } else if (m_pressFlag) {
            bmp = m_pressBitmap;
        } else if (m_hoverFlag) {
            bmp = m_hoverBitmap;
        } else {
            bmp = m_bitmap;
        }
        if (bmp.bmp().IsOk()) {
            gc->DrawBitmap(bmp.bmp(), (size.x - bmp.GetBmpWidth()) / 2, (size.y - bmp.GetBmpHeight()) / 2, bmp.GetBmpWidth(),
                           bmp.GetBmpHeight());
        }
	}
}

void FFButton::updateState()
{
    if (!IsEnabled() || !m_enable) {
		SetForegroundColour(m_fontDisableColor);
	} else if (m_pressFlag) {
		SetForegroundColour(m_fontPressColor);
	} else if (m_hoverFlag) {
		SetForegroundColour(m_fontHoverColor);
	} else {
		SetForegroundColour(m_fontColor);
	}
	Refresh();
}

void FFButton::sendEvent()
{
	wxCommandEvent event(wxEVT_BUTTON);
	event.SetEventObject(this);
	event.SetId(GetId());
	wxPostEvent(this, event);
}


FFPushButton::FFPushButton(wxWindow *parent,
						    wxWindowID id,
						    const wxString &normalIcon,
						    const wxString &hoverIcon,
							const wxString &pressIcon,
							const wxString &disableIcon,
							const int iconSize)
    : wxButton(parent, id, wxEmptyString, wxPoint(10, 10), wxDefaultSize, wxNO_BORDER)
    , m_normalIcon(normalIcon)
    , m_hoverIcon(hoverIcon)
    , m_pressIcon(pressIcon)
    , m_disableIcon(disableIcon)
{
    //SetBitmap(wxBitmap(normalIcon));
    SetMinSize(wxSize(FromDIP(21), FromDIP(21)));
    SetMaxSize(wxSize(FromDIP(21), FromDIP(21)));
    m_normalBitmap  = create_scaled_bitmap(normalIcon.ToStdString(), this, iconSize);
    m_hoverBitmap   = create_scaled_bitmap(hoverIcon.ToStdString(), this, iconSize);
    m_pressBitmap   = create_scaled_bitmap(pressIcon.ToStdString(), this, iconSize);
    m_disableBitmap = create_scaled_bitmap(disableIcon.ToStdString(), this, iconSize);
    Bind(wxEVT_PAINT, &FFPushButton::OnPaint, this);
    Bind(wxEVT_LEFT_DOWN, &FFPushButton::OnMousePress, this);
    Bind(wxEVT_LEFT_UP, &FFPushButton::OnMouseRelease, this);
    Bind(wxEVT_ENTER_WINDOW, &FFPushButton::OnMouseEnter, this);
    Bind(wxEVT_LEAVE_WINDOW, &FFPushButton::OnMouseLeave, this);
	Bind(wxEVT_SET_FOCUS, &FFPushButton::OnSetFocus, this);
	Bind(wxEVT_KILL_FOCUS, &FFPushButton::OnKillFocus, this);
}

void FFPushButton::OnPaint(wxPaintEvent &event) 
{
    wxPaintDC dc(this);
    if (IsEnabled()) {
        if (m_isPressed) {
            dc.DrawBitmap(m_pressBitmap, 0, 0, true);
        } else if (m_isHover) {
            dc.DrawBitmap(m_hoverBitmap, 0, 0, true);
        } else {
            dc.DrawBitmap(m_normalBitmap, 0, 0, true);
        }
    } else {
        dc.DrawBitmap(m_disableBitmap, 0, 0, true);
    }
}
