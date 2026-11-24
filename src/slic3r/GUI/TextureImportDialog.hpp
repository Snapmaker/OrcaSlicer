#pragma once

#include "GUI_Utils.hpp"
#include "Widgets/ProgressDialog.hpp"
#include "libslic3r/TexturePainting.hpp"
#include "libslic3r/FilamentColorLibrary.hpp"

#include <cstddef>
#include <wx/gdicmn.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include "Widgets/PopupWindow.hpp"
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/string.h>
#include <wx/textctrl.h>
#include "Widgets/TextInput.hpp"
#include <wx/checkbox.h>
#include <wx/button.h>
#include "Widgets/Button.hpp"
#include <wx/glcanvas.h>
#include <wx/event.h>

#include <array>
#include <atomic>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>
#include <string>

class AccentSlider;
class TextInput;

namespace Slic3r { namespace GUI {

struct GapPreviewState;

wxDECLARE_EVENT(EVT_TEXTURE_COMPUTE_DONE, wxCommandEvent);
wxDECLARE_EVENT(EVT_TEXTURE_COMPUTE_PROGRESS, wxCommandEvent);
wxDECLARE_EVENT(EVT_TEXTURE_COMPUTE_ERROR, wxCommandEvent);
wxDECLARE_EVENT(EVT_TEXTURE_MESH_REPAIR_DECISION, wxCommandEvent);

enum class TextureImportState {
    Idle,
    Computing,
    Ready,
    Error
};

enum class TextureAutoMixMode {
    CMYW,
    RYBW
};

enum class TextureFilamentKind {
    ExistingPhysical,
    ExistingMixed,
    NewPhysical,
    NewMixed
};

struct TextureFilamentEntry {
    TextureFilamentKind kind{TextureFilamentKind::ExistingPhysical};
    int                 dialog_index{-1};
    size_t              project_config_index{size_t(-1)};
    std::string         color_hex;
    std::string         name;
    std::string         type;
    std::string         preset_name;
    std::vector<unsigned int> mixed_components;
    std::vector<int>          mixed_ratios;
};

struct TextureNewMixedFilament {
    int                       dialog_index{-1};
    std::string               color_hex;
    std::vector<int>          component_dialog_indices;
    std::vector<int>          ratios;
};

struct FilamentMappingRow {
    int                        cluster_id    = -1;
    std::array<std::size_t, 3> source_color  = {0, 0, 0};
    std::string                source_hex;
    int                        target_filament_idx = 0;
    wxPanel*                   source_panel  = nullptr;
    wxPanel*                   target_panel  = nullptr;
};

class FilamentSelectPopup;
// Lightweight 3D preview panel using wxGLCanvas.
// Renders the texture-import comparison view (requirement doc section 3.2 preview model):
// ONE canvas and ONE GL context split into two viewports side by side —
// the user's textured input on the left half, the simplified
// multi-color / filament-mapped result on the right half. A single shared
// camera drives both halves (mirroring is implicit), and geometry lives in
// VBOs so weak GPUs pay one context, one clear and one buffer swap per frame
// with server-side draws instead of immediate-mode vertex submission.
class TexturePreviewCanvas : public wxGLCanvas
{
public:
    enum class RenderMode { Original, MultiColor, FilamentMap };

    TexturePreviewCanvas(wxWindow* parent, const wxGLAttributes& attrs);
    ~TexturePreviewCanvas();

    // Left pane (original project): original textured geometry.
    void set_mesh_data(
        const std::vector<std::array<float, 3>>& vertices,
        const std::vector<std::array<int, 3>>&   indices);

    void set_texture_data(
        const std::vector<std::array<float, 2>>& uvs,
        const unsigned char* tex_data, int tex_w, int tex_h, int tex_channels);

    void set_texture_render_data(
        const std::vector<std::vector<unsigned char>>& tex_pixels_rgb,
        const std::vector<int>& tex_widths,
        const std::vector<int>& tex_heights,
        const std::vector<std::array<std::array<float,2>, 3>>& face_uvs,
        const std::vector<int>& face_tex_ids);

    void set_original_face_colors(const std::vector<std::array<std::size_t, 3>>& face_colors);

    // Right pane (simplified): remeshed geometry with per-face colors.
    void set_painted_mesh_data(
        const std::vector<std::array<float, 3>>& vertices,
        const std::vector<std::array<int, 3>>&   indices);
    void set_face_colors(const std::vector<std::array<std::size_t, 3>>& face_colors);
    void set_filament_color_map(const std::map<std::array<std::size_t, 3>, std::array<float, 3>>& color_map);

    // Right pane visualization: MultiColor (cluster colors) or FilamentMap.
    // The left pane always renders the original textured model.
    void set_simplified_mode(RenderMode mode);
    void set_computing_overlay(bool show);
    void reset_view();

    // DPI hook: badge textures are rendered once from the current font/scale;
    // a scale change must invalidate them so the next paint re-rasterizes.
    void invalidate_badge_textures() { m_badge_tex_dirty = true; }

    // Translucent caption badges ("Original Model" / "Simplified") overlaid on
    // the two panes (step-1 2026-11 design). Rendered as GL textures so they
    // composite over the model exactly like the reset icons.
    void set_badge_labels(const wxString& left, const wxString& right);

private:
    void on_paint(wxPaintEvent& evt);
    void on_size(wxSizeEvent& evt);
    void on_mouse(wxMouseEvent& evt);
    void ensure_gl_ready();
    void render();
    // Sets projection+modelview for one pane half: shared camera, pane-specific
    // bounding sphere (the original and the remeshed geometry differ slightly).
    void setup_pane_projection(int vp_w, int vp_h, float radius, const std::array<float, 3>& center);
    void draw_original_pane();
    void draw_simplified_pane();
    // Immediate-mode fallbacks, used only when VBOs are unavailable.
    void render_mesh(RenderMode mode);
    void render_textured_original();
    void render_reset_overlay(const wxSize& logical_size, const wxSize& viewport_size);
    // The two preview panes as equal rounded squares separated by a fixed gap
    // (step-1 design's visual split), in logical client pixels.
    std::pair<wxRect, wxRect> pane_rects() const;
    // White (surround-coloured) quarter-disc masks that round off each pane's
    // GL corners; drawn in the overlay pass after both panes.
    void render_pane_rounding(const wxSize& viewport_size, float scale_x, float scale_y);
    void render_pane_badges(const wxSize& logical_size, const wxSize& viewport_size);
    void upload_reset_icon_textures();
    unsigned int upload_reset_icon_texture(const std::string& icon_name);
    bool handle_reset_overlay_mouse(wxMouseEvent& evt);
    void upload_textures();
    void compute_smooth_normals();
    void update_bounding_box();
    void update_painted_bounding_box();
    // One reset button per pane (bottom-right of each square); both drive the
    // single shared camera, so either one resets the whole split view.
    std::vector<wxRect> reset_overlay_rects() const;
    unsigned int upload_badge_texture(const wxString& text);

    // ---- VBO cache (rebuilt lazily inside render(), with the context current) ----
    bool vbo_supported() const;
    void rebuild_original_vbos();
    void rebuild_painted_vbos();
    void rebuild_painted_color_vbo();
    void draw_original_vbos();
    void draw_painted_vbos();

    wxGLContext*  m_context        = nullptr;
    bool          m_gl_initialized = false;
    RenderMode    m_mode           = RenderMode::MultiColor; // right pane colorization

    // Single shared camera for both halves; rot_x sign follows the Z-up
    // convention of the main 3D scene.
    float   m_zoom     = 1.0f;
    float   m_rot_x    = 30.0f;
    float   m_rot_y    = 30.0f;
    float   m_pan_x    = 0.0f;
    float   m_pan_y    = 0.0f;
    wxPoint m_last_mouse_pos;
    enum class DragMode { None, Rotate, Pan };
    DragMode m_drag_mode = DragMode::None;

    std::vector<std::array<float, 3>> m_vertices;
    std::vector<std::array<int, 3>>   m_indices;
    std::vector<std::array<float, 2>> m_uvs;
    std::vector<std::array<float, 3>> m_painted_vertices;
    std::vector<std::array<int, 3>>   m_painted_indices;
    std::vector<std::array<float, 3>> m_face_colors_rgb;
    std::vector<std::array<float, 3>> m_original_face_colors_rgb;
    std::vector<std::array<float, 3>> m_filament_colors_rgb;
    std::map<std::array<std::size_t, 3>, std::array<float, 3>> m_color_map;

    unsigned int m_tex_id       = 0;
    int          m_tex_w        = 0;
    int          m_tex_h        = 0;
    int          m_tex_channels = 3;
    bool         m_tex_dirty    = false;
    std::vector<unsigned char> m_tex_data;

    std::vector<unsigned int> m_gl_tex_ids;
    std::vector<std::vector<unsigned char>> m_tex_pixels_rgb;
    std::vector<int> m_tex_widths;
    std::vector<int> m_tex_heights;
    std::vector<std::array<std::array<float,2>, 3>> m_face_uvs;
    std::vector<int> m_face_tex_ids;
    bool m_multi_tex_dirty = false;

    std::vector<std::array<float, 3>> m_vertex_normals;

    std::array<float, 3> m_center = {0, 0, 0};
    float                m_radius = 1.0f;
    std::array<float, 3> m_painted_center = {0, 0, 0};
    float                m_painted_radius = 1.0f;

    // Original-pane VBOs: one interleaved buffer per texture group. Textured
    // groups store (pos, normal, uv) and take UVs from the bound texture;
    // untextured groups store (pos, normal, color) instead.
    struct OriginalGroupVBO {
        unsigned int vbo          = 0;
        int          corner_count = 0;
        bool         has_uv       = false;
        int          tex_index    = -1; // index into m_gl_tex_ids, -1 = untextured
    };
    std::vector<OriginalGroupVBO> m_orig_group_vbos;
    bool                          m_orig_vbos_dirty = true;

    // Simplified-pane VBOs: geometry (pos, flat normal) is separate from colors
    // so remapping filaments only re-uploads the small color buffer.
    unsigned int m_painted_vbo        = 0;
    int          m_painted_corner_count = 0;
    bool         m_painted_vbo_dirty  = true;
    unsigned int m_painted_color_vbo  = 0;
    bool         m_painted_color_dirty = true;

    unsigned int m_reset_icon_tex        = 0;
    unsigned int m_reset_icon_hover_tex  = 0;
    unsigned int m_reset_icon_dark_tex   = 0;
    unsigned int m_reset_icon_dark_hover_tex = 0;
    bool         m_reset_overlay_hovered = false;
    bool         m_reset_overlay_pressed = false;

    wxString     m_badge_left_text;
    wxString     m_badge_right_text;
    unsigned int m_badge_left_tex  = 0;
    unsigned int m_badge_right_tex = 0;
    bool         m_badge_tex_dirty = true;

    bool          m_computing_overlay = false;
};


class TextureImportDialog : public DPIDialog
{
public:
    TextureImportDialog(wxWindow*                        parent,
                        const Slic3r::TexturedMesh&      textured_mesh,
                        const std::vector<TextureFilamentEntry>& filament_entries,
                        std::function<bool()>            initial_cancel_callback = {},
                        std::function<bool(int)>         initial_progress_callback = {});
    ~TextureImportDialog();

    int ShowModal() override;
    void on_dpi_changed(const wxRect& suggested_rect) override;

    Slic3r::PaintedMesh               get_painted_mesh() const;
    std::vector<Slic3r::FilamentMatch> get_matches() const;
    bool                               was_skipped() const { return m_skipped; }
    bool                               fallback_to_geometry_only() const { return m_fallback_to_geometry_only; }
    // Colors of virtual filaments that need to be created after dialog confirmation.
    // Index i corresponds to filament index (m_existing_filament_count + i).
    const std::vector<std::array<float, 4>>& get_new_filament_colors() const { return m_new_filament_colors; }
    const std::vector<std::string>& get_new_filament_preset_names() const { return m_new_filament_preset_names; }
    const std::vector<TextureNewMixedFilament>& get_new_mixed_filaments() const { return m_new_mixed_filaments; }
    const std::vector<TextureFilamentEntry>& get_filament_entries() const { return m_filament_entries; }
    size_t get_existing_filament_count() const { return m_existing_filament_count; }
    // Unique colors of the source model: per-face texture sampling for UV-textured meshes,
    // loader-baked precomputed_face_colors for vertex/MTL-colored ones. 0 until sampled.
    // Unique source-model colors (texture pixels, or face colors when the
    // loader precomputed them); see the long long member below for semantics.
    int get_original_color_count() const { return (int)std::min<long long>(m_original_color_count, std::numeric_limits<int>::max()); }
    // Unique colors of the current simplified result (= m_painted.cluster_colors.size()).
    // Updates with every recomputation; 0 before the first computation completes.
    int get_simplified_color_count() const;

private:
    void build_ui();
    void build_step_indicator(wxSizer* root_sizer);
    void build_preview_panel(wxWindow* parent, wxSizer* sizer);
    void build_params_panel(wxWindow* parent, wxSizer* sizer);
    void build_mapping_panel(wxWindow* parent, wxSizer* sizer);
    void build_bottom_buttons(wxSizer* sizer);

    // Two-step flow (2026-11 design): step 1 simplifies the texture palette,
    // step 2 maps clusters onto filaments. One window, one canvas, one state
    // machine — only the panels swap.
    enum class ImportStep { Simplify, Mapping };
    void switch_step(ImportStep step);
    // Step-dependent canvas height (square panes on step 1, short wide panes
    // on step 2 per the Figma pages).
    void apply_preview_metrics();
    void update_step_indicator();
    void update_step_buttons();

    void set_state(TextureImportState new_state);
    void update_ui_for_state();
    // Doc §3.3.1: X/Esc exit runs a confirmation before terminating the whole
    // import command; returns false when the user declines (stay in dialog).
    bool confirm_terminate_import();

    void start_computation(bool auto_color = false, bool initial = false);
    void cancel_computation();
    void on_computation_complete(wxCommandEvent& evt);
    void on_computation_progress(wxCommandEvent& evt);
    void on_computation_error(wxCommandEvent& evt);
    void on_mesh_repair_decision_required(wxCommandEvent& evt);

    void rebuild_mapping_rows();
    void do_auto_match();
    // "+ Add Material" flow (doc §3.3.4). Candidate families are ALL materials
    // the user checked in the system-materials page (visible+compatible
    // filament presets), project-loaded ones first; the official color panel
    // then offers the chosen family's colors.
    struct AddMaterialFamily {
        std::string              preset_name; // full preset name for add_virtual_filament
        std::string              label;       // display name
        Slic3r::FilamentColorInfo info;       // official color library entry
        // Snapmaker-branded family (official group; leads dropdown sort).
        bool                     official = false;
    };
    void request_add_filament(size_t row_index);
    void assign_filament_to_row(size_t row_index, int filament_idx);
    std::vector<AddMaterialFamily> collect_add_material_families();
    // Cache of collect_add_material_families(); invalidated when the
    // project preset/color state changes (see request_add_filament).
    std::vector<AddMaterialFamily> m_families_cache;
    std::string                    m_families_state_key;
    void legacy_add_material_picker(size_t row_index);
    // Reorder m_current_matches into a canonical, predictable order (ascending
    // filament_index, with unmapped entries pushed to the end). Used right
    // after the initial computation so the first view the user sees has a
    // stable, intuitive layout.
    void sort_current_matches_by_filament_index();
    std::vector<Slic3r::FilamentMatch> build_matches_from_rows() const;
    void update_filament_color_map();
    void show_filament_popup(size_t row_index);
    void dismiss_filament_popup();
    void dismiss_filament_popup_on_wheel(wxMouseEvent& evt);
    void apply_auto_standard_mix(TextureAutoMixMode mode);
    void reset_auto_mix();
    // Step-2 mix mode selector (custom dropdown: the shared ComboBox's popup
    // cannot be forced to open downward). 0 = Direct Filament Mapping,
    // 1 = CMYW Auto Mix. select_* applies the mode; set_* only syncs the UI.
    void select_mix_mode(int index);
    void set_mix_mode_ui(int index);
    void show_mix_mode_popup();
    bool add_decomposed_mixed_filament(size_t row_index);
    int  add_virtual_filament(const std::array<float, 4>& rgba, const std::string& hex,
                              const std::string& preset_name = std::string());
    int  add_virtual_mixed_filament(const std::string& color_hex,
                                    const std::vector<int>& component_dialog_indices,
                                    const std::vector<int>& ratios);
    size_t max_filament_count() const;
    bool can_add_virtual_filament() const;
    // Recomputes m_drop_warning_label visibility from m_filaments_dropped and
    // m_state. Safe to call whether or not the label has been created yet.
    // Visibility reflects ONLY the result of the most recent do_auto_match():
    // if the latest match did not drop any cluster, the label is hidden even
    // if a previous match had dropped (no historical accumulation).
    void update_drop_warning_visibility();
    void compact_used_virtual_filaments();
    int  find_closest_filament_index(const std::array<std::size_t, 3>& color) const;
    // Returns a vector indexed by dialog_index whose value is the 1-based
    // display number that mirrors the final sidebar ordering produced by
    // apply_textured_mesh_import_result (Plater.cpp): ExistingPhysical,
    // NewPhysical, ExistingMixed, NewMixed. Used so the dialog shows the
    // same IDs the sidebar will show after OK, instead of the raw
    // dialog_index + 1 (which interleaves physicals and mixeds).
    // MUST mirror ordering in apply_textured_mesh_import_result (Plater.cpp:9896).
    std::vector<int> compute_display_numbers() const;

    void on_color_preset_clicked(wxCommandEvent& evt);
    void on_color_slider_changed(wxCommandEvent& evt);
    void on_color_spin_text_changed(wxCommandEvent& evt);
    void on_smooth_slider_changed(wxCommandEvent& evt);
    void on_smooth_spin_text_changed(wxCommandEvent& evt);
    void on_gap_slider_changed(wxCommandEvent& evt);
    void on_gap_spin_text_changed(wxCommandEvent& evt);
    void on_keep_colors_toggled(wxCommandEvent& evt);
    void on_gap_spin_commit();
    void on_apply_clicked(wxCommandEvent& evt);
    void on_skip_clicked(wxCommandEvent& evt);
    void on_ok_clicked(wxCommandEvent& evt);
    // Step navigation: "Next" recomputes first when the parameters are dirty
    // (m_pending_step_advance carries the intent across the async completion).
    void on_next_clicked(wxCommandEvent& evt);
    void on_back_clicked(wxCommandEvent& evt);
    void on_reset_clicked(wxCommandEvent& evt);

    void set_color_count_value(int value, bool update_spin);
    void set_smooth_value(int value, bool update_spin);
    void set_gap_value(double value, bool update_spin);
    void preview_spin_text_value(TextInput* input, AccentSlider* slider, int& param,
                                 int min_value, int max_value, const wxString& text,
                                 std::function<void()> on_value_changed = {});
    void update_color_count_preset_buttons();

    // ---- Gap area merge (small-fragment absorption) ----
    // Distinct colors visible after gap merging: display colors first, the
    // simplified palette as fallback.
    int  visible_simplified_color_count() const;
    // Refresh the "Original N colors / Simplified M colors" caption.
    void update_color_counts_label();
    std::unique_ptr<GapPreviewState> m_gap_preview;
    std::unique_ptr<GapPreviewState>   m_pending_gap_preview;
    GapPreviewState&       gap_preview();
    const GapPreviewState& gap_preview() const;
    // Install the worker-built patch set, then refresh the gap preview at the
    // current slider value (a recompute keeps the user's gap setting).
    void install_gap_preview(std::unique_ptr<GapPreviewState> preview);
    // Preview only: replay or undo region merges so every region below the
    // gap percent of the mesh is absorbed by its neighbor. Cheap and
    // history-independent (prefix replay of a precomputed event list).
    void apply_gap_area();
    // Fold the current gap-merge preview colors into m_painted (face_colors +
    // cluster_colors) WITHOUT rebuilding the matches. Used by the computation
    // completion path so the standard match chain runs on the merged palette.
    // Returns false when there is no gap state consistent with m_painted.
    bool fold_gap_into_painted();
    // Write gap preview colors into m_painted for filament matching. Does not
    // overwrite the last simplified face colors or reset the gap slider.
    void commit_gap_area_to_painted();
    void refresh_gap_preview();
    // Number of merge events whose absorbed region area is below area_threshold.
    int  gap_event_count_for_threshold(double area_threshold) const;

    bool has_valid_result() const;
    bool is_params_dirty() const;
    void update_confirm_button_state();
    void style_confirm_button(bool dirty);
    void style_apply_button(bool dirty);

    Slic3r::TexturedMesh               m_textured_mesh;
    std::vector<std::string>           m_filament_color_strs;   // existing + virtual
    std::vector<std::string>           m_filament_names;        // existing + virtual
    std::vector<std::array<float, 4>>  m_filament_colors_rgba;  // existing + virtual
    std::vector<TextureFilamentEntry>  m_filament_entries;      // aligned with m_filament_colors_rgba
    size_t                             m_existing_filament_count = 0;
    std::vector<std::array<float, 4>>  m_new_filament_colors;   // only virtual (to be created)
    std::vector<std::string>           m_new_filament_preset_names; // only virtual, aligned with m_new_filament_colors
    std::vector<TextureNewMixedFilament> m_new_mixed_filaments;
    std::string                        m_default_virtual_filament_preset_name;

    TextureImportState                 m_state = TextureImportState::Idle;
    bool                               m_skipped = false;
    bool                               m_fallback_to_geometry_only = false;
    // True iff *the most recent* do_auto_match() ran into the global filament
    // limit and had to drop one or more clusters. Reset to false on every
    // do_auto_match() entry so it never accumulates across runs: a run that
    // does not drop anything must observe false here, regardless of whether
    // previous runs dropped. Drives the inline orange warning above the
    // bottom buttons; never affects the mapping itself.
    bool                               m_filaments_dropped = false;
    TextureAutoMixMode                 m_auto_mix_mode = TextureAutoMixMode::CMYW;

    Slic3r::PaintedMesh               m_painted;
    std::vector<Slic3r::FilamentMatch> m_current_matches;

    std::unique_ptr<std::thread>       m_worker;
    std::atomic<bool>                  m_cancel_flag{false};
    std::mutex                         m_result_mutex;
    Slic3r::PaintedMesh               m_pending_result;
    // Deep copy of the whole filament/mapping state taken right before a
    // one-click auto mix is applied; reset_auto_mix() restores it verbatim.
    struct AutoMixSnapshot {
        std::vector<std::array<float, 4>>      colors_rgba;
        std::vector<std::string>               color_strs;
        std::vector<std::string>               names;
        std::vector<TextureFilamentEntry>      entries;
        std::vector<std::array<float, 4>>      new_colors;
        std::vector<std::string>               new_preset_names;
        std::vector<TextureNewMixedFilament>   new_mixed;
        std::vector<Slic3r::FilamentMatch>     matches;
        bool                                   filaments_dropped = false;
    };
    std::optional<AutoMixSnapshot>     m_auto_mix_snapshot;
    std::function<bool()>              m_initial_cancel_callback;
    std::function<bool(int)>           m_initial_progress_callback;
    bool                               m_current_computation_initial = false;
    bool                               m_initial_computation_pending = false;
    bool                               m_initial_computation_cancelled = false;
    bool                               m_initial_computation_failed = false;
    bool                               m_initial_tooltips_set = false;
    bool                               m_current_computation_auto_color = false;
    Slic3r::TexturePaintingSettings::MeshRepairDecision m_mesh_repair_decision =
        Slic3r::TexturePaintingSettings::MeshRepairDecision::Ask;

    Button*       m_btn_color_4    = nullptr;
    Button*       m_btn_color_8    = nullptr;
    Button*       m_btn_color_16   = nullptr;
    Button*       m_btn_color_32   = nullptr;
    Button*       m_btn_color_64   = nullptr;
    AccentSlider* m_color_slider   = nullptr;
    TextInput*    m_color_spin     = nullptr;
    AccentSlider* m_smooth_slider  = nullptr;
    TextInput*    m_smooth_spin    = nullptr;
    // "Gap Area" parameter (percent of mesh area, 0..5; wired into the
    // gap-merge preview via apply_gap_area / commit_gap_area_to_painted).
    AccentSlider* m_gap_slider     = nullptr;
    TextInput*    m_gap_spin       = nullptr;
    wxCheckBox*   m_keep_colors_cb = nullptr;
    Button*       m_btn_apply      = nullptr;
    // Red inline warning under the color-count row (doc section 3.3.1 note 4):
    // shown when the requested count exceeds the original model's color count
    // (Apply is blocked meanwhile) or when the raw input exceeded the project
    // maximum and was clamped back to it.
    wxStaticText* m_color_warning_label = nullptr;
    // True iff m_param_color_count does not exceed the original model's color
    // count; while false, Apply stays disabled (doc: proceeding is blocked).
    bool color_count_input_valid() const;
    void update_color_count_warning();
    // Red border + red digits on the color-count box while the raw input is
    // out of 1..max range; restores the TextInput factory look otherwise.
    void update_color_input_error_style();
    // Red border + red digits on the smooth input while raw text is outside
    // 0..10; restores the TextInput factory look otherwise.
    void update_smooth_input_error_style();
    // Red border + red digits on the gap input while raw text is outside
    // 0.00-5.00; restores the TextInput factory look otherwise.
    void update_gap_input_error_style();

    wxPanel*               m_mix_mode_trigger = nullptr;
    int                    m_mix_mode_index = 0;
    wxPopupTransientWindow* m_mix_mode_popup = nullptr;
    // Step shell: the step indicator and the per-step panel hosts. The preview
    // canvas lives between the two step panels in the content sizer and never
    // changes parent on step switches; the window keeps the standard native
    // title bar like the app's other dialogs.
    ImportStep             m_step = ImportStep::Simplify;
    bool                   m_pending_step_advance = false;
    wxPanel*               m_step_indicator = nullptr;
    wxPanel*               m_params_panel   = nullptr;
    wxPanel*               m_mapping_panel  = nullptr;
    // Set right after the mix popup was dismissed without picking a mode while
    // the cursor is still over the trigger button; prevents the ENTER_WINDOW
    // binding from instantly reopening it (mirrors m_skip_next_filament_popup_row).
    // True while the raw color-count input is above the project maximum and is
    // being displayed clamped; drives the inline warning (doc boundary case 6).
    bool                  m_color_max_exceeded = false;
    wxScrolledWindow*     m_mapping_scroll = nullptr;
    wxBoxSizer*           m_mapping_sizer  = nullptr;
    std::vector<FilamentMappingRow> m_mapping_rows;
    FilamentSelectPopup*  m_filament_popup = nullptr;
    int                   m_filament_popup_row = -1;
    int                   m_skip_next_filament_popup_row = -1;

    // Single split-screen canvas: left half = original textured model,
    // right half = simplified multi-color result (see TexturePreviewCanvas).
    TexturePreviewCanvas* m_preview_canvas = nullptr;
    // Step-1-only spacer above the preview (panes sit lower on step 1);
    // toggled by apply_preview_metrics(). A real window (not a sizer spacer)
    // so Show/Hide can toggle it portably.
    wxPanel* m_preview_step_spacer = nullptr;
    wxStaticText*         m_lbl_orig_colors   = nullptr;
    wxStaticText*         m_lbl_result_colors = nullptr;
    long long             m_original_color_count = 0;
    wxStaticText*         m_lbl_color_counts     = nullptr;
    // (Re)builds the "colors before/after simplification" labels under the two
    // preview panes from m_original_color_count and m_painted.cluster_colors.
    void update_preview_color_labels();

    ProgressDialog* m_progress_dlg = nullptr;

    Button*       m_btn_skip = nullptr;
    Button*       m_btn_ok   = nullptr;
    Button*       m_btn_next = nullptr;
    Button*       m_btn_back = nullptr;
    Button*       m_btn_reset = nullptr;
    wxStaticText* m_drop_warning_label = nullptr;

    int   m_param_color_count = 4;
    int   m_param_smooth      = 5;
    double m_param_gap_area   = 0.0;
    // When set, gap merging must keep every palette color alive: an excessive
    // threshold is capped to the largest value that preserves the color count.
    bool   m_keep_colors      = true;

    int   m_applied_color_count = -1;
    int   m_applied_smooth      = -1;
    double m_applied_gap_area   = 0.0;
    // Raw gap input outside 0..5: red error state, Apply/Next gated.
    bool   m_gap_out_of_range   = false;
    // Raw smooth input outside 0..10: same red-error pattern.
    bool   m_smooth_out_of_range = false;
    // Set by preview_spin_text_value when the raw smooth text is out of
    // range (the param itself is clamped there).
    bool   m_smooth_raw_out_of_range = false;
    wxStaticText* m_smooth_warning_label = nullptr;
    wxStaticText* m_gap_warning_label = nullptr;
    bool   m_applied_keep_colors = true;
    wxStaticText* m_hint_label  = nullptr;

    static const int ID_COLOR_4     = wxID_HIGHEST + 200;
    static const int ID_COLOR_8     = wxID_HIGHEST + 201;
    static const int ID_COLOR_16    = wxID_HIGHEST + 202;
    static const int ID_COLOR_32    = wxID_HIGHEST + 203;
    static const int ID_COLOR_64    = wxID_HIGHEST + 204;
    static const int ID_BTN_APPLY   = wxID_HIGHEST + 205;
    static const int ID_BTN_SKIP    = wxID_HIGHEST + 206;
    static const int ID_BTN_NEXT    = wxID_HIGHEST + 207;
    static const int ID_BTN_BACK    = wxID_HIGHEST + 208;
    static const int ID_BTN_RESET   = wxID_HIGHEST + 209;

    wxDECLARE_EVENT_TABLE();
};

}} // namespace Slic3r::GUI
