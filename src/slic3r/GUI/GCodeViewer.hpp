#ifndef slic3r_GCodeViewer_hpp_
#define slic3r_GCodeViewer_hpp_

// EDGE (libvgcode stage 3): the G-code viewer renders through libvgcode (PrusaSlicer 2.8, via
// OrcaSlicer #10735). Taken from OrcaSlicer c833ccdf6f (2026-09-17, before #15674 / #15809) and
// adapted to EdgeSlicer; EdgeSlicer additions are marked EDGE.

#include "3DScene.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/CostEstimate.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "IMSlider.hpp"
#include "GLModel.hpp"
#include "I18N.hpp"

#include <boost/iostreams/device/mapped_file.hpp>

#include "LibVGCode/LibVGCodeWrapper.hpp"
// needed for tech VGCODE_ENABLE_COG_AND_TOOL_MARKERS
#include <libvgcode/include/Types.hpp>

#include <array>
#include <cstdint>
#include <float.h>
#include <set>
#include <unordered_set>

namespace Slic3r {

class Print;
class TriangleMesh;
class PresetBundle;

namespace GUI {

class PartPlateList;
class OpenGLManager;

static const float GCODE_VIEWER_SLIDER_SCALE = 0.6f;
static const float SLIDER_DEFAULT_RIGHT_MARGIN  = 10.0f;
static const float SLIDER_DEFAULT_BOTTOM_MARGIN = 10.0f;
static const float SLIDER_RIGHT_MARGIN = 124.0f;
static const float SLIDER_BOTTOM_MARGIN = 64.0f;
class GCodeViewer
{
public:
    struct SequentialView
    {
#if ENABLE_ACTUAL_SPEED_DEBUG
        struct ActualSpeedImguiWidget
        {
            std::pair<float, float> y_range = { 0.0f, 0.0f };
            std::vector<std::pair<float, ColorRGBA>> levels;
            struct Item
            {
              float pos{ 0.0f };
              float speed{ 0.0f };
              bool internal{ false };
            };
            std::vector<Item> data;
            int plot(const char* label, const std::array<float, 2>& frame_size = { 0.0f, 0.0f });
        };
#endif // ENABLE_ACTUAL_SPEED_DEBUG

        class Marker
        {
            GLModel m_model;
            Vec3f m_world_position;
            // for seams, the position of the marker is on the last endpoint of the toolpath containing it
            // the offset is used to show the correct value of tool position in the "ToolPosition" window
            // see implementation of render() method
            Vec3f m_world_offset;
            float m_z_offset{ 0.0f };
            // z offset of the model
            float m_model_z_offset{ 0.5f };
            bool m_visible{ true };
            bool m_is_dark = false;
            float m_scale_factor{ 1.0f };
#if ENABLE_ACTUAL_SPEED_DEBUG
            ActualSpeedImguiWidget m_actual_speed_imgui_widget;
#endif // ENABLE_ACTUAL_SPEED_DEBUG

        public:
            float m_scale = 1.0f;

            void init(std::string filename);

            const BoundingBoxf3& get_bounding_box() const { return m_model.get_bounding_box(); }

            void set_world_position(const Vec3f& position) { m_world_position = position; }
            void set_world_offset(const Vec3f& offset) { m_world_offset = offset; }
            void set_z_offset(float z_offset) { m_z_offset = z_offset; }

#if ENABLE_ACTUAL_SPEED_DEBUG
            void set_actual_speed_y_range(const std::pair<float, float>& y_range) {
                m_actual_speed_imgui_widget.y_range = y_range;
            }
            void set_actual_speed_levels(const std::vector<std::pair<float, ColorRGBA>>& levels) {
                m_actual_speed_imgui_widget.levels = levels;
            }
            void set_actual_speed_data(const std::vector<ActualSpeedImguiWidget::Item>& data) {
                m_actual_speed_imgui_widget.data = data;
            }
#endif // ENABLE_ACTUAL_SPEED_DEBUG

            bool is_visible() const { return m_visible; }
            void set_visible(bool visible) { m_visible = visible; }

            void render(int canvas_width, int canvas_height, const libvgcode::EViewType& view_type);
            void render_position_window(const libvgcode::Viewer* viewer, int canvas_width, int canvas_height, const libvgcode::EViewType& view_type);
            void on_change_color_mode(bool is_dark) { m_is_dark = is_dark; }
        };

        class GCodeWindow
        {
            struct Line
            {
                std::string command;
                std::string parameters;
                std::string comment;
            };
            bool m_is_dark = false;
            uint64_t m_selected_line_id{ 0 };
            size_t m_last_lines_size{ 0 };
            std::string m_filename;
            boost::iostreams::mapped_file_source m_file;
            // map for accessing data in file by line number
            std::vector<size_t> m_lines_ends;
            // current visible lines
            std::vector<Line> m_lines;

        public:
            float m_scale = 1.0f;
            GCodeWindow() = default;
            ~GCodeWindow() { stop_mapping_file(); }
            void load_gcode(const std::string& filename, const std::vector<size_t> &lines_ends);
            void reset() {
                stop_mapping_file();
                m_lines_ends.clear();
                m_lines_ends.shrink_to_fit();
                m_lines.clear();
                m_lines.shrink_to_fit();
                m_filename.clear();
                m_filename.shrink_to_fit();
            }

            //BBS: GUI refactor: add canvas size
            //void render(float top, float bottom, uint64_t curr_line_id) const;
            void render(float top, float bottom, float right, uint64_t curr_line_id) const;
            void on_change_color_mode(bool is_dark) { m_is_dark = is_dark; }

            void stop_mapping_file();
        };

        Marker marker;
        GCodeWindow gcode_window;
        float m_scale = 1.0;
        bool m_show_marker = false;
        void render(const bool has_render_path, float legend_height, const libvgcode::Viewer* viewer, uint32_t gcode_id, int canvas_width, int canvas_height, int right_margin, const libvgcode::EViewType& view_type);
    };
    // helper to render shells
    struct Shells
    {
        GLVolumeCollection volumes;
        bool               visible{false};
        // BBS: always load shell when preview
        int  print_id{-1};
        int  print_modify_count{-1};
        bool previewing{false};
    };
    //BBS
    ConflictResultOpt m_conflict_result;
    Shells            m_shells;

private:
    std::vector<int> m_plater_extruder;
    bool m_gl_data_initialized{ false };
    unsigned int m_last_result_id{ 0 };
    //BBS: save m_gcode_result as well
    const GCodeProcessorResult* m_gcode_result{ nullptr };
    std::array<unsigned int, static_cast<size_t>(EMoveType::Count)> m_move_type_counts{};
    std::array<std::array<float, static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count)>, static_cast<size_t>(EMoveType::Count)> m_move_type_times{};
    std::array<float, static_cast<size_t>(EMoveType::Count)> m_move_type_distances{};
    //BBS: add only gcode mode
    bool m_only_gcode_in_preview {false};

    // bounding box of toolpaths
    BoundingBoxf3 m_paths_bounding_box;
    // bounding box of toolpaths + marker tools
    BoundingBoxf3 m_max_bounding_box;
    //BBS: add shell bounding box
    BoundingBoxf3 m_shell_bounding_box;
    float m_max_print_height{ 0.0f };
    float m_z_offset{ 0.0f };

    ConfigOptionMode m_user_mode;
    bool m_fold = {false};

    size_t m_extruders_count{ 0 };
    std::vector<float> m_filament_diameters;
    std::vector<float> m_filament_densities;
    SequentialView m_sequential_view;
    IMSlider* m_moves_slider;
    IMSlider* m_layers_slider;
#if VGCODE_ENABLE_COG_AND_TOOL_MARKERS
    // whether or not to render the cog model with fixed screen size
    bool m_cog_marker_fixed_screen_size{ true };
    float m_cog_marker_size{ 1.0f };
    bool m_tool_marker_fixed_screen_size{ false };
    float m_tool_marker_size{ 1.0f };
#endif // VGCODE_ENABLE_COG_AND_TOOL_MARKERS

    /*BBS GUI refactor, store displayed items in color scheme combobox */
    std::vector<libvgcode::EViewType> view_type_items;
    std::vector<std::string> view_type_items_str;
    int       m_view_type_sel = 0;
    int       m_last_extruder_count_default_applied{0};  // 0=unset, 1=single, 2+=multi
    std::vector<EMoveType> options_items;

    bool m_legend_visible{ true };
    bool m_legend_enabled{ true };

    float m_legend_height{ 0.0f };
    PrintEstimatedStatistics m_print_statistics;
    // EDGE (#358): cost of the loaded G-code (current plate), and whether the breakdown under "Cost" is unfolded.
    CostBreakdown m_cost_breakdown;
    mutable bool m_cost_expanded{ false };
    mutable bool m_all_plates_cost_expanded{ false };
    std::array<float, 2> m_detected_point_sizes = { 0.0f, 0.0f };
    GCodeProcessorResult::SettingsIds m_settings_ids;

    std::vector<CustomGCode::Item> m_custom_gcode_per_print_z;

    bool m_contained_in_bed{ true };
    // EDGE (#642 memory guard): set when the toolpaths were not loaded because memory ran low while
    // slicing; libvgcode then holds only a per-layer summary (layers, times, roles) and draws nothing.
    mutable bool m_no_render_path { false };
    bool m_is_dark = false;

    libvgcode::Viewer m_viewer;
    bool m_loaded_as_preview{ false };

public:
    GCodeViewer();
    ~GCodeViewer();

    void on_change_color_mode(bool is_dark);
    float m_scale = 1.0;
    void set_scale(float scale = 1.0);
    void init(ConfigOptionMode mode, Slic3r::PresetBundle* preset_bundle);
    void update_by_mode(ConfigOptionMode mode);

    // extract rendering data from the given parameters
    //BBS: add only gcode mode
    // EDGE (#642): skip_toolpaths loads only the per-layer summary (no toolpaths on the GPU), for the
    // memory guard's "continue" answer.
    void load_as_gcode(const GCodeProcessorResult& gcode_result, const Print& print, const std::vector<std::string>& str_tool_colors,
        const std::vector<std::string>& str_color_print_colors, const BuildVolume& build_volume,
        const std::vector<BoundingBoxf3>& exclude_bounding_box, ConfigOptionMode mode, bool only_gcode = false, bool skip_toolpaths = false);
    void load_as_preview(libvgcode::GCodeInputData&& data);
    void update_shells_color_by_extruder(const DynamicPrintConfig* config);
    void set_shell_transparency(float alpha = 0.15f);

    void reset();
    //BBS: always load shell at preview
    void reset_shell();
    void load_shells(const Print& print, bool initialized, bool force_previewing = false);
    void set_shells_on_preview(bool is_previewing) { m_shells.previewing = is_previewing; }
    //BBS: add all plates filament statistics
    void render_all_plates_stats(const std::vector<const GCodeProcessorResult*>& gcode_result_list, bool show = true) const;
    //BBS: GUI refactor: add canvas width and height
    void render(int canvas_width, int canvas_height, int right_margin);
    bool has_data() const { return !m_viewer.get_extrusion_roles().empty(); }
    // EDGE (phone / remote preview, GLCanvas3D::render_gcode_preview_image): the toolpaths alone,
    // with whatever camera is current on the Plater; no legend, sliders or other ImGui.
    void render_toolpaths_for_capture();
    // EDGE: id of the GCodeProcessorResult loaded last (the phone preview checks it is current).
    unsigned int loaded_result_id() const { return m_last_result_id; }
    // EDGE (#642): true while only the per-layer summary is loaded (memory guard).
    bool toolpaths_skipped() const { return m_no_render_path; }

    bool can_export_toolpaths() const;
    std::vector<int> get_plater_extruder();

    const float                get_max_print_height() const { return m_max_print_height; }
    const BoundingBoxf3& get_paths_bounding_box() const { return m_paths_bounding_box; }
    const BoundingBoxf3& get_max_bounding_box() const { return m_max_bounding_box; }
    const BoundingBoxf3& get_shell_bounding_box() const { return m_shell_bounding_box; }
    std::vector<double> get_layers_zs() const {
        const std::vector<float> zs = m_viewer.get_layers_zs();
        std::vector<double> ret;
        std::transform(zs.begin(), zs.end(), std::back_inserter(ret), [](float z) { return static_cast<double>(z); });
        return ret;
    }
    std::vector<float> get_layers_times() const { return m_viewer.get_layers_estimated_times(); }

    const libvgcode::Interval& get_layers_z_range() const { return m_viewer.get_layers_view_range(); }

    const SequentialView& get_sequential_view() const { return m_sequential_view; }
    void update_sequential_view_current(unsigned int first, unsigned int last);

    /* BBS IMSlider */
    IMSlider *get_moves_slider() { return m_moves_slider; }
    IMSlider *get_layers_slider() { return m_layers_slider; }
    void enable_moves_slider(bool enable) const;
    void update_moves_slider(bool set_to_max = false);
    void update_layers_slider_mode();

    const libvgcode::Interval& get_gcode_view_full_range() const { return m_viewer.get_view_full_range(); }
    const libvgcode::Interval& get_gcode_view_enabled_range() const { return m_viewer.get_view_enabled_range(); }
    const libvgcode::Interval& get_gcode_view_visible_range() const { return m_viewer.get_view_visible_range(); }
    const libvgcode::PathVertex& get_gcode_vertex_at(size_t id) const { return m_viewer.get_vertex_at(id); }

    bool is_contained_in_bed() const { return m_contained_in_bed; }
    //BBS: add only gcode mode
    bool is_only_gcode_in_preview() const { return m_only_gcode_in_preview; }

    void set_view_type(libvgcode::EViewType type) {
        m_viewer.set_view_type(type);
    }
    void reset_visible(libvgcode::EViewType type) {
        if (type == libvgcode::EViewType::FeatureType) {
            auto roles = m_viewer.get_extrusion_roles();
            for (size_t i = 0; i < roles.size(); ++i) {
                auto role = roles[i];
                if (!m_viewer.is_extrusion_role_visible(role)) {
                    m_viewer.toggle_extrusion_role_visibility(role);
                }
            }
        }
    }

    libvgcode::EViewType get_view_type() const { return m_viewer.get_view_type(); }

    // ORCA: darken the layers not scrubbed to while using the preview layer slider
    void set_dim_previous_layers(bool value) { m_viewer.set_dim_previous_layers(value); }
    bool is_dim_previous_layers() const { return m_viewer.is_dim_previous_layers(); }
    // ORCA: brightness of those darkened layers, 1.0 = unchanged, 0.0 = black
    void set_dim_previous_layers_brightness(float value) { m_viewer.set_dim_previous_layers_brightness(value); }
    float get_dim_previous_layers_brightness() const { return m_viewer.get_dim_previous_layers_brightness(); }

    void set_layers_z_range(const std::array<unsigned int, 2>& layers_z_range);

    bool is_legend_shown() const { return m_legend_visible && m_legend_enabled; }
    void show_legend(bool show) { m_legend_visible = show; }
    bool is_legend_enabled() const { return m_legend_enabled; }
    void enable_legend(bool enable) { m_legend_enabled = enable; }
    float get_legend_height() { return m_legend_height; }

    void export_toolpaths_to_obj(const char* filename) const;

    size_t get_extruders_count() { return m_extruders_count; }
    void push_combo_style();
    void pop_combo_style();

    void invalidate_legend() { /*TODO: m_legend_resizer.reset();*/ }

#if VGCODE_ENABLE_COG_AND_TOOL_MARKERS
    float get_cog_marker_scale_factor() const { return m_viewer.get_cog_marker_scale_factor(); }
    void set_cog_marker_scale_factor(float factor) { return m_viewer.set_cog_marker_scale_factor(factor); }
#endif // VGCODE_ENABLE_COG_AND_TOOL_MARKERS

private:
    //BBS: always load shell at preview
    //void load_shells(const Print& print);
    void render_toolpaths();
    void render_shells(int canvas_width, int canvas_height);

    //BBS: GUI refactor: add canvas size
    void render_legend(float &legend_height, int canvas_width, int canvas_height, int right_margin);
    // EDGE (#358): the one "Cost" section of every statistics view (current plate, Feature type
    // summary, all plates): the total on one line, a click on [+] unfolds filament per slot (model /
    // support / flush / tower), machine time and the total. value_x 0 puts the value right after the label.
    void render_cost_section(const CostBreakdown &cost, const std::string &label, float value_x, float window_padding,
                             bool &expanded, const char *imgui_id) const;
    void render_slider(int canvas_width, int canvas_height);
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GCodeViewer_hpp_
