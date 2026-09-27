#ifndef slic3r_GUI_SwitchButton_hpp_
#define slic3r_GUI_SwitchButton_hpp_

#include "../wxExtensions.hpp"
#include "StateColor.hpp"
#include "StaticBox.hpp"

#include <vector>
#include <wx/sizer.h>
#include <wx/tglbtn.h>
#include "Button.hpp"

wxDECLARE_EVENT(wxCUSTOMEVT_SWITCH_POS, wxCommandEvent);
wxDECLARE_EVENT(wxCUSTOMEVT_MULTISWITCH_SELECTION, wxCommandEvent);

class SwitchButton : public wxBitmapToggleButton
{
public:
	SwitchButton(wxWindow * parent = NULL, wxWindowID id = wxID_ANY);

public:
	void SetLabels(wxString const & lbl_on, wxString const & lbl_off);

	void SetTextColor(StateColor const &color);

	void SetTextColor2(StateColor const &color);

    void SetTrackColor(StateColor const &color);

	void SetThumbColor(StateColor const &color);

	void SetValue(bool value) override;

	void Rescale();

    bool SetBackgroundColour(const wxColour& colour) override;

private:
	void update();

private:
	ScalableBitmap m_on;
	ScalableBitmap m_off;

	wxString labels[2];
    StateColor   text_color;
    StateColor   text_color2;
	StateColor   track_color;
	StateColor   thumb_color;
};

class ModeSwitchButton : public StaticBox
{
public:
    ModeSwitchButton(wxWindow* parent = nullptr, wxWindowID id = wxID_ANY);

    int  GetSelection() const { return m_selection; }
    void SetSelection(int selection);
    void SelectAndNotify(int selection);

    void Rescale();
    void msw_rescale() { Rescale(); }

    bool Enable(bool enable = true) override;
    void SetDevMode(bool enable = true);
    bool GetDevMode() const {return m_dev_mode;};

protected:
    void doRender(wxDC& dc) override;

private:
    void mouseDown(wxMouseEvent& event);
    void mouseReleased(wxMouseEvent& event);
    void mouseCaptureLost(wxMouseCaptureLostEvent& event);
    int  hit_test_selection(const wxPoint& point) const;
    wxRect thumb_rect_for(int selection) const;
    void update_tooltip();

private:
    int      m_selection { 0 };
    bool     m_pressed   { false };
    bool     m_enabled   { true };
    bool     m_dev_mode  { false };
    wxString   m_tooltips[4];
    StateColor dot_active;
    StateColor dot_dimmed;
    StateColor text_color;
    StateColor track_background;
    StateColor track_border;
};

class SwitchBoard : public wxWindow
{
public:
    SwitchBoard(wxWindow *parent = NULL, wxString leftL = "", wxString right = "", wxSize size = wxDefaultSize);
    wxString leftLabel;
    wxString rightLabel;

	void updateState(wxString target);

	bool switch_left{false};
    bool switch_right{false};
    bool is_enable {true};

    void* client_data = nullptr;/*MachineObject* in StatusPanel*/

public:
    bool Enable(bool enable = true) override;
    bool Disable() { return Enable(false); }
    bool IsEnabled(){return is_enable;};

    void  SetClientData(void* data) { client_data = data; };
    void* GetClientData() { return client_data; };

    void SetAutoDisableWhenSwitch() { auto_disable_when_switch = true; };

protected:
    void paintEvent(wxPaintEvent& evt);
    void render(wxDC& dc);
    void doRender(wxDC& dc);
    void on_left_down(wxMouseEvent& evt);

private:
    bool auto_disable_when_switch = false;
};

class MultiSwitchButton : public StaticBox
{
public:
    MultiSwitchButton(wxWindow *parent = nullptr, wxWindowID id = wxID_ANY, const wxPoint &pos = wxDefaultPosition,
                      const wxSize &size = wxDefaultSize, long style = 0);
    ~MultiSwitchButton();

    int AppendOption(const wxString &option, void *clientData = nullptr);
    void SetOptions(const std::vector<wxString> &options);
    void DeleteAllOptions();

    unsigned int GetCount() const;

    int      GetSelection() const;
    void     SetSelection(int index);
    wxString GetSelectedText() const;

    Button*  GetButton(unsigned int index) const
    {
        return index >= 0 && index < btns.size() ? btns[index] : nullptr;
    }

    wxString GetOptionText(unsigned int index) const;
    // The button takes the width of the new text.
    void     SetOptionText(unsigned int index, const wxString &text);

    // Snapmaker Orca: the entries continue on further rows when one row of them would be wider
    // than `width` (0: one row, whatever its width); kept over later SetOptions. The speed
    // selector of the Process tab uses it in a narrow sidebar.
    void SetMaxRowWidth(int width);
    int  GetMaxRowWidth() const { return m_max_row_width; }
    // The width the button of entry `index` would take with `option` as its text (text, padding,
    // the indicator dot when the entry shows one), as AppendOption sizes it. Measured without
    // changing the entry; an index beyond the entries counts as without a dot.
    int  MeasureOption(unsigned int index, const wxString &option) const;

    void *GetOptionData(unsigned int index) const;
    void  SetOptionData(unsigned int index, void *clientData);

    void SetBackgroundColor(const StateColor &color);
    void SetTextColor(const StateColor &color);
    void SetButtonTextColor(int index, const StateColor &color)
    {
        if (index >= btns.size()) return;

        btns[index]->SetTextColor(color);
        btns[index]->Refresh();
    }
    void SetButtonCornerRadius(double radius);
    void SetButtonPadding(const wxSize &padding);
    // Snapmaker Orca: the dot of an entry (Button::SetIndicator), shown on the selected entry too.
    void SetOptionIndicator(unsigned int index, bool on)
    {
        if (index < btns.size())
            btns[index]->SetIndicator(on);
    }

    void Rescale();

protected:
    void button_clicked(wxCommandEvent &event);
    void update_button_styles();

    bool send_selection_event();

private:
    Button *make_button(const wxString &option, void *clientData);
    // Lays the buttons out in rows of at most m_max_row_width (one row when 0).
    void    rebuild_rows();

    std::vector<Button *> btns;
    wxBoxSizer           *sizer = nullptr;   // vertical: one horizontal row sizer per row
    int                   sel   = -1;
    int                   m_max_row_width = 0;
    // The height of one row of entries: the minimum height of the control (20 px, as before the
    // rows), multiplied by the rows in rebuild_rows.
    int                   m_row_height    = 20;

    StateColor m_bg_color;
    StateColor m_text_color;
    double     m_button_radius;
    wxSize     m_button_padding;
};

#endif // !slic3r_GUI_SwitchButton_hpp_
