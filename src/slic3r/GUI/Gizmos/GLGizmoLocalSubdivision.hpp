#ifndef slic3r_GLGizmoLocalSubdivision_hpp_
#define slic3r_GLGizmoLocalSubdivision_hpp_

#include "GLGizmoPainterBase.hpp"

namespace Slic3r {
namespace GUI {

// Painting-style gizmo used to select a group of facets of the selected object
// and subdivide exactly those facets with TriangleMeshDeal::smooth_triangle_mesh_local.
// The facet selection is transient: it lives only while the gizmo is open and is
// consumed by the "Subdivide" button.
class GLGizmoLocalSubdivision : public GLGizmoPainterBase
{
public:
    GLGizmoLocalSubdivision(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);

    void render_painter_gizmo() override;

protected:
    void on_render_input_window(float x, float y, float bottom_limit) override;
    std::string on_get_name() const override;
    PainterGizmoType get_painter_type() const override;
    wxString handle_snapshot_action_name(bool shift_down, Button button_down) const override;
    std::string get_gizmo_entering_text() const override { return _u8L("Entering Local subdivision"); }
    std::string get_gizmo_leaving_text() const override { return _u8L("Leaving Local subdivision"); }
    std::string get_action_snapshot_name() const override { return _u8L("Local subdivision facet selection"); }

    // Left button paints the selection; the right button erases it.
    EnforcerBlockerType get_right_button_state_type() const override { return EnforcerBlockerType::NONE; }
    ColorRGBA get_cursor_sphere_right_button_color() const override { return { 0.f, 0.f, 0.f, 0.25f }; }

    // Opened from the object context menu only, never from the toolbar.
    bool on_is_selectable() const override { return false; }

private:
    bool on_init() override;
    // The selection is transient, so nothing is stored on the ModelVolume.
    void update_model_object() override {}
    void update_from_model_object(bool first_update = false) override;
    void on_opening() override {}
    void on_shutdown() override {}

    void apply_subdivision();
    void close();

    wchar_t m_current_tool = 0;
    bool m_detect_geometry_edge = true;
    std::map<std::string, wxString> m_desc;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoLocalSubdivision_hpp_
