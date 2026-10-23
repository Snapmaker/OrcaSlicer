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

void GLGizmoLocalSubdivision::render_painter_gizmo()
{
    const Selection& selection = m_parent.get_selection();

    glsafe(::glEnable(GL_BLEND));
    glsafe(::glEnable(GL_DEPTH_TEST));

    // While the preview is shown the region is highlighted inside the preview mesh
    // itself; skip the painting overlay, which is drawn on the original geometry.
    if (m_shown_level == 0)
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
        if (ImGui::Button("+##subdiv_plus", ImVec2(btn_w, 0.f)) && m_subdiv_level < MaxSubdivLevel)
            ++m_subdiv_level;

        ImGui::PopStyleColor(4); // Button, ButtonHovered, ButtonActive, Text
    }
    update_preview();

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
        m_preview_dirty = true;
        m_parent.set_as_dirty();
    }

    m_imgui->disabled_begin(selected_facets == 0 || m_subdiv_level == 0);
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
    if (! m_preview_dirty && m_shown_level == m_subdiv_level)
        return;

    ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (mo == nullptr)
        return;

    if (m_preview_dirty) {
        // Rebuild the caches from the current painting state.
        restore_preview();
        m_previews.clear();
        // The selectors are built by update_from_model_object; without them there is
        // nothing to preview yet.
        if (! m_triangle_selectors.empty()) {
            int idx = -1;
            for (ModelVolume* mv : mo->volumes) {
                if (! mv->is_model_part())
                    continue;
                ++idx;
                if (size_t(idx) >= m_triangle_selectors.size())
                    break;
                std::vector<int> facets = m_triangle_selectors[idx]->get_facets_indices_by_state(EnforcerBlockerType::ENFORCER);
                if (facets.empty())
                    continue;
                Preview p;
                p.volume      = mv;
                p.volume_idx  = idx;
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
            const indexed_triangle_set& src        = p.meshes.back();
            const size_t                unselected = src.indices.size() - p.next_facets.size();
            bool                        ok         = false;
            indexed_triangle_set        next       = TriangleMeshDeal::smooth_triangle_mesh_local_its(src, p.next_facets, ok);
            if (! ok)
                break;
            const size_t region = next.indices.size() - unselected;
            p.next_facets.clear();
            p.next_facets.resize(region);
            for (size_t i = 0; i < region; ++i)
                p.next_facets[i] = uint32_t(i);
            p.meshes.emplace_back(std::move(next));
            p.region_counts.push_back(region);
        }
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
                if (v->volume_idx() == p.volume_idx) {
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
        m_shown_level = anything_shown ? m_subdiv_level : 0;
    } else {
        restore_preview();
    }
    m_parent.set_as_dirty();
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

    glsafe(::glPushAttrib(GL_DEPTH_TEST));
    glsafe(::glEnable(GL_DEPTH_TEST));
    gouraud_shader->start_using();
    const Camera&     camera      = wxGetApp().plater()->get_camera();
    const Transform3d view_matrix = camera.get_view_matrix();
    for (Preview& p : m_previews) {
        if (! p.model_region.is_initialized() && ! p.model.is_initialized())
            continue;
        const GLVolume* glv = nullptr;
        for (const auto& i : selection.get_volume_idxs()) {
            const GLVolume* v = selection.get_volume(i);
            if (v->volume_idx() == p.volume_idx) {
                glv = v;
                break;
            }
        }
        if (glv == nullptr)
            continue;
        const Transform3d trafo_matrix      = glv->world_matrix();
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
    }
    gouraud_shader->stop_using();
    glsafe(::glPopAttrib());
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
    // level (smooth_triangle_mesh_local_its returned ok == false): apply nothing.
    for (const Preview& p : m_previews)
        if (int(p.meshes.size()) <= m_subdiv_level)
            return;

    // Upper bound of the face count after the subdivision: unselected faces stay,
    // every painted face becomes at most 4^level faces.
    size_t growth = 1;
    for (int i = 0; i < m_subdiv_level; ++i)
        growth *= 4;
    size_t future_face_count = 0;
    for (const ModelVolume* mv : mo->volumes) {
        if (! mv->is_model_part())
            continue;
        size_t painted = 0;
        for (const Preview& p : m_previews)
            if (p.volume == mv) {
                painted = p.facet_count;
                break;
            }
        future_face_count += mv->mesh().its.indices.size() + painted * (growth - 1);
    }

    if (future_face_count > 1000000) {
        auto         name_str = wxString::FromUTF8(mo->name);
        auto         content  = wxString::Format(
            _L("\"%s\" will exceed 1 million faces after this subdivision, which may increase slicing time. Do you want to continue?"), name_str);
        WarningDialog dlg(static_cast<wxWindow*>(wxGetApp().mainframe), content, wxEmptyString, wxYES_NO);
        if (dlg.ShowModal() == wxID_NO)
            return;
    }

    auto      plater     = wxGetApp().plater();
    const int object_idx = m_parent.get_selection().get_object_idx();

    plater->take_snapshot("Local subdivision");
    plater->clear_before_change_mesh(object_idx);

    for (Preview& p : m_previews) {
        const int level = std::min(m_subdiv_level, int(p.meshes.size()) - 1);
        if (level <= 0)
            continue;
        p.volume->set_mesh(std::move(p.meshes[size_t(level)]));
        p.volume->calculate_convex_hull();
        p.volume->invalidate_convex_hull_2d();
        p.volume->set_new_unique_id();
    }
    mo->invalidate_bounding_box();
    mo->ensure_on_bed();
    plater->changed_mesh(object_idx);
    wxGetApp().obj_list()->update_item_error_icon(object_idx, -1);

    close();
}

void GLGizmoLocalSubdivision::on_shutdown()
{
    restore_preview();
    m_previews.clear();
    m_preview_dirty = true;
}

void GLGizmoLocalSubdivision::close()
{
    // Closing the gizmo is done by opening it again (toggle semantics).
    GLGizmosManager& gizmos_mgr = m_parent.get_gizmos_manager();
    gizmos_mgr.open_gizmo(GLGizmosManager::EType::LocalSubdivision);
}

} // namespace Slic3r::GUI
