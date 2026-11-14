#pragma once

#include "MixedColorMatchHelpers.hpp"
#include "I18N.hpp"
#include "MFDTheme.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/TextInput.hpp"
#include "wxExtensions.hpp"
#include <wx/stattext.h>
#include <wx/sizer.h>
#include <wx/dcbuffer.h>
#include <wx/panel.h>
#include <wx/textctrl.h>
#include <wx/graphics.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

namespace Slic3r::GUI {

// Slider drawn in the dialog's own style (same look as MatchRangeSlider in
// MixedFilamentDialog.cpp). The native trackbar ignores the dark theme.
// Mirrors the part of the wxSlider interface the stop editor uses.
class MixedGradientGapSlider : public wxPanel
{
public:
    MixedGradientGapSlider(wxWindow* parent, int value, int min_value, int max_value)
        : wxPanel(parent, wxID_ANY), m_min(min_value), m_max(std::max(min_value, max_value))
    {
        m_value = std::clamp(value, m_min, m_max);
        SetMinSize(wxSize(FromDIP(60), FromDIP(22)));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        MFDTheme::apply_window(this);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
            m_dragging = true;
            if (!HasCapture())
                CaptureMouse();
            change(value_from_x(e.GetX()));
        });
        Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
            if (m_dragging)
                change(value_from_x(e.GetX()));
        });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
            m_dragging = false;
            if (HasCapture())
                ReleaseMouse();
        });
        Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { m_dragging = false; });
    }
    ~MixedGradientGapSlider() override
    {
        if (HasCapture())
            ReleaseMouse();
    }

    int  GetValue() const { return m_value; }
    int  GetMax() const { return m_max; }
    void SetRange(int min_value, int max_value)
    {
        m_min   = min_value;
        m_max   = std::max(min_value, max_value);
        m_value = std::clamp(m_value, m_min, m_max);
        Refresh();
    }
    void SetValue(int value)
    {
        m_value = std::clamp(value, m_min, m_max);
        Refresh();
    }

private:
    int value_from_x(int x) const
    {
        const double thumb = FromDIP(12);
        const double track = std::max(1.0, GetClientSize().x - thumb);
        return m_min + int(std::lround((x - thumb / 2.0) / track * (m_max - m_min)));
    }
    // Only a change made by the user is reported; SetValue() stays silent.
    void change(int value)
    {
        value = std::clamp(value, m_min, m_max);
        if (value == m_value)
            return;
        m_value = value;
        Refresh();
        wxCommandEvent event(wxEVT_SLIDER, GetId());
        event.SetEventObject(this);
        event.SetInt(m_value);
        ProcessWindowEvent(event);
    }
    void paint()
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(MFDTheme::card_background()));
        dc.Clear();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc)
            return;
        gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
        const double   scale    = FromDIP(100) / 100.0;
        const wxSize   size     = GetClientSize();
        const double   thumb    = 12.0 * scale;
        const double   track_h  = 3.0 * scale;
        const double   track_x  = thumb / 2.0;
        const double   track_w  = std::max(1.0, size.x - thumb);
        const double   track_y  = (size.y - track_h) / 2.0;
        const double   fraction = m_max > m_min ? double(m_value - m_min) / double(m_max - m_min) : 0.0;
        const wxColour accent   = StateColor::darkModeColorFor(wxColour("#009688"));
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(StateColor::darkModeColorFor(wxColour("#EBEBEB"))));
        gc->DrawRoundedRectangle(track_x, track_y, track_w, track_h, track_h / 2.0);
        if (fraction > 0.0) {
            // Never narrower than its own corner rounding; the thumb covers the difference.
            gc->SetBrush(wxBrush(accent));
            gc->DrawRoundedRectangle(track_x, track_y, std::max(track_w * fraction, track_h), track_h, track_h / 2.0);
        }
        const double radius = thumb / 2.0 - scale;
        gc->SetBrush(*wxWHITE_BRUSH);
        gc->SetPen(wxPen(accent, std::max(1, int(std::lround(1.5 * scale)))));
        gc->DrawEllipse(track_x + track_w * fraction - radius, size.y / 2.0 - radius, 2.0 * radius, 2.0 * radius);
    }

    int  m_value{0};
    int  m_min{0};
    int  m_max{100};
    bool m_dragging{false};
};

// The recipe and predictor are supplied by the dialog so this control does not
// keep a second copy of the selected materials or calibration.
class MixedGradientStops : public wxPanel
{
public:
    using Recipe = std::function<MixedFilament()>;
    MixedGradientStops(wxWindow*                                      parent,
                       Recipe                                         recipe,
                       MixedFilamentDisplayContext                    context,
                       std::function<void(const std::vector<float>&)> changed,
                       std::function<void(const std::vector<float>&)> widths_changed,
                       std::vector<wxString>                          filament_names,
                       std::function<void(size_t, int)>               filament_changed)
        : wxPanel(parent)
        , m_recipe(std::move(recipe))
        , m_context(std::move(context))
        , m_changed(std::move(changed))
        , m_widths_changed(std::move(widths_changed))
        , m_filament_names(std::move(filament_names))
        , m_filament_changed(std::move(filament_changed))
    {
        MFDTheme::apply_window(this);
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        m_canvas    = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(60)));
        m_canvas->SetMinSize(wxSize(-1, FromDIP(60)));
        m_canvas->SetBackgroundStyle(wxBG_STYLE_PAINT);
        MFDTheme::apply_window(m_canvas);
        sizer->Add(m_canvas, 0, wxEXPAND);
        m_filament_combo = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(-1, FromDIP(30)), 0, nullptr,
                                        wxCB_READONLY);
        for (size_t i = 0; i < m_context.physical_colors.size(); ++i) {
            wxBitmap* icon = get_extruder_color_icon(m_context.physical_colors[i], std::to_string(i + 1), FromDIP(20), FromDIP(20));
            m_filament_combo->Append(m_filament_names[i], icon ? icon->ConvertToImage() : wxNullImage);
        }
        m_filament_combo->SetToolTip(
            _L("Choose a filament for the selected color stop. Selecting a filament already in use swaps the two stops."));
        // The rows below share the bar's side inset, so their edges line up with it.
        const int side = FromDIP(12);
        // Nested so the gap under the combo disappears with it when a transition is selected.
        auto* combo_row = new wxBoxSizer(wxVERTICAL);
        combo_row->Add(m_filament_combo, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
        sizer->Add(combo_row, 0, wxEXPAND | wxLEFT | wxRIGHT, side);
        m_filament_combo->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) {
            const int selected = m_filament_combo->GetSelection();
            if (m_selected % 2 == 0 && selected != wxNOT_FOUND && m_filament_changed)
                m_filament_changed(m_selected / 2, selected);
        });
        // One row for both numbers: position on the left, solid width against the right edge,
        // directly above the gap field.
        auto* row        = new wxBoxSizer(wxHORIZONTAL);
        m_position_label = label(this, _L("Position:"));
        row->Add(m_position_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        m_position_box = input(this, "0.0");
        m_position     = m_position_box->GetTextCtrl();
        m_position_box->Enable(false);
        row->Add(m_position_box, 0, wxALIGN_CENTER_VERTICAL);
        row->AddStretchSpacer();
        m_width_panel = new wxPanel(this);
        MFDTheme::apply_window(m_width_panel);
        auto* width_row = new wxBoxSizer(wxHORIZONTAL);
        // Shortened from "Solid-color width:" so both fields fit one row.
        width_row->Add(label(m_width_panel, _L("Solid width:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        auto* width_box = input(m_width_panel, "3.0");
        m_width         = width_box->GetTextCtrl();
        width_row->Add(width_box, 0, wxALIGN_CENTER_VERTICAL);
        m_width_panel->SetSizer(width_row);
        m_width_panel->SetToolTip(
            _L("Width of this solid color as a percentage of the complete gradient. Drag the square handles below the bar to adjust it."));
        row->Add(m_width_panel, 0, wxALIGN_CENTER_VERTICAL);
        m_width_panel->Hide();
        sizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, side);
        // The minimum gap still limits how close two handles can be dragged, at DEFAULT_GAP,
        // but its row is not shown. SHOW_GAP_ROW brings the controls back.
        auto* gap_row = new wxBoxSizer(wxHORIZONTAL);
        gap_row->Add(label(this, _L("Min Stop Gap:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        m_gap_slider = new MixedGradientGapSlider(this, DEFAULT_GAP, 0, 50);
        gap_row->Add(m_gap_slider, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        auto* gap_box = input(this, wxString::Format("%d", DEFAULT_GAP));
        m_gap         = gap_box->GetTextCtrl();
        gap_row->Add(gap_box, 0, wxALIGN_CENTER_VERTICAL);
        sizer->Add(gap_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, side);
        sizer->Show(gap_row, SHOW_GAP_ROW, true);
        SetSizer(sizer);
        auto position_changed = [this](wxEvent& event) {
            double     pct;
            const auto stops = mixed_gradient_stops(m_recipe(), m_context.num_physical);
            if (m_position->GetValue().ToDouble(&pct) && std::isfinite(pct) && m_selected > 0 && m_selected + 1 < stops.size()) {
                double t = pct / 100.0;
                if (m_selected % 2 != 0)
                    t = stops[m_selected - 1] + t * (stops[m_selected + 1] - stops[m_selected - 1]);
                move_stop(t);
            }
            sync_position();
            event.Skip();
        };
        auto width_changed = [this](wxEvent& event) {
            double pct;
            if (m_width->GetValue().ToDouble(&pct) && std::isfinite(pct))
                set_width(pct / 100.0);
            sync_position();
            event.Skip();
        };
        m_width->Bind(wxEVT_TEXT_ENTER, width_changed);
        m_width->Bind(wxEVT_KILL_FOCUS, width_changed);
        m_canvas->SetToolTip(_L("Drag circles to move colors, white ticks to adjust transitions, and the square handles below each "
                                "interior color to adjust its solid-color width."));
        m_position->Bind(wxEVT_TEXT_ENTER, position_changed);
        m_position->Bind(wxEVT_KILL_FOCUS, position_changed);
        m_gap_slider->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) { set_gap(m_gap_slider->GetValue()); });
        auto gap_changed = [this](wxEvent& event) {
            long pct;
            if (m_gap->GetValue().ToLong(&pct))
                set_gap(int(std::clamp(pct, 0L, long(m_gap_slider->GetMax()))));
            else
                m_gap->ChangeValue(wxString::Format("%d", m_gap_slider->GetValue()));
            event.Skip();
        };
        m_gap->Bind(wxEVT_TEXT_ENTER, gap_changed);
        m_gap->Bind(wxEVT_KILL_FOCUS, gap_changed);
        m_canvas->Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
        m_canvas->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
            const auto stops = mixed_gradient_stops(m_recipe(), m_context.num_physical);
            if (stops.empty())
                return;
            m_width_edge           = 0;
            const auto   widths    = mixed_gradient_solid_half_widths(m_recipe(), m_context.num_physical,
                                                                      m_context.preview_settings.gradient_middle_window);
            const double bar_width = std::max(1, m_canvas->GetClientSize().x - FromDIP(24));
            const double bottom    = m_canvas->GetClientSize().y - FromDIP(12);
            double       nearest   = FromDIP(9);
            if (std::abs(e.GetY() - bottom - FromDIP(3)) <= FromDIP(8)) {
                for (size_t i = 1; i + 1 < widths.size(); ++i) {
                    for (int side : {-1, 1}) {
                        const double x = FromDIP(12) + (stops[2 * i] + side * widths[i]) * bar_width;
                        if (std::abs(e.GetX() - x) < nearest) {
                            nearest      = std::abs(e.GetX() - x);
                            m_selected   = 2 * i;
                            m_width_edge = side;
                        }
                    }
                }
            }
            if (m_width_edge != 0) {
                sync_position();
                m_dragging = true;
                if (!m_canvas->HasCapture())
                    m_canvas->CaptureMouse();
                m_canvas->Refresh();
                return;
            }
            const double t = progress(e.GetX());
            m_selected     = 0;
            for (size_t i = 1; i < stops.size(); ++i)
                if (std::abs(stops[i] - t) < std::abs(stops[m_selected] - t))
                    m_selected = i;
            sync_position();
            m_dragging = m_selected > 0 && m_selected + 1 < stops.size();
            if (m_dragging && !m_canvas->HasCapture())
                m_canvas->CaptureMouse();
            m_canvas->Refresh();
        });
        m_canvas->Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
            if (m_dragging) {
                if (m_width_edge != 0) {
                    const auto stops = mixed_gradient_stops(m_recipe(), m_context.num_physical);
                    if (m_selected < stops.size())
                        set_width(2.0 * m_width_edge * (progress(e.GetX()) - stops[m_selected]));
                } else
                    move_stop(progress(e.GetX()));
            }
        });
        m_canvas->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
            m_dragging = false;
            if (m_canvas->HasCapture())
                m_canvas->ReleaseMouse();
            m_canvas->Refresh();
        });
        m_canvas->Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) {
            m_dragging = false;
            m_canvas->Refresh();
        });
        refresh_recipe();
    }
    ~MixedGradientStops() override
    {
        if (m_canvas->HasCapture())
            m_canvas->ReleaseMouse();
    }
    void refresh_recipe()
    {
        const auto stops   = mixed_gradient_stops(m_recipe(), m_context.num_physical);
        const int  max_gap = stops.size() > 1 ? int(100 / (stops.size() - 1)) : 50;
        // With the row hidden nothing can raise the gap again after more colors lowered it,
        // so it is taken from the default each time instead of from its last value.
        const int gap = std::min(SHOW_GAP_ROW ? m_gap_slider->GetValue() : DEFAULT_GAP, max_gap);
        m_gap_slider->SetRange(0, max_gap);
        m_gap_slider->SetValue(gap);
        m_gap->ChangeValue(wxString::Format("%d", gap));
        sync_position();
        m_canvas->Refresh();
    }

private:
    static constexpr bool SHOW_GAP_ROW = false; // the "Min Stop Gap" slider and field
    static constexpr int  DEFAULT_GAP  = 10;    // percent of the gradient

    wxStaticText* label(wxWindow* parent, const wxString& text)
    {
        auto* result = new wxStaticText(parent, wxID_ANY, text);
        result->SetFont(::Label::Body_13);
        MFDTheme::apply_text(result);
        return result;
    }
    // The application's own themed field, with the unit drawn inside it.
    TextInput* input(wxWindow* parent, const wxString& text)
    {
        const wxSize size(FromDIP(70), FromDIP(26));
        auto*        result = new TextInput(parent, text, "%", "", wxDefaultPosition, size, wxTE_PROCESS_ENTER);
        result->SetMinSize(size);
        return result;
    }
    void sync_position()
    {
        const auto stops = mixed_gradient_stops(m_recipe(), m_context.num_physical);
        if (m_selected >= stops.size())
            m_selected = 0;
        const auto ids                = mixed_gradient_components(m_recipe(), m_context.num_physical);
        const bool color_stop         = m_selected % 2 == 0 && m_selected / 2 < ids.size();
        bool       changed_visibility = m_filament_combo->IsShown() != color_stop;
        m_filament_combo->Show(color_stop);
        if (color_stop) {
            const unsigned int selected_id = ids[m_selected / 2];
            for (size_t i = 0; i < m_filament_names.size(); ++i) {
                wxString text = m_filament_names[i];
                if (i + 1 != selected_id && std::find(ids.begin(), ids.end(), i + 1) != ids.end())
                    text += " (" + _L("Switch") + ")";
                m_filament_combo->SetString(i, text);
            }
            m_filament_combo->SetSelection(int(selected_id) - 1);
            m_filament_combo->SetIcon(create_scaled_bitmap("drop_down", m_filament_combo, 16));
        }
        double position = stops.empty() ? 0.0 : stops[m_selected];
        if (m_selected % 2 != 0) {
            const double range = stops[m_selected + 1] - stops[m_selected - 1];
            position           = range > 1e-6 ? (position - stops[m_selected - 1]) / range : 0.5;
        }
        m_position_label->SetLabel(m_selected % 2 != 0 ? _L("Position (relative):") : _L("Position:"));
        m_position->ChangeValue(wxString::Format("%.1f", position * 100.0));
        m_position_box->Enable(m_selected > 0 && m_selected + 1 < stops.size());
        const bool interior           = m_selected > 0 && m_selected + 1 < stops.size() && m_selected % 2 == 0;
        changed_visibility            = changed_visibility || m_width_panel->IsShown() != interior;
        m_width_panel->Show(interior);
        if (interior) {
            const auto widths = mixed_gradient_solid_half_widths(m_recipe(), m_context.num_physical,
                                                                 m_context.preview_settings.gradient_middle_window);
            m_width->ChangeValue(wxString::Format("%.1f", 200.0 * widths[m_selected / 2]));
        }
        Layout();
        if (changed_visibility && GetParent()) {
            GetParent()->Layout();
            if (GetParent()->GetParent())
                GetParent()->GetParent()->Layout();
        }
    }
    void set_width(double width)
    {
        auto       entry = m_recipe();
        const auto stops = mixed_gradient_stops(entry, m_context.num_physical);
        if (m_selected == 0 || m_selected + 1 >= stops.size() || m_selected % 2 != 0)
            return;
        auto widths = mixed_gradient_solid_half_widths(entry, m_context.num_physical, m_context.preview_settings.gradient_middle_window);
        for (float& value : widths)
            value *= 2.f;
        const double maximum   = 2.0 * std::min(stops[m_selected] - stops[m_selected - 1], stops[m_selected + 1] - stops[m_selected]);
        widths[m_selected / 2] = float(std::clamp(width, 0.0, maximum));
        m_widths_changed(widths);
        sync_position();
        m_canvas->Refresh();
    }
    void set_gap(int pct)
    {
        pct = std::clamp(pct, 0, m_gap_slider->GetMax());
        m_gap_slider->SetValue(pct);
        m_gap->ChangeValue(wxString::Format("%d", pct));
        auto stops = mixed_gradient_stops(m_recipe(), m_context.num_physical);
        if (stops.size() > 1) {
            const float gap = pct / 100.f;
            stops.front()   = 0.f;
            for (size_t i = 1; i < stops.size(); ++i)
                stops[i] = std::max(stops[i], stops[i - 1] + gap);
            stops.back() = 1.f;
            for (size_t i = stops.size() - 1; i > 0; --i)
                stops[i - 1] = std::min(stops[i - 1], stops[i] - gap);
            stops.front() = 0.f;
            m_changed(stops);
        }
        sync_position();
        m_canvas->Refresh();
    }
    double progress(int x) const
    {
        return std::clamp(double(x - FromDIP(12)) / std::max(1, m_canvas->GetClientSize().x - FromDIP(24)), 0.0, 1.0);
    }
    void move_stop(double t)
    {
        auto stops = mixed_gradient_stops(m_recipe(), m_context.num_physical);
        if (m_selected == 0 || m_selected + 1 >= stops.size())
            return;
        const float gap = float(m_gap_slider->GetValue() / 100.0);
        const float lo = stops[m_selected - 1] + gap, hi = stops[m_selected + 1] - gap;
        if (lo > hi)
            return;
        stops[m_selected] = std::clamp(float(t), lo, hi);
        m_changed(stops);
        sync_position();
        m_canvas->Refresh();
    }
    void paint()
    {
        wxAutoBufferedPaintDC dc(m_canvas);
        dc.SetBackground(wxBrush(GetBackgroundColour()));
        dc.Clear();
        const auto                         entry = m_recipe();
        const auto                         stops = mixed_gradient_stops(entry, m_context.num_physical);
        const auto                         ids   = mixed_gradient_components(entry, m_context.num_physical);
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc || stops.empty())
            return;
        gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
        // Geometry and handle styling from FS's MFDGradientAccordion.
        const double margin = FromDIP(12), bw = FromDIP(2);
        const double width    = std::max(1.0, m_canvas->GetClientSize().x - 2.0 * margin);
        const double height   = std::max(1.0, m_canvas->GetClientSize().y - 2.0 * margin);
        const double cy       = margin + height / 2.0;
        const int    segments = std::max(1, int(width));
        wxColour     previous(mixed_gradient_display_color(entry, m_context, 0.0));
        gc->SetPen(*wxTRANSPARENT_PEN);
        for (int x = 0; x < segments; ++x) {
            const double   left  = margin + width * x / segments;
            const double   right = margin + width * (x + 1) / segments;
            const wxColour next(mixed_gradient_display_color(entry, m_context, double(x + 1) / segments));
            gc->SetBrush(gc->CreateLinearGradientBrush(left, margin, right, margin, previous, next));
            gc->DrawRectangle(left, margin, right - left + 0.5, height);
            previous = next;
        }
        gc->SetPen(wxPen(MFDTheme::input_border(), bw));
        gc->SetBrush(*wxTRANSPARENT_BRUSH);
        gc->DrawRectangle(margin, margin, width, height);
        gc->SetPen(wxPen(*wxWHITE, FromDIP(1.5)));
        gc->StrokeLine(margin, cy, margin + width, cy);

        if (m_dragging && m_selected > 0 && m_selected + 1 < stops.size()) {
            const double gap       = m_gap_slider->GetValue() / 100.0;
            wxDash       dashes[2] = {4, 4};
            wxPen        pen(*wxWHITE, FromDIP(1.5), wxPENSTYLE_USER_DASH);
            pen.SetDashes(2, dashes);
            gc->SetPen(pen);
            for (const double limit : {stops[m_selected - 1] + gap, stops[m_selected + 1] - gap}) {
                const double x = margin + limit * width;
                gc->StrokeLine(x, margin, x, margin + height);
            }
        }
        const auto solid_widths = mixed_gradient_solid_half_widths(entry, m_context.num_physical,
                                                                   m_context.preview_settings.gradient_middle_window);
        for (size_t i = 1; i + 1 < solid_widths.size(); ++i) {
            const bool selected = m_selected == 2 * i;
            gc->SetPen(wxPen(*wxWHITE, FromDIP(selected ? 2 : 1)));
            gc->SetBrush(wxBrush(MFDTheme::card_background()));
            const double bottom      = margin + height;
            const double half_handle = FromDIP(3);
            for (int side : {-1, 1}) {
                const double x = margin + (stops[2 * i] + side * solid_widths[i]) * width;
                gc->StrokeLine(x, margin, x, bottom);
                gc->DrawRectangle(x - half_handle, bottom, 2 * half_handle, 2 * half_handle);
            }
        }
        for (size_t i = 0; i < stops.size(); ++i) {
            const double x       = margin + stops[i] * width;
            const bool   grabbed = m_dragging && i == m_selected;
            if (i % 2 == 0) {
                const unsigned id     = ids[i / 2];
                const wxColour color  = id > 0 && id <= m_context.physical_colors.size() ? wxColour(m_context.physical_colors[id - 1]) :
                                                                                           wxColour(128, 128, 128);
                const wxColour border = color.GetLuminance() > 0.5 ? wxColour(100, 100, 100) : *wxWHITE;
                const double   radius = grabbed ? FromDIP(10) : FromDIP(7);
                gc->SetPen(wxPen(border, i == m_selected ? bw + FromDIP(2.5) : bw));
                gc->SetBrush(wxBrush(color));
                gc->DrawEllipse(x - radius, cy - radius, radius * 2.0, radius * 2.0);
            } else {
                const double tick_width  = grabbed ? FromDIP(5.5) : FromDIP(3.0);
                const double tick_height = i == m_selected ? FromDIP(9.0) : FromDIP(6.0);
                gc->SetPen(wxPen(*wxWHITE, tick_width));
                gc->StrokeLine(x, cy - tick_height, x, cy + tick_height);
            }
        }
    }

    Recipe                                         m_recipe;
    MixedFilamentDisplayContext                    m_context;
    std::function<void(const std::vector<float>&)> m_changed;
    std::function<void(const std::vector<float>&)> m_widths_changed;
    std::vector<wxString>                          m_filament_names;
    std::function<void(size_t, int)>               m_filament_changed;
    ComboBox*                                      m_filament_combo{nullptr};
    TextInput*                                     m_position_box{nullptr};
    wxPanel*                                       m_width_panel{nullptr};
    wxTextCtrl*                                    m_width{nullptr};
    int                                            m_width_edge{0};
    wxPanel*                                       m_canvas{nullptr};
    wxStaticText*                                  m_position_label{nullptr};
    wxTextCtrl *                                   m_position{nullptr}, *m_gap{nullptr};
    MixedGradientGapSlider*                        m_gap_slider{nullptr};
    size_t                                         m_selected{0};
    bool                                           m_dragging{false};
};
} // namespace Slic3r::GUI
