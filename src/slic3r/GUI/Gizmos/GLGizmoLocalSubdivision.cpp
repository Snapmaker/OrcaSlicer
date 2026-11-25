#include "GLGizmoLocalSubdivision.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMeshDeal.hpp"

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

#include <GL/glew.h>

namespace Slic3r::GUI {

GLGizmoLocalSubdivision::GLGizmoLocalSubdivision(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoPainterBase(parent, icon_filename, sprite_id), m_current_tool(ImGui::CircleButtonIcon)
{
    m_tool_type   = ToolType::BRUSH;
    m_cursor_type = TriangleSelector::CursorType::CIRCLE;
}

std::string GLGizmoLocalSubdivision::on_get_name() const
{
    return _u8L("Local Subdivision");
}

bool GLGizmoLocalSubdivision::on_init()
{
    const wxString ctrl = _L("Ctrl+");
    const wxString alt  = _L("Alt+");

    m_desc["clipping_of_view_caption"] = alt + _L("Mouse wheel");
    m_desc["clipping_of_view"]         = _L("Section view");
    m_desc["reset_direction"]          = _L("Reset direction");
    m_desc["cursor_size_caption"]      = ctrl + _L("Mouse wheel");
    m_desc["cursor_size"]              = _L("Brush size");
    m_desc["height_range_caption"]     = ctrl + _L("Mouse wheel");
    m_desc["height_range"]             = _L("Height range");
    m_desc["smart_fill_angle_caption"] = ctrl + _L("Mouse wheel");
    m_desc["smart_fill_angle"]         = _L("Smart fill angle");
    m_desc["edge_detection"]           = _L("Detect geometry edge");
    m_desc["enforce_caption"]          = _L("Left mouse button");
    m_desc["enforce"]                  = _L("Select facets");
    m_desc["remove_caption"]           = _L("Right mouse button");
    m_desc["remove"]                   = _L("Unselect facets");
    m_desc["remove_all"]               = _L("Erase all selection");
    m_desc["tool_type"]                = _L("Tool type");
    m_desc["subdiv_level"]             = _L("Subdivision level");
    m_desc["subdivide"]                = _L("Subdivide");
    m_desc["close"]                    = _L("Close");
    m_desc["selected_facets"]          = _L("Selected facets");
    return true;
}

bool GLGizmoLocalSubdivision::gizmo_event(SLAGizmoEventType action, const Vec2d& mouse_position, bool shift_down, bool alt_down, bool control_down)
{
    if ((action == SLAGizmoEventType::LeftUp || action == SLAGizmoEventType::RightUp)
      && m_button_down != Button::None) {
        // A paint stroke only changes the transient selection, not the model, so the
        // GizmoAction snapshot the base FinalizePaintingStroke takes here would be an
        // empty undo step. Intercepting the stroke end keeps one Ctrl+Z after the
        // subdivision a direct jump back to the original mesh instead of stepping
        // through the gizmo session. The stroke state must still be ended exactly
        // like the base class does it: a stale m_last_mouse_click would make the next
        // click interpolate a line of paint from the previous click position.
        if (m_finishingPaintingInteraction)
            return true;
        m_finishingPaintingInteraction = true;
        DiscardPaintingPreviewsAndFlush();
        update_model_object();
        m_button_down             = Button::None;
        m_strokeHasCommittedState = false;
        m_last_mouse_click        = Vec2d::Zero();
        _heightRangeLastBottomZ.reset();
        m_finishingPaintingInteraction = false;
        // Finalizing cleared the hover preview; rebuild it here since macOS/Linux send no move after mouse-up.
        GLGizmoPainterBase::gizmo_event(SLAGizmoEventType::Moving, mouse_position, shift_down, alt_down, control_down);
        return true;
    }
    return GLGizmoPainterBase::gizmo_event(action, mouse_position, shift_down, alt_down, control_down);
}

void GLGizmoLocalSubdivision::render_painter_gizmo()
{
    const Selection& selection = m_parent.get_selection();

    glsafe(::glEnable(GL_BLEND));
    glsafe(::glEnable(GL_DEPTH_TEST));

    // Parts replaced by a preview mesh skip their selector draw inside
    // render_triangles (render_selector_for_volume); every other part - unpainted
    // siblings, previews that could not reach level 1 - keeps its original draw.
    render_triangles(selection);
    m_c->object_clipper()->render_cut();
    m_c->instances_hider()->render_cut();

    render_preview_meshes();
    render_cursor();

    glsafe(::glDisable(GL_BLEND));
}

void GLGizmoLocalSubdivision::update_from_model_object(bool first_update)
{
    restore_preview();
    m_previews.clear();
    m_preview_dirty = true;

    wxBusyCursor wait;
    const ModelObject* mo = m_c->selection_info()->model_object();
    // Model data replaces these selectors, so drop the active stroke instead of writing it back.
    ResetPaintingStroke();
    DetachTriangleSelectorGlResources();
    m_triangle_selectors.clear();
    std::vector<ColorRGBA> ebt_colors;
    ebt_colors.push_back(GLVolume::NEUTRAL_COLOR);
    ebt_colors.push_back(TriangleSelectorGUI::enforcers_color);
    ebt_colors.push_back(TriangleSelectorGUI::blockers_color);
    for (const ModelVolume* mv : mo->volumes) {
        if (! mv->is_model_part())
            continue;
        const TriangleMesh* mesh = &mv->mesh();
        m_triangle_selectors.emplace_back(std::make_unique<TriangleSelectorPatch>(*mesh, ebt_colors));
        m_triangle_selectors.back()->request_update_render_data();
    }
}

PainterGizmoType GLGizmoLocalSubdivision::get_painter_type() const
{
    return PainterGizmoType::LOCAL_SUBDIVISION;
}

wxString GLGizmoLocalSubdivision::handle_snapshot_action_name(bool shift_down, Button button_down) const
{
    return shift_down || button_down == Button::Right ? _L("Remove selection") : _L("Facet selection");
}

void GLGizmoLocalSubdivision::on_render_input_window(float x, float y, float bottom_limit)
{
    wchar_t old_tool = m_current_tool;
    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    GizmoImguiBegin(get_name(), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    const float space_size             = m_imgui->get_style_scaling() * 8;
    const float cursor_slider_left     = m_imgui->calc_text_size(m_desc.at("cursor_size")).x + m_imgui->scaled(1.5f);
    const float height_slider_left     = m_imgui->calc_text_size(m_desc.at("height_range")).x + m_imgui->scaled(1.5f);
    const float smart_fill_slider_left = m_imgui->calc_text_size(m_desc.at("smart_fill_angle")).x + m_imgui->scaled(1.5f);
    const float subdiv_level_left      = m_imgui->calc_text_size(m_desc.at("subdiv_level")).x + m_imgui->scaled(1.5f);
    const float empty_button_width     = m_imgui->calc_button_size("").x;
    const float sliders_left_width     = std::max(subdiv_level_left, std::max(smart_fill_slider_left, std::max(cursor_slider_left, height_slider_left)));
    const float slider_icon_width      = m_imgui->get_slider_icon_size().x;
    const float max_tooltip_width      = ImGui::GetFontSize() * 20.0f;
    const float sliders_width          = m_imgui->scaled(7.0f);
    const float drag_left_width        = ImGui::GetStyle().WindowPadding.x + sliders_left_width + sliders_width - space_size;

    // Tool buttons: circle brush, single triangle picker, height range brush, fill
    // (whole connected area, or limited by the smart fill angle when edge detection is on).
    ImGui::AlignTextToFramePadding();
    m_imgui->text(m_desc.at("tool_type"));
    std::array<wchar_t, 4> tool_ids = { ImGui::CircleButtonIcon, ImGui::TriangleButtonIcon, ImGui::HeightRangeIcon, ImGui::FillButtonIcon };
    std::array<wchar_t, 4> icons;
    if (m_is_dark_mode)
        icons = { ImGui::CircleButtonDarkIcon, ImGui::TriangleButtonDarkIcon, ImGui::HeightRangeDarkIcon, ImGui::FillButtonDarkIcon };
    else
        icons = { ImGui::CircleButtonIcon, ImGui::TriangleButtonIcon, ImGui::HeightRangeIcon, ImGui::FillButtonIcon };
    std::array<wxString, 4> tool_tips = { _L("Circle"), _L("Triangle"), _L("Height Range"), _L("Fill") };
    for (int i = 0; i < tool_ids.size(); i++) {
        std::string  str_label = std::string("##");
        std::wstring btn_name  = icons[i] + boost::nowide::widen(str_label);

        if (i != 0) ImGui::SameLine((empty_button_width + m_imgui->scaled(1.75f)) * i + m_imgui->scaled(1.3f));

        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.f, 0.f, 0.f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f));
        if (m_current_tool == tool_ids[i]) {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.f, 0.59f, 0.53f, 0.25f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.f, 0.59f, 0.53f, 0.25f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.f, 0.59f, 0.53f, 0.30f));
            ImGui::PushStyleColor(ImGuiCol_Border,        ImGuiWrapper::COL_ORCA);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 1.0);
        }
        bool btn_clicked = ImGui::Button(into_u8(btn_name).c_str());
        if (m_current_tool == tool_ids[i]) {
            ImGui::PopStyleColor(4);
            ImGui::PopStyleVar(2);
        }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(1);

        if (btn_clicked && m_current_tool != tool_ids[i]) {
            m_current_tool = tool_ids[i];
            for (auto& triangle_selector : m_triangle_selectors) {
                triangle_selector->seed_fill_unselect_all_triangles();
                triangle_selector->request_update_render_data();
            }
        }

        if (ImGui::IsItemHovered())
            m_imgui->tooltip(tool_tips[i], max_tooltip_width);
    }
    (void) old_tool;

    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.1));

    // The parameter area under the tool buttons differs in height per tool: the
    // triangle picker has none, the brushes have one slider row, the fill has a
    // checkbox plus a slider row. The current layout is padded up to the
    // tallest layout drawn so far this session (m_param_area_max_height), so
    // switching tools never changes the panel height. The analytic two-row
    // estimate below only seeds the maximum.
    //
    // The estimate follows the widget math: BBLCheckbox and BBLSliderScalarIn
    // size their row as label height + 2x FramePadding (the drag input uses
    // GetFrameHeight), and the measurement below starts one ItemSpacing under
    // the dummy above, so that leading gap is counted for each row of the
    // two-row fill layout.
    const ImGuiStyle& style             = ImGui::GetStyle();
    const float       widget_row_height = std::max(ImGui::GetFrameHeight(), ImGui::GetTextLineHeight() + style.FramePadding.y * 2.f);
    const float       param_area_min_height = widget_row_height * 2.f + style.ItemSpacing.y * 2.f;
    const float       param_area_top        = ImGui::GetCursorPosY();

    // The brush and height-range tools have a single slider row; push it down
    // so it sits in the middle of the reserved two-row area (the other half of
    // the leftover is padded below after the measurement, keeping the row
    // centered between the tool buttons and the separator).
    const float inner_top_pad =
        (m_current_tool == ImGui::CircleButtonIcon || m_current_tool == ImGui::HeightRangeIcon) ?
            std::max(0.f, (widget_row_height - style.ItemSpacing.y) * 0.5f) :
            0.f;
    if (inner_top_pad > 0.f)
        ImGui::Dummy(ImVec2(0.f, inner_top_pad));

    if (m_current_tool == ImGui::CircleButtonIcon) {
        m_cursor_type = TriangleSelector::CursorType::CIRCLE;
        m_tool_type   = ToolType::BRUSH;

        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc.at("cursor_size"));
        ImGui::SameLine(sliders_left_width);
        ImGui::PushItemWidth(sliders_width);
        m_imgui->bbl_slider_float_style("##cursor_radius", &m_cursor_radius, get_cursor_radius_min(), get_cursor_radius_max(), "%.2f", 1.0f, true);
        ImGui::SameLine(drag_left_width);
        ImGui::PushItemWidth(1.5 * slider_icon_width);
        ImGui::BBLDragFloat("##cursor_radius_input", &m_cursor_radius, 0.05f, 0.0f, 0.0f, "%.2f");
    } else if (m_current_tool == ImGui::TriangleButtonIcon) {
        // Single-triangle picker: the base class paints exactly the facet under the cursor.
        m_cursor_type = TriangleSelector::CursorType::POINTER;
        m_tool_type   = ToolType::BRUSH;
    } else if (m_current_tool == ImGui::HeightRangeIcon) {
        m_tool_type   = ToolType::BRUSH;
        m_cursor_type = TriangleSelector::CursorType::HEIGHT_RANGE;

        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc["height_range"] + ":");
        ImGui::SameLine(sliders_left_width);
        ImGui::PushItemWidth(sliders_width);
        m_imgui->bbl_slider_float_style("##cursor_height", &m_cursor_height, get_cursor_height_min(), get_cursor_height_max(), "%.2f", 1.0f, true);
        ImGui::SameLine(drag_left_width);
        ImGui::PushItemWidth(1.5 * slider_icon_width);
        ImGui::BBLDragFloat("##cursor_height_input", &m_cursor_height, 0.05f, 0.0f, 0.0f, "%.2f");
    } else if (m_current_tool == ImGui::FillButtonIcon) {
        m_cursor_type = TriangleSelector::CursorType::POINTER;
        m_tool_type   = ToolType::BUCKET_FILL;

        m_imgui->bbl_checkbox(m_desc["edge_detection"], m_detect_geometry_edge);
        if (m_detect_geometry_edge) {
            // Turning the edge detection back on restores the default angle after it was
            // set negative to fill the whole connected area.
            if (m_smart_fill_angle < 0.f)
                m_smart_fill_angle = 30.f;
            ImGui::AlignTextToFramePadding();
            m_imgui->text(m_desc["smart_fill_angle"]);
            std::string format_str = std::string("%.f") + I18N::translate_utf8("°", "Face angle threshold,"
                                                                                "placed after the number with no whitespace in between.");
            ImGui::SameLine(sliders_left_width);
            ImGui::PushItemWidth(sliders_width);
            if (m_imgui->bbl_slider_float_style("##smart_fill_angle", &m_smart_fill_angle, SmartFillAngleMin, SmartFillAngleMax, format_str.data(), 1.0f, true)) {
                for (auto& triangle_selector : m_triangle_selectors) {
                    triangle_selector->seed_fill_unselect_all_triangles();
                    triangle_selector->request_update_render_data();
                }
            }
            ImGui::SameLine(drag_left_width);
            ImGui::PushItemWidth(1.5 * slider_icon_width);
            ImGui::BBLDragFloat("##smart_fill_angle_input", &m_smart_fill_angle, 0.05f, 0.0f, 0.0f, "%.2f");
        } else {
            // Negative angle disables the edge detection, the fill covers the whole
            // connected area regardless of the face angles.
            m_smart_fill_angle = -1.f;
        }
    }

    // The remaining unused height goes above the separator, so the bottom
    // cluster (counters, subdivision level stepper, action row) keeps its
    // position: the subdivision level stays right above the Subdivide button
    // while the panel height remains fixed across tools.
    const float param_area_height = ImGui::GetCursorPosY() - param_area_top;
    // Self-calibrating fixed height: pad the current layout up to the tallest
    // layout drawn so far this session (the two-row analytic estimate below
    // acts as a floor). Monotonic by construction, so switching tools never
    // shrinks the panel.
    m_param_area_max_height = std::max(m_param_area_max_height, param_area_min_height);
    m_param_area_max_height = std::max(m_param_area_max_height, param_area_height);
    // The padding Dummy occupies one ItemSpacing of its own on top of its
    // height, so shorten it by that amount to keep the total extent of the
    // section equal to the measured maximum.
    const float pad_total = std::max(0.f, m_param_area_max_height - param_area_height - ImGui::GetStyle().ItemSpacing.y);
    if (pad_total > 0.f)
        ImGui::Dummy(ImVec2(0.f, pad_total));

    ImGui::Separator();

    // A little breathing room around the counters and the subdivision level
    // stepper so they do not hug the separator and each other.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(ImGui::GetStyle().ItemSpacing.x, ImGui::GetStyle().ItemSpacing.y + ImGui::GetFontSize() * 0.5f));

    // How many facets are currently painted. Count the original (level-0) faces, not
    // the leaves of the selector tree: the selector subdivides triangles while painting,
    // so one painted face can hold many selected leaf triangles. The selectors are
    // rewalked only while the preview cache is dirty (a paint stroke just ended);
    // idle frames reuse the cached total instead of walking and allocating per frame.
    if (m_preview_dirty) {
        m_selected_facet_count = 0;
        for (const auto& triangle_selector : m_triangle_selectors)
            m_selected_facet_count += int(triangle_selector->get_facets_indices_by_state(EnforcerBlockerType::ENFORCER).size());
    }
    m_imgui->text(format_wxstr("%1%: %2%", m_desc["selected_facets"], m_selected_facet_count));

    // Subdivision level stepper with live preview: [-] value [+]. Each change swaps
    // the rendered mesh of the painted region for the subdivided one.
    ImGui::AlignTextToFramePadding();
    m_imgui->text(m_desc["subdiv_level"]);
    {
        const ImVec4 white(1.f, 1.f, 1.f, 1.f);
        const ImVec4 black(0.f, 0.f, 0.f, 1.f);
        const ImVec4 grey_hover(0.9f, 0.9f, 0.9f, 1.f);
        const ImVec4 grey_active(0.8f, 0.8f, 0.8f, 1.f);
        const float  btn_w = m_imgui->scaled(1.7f);
        const float  box_w = m_imgui->scaled(2.0f);

        ImGui::PushStyleColor(ImGuiCol_Button, white);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, grey_hover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, grey_active);
        ImGui::PushStyleColor(ImGuiCol_Text, black);

        ImGui::SameLine(sliders_left_width);
        if (ImGui::Button("-##subdiv_minus", ImVec2(btn_w, 0.f)) && m_subdiv_level > 0)
            --m_subdiv_level;

        ImGui::SameLine(0.f, 0.f);
        char value_label[16] = {0};
        ::snprintf(value_label, sizeof(value_label), "%d", m_subdiv_level);
        // A button renders its label centered (InputText is always left aligned);
        // its colors are overridden so it looks like a plain white value box.
        ImGui::PushStyleColor(ImGuiCol_Button, white);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, white);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, white);
        ImGui::Button(value_label, ImVec2(box_w, 0.f));
        ImGui::PopStyleColor(3);

        ImGui::SameLine(0.f, 0.f);
        if (ImGui::Button("+##subdiv_plus", ImVec2(btn_w, 0.f)) && m_subdiv_level < MaxSubdivLevel) {
            const int next_level = m_subdiv_level + 1;
            // The preview builds every level eagerly on the UI thread and keeps the
            // intermediate meshes, so a step that projects past the face budget can
            // freeze the editor long before the apply path runs. Ask here, at the
            // step — but only when this step still has work to do: stepping back
            // down and up again reuses the cached meshes (and a cached failure
            // never recomputes), so the warning must not repeat either. Mirrors
            // update_preview's any-growth-pending predicate.
            bool needs_compute = m_preview_dirty;
            for (const Preview& p : m_previews)
                if (int(p.meshes.size()) <= next_level && ! p.next_facets.empty() && p.failed_level != int(p.meshes.size()))
                    needs_compute = true;
            bool over_budget = false;
            const ModelObject* mo = m_c->selection_info()->model_object();
            if (needs_compute && m_selected_facet_count > 0) {
                size_t growth = 1;
                for (int i = 0; i <= m_subdiv_level; ++i)
                    growth *= 4;
                size_t future_face_count = size_t(m_selected_facet_count) * (growth - 1);
                for (const ModelVolume* mv : mo->volumes)
                    if (mv->is_model_part())
                        future_face_count += mv->mesh().its.indices.size();
                over_budget = future_face_count > 1000000;
            }
            if (over_budget) {
                auto name_str = wxString::FromUTF8(mo->name);
                auto content  = wxString::Format(
                    _L("\"%s\" will exceed 1 million faces after this subdivision, which may increase slicing time. Do you want to continue?"), name_str);
                WarningDialog dlg(static_cast<wxWindow*>(wxGetApp().mainframe), content, wxEmptyString, wxYES_NO);
                if (dlg.ShowModal() != wxID_NO)
                    ++m_subdiv_level;
            } else
                ++m_subdiv_level;
        }

        ImGui::PopStyleColor(4); // Button, ButtonHovered, ButtonActive, Text
    }
    ImGui::PopStyleVar();

    update_preview();

    // One action row at the very bottom: the primary "Subdivide" on the far
    // left, stretched over the width left by the two fixed-size buttons, then
    // "Close" and "Erase all selection".
    const float close_button_width = m_imgui->calc_button_size(m_desc["close"]).x;
    const float clear_button_width = m_imgui->calc_button_size(m_desc["remove_all"]).x;
    // Comfortable minimum width for the primary button when another row of the
    // panel defines the window width.
    const float subdiv_min_width = m_imgui->calc_text_size(m_desc["subdivide"]).x + ImGui::GetStyle().FramePadding.x * 6.f;
    const float subdiv_button_width =
        std::max(subdiv_min_width,
                 ImGui::GetContentRegionAvail().x - close_button_width - clear_button_width - 2.f * ImGui::GetStyle().ItemSpacing.x);

    m_imgui->disabled_begin(m_selected_facet_count == 0 || m_subdiv_level == 0);
    m_imgui->push_confirm_button_style();
    if (m_imgui->button(m_desc["subdivide"], subdiv_button_width, 0.f))
        apply_subdivision();
    m_imgui->pop_confirm_button_style();
    m_imgui->disabled_end();

    ImGui::SameLine();
    if (m_imgui->bbl_button(m_desc["close"]))
        close();

    ImGui::SameLine();
    if (m_imgui->button(m_desc.at("remove_all"))) {
        // Like paint strokes, clearing the selection changes nothing on the model,
        // so no undo snapshot is taken for it.
        ModelObject*         mo = m_c->selection_info()->model_object();
        int                  idx = -1;
        for (ModelVolume* mv : mo->volumes)
            if (mv->is_model_part()) {
                ++idx;
                m_triangle_selectors[idx]->reset();
                m_triangle_selectors[idx]->request_update_render_data(true);
            }
        m_preview_dirty = true;
        m_parent.set_as_dirty();
    }

    GizmoImguiEnd();

    ImGuiWrapper::pop_toolbar_style();
}

void GLGizmoLocalSubdivision::restore_preview()
{
    // Give the volumes we hid their rendering back. Only touch the volumes we hid
    // ourselves; the painter base hides other instances while the gizmo is open.
    if (m_shown_level > 0) {
        ModelObject* mo       = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
        const int    instance = m_c->selection_info() ? m_c->selection_info()->get_active_instance() : -1;
        if (mo != nullptr && instance >= 0) {
            for (Preview& p : m_previews)
                if (p.hidden)
                    m_parent.toggle_model_objects_visibility(true, mo, instance, p.volume);
        } else {
            m_parent.toggle_model_objects_visibility(true);
        }
        m_shown_level = 0;
    }
    for (Preview& p : m_previews) {
        p.model.reset();
        p.model_region.reset();
        p.hidden = false;
    }
}

void GLGizmoLocalSubdivision::update_preview()
{
    // A preview whose next level failed carries a cached failure record, so no
    // growth is pending for it. Without this, an unchanged failing selection
    // (e.g. non-manifold edges) retried the same computation on every frame and
    // re-requested redraws, keeping the UI thread busy.
    bool any_growth_pending = false;
    for (const Preview& p : m_previews)
        if (int(p.meshes.size()) <= m_subdiv_level && ! p.next_facets.empty() && p.failed_level != int(p.meshes.size()))
            any_growth_pending = true;
    if (! m_preview_dirty && ! any_growth_pending && m_shown_level == m_subdiv_level)
        return;

    // Set when this frame changed what is shown on screen; a failed selection that
    // cannot be recomputed (cached failure above) must not keep re-requesting
    // redraws, or the panel never goes idle.
    bool changed = false;

    ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (mo == nullptr)
        return;

    if (m_preview_dirty) {
        // Rebuild the caches from the current painting state.
        changed = true;
        restore_preview();
        m_previews.clear();
        // The selectors are built by update_from_model_object; without them there is
        // nothing to preview yet.
        if (! m_triangle_selectors.empty()) {
            int idx = -1; // counts model parts only, indexing m_triangle_selectors
            for (int full_idx = 0; full_idx < int(mo->volumes.size()); ++full_idx) {
                ModelVolume* mv = mo->volumes[size_t(full_idx)];
                if (! mv->is_model_part())
                    continue;
                ++idx;
                if (size_t(idx) >= m_triangle_selectors.size())
                    break;
                std::vector<int> facets = m_triangle_selectors[idx]->get_facets_indices_by_state(EnforcerBlockerType::ENFORCER);
                if (facets.empty())
                    continue;
                Preview p;
                p.object_id     = mo->id();
                p.volume        = mv;
                p.gl_volume_idx = full_idx;
                p.origin        = mv->mesh_ptr();
                p.facet_count = facets.size();
                p.next_facets.assign(facets.begin(), facets.end());
                p.meshes.emplace_back(mv->mesh().its); // level 0 = the original mesh
                p.region_counts.push_back(facets.size());
                m_previews.emplace_back(std::move(p));
            }
        }
        m_preview_dirty = false;
    }

    // Grow the caches level by level. The region faces of one iteration occupy the
    // front of the result face list, so they are the selection of the next iteration.
    for (Preview& p : m_previews) {
        while (int(p.meshes.size()) <= m_subdiv_level && ! p.next_facets.empty()) {
            const int next_level = int(p.meshes.size());
            if (p.failed_level == next_level)
                break; // cached failure for the current inputs: do not recompute
            const indexed_triangle_set& src        = p.meshes.back();
            const size_t                unselected = src.indices.size() - p.next_facets.size();
            bool                        ok         = false;
            TriangleMeshDeal::SubdivisionFailure failure = TriangleMeshDeal::SubdivisionFailure::None;
            indexed_triangle_set        next       = TriangleMeshDeal::smooth_triangle_mesh_local_its(src, p.next_facets, ok, &failure);
            if (! ok) {
                p.failed_level = next_level;
                break;
            }
            const size_t region = next.indices.size() - unselected;
            p.next_facets.clear();
            p.next_facets.resize(region);
            for (size_t i = 0; i < region; ++i)
                p.next_facets[i] = uint32_t(i);
            p.meshes.emplace_back(std::move(next));
            p.region_counts.push_back(region);
            changed = true;
        }
    }

    // A region that cannot be subdivided stops growing at the level where it failed
    // (non-manifold pairing guard or the child-face validation): say so instead of
    // silently showing a lower level than the slider requests. Throttled to one
    // notification per (requested, achieved) level pair, so repainting does not
    // re-notify on every mouse move.
    int lowest_achieved = m_subdiv_level;
    for (const Preview& p : m_previews)
        lowest_achieved = std::min(lowest_achieved, int(p.meshes.size()) - 1);
    if (m_subdiv_level > 0 && ! m_previews.empty() && lowest_achieved < m_subdiv_level) {
        if (m_last_failed_levels != std::make_pair(m_subdiv_level, lowest_achieved)) {
            m_last_failed_levels = {m_subdiv_level, lowest_achieved};
            wxGetApp().plater()->get_notification_manager()->push_notification(
                NotificationManager::NotificationLevel::WarningNotificationLevel, 5,
                subdivision_failure_text(lowest_achieved, false).ToStdString());
        }
    } else {
        m_last_failed_levels = {0, 0};
    }

    if (m_subdiv_level > 0 && ! m_previews.empty()) {
        // Hide the original volumes and draw the subdivided level ourselves: the
        // surroundings with the raw volume color (same color the level-0 rendering
        // uses through the transparent pass, alpha included), the region with the
        // painting color.
        const Selection& selection      = m_parent.get_selection();
        const int        instance_idx   = m_c->selection_info()->get_active_instance();
        bool             anything_shown = false;
        for (Preview& p : m_previews) {
            const int level = std::min(m_subdiv_level, int(p.meshes.size()) - 1);
            if (level <= 0)
                continue;
            const GLVolume* glv = nullptr;
            for (const auto& i : selection.get_volume_idxs()) {
                const GLVolume* v = selection.get_volume(i);
                if (v->volume_idx() == p.gl_volume_idx) {
                    glv = v;
                    break;
                }
            }
            if (glv == nullptr)
                continue;

            const size_t         region    = p.region_counts[size_t(level)];
            const auto&          level_its = p.meshes[size_t(level)];

            // Region geometry, rendered by the gizmo with the level-0 painting color.
            indexed_triangle_set region_its;
            region_its.vertices = level_its.vertices;
            region_its.indices.assign(level_its.indices.begin(), level_its.indices.begin() + region);
            p.model_region.reset();
            p.model_region.init_from(region_its);
            p.model_region.set_color(TriangleSelectorGUI::enforcers_color);
            if (! p.model_region.is_initialized())
                continue;

            // The rest is drawn exactly once (a second draw would stack the
            // translucency) in the same neutral gray the model shows at level 0
            // while the painting gizmo is open.
            p.model.reset();
            if (region < level_its.indices.size()) {
                indexed_triangle_set rest_its;
                rest_its.vertices = level_its.vertices;
                rest_its.indices.assign(level_its.indices.begin() + region, level_its.indices.end());
                p.model.init_from(rest_its);
                p.model.set_color(GLVolume::NEUTRAL_COLOR);
                if (! p.model.is_initialized())
                    continue;
            }

            m_parent.toggle_model_objects_visibility(false, mo, instance_idx, p.volume);
            p.hidden        = true;
            anything_shown  = true;
        }
        const int new_shown_level = anything_shown ? m_subdiv_level : 0;
        if (new_shown_level != m_shown_level)
            changed = true;
        m_shown_level = new_shown_level;
    } else {
        if (m_shown_level != 0)
            changed = true;
        restore_preview();
    }
    if (changed)
        m_parent.set_as_dirty();
}

bool GLGizmoLocalSubdivision::render_selector_for_volume(const ModelVolume* mv) const
{
    if (m_shown_level == 0)
        return true;
    const Preview* p = nullptr;
    for (const Preview& cand : m_previews)
        if (cand.volume == mv) {
            p = &cand;
            break;
        }
    if (p == nullptr)
        return true; // not painted: the selector is this volume's only draw
    const int level = std::min(m_subdiv_level, int(p->meshes.size()) - 1);
    if (level <= 0)
        return true; // preview stuck at level 0 (e.g. non-manifold selection): nothing replaces the selector
    if (! p->model.is_initialized() && ! p->model_region.is_initialized())
        return true; // render_preview_meshes skips uninitialized models
    // Mirror the GLVolume match of render_preview_meshes: without it the preview
    // would not draw this volume either.
    const Selection& selection = m_parent.get_selection();
    for (const auto& i : selection.get_volume_idxs()) {
        const GLVolume* v = selection.get_volume(i);
        if (v != nullptr && v->volume_idx() == p->gl_volume_idx)
            return false; // the preview mesh replaces the selector for this volume
    }
    return true;
}

void GLGizmoLocalSubdivision::render_preview_meshes()
{
    if (m_shown_level == 0 || m_previews.empty())
        return;

    const Selection& selection = m_parent.get_selection();
    // Same shader as the canvas volume rendering, so the preview shading matches
    // exactly what is shown at level 0.
    auto* gouraud_shader = wxGetApp().get_shader("gouraud");
    if (gouraud_shader == nullptr)
        return;

    glsafe(::glPushAttrib(GL_DEPTH_BUFFER_BIT));
    glsafe(::glEnable(GL_DEPTH_TEST));
    gouraud_shader->start_using();
    // The section plane (Alt+wheel) and the z range must reach this draw: the canvas
    // volume renderer normally sets them per volume, but the previewed volumes are
    // hidden and the opaque pass is delegated to the gizmo, so nothing refreshes the
    // uniforms and the preview would clip against stale values from earlier frames.
    ClippingPlaneDataWrapper clp_data = get_clipping_plane_data();
    gouraud_shader->set_uniform("clipping_plane", clp_data.clp_dataf);
    gouraud_shader->set_uniform("z_range", clp_data.z_range);
    const Camera&     camera      = wxGetApp().plater()->get_camera();
    const Transform3d view_matrix = camera.get_view_matrix();
    for (Preview& p : m_previews) {
        if (! p.model_region.is_initialized() && ! p.model.is_initialized())
            continue;
        const GLVolume* glv = nullptr;
        for (const auto& i : selection.get_volume_idxs()) {
            const GLVolume* v = selection.get_volume(i);
            if (v->volume_idx() == p.gl_volume_idx) {
                glv = v;
                break;
            }
        }
        if (glv == nullptr)
            continue;
        const Transform3d trafo_matrix      = glv->world_matrix();
        // A mirrored volume (negative-determinant transform) reverses the projected
        // winding, and the canvas leaves back-face culling enabled at this point
        // (the transparent pass re-enables it at its end), so the raw GLModel draws
        // must flip the front face for such volumes and restore it after - the same
        // rule as GLGizmoPainterBase::render_triangles and GLVolume::render.
        const bool is_left_handed = trafo_matrix.matrix().determinant() < 0.;
        if (is_left_handed)
            glsafe(::glFrontFace(GL_CW));
        const Transform3d view_model_matrix = view_matrix * trafo_matrix;
        gouraud_shader->set_uniform("view_model_matrix", view_model_matrix);
        gouraud_shader->set_uniform("projection_matrix", camera.get_projection_matrix());
        const Matrix3d view_normal_matrix =
            view_matrix.matrix().block(0, 0, 3, 3) * trafo_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();
        gouraud_shader->set_uniform("view_normal_matrix", view_normal_matrix);
        gouraud_shader->set_uniform("volume_world_matrix", trafo_matrix);
        if (p.model.is_initialized())
            p.model.render();
        if (p.model_region.is_initialized())
            p.model_region.render();
        if (is_left_handed)
            glsafe(::glFrontFace(GL_CCW));
    }
    gouraud_shader->stop_using();
    glsafe(::glPopAttrib());
}

wxString GLGizmoLocalSubdivision::subdivision_failure_text(int lowest_achieved, bool apply) const
{
    // The only reachable rejection is the non-manifold guard (more than two
    // selected faces on one edge); geometry problems ship the initial Loop
    // result instead of failing.
    if (apply)
        return wxString::Format(_L("Subdivision failed: the selected region contains non-manifold edges (reached level %d of %d). No changes were applied."),
                                lowest_achieved, m_subdiv_level);
    return wxString::Format(_L("Local subdivision stopped at level %d of %d: the selected region contains non-manifold edges."),
                            lowest_achieved, m_subdiv_level);
}

void GLGizmoLocalSubdivision::apply_subdivision()
{
    ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (mo == nullptr)
        return;

    update_preview();
    if (m_previews.empty() || m_subdiv_level == 0)
        return;

    // The subdivision of a volume failed when its cache did not reach the wanted
    // level (smooth_triangle_mesh_local_its returned ok == false): apply nothing
    // and tell the user why the button had no effect.
    int lowest_achieved = m_subdiv_level;
    for (const Preview& p : m_previews)
        lowest_achieved = std::min(lowest_achieved, int(p.meshes.size()) - 1);
    if (lowest_achieved < m_subdiv_level) {
        wxGetApp().plater()->get_notification_manager()->push_notification(
            NotificationManager::NotificationLevel::WarningNotificationLevel, 10,
            subdivision_failure_text(lowest_achieved, true).ToStdString());
        return;
    }

    // The 1-million-face guard moved to the level stepper: the preview has already
    // built (and paid for) the meshes by the time the apply path could ask.

    auto      plater     = wxGetApp().plater();
    const int object_idx = m_parent.get_selection().get_object_idx();

    // First pass: which previews can still be applied. Locate the object by its
    // stable id (deleting another object shifts list indexes while the gizmo is
    // open), skip volumes that were removed, and skip results whose source mesh was
    // swapped in the meantime - they are stale and would overwrite the newer mesh.
    Model&                model = plater->model();
    std::vector<Preview*> apply_list;
    for (Preview& p : m_previews) {
        const int level = std::min(m_subdiv_level, int(p.meshes.size()) - 1);
        if (level <= 0)
            continue;
        int cur_obj_idx = -1;
        for (size_t i = 0; i < model.objects.size(); ++i) {
            if (model.objects[i]->id() == p.object_id) {
                cur_obj_idx = (int) i;
                break;
            }
        }
        if (cur_obj_idx < 0)
            continue;
        ModelObject* obj = model.objects[cur_obj_idx];
        if (std::find(obj->volumes.begin(), obj->volumes.end(), p.volume) == obj->volumes.end())
            continue;
        if (p.volume->mesh_ptr() != p.origin)
            continue;
        apply_list.push_back(&p);
    }
    if (apply_list.empty())
        return;

    // Move the result meshes out of the caches: closing the gizmo below clears them.
    std::vector<std::pair<ModelVolume*, indexed_triangle_set>> meshes_to_apply;
    for (Preview* p : apply_list) {
        const int level = std::min(m_subdiv_level, int(p->meshes.size()) - 1);
        meshes_to_apply.emplace_back(p->volume, std::move(p->meshes[size_t(level)]));
    }

    // Close the gizmo BEFORE the snapshot is taken: the undo stack stores the gizmo
    // state with each snapshot, so the snapshot must show no gizmo - one Ctrl+Z then
    // returns to the original mesh without reopening the subdivision interface.
    close();

    plater->take_snapshot("Local subdivision");

    for (auto& [volume, mesh] : meshes_to_apply) {
        volume->set_mesh(std::move(mesh));
        volume->calculate_convex_hull();
        volume->invalidate_convex_hull_2d();
        volume->set_new_unique_id();
        // The four FacetsAnnotation stores (supports, seams, MMU segmentation, fuzzy
        // skin) are indexed by face number; subdivision replaced the selected faces
        // with up to four new ones, so the stale markers would land on wrong faces.
        // Reset ONLY the subdivided volumes: the object-level
        // Plater::clear_before_change_mesh() would also wipe the painting of the
        // object's other volumes, whose meshes this operation did not touch.
        volume->reset_extra_facets();
    }
    mo->invalidate_bounding_box();
    mo->ensure_on_bed();
    plater->changed_mesh(object_idx);
    wxGetApp().obj_list()->update_item_error_icon(object_idx, -1);
}

void GLGizmoLocalSubdivision::on_shutdown()
{
    restore_preview();
    m_previews.clear();
    m_preview_dirty = true;
}

void GLGizmoLocalSubdivision::close()
{
    // Closing is done by opening the gizmo again (toggle semantics), but only while
    // it is still open: refresh paths may already have closed it, and a blind toggle
    // would reopen it.
    GLGizmosManager& gizmos_mgr = m_parent.get_gizmos_manager();
    if (gizmos_mgr.get_current_type() == GLGizmosManager::LocalSubdivision)
        gizmos_mgr.open_gizmo(GLGizmosManager::EType::LocalSubdivision);
}

} // namespace Slic3r::GUI
