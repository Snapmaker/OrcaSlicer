#pragma once

#include "libslic3r/Point.hpp"

#include <array>
#include <cfloat>
#include <vector>

namespace Slic3r {
namespace GUI {

class GLModel;

// The last 3D scene pass, kept as a texture for frames that only rebuild the overlay.
class SceneCache
{
public:
    // Scene pass inputs that change without any frame request.
    struct Key
    {
        std::array<unsigned int, 2> size{ { 0, 0 } };
        Transform3d view_matrix{ Transform3d::Identity() };
        Transform3d projection_matrix{ Transform3d::Identity() };
        // Hovered volumes that draw their sinking contour, hovered plate icons, hovered gizmo grabber.
        std::vector<int> sinking_hover_volume_idxs;
        // Volumes whose sinking contour is forced on top after a gizmo move. The flag is cleared on
        // the next mouse event (GLCanvas3D::on_mouse), so it changes without a frame request, the
        // same way hover does.
        std::vector<int> forced_sinking_volume_idxs;
        std::vector<int> hover_plate_icon_idxs;
        int gizmo_hover_id{ -1 };
        bool render_preview{ true };
        // Arguments of GLVolumeCollection::update_lod() that can change without marking the scene
        // dirty. A replay skips update_lod(), so keying on them makes it miss, not a stale frame.
        bool   lod_allowed{ false };
        float  lod_pixel_scale{ 1.f };
        double lod_pin_above_z{ DBL_MAX };

        bool operator == (const Key& other) const {
            return size == other.size && render_preview == other.render_preview &&
                   gizmo_hover_id == other.gizmo_hover_id &&
                   lod_allowed == other.lod_allowed &&
                   lod_pixel_scale == other.lod_pixel_scale &&
                   lod_pin_above_z == other.lod_pin_above_z &&
                   sinking_hover_volume_idxs == other.sinking_hover_volume_idxs &&
                   forced_sinking_volume_idxs == other.forced_sinking_volume_idxs &&
                   hover_plate_icon_idxs == other.hover_plate_icon_idxs &&
                   view_matrix.isApprox(other.view_matrix) &&
                   projection_matrix.isApprox(other.projection_matrix);
        }
    };

    // Texture coordinate the fork's flat_texture shader derives from a quad vertex position:
    // tex_coord = blit_uv_matrix() * (x, -y, 1). Kept out of render() so that the mapping of the
    // four quad corners can be checked without an OpenGL context.
    static Matrix3f blit_uv_matrix() {
        Matrix3f m = Matrix3f::Identity();
        m(0, 0) =  0.5f;
        m(0, 2) =  0.5f;
        m(1, 1) = -0.5f;
        m(1, 2) =  0.5f;
        return m;
    }

    // Copies the bound read framebuffer, sized by key.size, and remembers key.
    void capture(Key key);
    bool matches(const Key& key) const { return m_valid && m_key == key; }
    // Draws the last capture over the whole viewport, on the given full screen quad.
    void render(GLModel& quad);
    void invalidate() { m_valid = false; }
    // Frees the texture.
    void reset();

private:
    unsigned int m_texture_id{ 0 };
    std::array<unsigned int, 2> m_texture_size{ { 0, 0 } };
    Key m_key;
    bool m_valid{ false };
};

} // namespace GUI
} // namespace Slic3r
