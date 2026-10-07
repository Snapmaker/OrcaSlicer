#include "GLGizmoBase.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"

#include <glad/gl.h>

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI_Colors.hpp"
// The per-gizmo dock preference lives in AppConfig, so the full definition is
// needed here - GUI_App.hpp only forward-declares it.
#include "libslic3r/AppConfig.hpp"
// ContentSizeIdeal (the width a panel's contents actually wanted) lives on the
// internal ImGuiWindow, not in the public API.
#include "imgui/imgui_internal.h"

#include <cmath>

// TODO: Display tooltips quicker on Linux

namespace Slic3r {
namespace GUI {

float GLGizmoBase::INV_ZOOM = 1.0f;


const float GLGizmoBase::Grabber::SizeFactor = 0.05f;
const float GLGizmoBase::Grabber::MinHalfSize = 4.0f;
const float GLGizmoBase::Grabber::DraggingScaleFactor = 1.25f;
const float GLGizmoBase::Grabber::FixedGrabberSize = 16.0f;
const float GLGizmoBase::Grabber::FixedRadiusSize = 80.0f;


ColorRGBA GLGizmoBase::DEFAULT_BASE_COLOR = { 0.625f, 0.625f, 0.625f, 1.0f };
ColorRGBA GLGizmoBase::DEFAULT_DRAG_COLOR = { 1.0f, 1.0f, 1.0f, 1.0f };
ColorRGBA GLGizmoBase::DEFAULT_HIGHLIGHT_COLOR = {1.0f, 0.38f, 0.0f, 1.0f};
std::array<ColorRGBA, 3> GLGizmoBase::AXES_HOVER_COLOR = {{
    {ColorRGBA::X().r() * 1.2f, ColorRGBA::X().g() * 1.4f, ColorRGBA::X().b() * 1.4f, 1.0f},
    {ColorRGBA::Y().r() * 1.2f, ColorRGBA::Y().g() * 1.2f, ColorRGBA::Y().b() * 1.2f, 1.0f},
    {ColorRGBA::Z().r() * 1.2f, ColorRGBA::Z().g() * 1.2f, ColorRGBA::Z().b() * 1.2f, 1.0f},
}};

std::array<ColorRGBA, 3> GLGizmoBase::AXES_COLOR = {{
    ColorRGBA::X(),
    ColorRGBA::Y(),
    ColorRGBA::Z()
}};

ColorRGBA            GLGizmoBase::CONSTRAINED_COLOR   = {0.5f, 0.5f, 0.5f, 1.0f};
ColorRGBA            GLGizmoBase::FLATTEN_COLOR       = {0.96f, 0.93f, 0.93f, 0.5f};
ColorRGBA            GLGizmoBase::FLATTEN_HOVER_COLOR = {1.0f, 1.0f, 1.0f, 0.75f};

// new style color
ColorRGBA            GLGizmoBase::GRABBER_NORMAL_COL        = {1.0f, 1.0f, 1.0f, 1.0f};
ColorRGBA            GLGizmoBase::GRABBER_HOVER_COL         = {0.863f, 0.125f, 0.063f, 1.0f};
ColorRGBA            GLGizmoBase::GRABBER_UNIFORM_COL       = {0, 1.0, 1.0, 1.0f};
ColorRGBA            GLGizmoBase::GRABBER_UNIFORM_HOVER_COL = {0, 0.7, 0.7, 1.0f};


void GLGizmoBase::update_render_colors()
{
    GLGizmoBase::AXES_COLOR = { {
                                ImGuiWrapper::from_ImVec4(RenderColor::colors[RenderCol_Grabber_X]),
                                ImGuiWrapper::from_ImVec4(RenderColor::colors[RenderCol_Grabber_Y]),
                                ImGuiWrapper::from_ImVec4(RenderColor::colors[RenderCol_Grabber_Z])
                                } };

    GLGizmoBase::FLATTEN_COLOR       = ImGuiWrapper::from_ImVec4(RenderColor::colors[RenderCol_Flatten_Plane]);
    GLGizmoBase::FLATTEN_HOVER_COLOR = ImGuiWrapper::from_ImVec4(RenderColor::colors[RenderCol_Flatten_Plane_Hover]);
}

void GLGizmoBase::load_render_colors()
{
    RenderColor::colors[RenderCol_Grabber_X]           = ImGuiWrapper::to_ImVec4(GLGizmoBase::AXES_COLOR[0]);
    RenderColor::colors[RenderCol_Grabber_Y]           = ImGuiWrapper::to_ImVec4(GLGizmoBase::AXES_COLOR[1]);
    RenderColor::colors[RenderCol_Grabber_Z]           = ImGuiWrapper::to_ImVec4(GLGizmoBase::AXES_COLOR[2]);
    RenderColor::colors[RenderCol_Flatten_Plane]       = ImGuiWrapper::to_ImVec4(GLGizmoBase::FLATTEN_COLOR);
    RenderColor::colors[RenderCol_Flatten_Plane_Hover] = ImGuiWrapper::to_ImVec4(GLGizmoBase::FLATTEN_HOVER_COLOR);
}

PickingModel GLGizmoBase::Grabber::s_cube;
PickingModel GLGizmoBase::Grabber::s_cone;

GLGizmoBase::Grabber::~Grabber()
{
    //if (s_cube.model.is_initialized())
    //    s_cube.model.reset();

    //if (s_cone.model.is_initialized())
    //    s_cone.model.reset();
}

float GLGizmoBase::Grabber::get_half_size(float size) const
{
    return std::max(size * SizeFactor, MinHalfSize);
}

float GLGizmoBase::Grabber::get_dragging_half_size(float size) const
{
    return get_half_size(size) * DraggingScaleFactor;
}

PickingModel &GLGizmoBase::Grabber::get_cube()
{
    if (!s_cube.model.is_initialized()) {
        // This cannot be done in constructor, OpenGL is not yet
        // initialized at that point (on Linux at least).
        indexed_triangle_set its = its_make_cube(1.0, 1.0, 1.0);
        its_translate(its, -0.5f * Vec3f::Ones());
        s_cube.model.init_from(its);
        s_cube.mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(std::move(its)));
    }
    return s_cube;
}

void GLGizmoBase::Grabber::register_raycasters_for_picking(int id)
{
    picking_id = id;
    // registration will happen on next call to render()
}

void GLGizmoBase::Grabber::unregister_raycasters_for_picking()
{
    wxGetApp().plater()->canvas3D()->remove_raycasters_for_picking(SceneRaycaster::EType::Gizmo, picking_id);
    picking_id = -1;
    raycasters = { nullptr };
}

void GLGizmoBase::Grabber::render(float size, const ColorRGBA& render_color)
{
    GLShaderProgram* shader = wxGetApp().get_current_shader();
    if (shader == nullptr)
        return;

    if (!s_cube.model.is_initialized()) {
        // This cannot be done in constructor, OpenGL is not yet
        // initialized at that point (on Linux at least).
        indexed_triangle_set its = its_make_cube(1.0, 1.0, 1.0);
        its_translate(its, -0.5f * Vec3f::Ones());
        s_cube.model.init_from(its);
        s_cube.mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(std::move(its)));
    }

    if (!s_cone.model.is_initialized()) {
        indexed_triangle_set its = its_make_cone(1.0, 1.0, double(PI) / 18.0);
        s_cone.model.init_from(its);
        s_cone.mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(std::move(its)));
    }

    //BBS set to fixed size grabber
    const float  grabber_size   = FixedGrabberSize * INV_ZOOM;
    const double extension_size = 0.75 * FixedGrabberSize * INV_ZOOM;

    s_cube.model.set_color(render_color);
    s_cone.model.set_color(render_color);

    const Camera& camera = wxGetApp().plater()->get_camera();
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    const Transform3d& view_matrix = camera.get_view_matrix();
    const Matrix3d view_matrix_no_offset = view_matrix.matrix().block(0, 0, 3, 3);

    auto render_extension = [&view_matrix, &view_matrix_no_offset, shader, this](int idx, PickingModel &model, const Transform3d &model_matrix) {
        shader->set_uniform("view_model_matrix", view_matrix * model_matrix);
        const Matrix3d view_normal_matrix = view_matrix_no_offset * model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();
        shader->set_uniform("view_normal_matrix", view_normal_matrix);
        model.model.render();

        if (raycasters[idx] == nullptr) {
            GLCanvas3D &canvas = *wxGetApp().plater()->canvas3D();
            raycasters[idx] = canvas.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, picking_id, *model.mesh_raycaster, model_matrix);
        } else {
            raycasters[idx]->set_transform(model_matrix);
        }
    };

    if (extensions == EGrabberExtension::PosZ) {
        const Transform3d model_matrix = matrix * Geometry::assemble_transform(center, angles, Vec3d(0.75 * extension_size, 0.75 * extension_size, 2.0 * extension_size));
        render_extension(0, s_cone, model_matrix);
    } else {
        const Transform3d model_matrix = matrix * Geometry::assemble_transform(center, angles, grabber_size * Vec3d::Ones());
        render_extension(0, s_cube, model_matrix);
        
        const Transform3d extension_model_matrix_base = matrix * Geometry::assemble_transform(center, angles);
        const Vec3d extension_scale(0.75 * extension_size, 0.75 * extension_size, 3.0 * extension_size);
        if ((int(extensions) & int(GLGizmoBase::EGrabberExtension::PosX)) != 0) {
            render_extension(1, s_cone, extension_model_matrix_base * Geometry::assemble_transform(2.0 * extension_size * Vec3d::UnitX(), Vec3d(0.0, 0.5 * double(PI), 0.0), extension_scale));
        }
        if ((int(extensions) & int(GLGizmoBase::EGrabberExtension::NegX)) != 0) {
            render_extension(2, s_cone, extension_model_matrix_base * Geometry::assemble_transform(-2.0 * extension_size * Vec3d::UnitX(), Vec3d(0.0, -0.5 * double(PI), 0.0), extension_scale));
        }
        if ((int(extensions) & int(GLGizmoBase::EGrabberExtension::PosY)) != 0) {
            render_extension(3, s_cone, extension_model_matrix_base * Geometry::assemble_transform(2.0 * extension_size * Vec3d::UnitY(), Vec3d(-0.5 * double(PI), 0.0, 0.0), extension_scale));
        }
        if ((int(extensions) & int(GLGizmoBase::EGrabberExtension::NegY)) != 0) {
            render_extension(4, s_cone, extension_model_matrix_base * Geometry::assemble_transform(-2.0 * extension_size * Vec3d::UnitY(), Vec3d(0.5 * double(PI), 0.0, 0.0), extension_scale));
        }
        if ((int(extensions) & int(GLGizmoBase::EGrabberExtension::PosZ)) != 0) {
            render_extension(5, s_cone, extension_model_matrix_base * Geometry::assemble_transform(2.0 * extension_size * Vec3d::UnitZ(), Vec3d::Zero(), extension_scale));
        }
        if ((int(extensions) & int(GLGizmoBase::EGrabberExtension::NegZ)) != 0) {
            render_extension(6, s_cone, extension_model_matrix_base * Geometry::assemble_transform(-2.0 * extension_size * Vec3d::UnitZ(), Vec3d(double(PI), 0.0, 0.0), extension_scale));
        }
    }
}

bool GLGizmoBase::render_combo(const std::string &label, const std::vector<std::string> &lines, int &selection_idx, float label_width, float item_width)
{
    ImGuiWrapper::push_combo_style(m_parent.get_scale());
    ImGui::AlignTextToFramePadding();
    m_imgui->text(label);
    ImGui::SameLine(label_width);
    ImGui::PushItemWidth(item_width);

    size_t selection_out = selection_idx;

    const char *selected_str = (selection_idx >= 0 && selection_idx < int(lines.size())) ? lines[selection_idx].c_str() : "";
    if (ImGui::BBLBeginCombo(("##" + label).c_str(), selected_str, 0)) {
        for (size_t line_idx = 0; line_idx < lines.size(); ++line_idx) {
            ImGui::PushID(int(line_idx));
            if (ImGui::Selectable("", line_idx == selection_idx)) selection_out = line_idx;

            ImGui::SameLine();
            ImGui::Text("%s", lines[line_idx].c_str());
            ImGui::PopID();
        }

        ImGui::EndCombo();
    }

    bool is_changed = selection_idx != selection_out;
    selection_idx   = selection_out;

    //if (is_changed) update_connector_shape();
    ImGuiWrapper::pop_combo_style();

    return is_changed;
}

void GLGizmoBase::render_cross_mark(const Vec3f &target, bool is_single)
{
    const float half_length = 4.0f;

    // ORCA: OpenGL Core Profile
#if !SLIC3R_OPENGL_ES
    if (!OpenGLManager::get_gl_info().is_core_profile())
        glsafe(::glLineWidth(2.0f));
#endif // !SLIC3R_OPENGL_ES

    auto render_line = [](const Vec3f& p1, const Vec3f& p2, const ColorRGBA& color) {
        GLModel::Geometry init_data;
        init_data.format = {GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3};
        init_data.color  = color;
        init_data.reserve_vertices(2);
        init_data.reserve_indices(2);

        // vertices
        init_data.add_vertex(p1);
        init_data.add_vertex(p2);

        // indices
        init_data.add_line(0, 1);

        GLModel model;
        model.init_from(std::move(init_data));
        model.render();
    };

    // draw line for x axis
    if (!is_single) {
        render_line(
            {target(0) - half_length, target(1), target(2)}, 
            {target(0) + half_length, target(1), target(2)},
            ColorRGBA::X()); // ORCA match axis colors
    }
    else {
        render_line(
            {target(0), target(1), target(2)}, 
            {target(0) + half_length, target(1), target(2)},
            ColorRGBA::X()); // ORCA match axis colors
    }
    // draw line for y axis
    if (!is_single) {
        render_line(
            {target(0), target(1) - half_length, target(2)}, 
            {target(0), target(1) + half_length, target(2)},
            ColorRGBA::Y()); // ORCA match axis colors
    } else {
        render_line(
            {target(0), target(1), target(2)}, 
            {target(0), target(1) + half_length, target(2)},
            ColorRGBA::Y()); // ORCA match axis colors
    }
    // draw line for z axis
    if (!is_single) {
        render_line(
            {target(0), target(1), target(2) - half_length}, 
            {target(0), target(1), target(2) + half_length},
            ColorRGBA::Z()); // ORCA match axis colors
    } else {
        render_line(
            {target(0), target(1), target(2)}, 
            {target(0), target(1), target(2) + half_length},
            ColorRGBA::Z()); // ORCA match axis colors
    }
}

GLGizmoBase::GLGizmoBase(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id)
    : m_parent(parent)
    , m_group_id(-1)
    , m_state(Off)
    , m_shortcut_key(NO_SHORTCUT_KEY_VALUE)
    , m_icon_filename(icon_filename)
    , m_sprite_id(sprite_id)
    , m_imgui(wxGetApp().imgui())
{
}


std::string GLGizmoBase::get_action_snapshot_name() const
{
    return "Gizmo action";
}

void GLGizmoBase::set_icon_filename(const std::string &filename) {
    m_icon_filename = filename;
}

void GLGizmoBase::set_hover_id(int id)
{
    // do not change hover id during dragging
    assert(!m_dragging);

    // allow empty grabbers when not using grabbers but use hover_id - flatten, rotate
//    if (!m_grabbers.empty() && id >= (int) m_grabbers.size())
//        return;
    
    m_hover_id = id;
    on_set_hover_id();    
}

bool GLGizmoBase::update_items_state()
{
    bool res = m_dirty;
    m_dirty  = false;
    return res;
}

bool GLGizmoBase::GizmoImguiBegin(const std::string &name, int flags)
{
    m_imgui->set_next_window_bg_alpha(gizmo_panel_opacity());
    return m_imgui->begin(name, flags);
}

float GLGizmoBase::gizmo_panel_opacity()
{
    AppConfig *cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return 1.f;
    const std::string v = cfg->get("gizmo_panel_opacity");
    if (v.empty())
        return 1.f;
    try {
        return std::clamp(std::stof(v), 0.3f, 1.f);
    } catch (...) {
        return 1.f;
    }
}

void GLGizmoBase::GizmoImguiEnd()
{
    last_input_window_width = ImGui::GetWindowWidth();

    // A docked panel is pinned to an exact rect, so GetWindowWidth() above only
    // reports back the width we forced on it - it can never say how wide the
    // contents actually wanted to be. ContentSizeIdeal is that measurement: the
    // extent the contents reached, independent of the window's own size, of
    // clipping and of the scrollbar. Keep it (plus the padding a window puts
    // around its contents) so the docked path has a real content width to use
    // rather than the width of its own previous guess.
    if (ImGuiWindow *win = ImGui::GetCurrentWindow()) {
        // Padding on both sides, plus the vertical scrollbar when the panel has
        // one: a docked panel taller than the view gets a scrollbar that takes
        // its width out of the content region, so a width that ignored it would
        // squeeze the contents and re-measure narrower every frame.
        const float ideal = win->ContentSizeIdeal.x + 2.0f * win->WindowPadding.x +
                            (win->ScrollbarY ? ImGui::GetStyle().ScrollbarSize : 0.f);
        if (m_dock_body_rendered && ideal > m_imgui->scaled(12.0f)) {
            // DockWidthSettle only commits a new width once the SAME candidate
            // has been measured on two consecutive frames - see
            // DockWidthSettle.hpp. That is what stops a one-frame transient
            // (sub-pixel window-position rounding, a hover/tooltip that
            // briefly touched the content bounds, the first measurement right
            // after the panel reopens docked) from being adopted and then
            // immediately contradicted by the next, ordinary frame - which is
            // what a bare "differs by more than 0.5px" check let happen: two
            // close-but-different widths kept taking turns being "the" width,
            // every frame, for as long as the panel stayed open.
            if (m_dock_width.update(ideal, 0.5f) && m_docked) {
                // The rect we drew this frame was built from the previous
                // (possibly absent) measurement, so draw one more with the
                // real one.
                m_imgui->set_requires_extra_frame();
                m_parent.set_as_dirty();
            }
        } else if (!m_dock_body_rendered) {
            // Title-row-only frame (collapsed, or the toggle-collapse frame):
            // nothing was measured, so any candidate that was mid-confirmation
            // must not survive to be compared against a later, unrelated
            // frame's measurement.
            m_dock_width.reset_pending();
        }
    }

    m_imgui->end();
}

void GLGizmoBase::GizmoImguiSetNextWIndowPos(float &x, float y, int flag, float pivot_x, float pivot_y)
{
    GizmoImguiSetNextWIndowPos(x, y, last_input_window_width, 0, flag, pivot_x, pivot_y);
}

void GLGizmoBase::GizmoImguiSetNextWIndowPos(float &x, float y, float w, float h, int flag, float pivot_x, float pivot_y)
{
    if (abs(w) > 0.01f) {
        if (x + w > m_parent.get_canvas_size().get_width()) {
            if (w > m_parent.get_canvas_size().get_width()) {
                x = 0;
            } else {
                x = m_parent.get_canvas_size().get_width() - w;
            }
        }
    }

    m_imgui->set_next_window_pos(x, y, flag, pivot_x, pivot_y);
}

// ----------------------------------------------------------------------------
// Panel docking / collapsing (shared by every tall gizmo panel)
// ----------------------------------------------------------------------------

std::string GLGizmoBase::get_dock_key() const
{
    // m_sprite_id IS the GLGizmosManager::EType value, so it is unique per gizmo
    // and, unlike on_get_name(), does not move with the UI language. A gizmo that
    // wants a readable key overrides this.
    return std::to_string(m_sprite_id);
}

void GLGizmoBase::load_dock_state()
{
    if (m_dock_state_loaded)
        return;
    m_dock_state_loaded = true;

    // Default undocked: a missing key reads as false, which is what we want, but
    // ask explicitly so a future default flip is a one-line change here.
    const std::string key = "gizmo_dock_" + get_dock_key();
    AppConfig *cfg = wxGetApp().app_config;
    if (cfg != nullptr && cfg->has(key))
        m_docked = cfg->get_bool(key);
    else
        m_docked = false;
}

void GLGizmoBase::store_dock_state()
{
    AppConfig *cfg = wxGetApp().app_config;
    if (cfg != nullptr)
        cfg->set_bool("gizmo_dock_" + get_dock_key(), m_docked);
}

int GLGizmoBase::dock_window_flags(int flags) const
{
    if (!m_docked)
        return flags;

    // A docked panel is pinned to an exact rect, so whatever the panel asked for
    // in the way of auto-sizing has to go, and it needs a vertical scrollbar for
    // the case where its contents are taller than the view. A size-to-content
    // panel instead keeps AlwaysAutoResize: it only takes the height its
    // contents need, anchored at the top of the dock zone.
    flags &= ~ImGuiWindowFlags_NoScrollbar;
    if (!m_dock_size_to_content)
        flags &= ~ImGuiWindowFlags_AlwaysAutoResize;
    flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
             ImGuiWindowFlags_NoTitleBar;
    return flags;
}

void GLGizmoBase::dock_setup_next_window(float &x, float &y, float bottom_limit, float window_width, bool size_to_content)
{
    load_dock_state();
    m_dock_size_to_content = size_to_content;

    if (!m_docked) {
        // Unchanged behaviour: the panel opens under its toolbar icon. A panel
        // that named a fixed width keeps it pinned (auto height); one that did
        // not is left to AlwaysAutoResize as before.
        if (window_width > 0.f) {
#if BBS_TOOLBAR_ON_TOP
            GizmoImguiSetNextWIndowPos(x, y, window_width, 0.f, ImGuiCond_Always, 0.0f, 0.0f);
#else
            GizmoImguiSetNextWIndowPos(x, y, window_width, 0.f, ImGuiCond_Always, 1.0f, 0.0f);
#endif
            ImGui::SetNextWindowSize(ImVec2(window_width, 0.f), ImGuiCond_Always);
        } else {
            GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
        }
        return;
    }

    if (size_to_content) {
        // Small panel: keep AlwaysAutoResize (dock_window_flags() keeps the flag
        // when m_dock_size_to_content is set) and anchor the window's right edge
        // to the canvas right edge at the top of the dock zone. Collapsed, the
        // window is just the title row and auto-resize shrinks to it by itself.
        const Size  cnv_size   = m_parent.get_canvas_size();
        const float collapse_h = m_parent.is_collapse_toolbar_on_left() ? 0.f
                                                                        : m_parent.get_collapse_toolbar_height();
        const float top = std::max(y, collapse_h + m_imgui->scaled(0.3f));
        m_imgui->set_next_window_pos((float) cnv_size.get_width(), top, ImGuiCond_Always, 1.0f, 0.0f);
        return;
    }

    const Size  cnv_size = m_parent.get_canvas_size();
    const float cnv_w    = (float) cnv_size.get_width();
    const float cnv_h    = (float) cnv_size.get_height();

    // A panel that has no width of its own (it was AlwaysAutoResize) is sized
    // from the width its contents measured, which GizmoImguiEnd() records into
    // m_dock_width from ContentSizeIdeal. That measurement must NOT come
    // from the window's own width: a docked window is pinned to the rect we gave
    // it, so reading its width back would only return our own previous guess and
    // latch the panel at whatever it opened with - which is the bug this replaces.
    // While collapsed the contents are only the title row, far too narrow to dock
    // to, so the last expanded (committed) width is what gets remembered.
    float width = window_width;
    if (width <= 0.f)
        width = m_dock_width.committed_width;
    if (width < m_imgui->scaled(12.0f)) {
        // First frame of a panel that has never been measured (a fresh session
        // opening straight into its remembered docked state). Nothing knows the
        // content width yet, so start from a sane guess; GizmoImguiEnd() measures
        // the contents this same frame and asks for another, which then lands on
        // the real width.
        width = m_imgui->scaled(18.0f);
    }
    width = std::min(width, cnv_w * 0.5f);

    // The sidebar collapse button sits at the top of whichever edge the sidebar
    // docks to. When that is the right edge, the docked panel starts below it
    // rather than under it; when the sidebar is on the left, the right edge is
    // clear and the panel can start at the top.
    const float collapse_h = m_parent.is_collapse_toolbar_on_left() ? 0.f
                                                                    : m_parent.get_collapse_toolbar_height();
    // `y` as handed in is already below the (top) main toolbar, so the panel
    // clears the view toolbar; take whichever of the two is lower.
    const float top    = std::max(y, collapse_h + m_imgui->scaled(0.3f));
    const float bottom = std::max(top + m_imgui->scaled(6.0f), bottom_limit);

    x = cnv_w - width;
    y = top;

    m_imgui->set_next_window_pos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
    // Full available height. ImGui adds the scrollbar itself once the contents
    // exceed it, because dock_window_flags() cleared NoScrollbar. Folded, the
    // panel is just its title row: a full-height empty strip looked like a crash.
    float height = std::min(bottom, cnv_h) - top;
    if (m_collapsed)
        height = std::min(height, ImGui::GetFrameHeight() + 2.0f * ImGui::GetStyle().WindowPadding.y);
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
}

bool GLGizmoBase::render_dock_icon_button(const char *id, bool pin, bool active, const wxString &tooltip)
{
    // Drawn rather than blitted: the imgui font atlas has no pin glyph, and
    // adding one would mean touching the shared icon font for two 16px marks.
    const float  sz     = ImGui::GetFrameHeight();
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    const bool clicked = ImGui::InvisibleButton(id, ImVec2(sz, sz));
    const bool hovered = ImGui::IsItemHovered();
    if (hovered && !tooltip.IsEmpty())
        m_imgui->tooltip(tooltip, ImGui::GetFontSize() * 20.0f);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (hovered)
        dl->AddRectFilled(origin, ImVec2(origin.x + sz, origin.y + sz),
                          ImGui::GetColorU32(ImGuiCol_ButtonHovered), ImGui::GetStyle().FrameRounding);

    // Follows the theme: the same colour the panel's own text uses, dimmed while
    // the button is off so an engaged pin reads as engaged in both themes.
    ImVec4 col = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    if (!active && !hovered)
        col.w *= 0.55f;
    const ImU32 c = ImGui::GetColorU32(col);

    const float cx = origin.x + 0.5f * sz;
    const float cy = origin.y + 0.5f * sz;
    const float r  = 0.30f * sz;
    const float th = std::max(1.0f, sz * 0.08f);

    if (pin) {
        // A push-pin: round head, tapered body, needle. When engaged it is drawn
        // filled and upright; when not, outlined and tilted, the usual
        // "pinned / not pinned" pair.
        if (active) {
            dl->AddCircleFilled(ImVec2(cx, cy - r * 0.55f), r * 0.62f, c, 12);
            dl->AddLine(ImVec2(cx, cy), ImVec2(cx, cy + r * 1.15f), c, th * 1.4f);
            dl->AddLine(ImVec2(cx - r * 0.75f, cy), ImVec2(cx + r * 0.75f, cy), c, th * 1.4f);
        } else {
            dl->AddCircle(ImVec2(cx + r * 0.35f, cy - r * 0.5f), r * 0.60f, c, 12, th);
            dl->AddLine(ImVec2(cx + r * 0.05f, cy + r * 0.05f), ImVec2(cx - r * 0.85f, cy + r * 0.95f), c, th);
            dl->AddLine(ImVec2(cx - r * 0.55f, cy - r * 0.25f), ImVec2(cx + r * 0.45f, cy + r * 0.75f), c, th);
        }
    } else {
        // Chevron: pointing down while expanded (click to fold), right while
        // collapsed (click to unfold).
        if (active) {
            dl->AddLine(ImVec2(cx - r, cy - r * 0.45f), ImVec2(cx, cy + r * 0.5f), c, th * 1.4f);
            dl->AddLine(ImVec2(cx, cy + r * 0.5f), ImVec2(cx + r, cy - r * 0.45f), c, th * 1.4f);
        } else {
            dl->AddLine(ImVec2(cx - r * 0.45f, cy - r), ImVec2(cx + r * 0.5f, cy), c, th * 1.4f);
            dl->AddLine(ImVec2(cx + r * 0.5f, cy), ImVec2(cx - r * 0.45f, cy + r), c, th * 1.4f);
        }
    }
    return clicked;
}

bool GLGizmoBase::dock_render_titlebar(const std::string &title)
{
    const float btn   = ImGui::GetFrameHeight();
    const float space = ImGui::GetStyle().ItemSpacing.x;
    const float avail = ImGui::GetContentRegionAvail().x;

    // The title fills the row up to the two buttons. It is NOT a collapse
    // trigger: a stray click on the title while working on the canvas folded
    // the whole panel and read as a crash. Only the chevron folds it.
    const float title_w = std::max(1.0f, avail - 2.0f * btn - 2.0f * space);

    const ImVec2 title_origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##gizmo_dock_title", ImVec2(title_w, btn));
    const bool   title_hover  = ImGui::IsItemHovered();

    // Draw the title text over its own hit area, vertically centred in the row.
    {
        ImDrawList *dl   = ImGui::GetWindowDrawList();
        ImVec4      col  = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        if (title_hover)
            col.w = std::min(1.0f, col.w * 1.2f);
        const ImVec2 ts = ImGui::CalcTextSize(title.c_str());
        dl->PushClipRect(title_origin, ImVec2(title_origin.x + title_w, title_origin.y + btn), true);
        dl->AddText(ImVec2(title_origin.x, title_origin.y + 0.5f * (btn - ts.y)), ImGui::GetColorU32(col),
                    title.c_str());
        dl->PopClipRect();
    }

    bool toggled_collapse = false;

    ImGui::SameLine(0.f, space);
    if (render_dock_icon_button("##gizmo_dock_chevron", /*pin=*/false, /*active=*/!m_collapsed,
                                m_collapsed ? _L("Expand panel") : _L("Collapse panel")))
        toggled_collapse = true;

    ImGui::SameLine(0.f, space);
    if (render_dock_icon_button("##gizmo_dock_pin", /*pin=*/true, /*active=*/m_docked,
                                m_docked ? _L("Undock panel") : _L("Dock panel to the right"))) {
        m_docked = !m_docked;
        store_dock_state();
        // A candidate measured under the dock state we are leaving must not
        // get confirmed against the first measurement taken under the new one
        // - the two are not the same layout.
        m_dock_width.reset_pending();
        // The window's rect changes this frame; ask for one more so it is drawn
        // in its new place without waiting for the next mouse move.
        m_imgui->set_requires_extra_frame();
        m_parent.set_as_dirty();
    }

    if (toggled_collapse) {
        m_collapsed = !m_collapsed;
        m_imgui->set_requires_extra_frame();
        m_parent.set_as_dirty();
    }

    if (!m_collapsed)
        ImGui::Separator();

    // Whether the body follows this frame. m_collapsed alone is not enough: the
    // toggles above flip it mid-frame, so on the frame the panel is expanded it
    // already reads "not collapsed" while the body is still absent - measuring
    // then would record the title row as the panel's width.
    m_dock_body_rendered = !m_collapsed && !toggled_collapse;

    return !m_collapsed;
}

void GLGizmoBase::register_grabbers_for_picking()
{
    for (size_t i = 0; i < m_grabbers.size(); ++i) {
        m_grabbers[i].register_raycasters_for_picking((m_group_id >= 0) ? m_group_id : i);
    }
}

void GLGizmoBase::unregister_grabbers_for_picking()
{
    for (size_t i = 0; i < m_grabbers.size(); ++i) {
        m_grabbers[i].unregister_raycasters_for_picking();
    }
}

void GLGizmoBase::render_grabbers(const BoundingBoxf3& box) const
{
#if ENABLE_FIXED_GRABBER
    render_grabbers((float)(GLGizmoBase::Grabber::FixedGrabberSize));
#else
    render_grabbers((float)((box.size().x() + box.size().y() + box.size().z()) / 3.0));
#endif
}

void GLGizmoBase::render_grabbers(float size) const
{
    render_grabbers(0, m_grabbers.size() - 1, size, false);
}

void GLGizmoBase::render_grabbers(size_t first, size_t last, float size, bool force_hover) const
{
    GLShaderProgram* shader = wxGetApp().get_shader("gouraud_light");
    if (shader == nullptr)
        return;
    shader->start_using();
    shader->set_uniform("emission_factor", 0.1f);
    glsafe(::glDisable(GL_CULL_FACE));
    for (size_t i = first; i <= last; ++i) {
        if (m_grabbers[i].enabled)
            m_grabbers[i].render(force_hover ? true : m_hover_id == (int)i, size);
    }
    glsafe(::glEnable(GL_CULL_FACE));
    shader->stop_using();
}

// help function to process grabbers
// call start_dragging, stop_dragging, on_dragging
bool GLGizmoBase::use_grabbers(const wxMouseEvent &mouse_event) {
    bool is_dragging_finished = false;
    if (mouse_event.Moving()) { 
        // it should not happen but for sure
        assert(!m_dragging);
        if (m_dragging) is_dragging_finished = true;
        else return false; 
    } 

    if (mouse_event.LeftDown()) {
        Selection &selection = m_parent.get_selection();
        if (!selection.is_empty() && m_hover_id != -1 /* &&
            (m_grabbers.empty() || m_hover_id < static_cast<int>(m_grabbers.size()))*/) {
            selection.setup_cache();

            m_dragging = true;
            for (auto &grabber : m_grabbers) grabber.dragging = false;
//            if (!m_grabbers.empty() && m_hover_id < int(m_grabbers.size()))
//                m_grabbers[m_hover_id].dragging = true;
            
            on_start_dragging();

            // Let the plater know that the dragging started
            m_parent.post_event(SimpleEvent(EVT_GLCANVAS_MOUSE_DRAGGING_STARTED));
            m_parent.set_as_dirty();
            return true;
        }
    } else if (m_dragging) {
        // when mouse cursor leave window than finish actual dragging operation
        bool is_leaving = mouse_event.Leaving();
        if (mouse_event.Dragging()) {
            Point      mouse_coord(mouse_event.GetX(), mouse_event.GetY());
            auto       ray = m_parent.mouse_ray(mouse_coord);
            UpdateData data(ray, mouse_coord);

            on_dragging(data);

            wxGetApp().obj_manipul()->set_dirty();
            m_parent.set_as_dirty();
            return true;
        }
        else if (mouse_event.LeftUp() || is_leaving || is_dragging_finished) {
            do_stop_dragging(is_leaving);
            return true;
        }
    }
    return false;
}

void GLGizmoBase::do_stop_dragging(bool perform_mouse_cleanup)
{
    for (auto& grabber : m_grabbers) grabber.dragging = false;
    m_dragging = false;

    // NOTE: This should be part of GLCanvas3D
    // Reset hover_id when leave window
    if (perform_mouse_cleanup) m_parent.mouse_up_cleanup();

    on_stop_dragging();

    // There is prediction that after draggign, data are changed
    // Data are updated twice also by canvas3D::reload_scene.
    // Should be fixed.
    m_parent.get_gizmos_manager().update_data();

    wxGetApp().obj_manipul()->set_dirty();

    // Let the plater know that the dragging finished, so a delayed
    // refresh of the scene with the background processing data should
    // be performed.
    m_parent.post_event(SimpleEvent(EVT_GLCANVAS_MOUSE_DRAGGING_FINISHED));
    // updates camera target constraints
    m_parent.refresh_camera_scene_box();
}

std::string GLGizmoBase::format(float value, unsigned int decimals) const
{
    return Slic3r::string_printf("%.*f", decimals, value);
}

void GLGizmoBase::set_dirty() {
    m_dirty = true;
}

void GLGizmoBase::render_input_window(float x, float y, float bottom_limit)
{
    on_render_input_window(x, y, bottom_limit);
    if (m_first_input_window_render) {
        // imgui windows that don't have an initial size needs to be processed once to get one
        // and are not rendered in the first frame
        // so, we forces to render another frame the first time the imgui window is shown
        // https://github.com/ocornut/imgui/issues/2949
        m_parent.set_as_dirty();
        m_parent.request_extra_frame();
        m_first_input_window_render = false;
    }
}



std::string GLGizmoBase::get_name(bool include_shortcut) const
{
    int key = get_shortcut_key();
    std::string out = on_get_name();
    if (include_shortcut && key >= WXK_CONTROL_A && key <= WXK_CONTROL_Z)
        out += std::string(" [") + char(int('A') + key - int(WXK_CONTROL_A)) + "]";
    return out;
}

} // namespace GUI
} // namespace Slic3r
