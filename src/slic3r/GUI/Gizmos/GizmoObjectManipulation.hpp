#ifndef slic3r_GizmoObjectManipulation_hpp_
#define slic3r_GizmoObjectManipulation_hpp_

#include <memory>

#include "libslic3r/Point.hpp"
#include "libslic3r/Geometry.hpp"
#include <float.h>

#include "slic3r/GUI/GUI_Geometry.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoAlignment.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoBase.hpp"

//#include "slic3r/GUI/GLCanvas3D.hpp"

namespace Slic3r {
namespace GUI {

class Selection;
class GLCanvas3D;

class GizmoObjectManipulation
{
public:
    static const double in_to_mm;
    static const double mm_to_in;
    static const double g_to_oz;
    static const double oz_to_g;

    struct Cache
    {
        Vec3d position;
        Vec3d position_rounded;
        Vec3d rotation;
        Vec3d rotation_rounded;
        Vec3d absolute_rotation;
        Vec3d absolute_rotation_rounded;
        Vec3d scale;
        Vec3d scale_rounded;
        Vec3d size;
        Vec3d size_rounded;

        wxString move_label_string;
        wxString rotate_label_string;
        wxString scale_label_string;

        Cache() { reset(); }
        void reset()
        {
            position = position_rounded = Vec3d(DBL_MAX, DBL_MAX, DBL_MAX);
            rotation = rotation_rounded = Vec3d(DBL_MAX, DBL_MAX, DBL_MAX);
            scale = scale_rounded = Vec3d(DBL_MAX, DBL_MAX, DBL_MAX);
            size = size_rounded = Vec3d(DBL_MAX, DBL_MAX, DBL_MAX);
            move_label_string = wxString();
            rotate_label_string = wxString();
            scale_label_string = wxString();
        }
        bool is_valid() const { return position != Vec3d(DBL_MAX, DBL_MAX, DBL_MAX); }
    };

    Cache m_cache;

    bool            m_imperial_units { false };
    bool            m_use_object_cs{false};
    // Mirroring buttons and their current state
    //enum MirrorButtonState {
    //    mbHidden,
    //    mbShown,
    //    mbActive
    //};
    //std::array<std::pair<ScalableButton*, MirrorButtonState>, 3> m_mirror_buttons;

    // Needs to be updated from OnIdle?
    bool            m_dirty = false;
    // Cached labels for the delayed update, not localized!
    std::string     m_new_title_string;
    std::string     m_new_move_label_string;
	std::string     m_new_rotate_label_string;
	std::string     m_new_scale_label_string;
    std::string     m_new_unit_string;
    Vec3d           m_new_position;
    Vec3d           m_new_rotation;
    Vec3d           m_new_absolute_rotation;
    Vec3d           m_new_scale;
    Vec3d           m_new_size;
    Vec3d           m_unscale_size;
    Vec3d           m_buffered_position;
    Vec3d           m_buffered_rotation;
    Vec3d           m_buffered_absolute_rotation;
    Vec3d           m_buffered_scale;
    Vec3d           m_buffered_size;
    Vec3d           cs_center;
    bool            m_new_enabled {true};
    bool            m_uniform_scale {true};
    // Does the object manipulation panel work in World or Local coordinates?
    ECoordinatesType m_coordinates_type{ECoordinatesType::World};

    bool            m_show_reset_0_rotation{false};
    bool            m_show_clear_rotation { false };
    bool            m_show_clear_scale { false };
    bool            m_show_drop_to_bed { false };
    enum class RotateType { None, Relative, Absolute
    };
    RotateType m_last_rotate_type{RotateType::None}; // 0:no input 1:relative 2:absolute

protected:
    float last_move_input_window_width = 0.0f;
    float last_rotate_input_window_width = 0.0f;
    float last_scale_input_window_width = 0.0f;

    GLGizmoAlignment* m_alignment_helper{nullptr};

public:
    GizmoObjectManipulation(GLCanvas3D& glcanvas);
    ~GizmoObjectManipulation() { delete m_alignment_helper; }

    // BBS: align/distribute helpers shown in the Move panel
    GLGizmoAlignment::AlignType m_align_type{GLGizmoAlignment::AlignType::NONE};
    enum class AlignChoiceType {
        AlignPartOrObject = 0,
        AlignParent,
    };
    AlignChoiceType   m_align_choice_type{AlignChoiceType::AlignParent};
    bool              m_align_to_parent_node{false};
    // Per-axis "origin" of the Align row (which point of the moved item goes to the target).
    // Persisted in AppConfig as align_origin_x/y/z; loaded lazily on the first render.
    AlignMath::Origin m_align_origin[3]{AlignMath::Origin::Auto, AlignMath::Origin::Auto, AlignMath::Origin::Auto};
    bool              m_align_origin_loaded{false};
    static std::vector<std::string> align_origin_labels(int axis);
    static wxString                 align_face_name(int axis, AlignMath::Side side);
    // Align selected: which item stays put. The mode is persisted (align_anchor_mode), an explicit
    // item is not.
    AlignMath::AnchorMode m_align_anchor_mode{AlignMath::AnchorMode::Last};
    bool                  m_align_anchor_item_set{false};
    int                   m_align_anchor_object_idx{-1};
    int                   m_align_anchor_instance_idx{-1};
    int                   m_align_anchor_volume_idx{-1};
    GLGizmoAlignment::AlignOptions make_align_options() const;
    void save_align_anchor_mode();
    bool render_align_anchor_combo(ImGuiWrapper *imgui_wrapper, float x, float y, float frame_width);
    bool render_spanning_combo(ImGuiWrapper *                  imgui_wrapper,
                               const char *                    id,
                               const std::vector<std::string> &lines,
                               size_t &                        selection_idx,
                               const std::string &             preview,
                               bool                            tinted,
                               float                           x,
                               float                           y,
                               float                           frame_width,
                               const wxString &                tooltip);
    void load_align_origins();
    void save_align_origin(int axis);
    // One origin dropdown at window-local (x, y), `frame_width` wide. Returns true when changed.
    bool render_align_origin_combo(ImGuiWrapper *imgui_wrapper, int axis, float x, float y, float frame_width);
    GLGizmoAlignment* get_alignment_helper() { return m_alignment_helper; }
    void show_align_icon(ImGuiWrapper *imgui_wrapper,
                         float         max_tooltip_width,
                         GLGizmoAlignment::AlignType align_type,
                         int             icon,
                         float           icon_size,
                         const wxString &function_tip,
                         const wxString &enable_tip,
                         bool            show_enable_tip = false);

    bool        IsShown();
    void        UpdateAndShow(const bool show);
    void update_ui_from_settings();

    void        set_dirty() { m_dirty = true; }
	// Called from the App to update the UI if dirty.
	void		update_if_dirty();

    void        set_uniform_scaling(const bool uniform_scale);
    bool        get_uniform_scaling() const { return m_uniform_scale; }
    void        set_use_object_cs(bool flag){ if (m_use_object_cs != flag) m_use_object_cs = flag; }
    bool        get_use_object_cs() { return m_use_object_cs; }
    // Does the object manipulation panel work in World or Local coordinates?
    void        set_coordinates_type(ECoordinatesType type);
    ECoordinatesType get_coordinates_type() const { return m_coordinates_type; }
    bool        is_world_coordinates() const { return m_coordinates_type == ECoordinatesType::World; }
    bool        is_instance_coordinates() const { return m_coordinates_type == ECoordinatesType::Instance; }
    bool        is_local_coordinates() const { return m_coordinates_type == ECoordinatesType::Local; }

    const Cache& get_cache() {return m_cache; }
    void reset_cache() { m_cache.reset(); }

    void limit_scaling_ratio(Vec3d &scaling_factor) const;
    void on_change(const std::string& opt_key, int axis, double new_value);
    bool render_combo(ImGuiWrapper *imgui_wrapper, const std::string &label, const std::vector<std::string> &lines, size_t &selection_idx, float label_width, float item_width);
    void do_render_move_window(ImGuiWrapper *imgui_wrapper, std::string window_name, float x, float y, float bottom_limit, GLGizmoBase *dock_owner = nullptr);
    void do_render_rotate_window(ImGuiWrapper *imgui_wrapper, std::string window_name, float x, float y, float bottom_limit, GLGizmoBase *dock_owner = nullptr);
    void do_render_scale_input_window(ImGuiWrapper* imgui_wrapper, std::string window_name, float x, float y, float bottom_limit, GLGizmoBase *dock_owner = nullptr);
    float max_unit_size(int number, Vec3d &vec1, Vec3d &vec2,std::string str);
    bool reset_button(ImGuiWrapper *imgui_wrapper, float caption_max, float unit_size, float space_size, float end_text_size);
    bool reset_zero_button(ImGuiWrapper *imgui_wrapper, float caption_max, float unit_size, float space_size, float end_text_size);
    bool bbl_checkbox(const wxString &label, bool &value);

    void show_move_tooltip_information(ImGuiWrapper *imgui_wrapper, float caption_max, float x, float y);
    void show_rotate_tooltip_information(ImGuiWrapper *imgui_wrapper, float caption_max, float x, float y);
    void show_scale_tooltip_information(ImGuiWrapper *imgui_wrapper, float caption_max, float x, float y);
    void set_init_rotation(const Geometry::Transformation &value);

private:
    void reset_settings_value();
    void update_settings_value(const Selection& selection);
    void update_buffered_value();

    // Show or hide scale/rotation reset buttons if needed
    void update_reset_buttons_visibility();
    //Show or hide mirror buttons
    //void update_mirror_buttons_visibility();

    // change values
    void change_position_value(int axis, double value);
    void change_rotation_value(int axis, double value);
    void change_absolute_rotation_value(int axis, double value);
    void change_scale_value(int axis, double value);
    void change_size_value(int axis, double value);
    void do_scale(int axis, const Vec3d &scale) const;
    void reset_position_value();
    void reset_rotation_value(bool reset_relative);
    void reset_scale_value();

    GLCanvas3D& m_glcanvas;
    unsigned int m_last_active_item { 0 };
    std::map<std::string, wxString> m_desc_move;
    std::map<std::string, wxString> m_desc_rotate;
    std::map<std::string, wxString> m_desc_scale;
    Vec3d                           m_init_rotation;
    Transform3d                     m_init_rotation_scale_tran;
};

}}

#endif // slic3r_GizmoObjectManipulation_hpp_