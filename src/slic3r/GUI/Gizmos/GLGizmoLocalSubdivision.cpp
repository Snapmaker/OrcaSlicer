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
    m_desc["subdivide"]                = _L("Subdivide");
    m_desc["close"]                    = _L("Close");
    m_desc["selected_facets"]          = _L("Selected facets");
    return true;
}

void GLGizmoLocalSubdivision::render_painter_gizmo()
{
    const Selection& selection = m_parent.get_selection();

    glsafe(::glEnable(GL_BLEND));
    glsafe(::glEnable(GL_DEPTH_TEST));

    render_triangles(selection);
    m_c->object_clipper()->render_cut();
    m_c->instances_hider()->render_cut();

    render_cursor();

    glsafe(::glDisable(GL_BLEND));
}

void GLGizmoLocalSubdivision::update_from_model_object(bool first_update)
{
    wxBusyCursor wait;
    const ModelObject* mo = m_c->selection_info()->model_object();
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
    const float empty_button_width     = m_imgui->calc_button_size("").x;
    const float sliders_left_width     = std::max(smart_fill_slider_left, std::max(cursor_slider_left, height_slider_left));
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

    ImGui::Separator();

    // How many facets are currently painted. Count the original (level-0) faces, not
    // the leaves of the selector tree: the selector subdivides triangles while painting,
    // so one painted face can hold many selected leaf triangles.
    int selected_facets = 0;
    for (const auto& triangle_selector : m_triangle_selectors)
        selected_facets += int(triangle_selector->get_facets_indices_by_state(EnforcerBlockerType::ENFORCER).size());
    m_imgui->text(format_wxstr("%1%: %2%", m_desc["selected_facets"], selected_facets));

    if (m_imgui->button(m_desc.at("remove_all"))) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Reset selection", UndoRedo::SnapshotType::GizmoAction);
        ModelObject*         mo = m_c->selection_info()->model_object();
        int                  idx = -1;
        for (ModelVolume* mv : mo->volumes)
            if (mv->is_model_part()) {
                ++idx;
                m_triangle_selectors[idx]->reset();
                m_triangle_selectors[idx]->request_update_render_data(true);
            }
        m_parent.set_as_dirty();
    }

    m_imgui->disabled_begin(selected_facets == 0);
    m_imgui->push_confirm_button_style();
    if (m_imgui->bbl_button(m_desc["subdivide"]))
        apply_subdivision();
    m_imgui->pop_confirm_button_style();
    m_imgui->disabled_end();

    ImGui::SameLine();
    if (m_imgui->bbl_button(m_desc["close"]))
        close();

    GizmoImguiEnd();

    ImGuiWrapper::pop_toolbar_style();
}

void GLGizmoLocalSubdivision::apply_subdivision()
{
    ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (mo == nullptr)
        return;

    // Gather the painted facets of every model part first, so the mesh is only
    // touched when something is actually selected.
    std::vector<std::pair<ModelVolume*, std::vector<int>>> work;
    // Upper bound of the face count after the subdivision: unselected faces stay,
    // every selected face becomes at most four.
    size_t future_face_count = 0;
    int idx = -1;
    for (ModelVolume* mv : mo->volumes) {
        if (! mv->is_model_part())
            continue;
        ++idx;
        std::vector<int> facets = m_triangle_selectors[idx]->get_facets_indices_by_state(EnforcerBlockerType::ENFORCER);
        future_face_count += mv->mesh().its.indices.size() + 3 * facets.size();
        if (! facets.empty())
            work.emplace_back(mv, std::move(facets));
    }
    if (work.empty())
        return;

    if (future_face_count > 1000000) {
        auto         name_str = wxString::FromUTF8(mo->name);
        auto         content  = wxString::Format(
            _L("\"%s\" will exceed 1 million faces after this subdivision, which may increase slicing time. Do you want to continue?"), name_str);
        WarningDialog dlg(static_cast<wxWindow*>(wxGetApp().mainframe), content, wxEmptyString, wxYES_NO);
        if (dlg.ShowModal() == wxID_NO)
            return;
    }

    auto        plater     = wxGetApp().plater();
    const int   object_idx = m_parent.get_selection().get_object_idx();

    plater->take_snapshot("Local subdivision");
    plater->clear_before_change_mesh(object_idx);

    for (auto& [mv, facets] : work) {
        std::vector<uint32_t> facet_indices(facets.begin(), facets.end());
        bool                  ok = false;
        TriangleMesh          result_mesh = TriangleMeshDeal::smooth_triangle_mesh_local(mv->mesh(), facet_indices, ok);
        if (ok) {
            mv->set_mesh(result_mesh);
            mv->calculate_convex_hull();
            mv->invalidate_convex_hull_2d();
            mv->set_new_unique_id();
        }
    }
    mo->invalidate_bounding_box();
    mo->ensure_on_bed();
    plater->changed_mesh(object_idx);
    wxGetApp().obj_list()->update_item_error_icon(object_idx, -1);

    close();
}

void GLGizmoLocalSubdivision::close()
{
    // Closing the gizmo is done by opening it again (toggle semantics).
    GLGizmosManager& gizmos_mgr = m_parent.get_gizmos_manager();
    gizmos_mgr.open_gizmo(GLGizmosManager::EType::LocalSubdivision);
}

} // namespace Slic3r::GUI
