#ifndef slic3r_3DScene_hpp_
#define slic3r_3DScene_hpp_

#include "libslic3r/libslic3r.h"
#include "libslic3r/Point.hpp"
#include "libslic3r/Line.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Color.hpp"
// BBS
#include "libslic3r/ObjectID.hpp"

#include "GLModel.hpp"
#include "GLShader.hpp"
#include "MeshUtils.hpp"

#include <functional>
#include <optional>

#ifndef NDEBUG
#define HAS_GLSAFE
#endif // NDEBUG

#ifdef HAS_GLSAFE
    extern void glAssertRecentCallImpl(const char *file_name, unsigned int line, const char *function_name);
    inline void glAssertRecentCall() { glAssertRecentCallImpl(__FILE__, __LINE__, __FUNCTION__); }
    #define glsafe(cmd) do { cmd; glAssertRecentCallImpl(__FILE__, __LINE__, __FUNCTION__); } while (false)
    #define glcheck() do { glAssertRecentCallImpl(__FILE__, __LINE__, __FUNCTION__); } while (false)
#else // HAS_GLSAFE
    inline void glAssertRecentCall() { }
    #define glsafe(cmd) cmd
    #define glcheck()
#endif // HAS_GLSAFE
extern std::vector<Slic3r::ColorRGBA> get_extruders_colors();
extern float                          FullyTransparentMaterialThreshold;
extern float                          FullTransparentModdifiedToFixAlpha;
extern Slic3r::ColorRGBA              adjust_color_for_rendering(const Slic3r::ColorRGBA &colors);


namespace Slic3r {
namespace GUI {
    class Size;
    class Camera;
}

class SLAPrintObject;
enum  SLAPrintObjectStep : unsigned int;
class BuildVolume;
class DynamicPrintConfig;
class ExtrusionPath;
class ExtrusionMultiPath;
class ExtrusionLoop;
class ExtrusionEntity;
class ExtrusionEntityCollection;
class ModelObject;
class ModelVolume;
class GLShaderProgram;
enum ModelInstanceEPrintVolumeState : unsigned char;

using ModelObjectPtrs = std::vector<ModelObject*>;

// Return appropriate color based on the ModelVolume.
extern ColorRGBA color_from_model_volume(const ModelVolume& model_volume);

class GLVolume {
public:
    std::string name;

    static ColorRGBA DISABLED_COLOR;
    static ColorRGBA SLA_SUPPORT_COLOR;
    static ColorRGBA SLA_PAD_COLOR;
    static ColorRGBA NEUTRAL_COLOR;
    static ColorRGBA UNPRINTABLE_COLOR;
    static std::array<ColorRGBA, 5> MODEL_COLOR;
    static ColorRGBA MODEL_MIDIFIER_COL;
    static ColorRGBA MODEL_NEGTIVE_COL;
    static ColorRGBA SUPPORT_ENFORCER_COL;
    static ColorRGBA SUPPORT_BLOCKER_COL;
    static ColorRGBA MODEL_HIDDEN_COL;
    // Precise Seam modifier colours (60% alpha), as upstream Orca #12974.
    static ColorRGBA PRECISE_SEAM_CENTER_COL;
    static ColorRGBA PRECISE_SEAM_LEFT_COL;
    static ColorRGBA PRECISE_SEAM_RIGHT_COL;
    static ColorRGBA PRECISE_SEAM_ENFORCED_COL;
    static ColorRGBA PRECISE_SEAM_NEUTRAL_COL;
    static ColorRGBA PRECISE_SEAM_BLOCKED_COL;

    static void update_render_colors();
    static void load_render_colors();
    static ColorRGBA brighten_color(const ColorRGBA& color, float multiplier = 1.25f);

    static float explosion_ratio;
    static float last_explosion_ratio;

    enum EHoverState : unsigned char
    {
        HS_None,
        HS_Hover,
        HS_Select,
        HS_Deselect
    };

    GLVolume(float r = 1.f, float g = 1.f, float b = 1.f, float a = 1.f);
    GLVolume(const ColorRGBA& color) : GLVolume(color.r(), color.g(), color.b(), color.a()) {}
    virtual ~GLVolume() = default;

    // BBS
protected:
    Geometry::Transformation m_instance_transformation;
    Geometry::Transformation m_volume_transformation;
    // BBS
    Vec3d m_offset_to_assembly{ 0.0, 0.0, 0.0 };

    // Shift in z required by sla supports+pad
    double        m_sla_shift_z;
    // Bounding box of this volume, in unscaled coordinates.
    std::optional<BoundingBoxf3> m_transformed_bounding_box;
    // Convex hull of the volume, if any.
    std::shared_ptr<const TriangleMesh> m_convex_hull;
    // Bounding box of this volume, in unscaled coordinates.
    std::optional<BoundingBoxf3> m_transformed_convex_hull_bounding_box;
    // Bounding box of the non sinking part of this volume, in unscaled coordinates.
    std::optional<BoundingBoxf3> m_transformed_non_sinking_bounding_box;

    class SinkingContours
    {
        static const float HalfWidth;
        GLVolume& m_parent;
        GUI::GLModel m_model;
        BoundingBoxf3 m_old_box;
        Vec3d m_shift{ Vec3d::Zero() };

    public:
        SinkingContours(GLVolume& volume) : m_parent(volume) {}
        void render();

    private:
        void update();
    };

    SinkingContours m_sinking_contours;

public:
    // Color of the triangles / quads held by this volume.
    ColorRGBA color;
    // Color used to render this volume.
    ColorRGBA render_color;

    struct CompositeID {
        CompositeID(int object_id, int volume_id, int instance_id) : object_id(object_id), volume_id(volume_id), instance_id(instance_id) {}
        CompositeID() : object_id(-1), volume_id(-1), instance_id(-1) {}
        // Object ID, which is equal to the index of the respective ModelObject in Model.objects array.
        int             object_id;
        // Volume ID, which is equal to the index of the respective ModelVolume in ModelObject.volumes array.
        // If negative, it is an index of a geometry produced by the PrintObject for the respective ModelObject,
        // and which has no associated ModelVolume in ModelObject.volumes. For example, SLA supports.
        // Volume with a negative volume_id cannot be picked independently, it will pick the associated instance.
        int             volume_id;
        // Instance ID, which is equal to the index of the respective ModelInstance in ModelObject.instances array.
        int             instance_id;
		bool operator==(const CompositeID &rhs) const { return object_id == rhs.object_id && volume_id == rhs.volume_id && instance_id == rhs.instance_id; }
		bool operator!=(const CompositeID &rhs) const { return ! (*this == rhs); }
		bool operator< (const CompositeID &rhs) const
			{ return object_id < rhs.object_id || (object_id == rhs.object_id && (volume_id < rhs.volume_id || (volume_id == rhs.volume_id && instance_id < rhs.instance_id))); }
    };
    CompositeID         composite_id;
    // Fingerprint of the source geometry. For ModelVolumes, it is the ModelVolume::ID and ModelInstanceID,
    // for generated volumes it is the timestamp generated by PrintState::invalidate() or PrintState::set_done(),
    // and the associated ModelInstanceID.
    // Valid geometry_id should always be positive.
    std::pair<size_t, size_t> geometry_id;
    // An ID containing the extruder ID (used to select color).
    int                 	extruder_id;

    size_t                  model_object_ID{0};

    // Various boolean flags.
    struct {
	    // Is this object selected?
	    bool                selected : 1;
	    // Is this object disabled from selection?
	    bool                disabled : 1;
	    // Is this object printable?
	    bool                printable : 1;
        // Is this object visible(in assemble view)?
	    bool                visible : 1;
	    // Whether or not this volume is active for rendering
	    bool                is_active : 1;
        // Ultra: hidden this frame by "Hide other plates while moving" - the volume sits on a
        // plate other than the one being worked on. Purely a render-time flag, recomputed for
        // every volume each frame (so it restores itself), never serialized, never undone.
        bool                plate_focus_hidden : 1;
	    // Whether or not to use this volume when applying zoom_to_volumes()
	    bool                zoom_to_volumes : 1;
	    // Wheter or not this volume is enabled for outside print volume detection in shader.
	    bool                shader_outside_printer_detection_enabled : 1;
	    // Wheter or not this volume is outside print volume.
	    bool                is_outside : 1;
        bool                partly_inside : 1;
        // Snapmaker: Whether or not this volume is too close to boundary for spiral lift
        bool                near_boundary_for_spiral_lift : 1;
	    // Wheter or not this volume has been generated from a modifier
	    bool                is_modifier : 1;
	    // Wheter or not this volume has been generated from the wipe tower
        bool                is_wipe_tower : 1;
	    // Wheter or not this volume has been generated from an extrusion path
	    bool                is_extrusion_path : 1;
	    // Wheter or not to always render this volume using its own alpha
	    bool                force_transparent : 1;
	    // Whether or not always use the volume's own color (not using SELECTED/HOVER/DISABLED/OUTSIDE)
	    bool                force_native_color : 1;
        // Whether or not render this volume in neutral
        bool                force_neutral_color : 1;
        // Whether or not to force rendering of sinking contours
        bool                force_sinking_contours : 1;
        // Ultra: render semi-transparent and excluded from picking (per-object Ghost view mode)
        bool                ghost : 1;
        // Is render for picking
        bool                picking : 1;
    };

    // Is mouse or rectangle selection over this object to select/deselect it ?
    EHoverState         	hover;

    GUI::GLModel            model;
    // raycaster used for picking
    std::unique_ptr<GUI::MeshRaycaster> mesh_raycaster;
    // BBS
    mutable std::vector<GUI::GLModel> mmuseg_models;
    mutable ObjectBase::Timestamp       mmuseg_ts;

    // Ranges of triangle and quad indices to be rendered.
    std::pair<size_t, size_t>   tverts_range;

    // If the qverts or tverts contain thick extrusions, then offsets keeps pointers of the starts
    // of the extrusions per layer.
    std::vector<coordf_t>       print_zs;
    // Offset into qverts & tverts, or offsets into indices stored into an OpenGL name_index_buffer.
    std::vector<size_t>         offsets;

    // Bounding box of this volume, in unscaled coordinates.
    BoundingBoxf3 bounding_box() const {
        return this->model.get_bounding_box();
    }

    void set_color(const ColorRGBA& rgba)        { color = rgba; }
    void set_render_color(const ColorRGBA& rgba) { render_color = rgba; }
    // Sets render color in dependence of current state
    void set_render_color();
    // set color according to model volume
    void set_color_from_model_volume(const ModelVolume& model_volume);

    const Geometry::Transformation& get_instance_transformation() const { return m_instance_transformation; }
    void set_instance_transformation(const Geometry::Transformation& transformation) { m_instance_transformation = transformation; set_bounding_boxes_as_dirty(); }
    void set_instance_transformation(const Transform3d& transform) { m_instance_transformation.set_matrix(transform); set_bounding_boxes_as_dirty(); }

    Vec3d get_instance_offset() const { return m_instance_transformation.get_offset(); }
    double get_instance_offset(Axis axis) const { return m_instance_transformation.get_offset(axis); }

    void set_instance_offset(const Vec3d& offset) { m_instance_transformation.set_offset(offset); set_bounding_boxes_as_dirty(); }
    void set_instance_offset(Axis axis, double offset) { m_instance_transformation.set_offset(axis, offset); set_bounding_boxes_as_dirty(); }

    Vec3d get_instance_rotation() const { return m_instance_transformation.get_rotation(); }
    double get_instance_rotation(Axis axis) const { return m_instance_transformation.get_rotation(axis); }

    void set_instance_rotation(const Vec3d& rotation) { m_instance_transformation.set_rotation(rotation); set_bounding_boxes_as_dirty(); }
    void set_instance_rotation(Axis axis, double rotation) { m_instance_transformation.set_rotation(axis, rotation); set_bounding_boxes_as_dirty(); }

    Vec3d get_instance_scaling_factor() const { return m_instance_transformation.get_scaling_factor(); }
    double get_instance_scaling_factor(Axis axis) const { return m_instance_transformation.get_scaling_factor(axis); }

    void set_instance_scaling_factor(const Vec3d& scaling_factor) { m_instance_transformation.set_scaling_factor(scaling_factor); set_bounding_boxes_as_dirty(); }
    void set_instance_scaling_factor(Axis axis, double scaling_factor) { m_instance_transformation.set_scaling_factor(axis, scaling_factor); set_bounding_boxes_as_dirty(); }

    Vec3d get_instance_mirror() const { return m_instance_transformation.get_mirror(); }
    double get_instance_mirror(Axis axis) const { return m_instance_transformation.get_mirror(axis); }

    void set_instance_mirror(const Vec3d& mirror) { m_instance_transformation.set_mirror(mirror); set_bounding_boxes_as_dirty(); }
    void set_instance_mirror(Axis axis, double mirror) { m_instance_transformation.set_mirror(axis, mirror); set_bounding_boxes_as_dirty(); }

    const Geometry::Transformation& get_volume_transformation() const { return m_volume_transformation; }
    void set_volume_transformation(const Geometry::Transformation& transformation) { m_volume_transformation = transformation; set_bounding_boxes_as_dirty(); }
    void set_volume_transformation(const Transform3d& transform) { m_volume_transformation.set_matrix(transform); set_bounding_boxes_as_dirty(); }

    Vec3d get_volume_offset() const { return m_volume_transformation.get_offset(); }
    double get_volume_offset(Axis axis) const { return m_volume_transformation.get_offset(axis); }

    void set_volume_offset(const Vec3d& offset) { m_volume_transformation.set_offset(offset); set_bounding_boxes_as_dirty(); }
    void set_volume_offset(Axis axis, double offset) { m_volume_transformation.set_offset(axis, offset); set_bounding_boxes_as_dirty(); }

    Vec3d get_volume_rotation() const { return m_volume_transformation.get_rotation(); }
    double get_volume_rotation(Axis axis) const { return m_volume_transformation.get_rotation(axis); }

    void set_volume_rotation(const Vec3d& rotation) { m_volume_transformation.set_rotation(rotation); set_bounding_boxes_as_dirty(); }
    void set_volume_rotation(Axis axis, double rotation) { m_volume_transformation.set_rotation(axis, rotation); set_bounding_boxes_as_dirty(); }

    Vec3d get_volume_scaling_factor() const { return m_volume_transformation.get_scaling_factor(); }
    double get_volume_scaling_factor(Axis axis) const { return m_volume_transformation.get_scaling_factor(axis); }

    void set_volume_scaling_factor(const Vec3d& scaling_factor) { m_volume_transformation.set_scaling_factor(scaling_factor); set_bounding_boxes_as_dirty(); }
    void set_volume_scaling_factor(Axis axis, double scaling_factor) { m_volume_transformation.set_scaling_factor(axis, scaling_factor); set_bounding_boxes_as_dirty(); }

    Vec3d get_volume_mirror() const { return m_volume_transformation.get_mirror(); }
    double get_volume_mirror(Axis axis) const { return m_volume_transformation.get_mirror(axis); }

    void set_volume_mirror(const Vec3d& mirror) { m_volume_transformation.set_mirror(mirror); set_bounding_boxes_as_dirty(); }
    void set_volume_mirror(Axis axis, double mirror) { m_volume_transformation.set_mirror(axis, mirror); set_bounding_boxes_as_dirty(); }

    double get_sla_shift_z() const { return m_sla_shift_z; }
    void set_sla_shift_z(double z) { m_sla_shift_z = z; }

    void set_convex_hull(std::shared_ptr<const TriangleMesh> convex_hull) { m_convex_hull = std::move(convex_hull); }
    void set_convex_hull(const TriangleMesh &convex_hull) { m_convex_hull = std::make_shared<const TriangleMesh>(convex_hull); }
    void set_convex_hull(TriangleMesh &&convex_hull) { m_convex_hull = std::make_shared<const TriangleMesh>(std::move(convex_hull)); }

    void set_offset_to_assembly(const Vec3d& offset) { m_offset_to_assembly = offset; set_bounding_boxes_as_dirty(); }
    Vec3d get_offset_to_assembly() { return m_offset_to_assembly; }

    int                 object_idx() const { return this->composite_id.object_id; }
    int                 volume_idx() const { return this->composite_id.volume_id; }
    int                 instance_idx() const { return this->composite_id.instance_id; }

    Transform3d         world_matrix() const;
    bool                is_left_handed() const;

    const BoundingBoxf3& transformed_bounding_box() const;
    // non-caching variant
    BoundingBoxf3        transformed_convex_hull_bounding_box(const Transform3d &trafo) const;
    // caching variant
    const BoundingBoxf3& transformed_convex_hull_bounding_box() const;
    // non-caching variant
    BoundingBoxf3        transformed_non_sinking_bounding_box(const Transform3d& trafo) const;
    // caching variant
    const BoundingBoxf3& transformed_non_sinking_bounding_box() const;
    // convex hull
    const TriangleMesh*  convex_hull() const { return m_convex_hull.get(); }

    bool                empty() const { return this->model.is_empty(); }

    void                set_range(double low, double high);

    virtual void        render();

    //BBS: add simple render function for thumbnail
    void simple_render(GLShaderProgram* shader, ModelObjectPtrs& model_objects, std::vector<ColorRGBA>& extruder_colors, bool ban_light =false);

    void                set_bounding_boxes_as_dirty() {
        m_transformed_bounding_box.reset();
        m_transformed_convex_hull_bounding_box.reset();
        m_transformed_non_sinking_bounding_box.reset();
    }

    bool                is_sla_support() const;
    bool                is_sla_pad() const;

    bool                is_sinking() const;
    bool                is_below_printbed() const;
    void                render_sinking_contours();

    // Return an estimate of the memory consumed by this class.
    size_t 				cpu_memory_used() const {
        return sizeof(*this) + this->model.cpu_memory_used() + this->print_zs.capacity() * sizeof(coordf_t) +
               this->offsets.capacity() * sizeof(size_t);
    }
    // Return an estimate of the memory held by GPU vertex buffers.
    size_t 				gpu_memory_used() const { return this->model.gpu_memory_used(); }
    size_t 				total_memory_used() const { return this->cpu_memory_used() + this->gpu_memory_used(); }
};

// BBS
class GLWipeTowerVolume : public GLVolume {
public:
    GLWipeTowerVolume(const std::vector<ColorRGBA>& colors);
    void render() override;

    std::vector<GUI::GLModel> model_per_colors;
    bool                              IsTransparent();

private:
    std::vector<ColorRGBA> m_colors;
};

typedef std::vector<GLVolume*> GLVolumePtrs;
typedef std::pair<GLVolume*, std::pair<unsigned int, double>> GLVolumeWithIdAndZ;
typedef std::vector<GLVolumeWithIdAndZ> GLVolumeWithIdAndZList;

class GLVolumeCollection
{
public:
    enum class ERenderType : unsigned char
    {
        Opaque,
        Transparent,
        All
    };

    struct PrintVolume
    {
        // see: Bed3D::EShapeType
        int type{ 0 };
        // data contains:
        // Rectangle:
        //   [0] = min.x, [1] = min.y, [2] = max.x, [3] = max.y
        // Circle:
        //   [0] = center.x, [1] = center.y, [3] = radius
        std::array<float, 4> data;
        //   [0] = min z, [1] = max z
        std::array<float, 2> zs;
    };

private:
    PrintVolume m_print_volume;
    PrintVolume m_render_volume;

    // z range for clipping in shaders
    std::array<float, 2> m_z_range;

    // plane coeffs for clipping in shaders
    std::array<double, 4> m_clipping_plane;

    // plane coeffs for render volumes with different colors in shaders
    // used by cut gizmo
    std::array<double, 4> m_color_clip_plane;
    bool m_use_color_clip_plane{ false };
    std::array<ColorRGBA, 2> m_color_clip_plane_colors{ ColorRGBA::RED(), ColorRGBA::BLUE() };
    // Per-side visibility for the two colour-clip halves (cut gizmo, phase 2).
    // 1.0 = solid, 0 < a < 1 = ghosted, NEGATIVE = hidden (the shader discards).
    // 1.0/1.0 is "both halves solid", i.e. exactly the pre-phase-2 behaviour.
    std::array<float, 2> m_color_clip_plane_alphas{ 1.f, 1.f };

    // Curved cut sheet (cut gizmo, Surface = Curved). When the texture id is
    // non-zero the two halves are split by the height field it holds instead of
    // by m_color_clip_plane, evaluated per fragment in the cut plane's frame.
    unsigned int m_curved_sheet_tex{ 0 };
    Transform3d  m_curved_sheet_matrix{ Transform3d::Identity() };
    // Half extents of the sheet's RECTANGULAR domain, (u,v), in the plane frame.
    Vec2f        m_curved_sheet_half_size{ 1.f, 1.f };
    // Non-zero when the texture holds f encoded as (f/range + 1)/2 rather than
    // f itself - the GL 2.1 fallback, where GL_LUMINANCE clamps to [0,1].
    float        m_curved_sheet_range{ 0.f };

    // DRAWN cut field (cut gizmo, Surface = Draw; 2026-09-12). A ruled strip is not a
    // height field over the plane, so the sheet's 2D texture cannot describe it. What
    // can is the question the split itself asks - IS THE POINT INSIDE THE CUTTER SOLID
    // - baked into a 3D sign field over the part's bounding box in the plane frame.
    // NEGATIVE is the upper half, matching m_color_clip_plane's own convention.
    unsigned int m_draw_field_tex{ 0 };
    Transform3d  m_draw_field_matrix{ Transform3d::Identity() };
    // The field's box in the plane frame: its origin and its size, so the shader can
    // normalise a plane-frame point into [0,1]^3.
    Vec3f        m_draw_field_origin{ Vec3f::Zero() };
    Vec3f        m_draw_field_size{ Vec3f::Ones() };

    struct Slope
    {
        // toggle for slope rendering
        bool active{ false };//local active
        bool isGlobalActive{false};
        float normal_z;
    };

    Slope m_slope;
    bool m_show_sinking_contours = false;

public:
    GLVolumePtrs volumes;

    GLVolumeCollection() {
        set_default_slope_normal_z();

        //BBS init render volume
        m_render_volume.type = -1;
    }
    ~GLVolumeCollection() { clear(); }

    std::vector<int> load_object(
        const ModelObject 		*model_object,
        int                      obj_idx,
        const std::vector<int>	&instance_idxs,
        const std::string 		&color_by,
        bool 					 opengl_initialized,
        bool                     need_raycaster = true);

    int load_object_volume(
        const ModelObject *model_object,
        int                obj_idx,
        int                volume_idx,
        int                instance_idx,
        const std::string &color_by,
        bool 			   opengl_initialized,
        bool               in_assemble_view = false,
        bool               use_loaded_id = false,
        bool               need_raycaster = true);
    // Load SLA auxiliary GLVolumes (for support trees or pad).
    void load_object_auxiliary(
        const SLAPrintObject           *print_object,
        int                             obj_idx,
        // pairs of <instance_idx, print_instance_idx>
        const std::vector<std::pair<size_t, size_t>>& instances,
        SLAPrintObjectStep              milestone,
        // Timestamp of the last change of the milestone
        size_t                          timestamp);

    int load_wipe_tower_preview(
        int obj_idx, float pos_x, float pos_y, float width, float depth, float height, float rotation_angle, bool size_unknown, float brim_width);

    GLVolume* new_toolpath_volume(const ColorRGBA& rgba);
    GLVolume* new_nontoolpath_volume(const ColorRGBA& rgba);

    int get_selection_support_threshold_angle(bool&) const;
    // Render the volumes by OpenGL.
    //BBS: add outline drawing logic
    void render(ERenderType                           type,
                bool                                  disable_cullface,
                const GUI::Camera&                    camera,
                const GUI::Size&                      cnv_size,
                std::function<bool(const GLVolume &)> filter_func   = std::function<bool(const GLVolume &)>(),
                bool                                  partly_inside_enable =true
           ) const;

    // Clear the geometry
    void clear() { for (auto *v : volumes) delete v; volumes.clear(); }

    bool empty() const { return volumes.empty(); }
    void set_range(double low, double high) { for (GLVolume *vol : this->volumes) vol->set_range(low, high); }

    void set_print_volume(const PrintVolume& print_volume) { m_print_volume = print_volume; }

    void set_z_range(float min_z, float max_z) { m_z_range[0] = min_z; m_z_range[1] = max_z; }
    void set_clipping_plane(const std::array<double, 4>& coeffs) { m_clipping_plane = coeffs; }

    const std::array<float, 2>& get_z_range() const { return m_z_range; }
    const std::array<double, 4>& get_clipping_plane() const { return m_clipping_plane; }

    void set_use_color_clip_plane(bool use) { m_use_color_clip_plane = use; }
    void set_color_clip_plane(const Vec3d& cp_normal, double offset) {
        for (int i = 0; i < 3; ++i)
            m_color_clip_plane[i] = -cp_normal[i];
        m_color_clip_plane[3] = offset;
    }
    void set_color_clip_plane_colors(const std::array<ColorRGBA, 2>& colors) { m_color_clip_plane_colors = colors; }
    // Per-side visibility for the two colour-clip halves: 1.0 solid, 0 < a < 1
    // ghosted, negative hidden (the shader discards those fragments). Reset to
    // { 1, 1 } to go back to two solid halves.
    void set_color_clip_plane_alphas(float side_1, float side_2) { m_color_clip_plane_alphas = { side_1, side_2 }; }
    // world -> cut plane frame, and the sheet's half extents (u,v) in that
    // frame. Pass tex == 0 to go back to the plain flat colour split.
    // range == 0 means the texture holds f directly; otherwise it holds
    // (f/range + 1)/2 and the shader undoes that.
    void set_curved_color_clip(unsigned int tex, const Transform3d& world_to_plane, double half_size_u, double half_size_v, double range = 0.) {
        m_curved_sheet_tex       = tex;
        m_curved_sheet_matrix    = world_to_plane;
        m_curved_sheet_half_size = Vec2f(float(half_size_u), float(half_size_v));
        m_curved_sheet_range     = float(range);
    }
    // world -> cut plane frame, plus the field's box in that frame (origin and size).
    // Pass tex == 0 to go back to the plain flat colour split. The two fields are
    // mutually exclusive in practice (a cut is Curved or Draw, never both), and the
    // shader gives the DRAWN one precedence so a stale sheet texture cannot win.
    void set_draw_color_clip(unsigned int tex, const Transform3d& world_to_plane, const Vec3d& origin, const Vec3d& size) {
        m_draw_field_tex    = tex;
        m_draw_field_matrix = world_to_plane;
        m_draw_field_origin = origin.cast<float>();
        m_draw_field_size   = Vec3f(float(size.x() > 1e-6 ? size.x() : 1.0),
                                    float(size.y() > 1e-6 ? size.y() : 1.0),
                                    float(size.z() > 1e-6 ? size.z() : 1.0));
    }

    bool is_slope_GlobalActive() const { return m_slope.isGlobalActive; }
    bool is_slope_active() const { return m_slope.active; }
    void set_slope_active(bool active) { m_slope.active = active; }
    void set_slope_GlobalActive(bool active) { m_slope.isGlobalActive = active; }

    float get_slope_normal_z() const { return m_slope.normal_z; }
    void set_slope_normal_z(float normal_z) { m_slope.normal_z = normal_z; }
    void set_default_slope_normal_z() { m_slope.normal_z = -::cos(Geometry::deg2rad(90.0f - 45.0f)); }
    void set_show_sinking_contours(bool show) { m_show_sinking_contours = show; }

    // returns true if all the volumes are completely contained in the print volume
    // returns the containment state in the given out_state, if non-null
    bool check_outside_state(const Slic3r::BuildVolume& build_volume, ModelInstanceEPrintVolumeState* out_state) const;
    void reset_outside_state();
    // Snapmaker: 检查是否有任何 volume 靠近边界（螺旋抬升风险）
    bool is_any_volume_near_boundary_for_spiral_lift() const;

    void update_colors_by_extruder(const DynamicPrintConfig *config, bool is_update_alpha = true);

    // Returns a vector containing the sorted list of all the print_zs of the volumes contained in this collection
    std::vector<double> get_current_print_zs(bool active_only) const;

    // Return an estimate of the memory consumed by this class.
    size_t 				cpu_memory_used() const;
    // Return an estimate of the memory held by GPU vertex buffers.
    size_t 				gpu_memory_used() const;
    size_t 				total_memory_used() const { return this->cpu_memory_used() + this->gpu_memory_used(); }
    // Return CPU, GPU and total memory log line.
    std::string         log_memory_info() const;
    
    void set_transparency(float alpha);

private:
    GLVolumeCollection(const GLVolumeCollection &other);
    GLVolumeCollection& operator=(const GLVolumeCollection &);
};

GLVolumeWithIdAndZList volumes_to_render(const GLVolumePtrs& volumes, GLVolumeCollection::ERenderType type, const Transform3d& view_matrix, std::function<bool(const GLVolume&)> filter_func = nullptr);

struct _3DScene
{
    static void thick_lines_to_verts(const Lines& lines, const std::vector<double>& widths, const std::vector<double>& heights, bool closed, double top_z, GUI::GLModel::Geometry& geometry);
    static void thick_lines_to_verts(const Lines3& lines, const std::vector<double>& widths, const std::vector<double>& heights, bool closed, GUI::GLModel::Geometry& geometry);
    static void extrusionentity_to_verts(const ExtrusionPath& extrusion_path, float print_z, const Point& copy, GUI::GLModel::Geometry& geometry);
    static void extrusionentity_to_verts(const ExtrusionLoop& extrusion_loop, float print_z, const Point& copy, GUI::GLModel::Geometry& geometry);
    static void extrusionentity_to_verts(const ExtrusionMultiPath& extrusion_multi_path, float print_z, const Point& copy, GUI::GLModel::Geometry& geometry);
    static void extrusionentity_to_verts(const ExtrusionEntityCollection& extrusion_entity_collection, float print_z, const Point& copy, GUI::GLModel::Geometry& geometry);
    static void extrusionentity_to_verts(const ExtrusionEntity* extrusion_entity, float print_z, const Point& copy, GUI::GLModel::Geometry& geometry);
};

}

#endif
