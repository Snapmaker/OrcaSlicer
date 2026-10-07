#ifndef slic3r_GLGizmoSVG_hpp_
#define slic3r_GLGizmoSVG_hpp_

// Include GLGizmoBase.hpp before I18N.hpp as it includes some libigl code,
// which overrides our localization "L" macro.
#include "GLGizmoBase.hpp"
#include "GLGizmoRotate.hpp"
#include "slic3r/GUI/SurfaceDrag.hpp"
#include "slic3r/GUI/GLTexture.hpp"
#include "slic3r/Utils/RaycastManager.hpp"
#include "slic3r/GUI/IconManager.hpp"

#include <optional>
#include <memory>
#include <atomic>

#include "libslic3r/Emboss.hpp"
#include "libslic3r/CodeEmboss.hpp"
#include "libslic3r/SimpleShape.hpp"
#include "libslic3r/ImageTrace.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Model.hpp"

#include <imgui/imgui.h>
#include <glad/gl.h>

namespace Slic3r{
class ModelVolume;
enum class ModelVolumeType : int;
}

namespace Slic3r::GUI {

struct Texture{
    unsigned id{0};
    unsigned width{0};
    unsigned height{0};
};

class GLGizmoSVG : public GLGizmoBase
{
public:
    explicit GLGizmoSVG(GLCanvas3D &parent);

    /// <summary>
    /// Create new embossed text volume by type on position of mouse
    /// </summary>
    /// <param name="volume_type">Object part / Negative volume / Modifier</param>
    /// <param name="mouse_pos">Define position of new volume</param>
    /// <returns>True on succesfull start creation otherwise False</returns>
    bool create_volume(ModelVolumeType volume_type, const Vec2d &mouse_pos); // first open file dialog

    /// <summary>
    /// Create new text without given position
    /// </summary>
    /// <param name="volume_type">Object part / Negative volume / Modifier</param>
    /// <returns>True on succesfull start creation otherwise False</returns>
    bool create_volume(ModelVolumeType volume_type); // first open file dialog

    /// <summary>
    /// Create volume from already selected svg file
    /// </summary>
    /// <param name="svg_file">File path</param>
    /// <param name="mouse_pos">Position on screen where to create volume</param>
    /// <param name="volume_type">Object part / Negative volume / Modifier</param>
    /// <returns>True on succesfull start creation otherwise False</returns>
    bool create_volume(std::string_view svg_file, const Vec2d &mouse_pos, ModelVolumeType volume_type = ModelVolumeType::MODEL_PART);
    bool create_volume(std::string_view svg_file, ModelVolumeType volume_type = ModelVolumeType::MODEL_PART);

    /// <summary>
    /// Ask user for QR code / barcode and create its parts (dark, light, logo) as SVG volumes
    /// </summary>
    /// <param name="volume_type">Object part / Negative volume / Modifier, INVALID means new object</param>
    /// <param name="mouse_pos">Position on screen where to create volumes, when not set it is near the selection</param>
    /// <returns>True on succesfull start creation otherwise False</returns>
    bool create_code(ModelVolumeType volume_type, const std::optional<Vec2d> &mouse_pos = {});

    /// <summary>
    /// Ask user for simple shape (circle, square, star, ...) and create it as SVG volume
    /// </summary>
    /// <param name="volume_type">Object part / Negative volume / Modifier, INVALID means new object</param>
    /// <param name="mouse_pos">Position on screen where to create volume, when not set it is near the selection</param>
    /// <returns>True on succesfull start creation otherwise False</returns>
    bool create_shape(ModelVolumeType volume_type, const std::optional<Vec2d> &mouse_pos = {});

    /// <summary>
    /// Trace a PNG / JPG image into shapes (one SVG volume per colour level)
    /// </summary>
    /// <param name="volume_type">Object part / Negative volume / Modifier, INVALID means new object</param>
    /// <param name="mouse_pos">Position on screen where to create volumes, when not set it is near the selection</param>
    /// <param name="image_path">Image to trace (dropped file), empty = ask for the file.
    /// A dropped image which is not over an object creates a new object.</param>
    /// <returns>True on succesfull start creation otherwise False</returns>
    bool create_image(ModelVolumeType volume_type, const std::optional<Vec2d> &mouse_pos = {}, const std::string &image_path = {});

    /// <summary>
    /// Check whether volume is object containing only emboss volume
    /// </summary>
    /// <param name="volume">Pointer to volume</param>
    /// <returns>True when object otherwise False</returns>
    static bool is_svg_object(const ModelVolume &volume);

    /// <summary>
    /// Check whether volume has emboss data
    /// </summary>
    /// <param name="volume">Pointer to volume</param>
    /// <returns>True when constain emboss data otherwise False</returns>
    static bool is_svg(const ModelVolume &volume);

protected:
    bool on_init() override;
    std::string on_get_name() const override;
    void on_render() override;
    void on_register_raycasters_for_picking() override;
    void on_unregister_raycasters_for_picking() override;
    void on_render_input_window(float x, float y, float bottom_limit) override;
    bool on_is_activable() const override { return true; }
    bool on_is_selectable() const override { return false; }
    void on_set_state() override;    
    void data_changed(bool is_serializing) override; // selection changed
    void on_set_hover_id() override{ m_rotate_gizmo.set_hover_id(m_hover_id); }
    void on_enable_grabber(unsigned int id) override { m_rotate_gizmo.enable_grabber(); }
    void on_disable_grabber(unsigned int id) override { m_rotate_gizmo.disable_grabber(); }
    void on_start_dragging() override;
    void on_stop_dragging() override;
    void on_dragging(const UpdateData &data) override;    

    /// <summary>
    /// Rotate by text on dragging rotate grabers
    /// </summary>
    /// <param name="mouse_event">Information about mouse</param>
    /// <returns>Propagete normaly return false.</returns>
    bool on_mouse(const wxMouseEvent &mouse_event) override;

    bool wants_enter_leave_snapshots() const override;
    std::string get_gizmo_entering_text() const override;
    std::string get_gizmo_leaving_text() const override;
    std::string get_action_snapshot_name() const override;
private:
    void set_volume_by_selection();
    void reset_volume();

    // create volume from text - main functionality
    bool process(bool make_snapshot = true);
    void close();
    void draw_window();
    void draw_preview();
    void draw_filename();
    void draw_depth();
    void draw_size();
    void draw_use_surface();
    void draw_distance();
    void draw_rotation();
    void draw_mirroring();
    void draw_face_the_camera();
    void draw_model_type();
    void draw_code();
    void draw_simple_shape();
    void edit_simple_shape();
    void draw_image_trace();
    void edit_image_trace();

    // Parts of QR code / barcode and levels of a traced image keep the same transformation and surface projection
    void sync_code_parts();
    void edit_code();
    // Start job to recreate mesh of other code part than edited one
    bool start_code_part_update(ModelVolume &volume, EmbossShape &&shape);

    // process mouse event
    bool on_mouse_for_rotation(const wxMouseEvent &mouse_event);
    bool on_mouse_for_translate(const wxMouseEvent &mouse_event);

    void volume_transformation_changed();
    
    struct GuiCfg;
    std::unique_ptr<const GuiCfg> m_gui_cfg;

    // actual selected only one volume - with emboss data
    ModelVolume *m_volume = nullptr;

    // Is used to edit eboss and send changes to job
    // Inside volume is current state of shape WRT Volume
    EmbossShape m_volume_shape; // copy from m_volume for edit

    // same index as volumes in 
    std::vector<std::string> m_shape_warnings;

    // When work with undo redo stack there could be situation that 
    // m_volume point to unexisting volume so One need also objectID
    ObjectID m_volume_id;

    // cancel for previous update of volume to cancel finalize part
    std::shared_ptr<std::atomic<bool>> m_job_cancel = nullptr;

    // Rotation gizmo
    GLGizmoRotate m_rotate_gizmo;
    std::optional<float> m_angle;
    std::optional<float> m_distance;

    // Value is set only when dragging rotation to calculate actual angle
    std::optional<float> m_rotate_start_angle;

    // TODO: it should be accessible by other gizmo too.
    // May be move to plater?
    RaycastManager m_raycast_manager;
    
    // When true keep up vector otherwise relative rotation
    bool m_keep_up = true;

    // Keep size aspect ratio when True.
    bool m_keep_ratio = true;

    // Keep data about dragging only during drag&drop
    std::optional<SurfaceDrag> m_surface_drag;

    // For volume on scaled objects
    std::optional<float> m_scale_width;
    std::optional<float> m_scale_height;
    std::optional<float> m_scale_depth;
    void calculate_scale();
    float get_scale_for_tolerance();

    // keep SVG data rendered on GPU
    Texture m_texture;

    // bounding box of shape
    // Note: Scaled mm to int value by m_volume_shape.scale
    BoundingBox m_shape_bb; 

    std::string m_filename_preview;

    IconManager m_icon_manager;
    IconManager::VIcons m_icons;

    // Set when edited volume is a simple shape (circle, star, ...)
    std::optional<SimpleShapeParams> m_simple_shape;

    // Set when edited volume is part of QR code / barcode
    std::optional<CodeEmbossMeta> m_code;
    // Set when edited volume is a level of a traced image (without the stored source image)
    std::optional<ImageTraceMeta> m_trace;
    // Transformation and surface projection of edited volume, which is already copied to other parts
    Transform3d m_code_synced_tr = Transform3d::Identity();
    bool        m_code_synced_use_surface = false;
    // Other than code parts are in object to project on
    bool m_code_can_use_surface = false;
    // cancel of jobs which update other parts of code
    std::vector<std::shared_ptr<std::atomic<bool>>> m_code_job_cancels;

    // only temporary solution
    static const std::string M_ICON_FILENAME;
};
} // namespace Slic3r::GUI

#endif // slic3r_GLGizmoSVG_hpp_
