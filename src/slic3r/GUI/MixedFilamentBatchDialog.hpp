#pragma once

#include "GUI_Utils.hpp"
#include "ThumbnailView.hpp"    // ThumbnailView enum (lightweight; avoids pulling in GLCanvas3D.hpp)
#include "MixedColorMatchHelpers.hpp"
#include "libslic3r/MixedFilament.hpp"

#include <wx/wx.h>

#include <array>
#include <atomic>
#include <cassert>
#include <functional>
#include <memory>
#include <thread>
#include <string>
#include <vector>

class Button;
class ComboBox;
class Label;
class ScalableButton;
class StaticBox;

namespace Slic3r { namespace GUI {

class MixedFilamentBatchDialog : public DPIDialog
{
public:
    MixedFilamentBatchDialog(wxWindow* parent);
    ~MixedFilamentBatchDialog() override;

    const BatchMatchResult& GetResult() const { return m_result; }
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    // ---- UI construction (build_ui is a thin orchestrator; each helper stays <150 lines) ----
    void build_ui();
    // No build_header(): the native OS title bar (wxCAPTION in ctor) shows "Mixed Color Match".
    void build_banners();           // error + warning panels
    void build_mode_row();          // Match Mode combo + segmented Start/Re-match tabs
    void build_manual_card(wxBoxSizer& parent);       // filament config (manual mode): ComboBox rows + add/remove
    void build_recommended_card(wxBoxSizer& parent);  // filament config (auto mode): numbered swatches + names
    void build_preview_card(wxBoxSizer& parent);      // single card: dual previews + badges + plate/view controls
    void build_mapping_card(wxBoxSizer& parent);      // color mapping card shell (title + info icon + grid host)
    // No build_progress(): the progress bar lives inside the footer panel (see build_footer).
    void build_footer();            // Cancel + Confirm + progress bar (pinned to bottom)

    void on_method_changed(wxCommandEvent&);
    void update_method_combo_tooltip(); // refresh combo tooltip to describe the active mode
    void on_manual_selection_changed();
    void update_add_remove_buttons(); // mirrors MixedFilamentDialog: hide add at max, remove at min
    // Compose drop_down arrow + numbered color badge into one transparent icon and set it as
    // the combo's left icon. Mirrors MixedFilamentDialog::set_combo_combined_icon: ComboBox
    // shows only the selected item's image when present (no native arrow), so we bake both
    // the arrow and the badge into a single SetIcon() image to keep the arrow visible.
    void set_manual_combo_icon(int row, int filament_idx);

    // Preview lifecycle
    void build_preview_panels();           // create wxStaticBitmaps once + render initial thumbnails
    void refresh_previews();               // swap cached bitmaps for current tray; lazy-renders match thumb
    void rebuild_match_thumb_cache();      // build m_match_colors from config + mappings, render current plate
    void render_match_thumb_for_plate(int plate_idx);    // render one plate's match thumbnail on demand
    void render_original_thumb_for_plate(int plate_idx); // render original (no color swap) at m_view

    // View + plate nav
    void on_view_changed(wxCommandEvent&);
    void on_tray_nav(int delta);           // -1 prev, +1 next
    void update_view();                    // re-render both previews for the current plate at m_view
    void update_nav_arrow_state();         // disable prev at first plate, next at last

    void start_batch_match();
    void cancel_batch_match();
    void launch_background_match();
    void handle_batch_match_result(const BatchMatchResult& result);

    void update_mapping_legend();

    void display_warning(const wxString& msg);
    void set_error(const wxString& msg);
    // Manual-mode ratio guard: if a single physical filament is picked by more than
    // kManualDominantRatioPct of the rows, warn that the mix is lopsided.
    void check_manual_filament_ratio();

    void set_match_buttons_state(bool matching);
    void update_recommended_card();
    void load_model_colors();
    void reset_match_preview();

    enum MatchingMethod { RECOMMENDED = 0, MANUAL = 1 };

    MatchingMethod m_matching_method = RECOMMENDED;
    BatchMatchResult m_result;
    bool             m_match_completed = false;
    bool             m_match_running   = false;
    std::shared_ptr<std::atomic<bool>> m_destroyed        { std::make_shared<std::atomic<bool>>(false) };
    std::shared_ptr<std::atomic<bool>> m_cancel_requested { std::make_shared<std::atomic<bool>>(false) };
    std::thread      m_worker_thread;

    std::vector<ModelColorEntry> m_model_colors;
    std::vector<std::string>     m_physical_colors;

    int   m_filament_selections[4] = {0, 1, 2, 3};

    int  m_tray_index = 1;
    int  m_tray_count = 1;
    ThumbnailView m_view = ThumbnailView::Iso;   // current preview viewpoint (drives render_thumbnail new overload)

    // Root layout
    wxBoxSizer*   m_root         = nullptr;

    // Top row
    wxPanel*  m_mode_row_panel  = nullptr; // white-bg host for the mode/match-mode row
    ComboBox* m_method_combo    = nullptr;
    Button*   m_btn_start_match = nullptr;
    Button*   m_btn_cancel_match = nullptr;
    Button*   m_btn_rematch     = nullptr;
    Button*   m_btn_confirm     = nullptr;
    Button*   m_btn_stop_match  = nullptr; // "Stop Matching" — inline beside the progress bar, shown only while matching

    // Preview card (single card holds both previews + plate/view controls).
    // m_preview_orig_panel / m_preview_match_panel are RoundedPreviewPanel instances but
    // held as wxPanel* (the rounded thumbnail + badge are painted in-handler, no children).
    StaticBox*      m_preview_card        = nullptr;
    wxPanel*        m_preview_orig_panel  = nullptr;
    wxPanel*        m_preview_match_panel = nullptr;
    ComboBox*       m_view_combo  = nullptr;   // Isometric / Top

    // Thumbnail cache: one wxBitmap per plate, bucketed by viewpoint.
    // Lazily rendered — a (viewpoint, plate) bitmap is produced on first access and kept.
    //   m_thumb_cache_by_view[i]:  original (un-matched) thumbnails for ThumbnailView i
    //   m_match_cache_by_view[i]: matched-color thumbnails for ThumbnailView i
    //   m_match_colors: ColorRGBA vector built from config + mappings, used for lazy rendering
    // Bucket 0 (Iso) is seeded from plate->thumbnail_data in build_preview_panels(); every
    // other bucket starts empty and is filled on demand via render_*_thumb_for_plate().
    // Bucket count is derived from the enum, not a magic 8, so adding a viewpoint fails
    // the static_assert below instead of silently going out of bounds.
    static constexpr size_t kNumThumbnailViews = static_cast<size_t>(ThumbnailView::Rear) + 1;
    // Manual-mode dominance threshold: when one physical filament accounts for more than
    // this fraction of the selected rows, the mix is lopsided and we warn the user.
    // 0.7 = 70%. Kept as a named constant (not a literal) so the threshold is grep-able
    // and adjustable in one place.
    static constexpr double kManualDominantRatioPct = 0.7;
    std::array<std::vector<wxBitmap>, kNumThumbnailViews> m_thumb_cache_by_view;
    std::array<std::vector<wxBitmap>, kNumThumbnailViews> m_match_cache_by_view;
    std::vector<ColorRGBA> m_match_colors;

    // combo index -> ThumbnailView mapping. Decouples the View combobox's Append order
    // from the enum values so reordering/localizing the combo cannot silently misroute
    // the selection (adversarial review item 3).
    static constexpr ThumbnailView kComboToView[] = {
        ThumbnailView::Iso,      // "Isometric"
        ThumbnailView::TopFront, // "Top-Front"
        ThumbnailView::Left,     // "Left"
        ThumbnailView::Right,    // "Right"
        ThumbnailView::Top,      // "Top"
        ThumbnailView::Bottom,   // "Bottom"
        ThumbnailView::Front,    // "Front"
        ThumbnailView::Rear,     // "Rear"
    };
    // Lock the enum order so kComboToView and kViewDir[] (GLCanvas3D.cpp) can't silently
    // drift if someone reorders ThumbnailView. The combo mapping above and the dir-name
    // table in render_thumbnail_internal both index by static_cast<size_t>(view), so a
    // reordered enum would route viewpoints to the wrong camera angle without failing
    // to compile.
    static_assert(static_cast<size_t>(ThumbnailView::Iso) == 0, "Iso must stay at index 0");
    static_assert(static_cast<size_t>(ThumbnailView::TopFront) == 1, "ThumbnailView order changed");
    static_assert(static_cast<size_t>(ThumbnailView::Left) == 2, "ThumbnailView order changed");
    static_assert(static_cast<size_t>(ThumbnailView::Right) == 3, "ThumbnailView order changed");
    static_assert(static_cast<size_t>(ThumbnailView::Top) == 4, "ThumbnailView order changed");
    static_assert(static_cast<size_t>(ThumbnailView::Bottom) == 5, "ThumbnailView order changed");
    static_assert(static_cast<size_t>(ThumbnailView::Front) == 6, "ThumbnailView order changed");
    static_assert(static_cast<size_t>(ThumbnailView::Rear) == 7, "ThumbnailView order changed");

    // Accessors return the cache for the current viewpoint, with bounds protection
    // (adversarial review item 4). m_view is always assigned via kComboToView, so the
    // assert is a defensive check against future regressions.
    std::vector<wxBitmap>& orig_cache() {
        assert(static_cast<size_t>(m_view) < m_thumb_cache_by_view.size());
        return m_thumb_cache_by_view[static_cast<size_t>(m_view)];
    }
    std::vector<wxBitmap>& match_cache() {
        assert(static_cast<size_t>(m_view) < m_match_cache_by_view.size());
        return m_match_cache_by_view[static_cast<size_t>(m_view)];
    }

    // Plate + view nav (ScalableButton with arrow icons; Enable() drives disabled state)
    ScalableButton* m_btn_tray_prev = nullptr;
    ScalableButton* m_btn_tray_next = nullptr;
    ComboBox*       m_tray_combo    = nullptr;

    // Filament config cards (manual + recommended; only one visible per mode, both styled alike)
    wxWindow*       m_manual_card            = nullptr;
    wxWindow*       m_recommended_card       = nullptr;
    wxStaticBitmap* m_recommended_swatches[4] = {nullptr};
    wxStaticText*   m_recommended_labels[4]   = {nullptr};
    ComboBox*       m_filament_combo[4]       = {nullptr};
    wxWindow*       m_manual_row_panels[4]    = {nullptr};
    int             m_manual_filament_count   = 0; // computed in ctor based on physical filaments
    // Add/remove buttons (mirrors MixedFilamentDialog: hidden at min/max count)
    ScalableButton* m_btn_remove_filament = nullptr;
    ScalableButton* m_btn_add_filament    = nullptr;

    // Legend / mapping
    StaticBox*      m_mapping_card      = nullptr;   // grows in height with content (no inner scroller)
    wxPanel*        m_legend_panel      = nullptr;   // plain panel holding the legend grid
    wxGridSizer*    m_legend_sizer      = nullptr;    // fixed-col grid (Mac-safe; wxWrapSizer miscomputes height on macOS)
    ScalableButton* m_mapping_info_icon = nullptr;    // info tooltip next to "Color Mapping" title

    // Error / warning
    wxPanel* m_error_panel   = nullptr;
    Label*   m_error_text    = nullptr;
    wxPanel* m_warning_panel = nullptr;
    Label*   m_warning_text  = nullptr;

    // Progress
    wxGauge* m_progress_bar = nullptr;

    wxScrolledWindow* m_scrolled_content = nullptr;
};

}} // namespace Slic3r::GUI
