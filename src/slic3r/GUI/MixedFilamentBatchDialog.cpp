#include "MixedFilamentBatchDialog.hpp"
#include "MixedFilamentBadge.hpp"
#include "MixedColorMatchHelpers.hpp"
#include "FilamentColorUtils.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "PresetBundle.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/FilamentColorLibrary.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/libslic3r.h"
#include "Widgets/ComboBox.hpp"
#include "Widgets/StaticBox.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "wxExtensions.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"
#include "PartPlate.hpp"
#include "BitmapCache.hpp"

#include <unordered_map>
#include <set>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <thread>

#include <wx/scrolwin.h>
#include <wx/dcbuffer.h>
#include <wx/wupdlock.h>
#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI {

// Canonical Full Spectrum palette used as a fallback when load_full_spectrum_colors() fails
// to read the live preset (library not loaded, preset missing, or <4 validated colors).
// Mirrors filaments_colours.json's "Snapmaker PLA Full Spectrum @U1" single-color SKUs.
// Single source of truth for the fallback path in both build_recommended_card and
// launch_background_match — avoids the old duplicate CMYW_ENTRIES / CMYW_COLORS constants.
static const std::vector<std::string> FULL_SPECTRUM_FALLBACK_COLORS = {
    "#08ABFB", // semi-translucent cyan
    "#D93B90", // semi-translucent magenta
    "#F9ED3D", // semi-translucent yellow
    "#9199A4", // semi-translucent gray
};

// Columns for the Color Mapping legend grid. Fixed (not growable) so each mapping
// item keeps its natural width and the card height is computed correctly on macOS
// (same rationale as MixedFilamentDialog's fixed-col swatch grid).
static constexpr int LEGEND_GRID_COLS = 4;

// Component-weight bounds for recipe generation (see launch_background_match).
// Recommended mode caps a single component at 70% (product spec); manual mode is
// unrestricted — an over-70% recipe surfaces as an advisory via check_manual_recipe_ratio
// instead of being hard-rejected at match time.
static constexpr int kMinComponentPercent = 0;   // recommended mode floor (near-pure allowed)
static constexpr int kMaxComponentPercent = 70;  // recommended mode cap; also the advisory threshold

// UI-list cap: the number of source/palette color rows the legend renders. Past this cap
// the tail colors are dropped (see load_palette_colors) and a deferred warning is shown once
// build_ui is up. This guards legend rendering against an unbounded color list — it is NOT
// the project filament-slot limit. The slot limit is MAXIMUM_FILAMENT_NUMBER (libslic3r.h),
// total_filaments = num_physical + enabled_mixed, enforced at apply time by
// add_batch_custom_filaments and predicted post-match by handle_batch_match_result.
static constexpr size_t kMaxColors = 64;

// ΔE color-difference grade thresholds (PRD 6.2.5). The match-quality tooltip bands a
// mapping's delta_e into Good / Fair / Poor. Product-owned values — adjust here when the
// spec changes (recently 3/5 → 4/8). Open intervals: <kDeltaEGoodMax Good,
// [kDeltaEGoodMax, kDeltaEFairMax) Fair, ≥kDeltaEFairMax Poor.
static constexpr double kDeltaEGoodMax = 4.0;  // <4.0 → Good
static constexpr double kDeltaEFairMax = 8.0;  // <8.0 → Fair, else Poor

// Recommended-mode (Full Spectrum) per-color transmittance-density (TD) values. These are NOT
// in the filament data model (filaments_colours.json carries no TD field), so they are pinned
// here as product constants — provided by product spec (snapshot in code per request).
//
// KEYED BY COLOR FAMILY (canonical color name), NOT by palette position. The recommended card's
// swatch order is normally preset-order (C/M/Y/G), but a match's ΔE fallback path
// (launch_background_match) can reorder the palette, so positional indexing would bind the
// wrong TD to a color after reorder. Looking up by the color's identity avoids that.
//
// Each entry: {substring-to-match-against-English-name, TD-label, TD-value}. The match is
// case-insensitive substring on the EN color name (which is always present in FilamentColorItem
// .colorNames regardless of UI locale — it's the SKU's canonical name in filaments_colours.json).
// White (W:8.8) is included per spec; Full Spectrum has no White SKU today, so it only surfaces
// if a White-named color is ever added to the preset.
// Each entry: {substring-to-match, TD-value}. The substring IS the family display name —
// tooltip shows it verbatim (e.g. "cyan 5.5"), no separate label/abbreviation needed.
struct TdEntry { const char* family; double value; };
static const std::vector<TdEntry> FULL_SPECTRUM_TD = {
    {"cyan",    5.5},
    {"magenta", 5.5},
    {"yellow",  9.5},
    {"white",   8.8},
    {"gray",    6.5},
    {"grey",    6.5}, // spelling variant, defensive
};

// Effective primary colour of a physical slot for the match. Dual-color slots carry
// their real colours in filament_multi_colors while filament_colour may stay empty or
// hold a placeholder; derive the primary (first valid colour — the app-wide rule used
// by PresetBundle's sync) so such slots enter the match with their actual colour instead
// of being skipped or misidentified. Slots with no valid multi-colour token (single-colour
// / empty / malformed) fall back verbatim to the raw filament_colour value.
static std::string slot_match_color(const std::string& multi_colors, int color_mode, const std::string& fallback)
{
    const auto parts = FilamentColorUtils::SplitMultiColors(multi_colors);
    if (parts.empty())
        return fallback;
    return FilamentColor::FromColors(parts, FilamentColorModeFromConfig(color_mode),
                                     fallback).PrimaryColor(fallback);
}

// Card geometry (DIP). CARD_WIDTH_DIP leaves room for a vertical scrollbar inside the
// 500-wide dialog: 500 − 2×12(margin) − 17(scrollbar) = 459. FILAMENT_COL_WIDTH_DIP pins
// each 2×2 grid cell so slots 1/2 and 3/4 stay equal regardless of filament-name length
// (wxFlexGridSizer otherwise sizes columns by content, so uneven names shift the columns).
static constexpr int CARD_WIDTH_DIP        = 459;  // 500 − 2×12 − 17
static constexpr int FILAMENT_COL_WIDTH_DIP = 207; // (459 − 2×16(padding) − 12(gap)) / 2 = 207.5 → floor

// Preview panel design sizes (DIP), kept in sync with the RoundedPreviewPanel ctor args
// in build_preview_card. Used as the std::max floor when reading GetClientSize() so an
// early call (before first Layout) still renders at full panel resolution instead of the
// old 100/160 floors — those were below the panel size and produced bitmaps that got
// stretched and looked blurry on first paint.
static constexpr int ORIG_PREVIEW_DIP = 180;
static constexpr int MATCH_PREVIEW_DIP = 227;

// ---------------------------------------------------------------------------
// RoundedPreviewPanel — a fixed-size square panel that draws a rounded-corner
// thumbnail with an overlay badge ("Original Model" / "After Match").
//
// Rounded corners: paint the real background (card white via SetBackgroundColour +
// dc.Clear()), then clip to a rounded rect and draw the rounded background (#E7E7E7)
// + thumbnail + badge inside it. The background is a real opaque window background,
// not a colour-matching fake — dc.Clear() is bounded to this window's backing store,
// so the four corners can't bleed onto sibling UI on macOS.
//
// Plain wxDC (not wxGraphicsContext) stays in the same coordinate space as
// wxAutoBufferedPaintDC — no antialias transform, no off-by-one clipping artifacts.
// ---------------------------------------------------------------------------

class RoundedPreviewPanel : public wxPanel
{
public:
    RoundedPreviewPanel(wxWindow* parent, int side_dip, int radius_dip, const wxString& badge_label)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
        , m_side_dip(side_dip)
        , m_radius_dip(radius_dip)
        , m_badge_label(badge_label)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        // Opaque window bg = parent card white. Marks the panel opaque so the macOS
        // compositor stops blending it over siblings, and fixes the colour dc.Clear()
        // paints. Wrapped in darkModeColorFor so dark mode maps it to the dialog's
        // dark window-bg slot (#2D2D31) — the light-mode value is plain white, so any
        // equivalent white (wxColour("#FFFFFF") / *wxWHITE / wxColour(255,255,255))
        // resolves to the same map entry (gDarkColors keys by RGBA value, not string).
        SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        SetMinSize(wxSize(FromDIP(side_dip), FromDIP(side_dip)));
        Bind(wxEVT_PAINT, &RoundedPreviewPanel::on_paint, this);
        // DPI change: the placeholder SVG (ScalableBitmap) caches its raster at the
        // DPI in effect when first created.  Reset the loaded flag so on_paint re-
        // rasterizes it at the new DPI.  The rounded-corner radius is already DIP-
        // aware (FromDIP in on_paint), so no explicit size rebuild needed.
        Bind(wxEVT_DPI_CHANGED, [this](wxDPIChangedEvent& e) {
            m_placeholder_loaded = false;
            Refresh();
            e.Skip();
        });
    }

    void set_bitmap(const wxBitmap& bmp)
    {
        m_src_bmp = bmp;
        m_bmp     = bmp;
        Refresh();
    }

private:
    int      m_side_dip;
    int      m_radius_dip;
    wxString m_badge_label;
    wxBitmap m_src_bmp;
    wxBitmap m_bmp;
    ScalableBitmap m_placeholder;
    bool           m_placeholder_loaded = false;

    void on_paint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize sz = GetClientSize();
        if (sz.x <= 0 || sz.y <= 0) return;
        const int r = FromDIP(m_radius_dip);

        const wxColour card_white = StateColor::darkModeColorFor(wxColour("#FFFFFF"));
        const wxColour panel_bg   = StateColor::darkModeColorFor(wxColour(231, 231, 231));

        // Step 1 — real background, not a faked-transparent white square. dc.Clear()
        // writes only into this window's own backing store, which the compositor bounds
        // to the window geometry — so the corners read as the card but can't bleed.
        dc.SetBackground(wxBrush(card_white));
        dc.Clear();

        // Step 2 — build a rounded-rect clip region:
        //   black rounded-rect on white → region = black interior
        wxRegion clip_rgn;
        {
            wxBitmap mask(sz);
            wxMemoryDC mdc;
            mdc.SelectObject(mask);
            mdc.SetBackground(*wxWHITE_BRUSH);
            mdc.Clear();
            mdc.SetBrush(*wxBLACK_BRUSH);
            mdc.SetPen(*wxTRANSPARENT_PEN);
            mdc.DrawRoundedRectangle(0, 0, sz.x, sz.y, r);
            mdc.SelectObject(wxNullBitmap);
            clip_rgn = wxRegion(mask, *wxWHITE);
        }
        dc.SetDeviceClippingRegion(clip_rgn);

        // Step 3 — rounded background (#E7E7E7) clipped to rounded rect
        dc.SetBrush(wxBrush(panel_bg));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRoundedRectangle(wxRect(sz), r);

        // Step 4 — thumbnail centered
        if (!m_bmp.IsOk() && !m_placeholder_loaded) {
            m_placeholder = ScalableBitmap(this, "mixed_filament_preview_placeholder", 60);
            m_placeholder_loaded = true;
        }
        const wxBitmap& draw_bmp = m_bmp.IsOk() ? m_bmp :
            (m_placeholder_loaded ? m_placeholder.bmp() : wxNullBitmap);
        if (draw_bmp.IsOk()) {
            const int bw = draw_bmp.GetWidth();
            const int bh = draw_bmp.GetHeight();
            if (bw > 0 && bh > 0)
                dc.DrawBitmap(draw_bmp, (sz.x - bw) / 2, (sz.y - bh) / 2, false);
        }

        // Step 5 — badge (white text on gray bg, top-left), drawn inside the rounded
        // clip so it's hard-bounded to the thumbnail area.
        dc.SetFont(Label::Body_12);
        const wxSize text_sz = dc.GetTextExtent(m_badge_label);
        const int pad_x = FromDIP(8);
        const int pad_y = FromDIP(4);
        const int inset = FromDIP(8);
        wxRect badge(inset, inset, text_sz.x + pad_x * 2, text_sz.y + pad_y * 2);
        // Inline resolve — not the shared dark map, to avoid the #000000 reverse-lookup collision.
        dc.SetBrush(wxBrush(wxGetApp().dark_mode() ? *wxBLACK : wxColour(147, 147, 147)));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(badge);
        dc.SetTextForeground(*wxWHITE);
        dc.DrawText(m_badge_label, badge.x + pad_x, badge.y + pad_y);

        dc.DestroyClippingRegion();
    }
};

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

MixedFilamentBatchDialog::MixedFilamentBatchDialog(wxWindow* parent)
    : DPIDialog(parent, wxID_ANY, _L("Color Mixing Match"),
                wxDefaultPosition, wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX)
{
    // Initial client size (mirrors the original SetSize(540,680) intent but pins the CLIENT
    // area so OS borders don't eat into the 500px design width). build_ui re-asserts this
    // after the sizer is attached.
    SetClientSize(FromDIP(500), FromDIP(700));
    SetMinClientSize(wxSize(FromDIP(500), FromDIP(560)));
    CentreOnScreen();

    if (wxGetApp().preset_bundle) {
        // Physical filament colors for the Manual-mode combo list. Go through
        // get_extruder_colors_from_plater_config (include_mixed=false) rather than reading
        // filament_colour.values directly: that function resizes the vector to filaments_cnt()
        // (preset_bundle->filament_presets.size()), padding with "#26A69A" when the raw config
        // array is shorter than the real extruder count. This keeps m_physical_colors.size()
        // in lock-step with the physical extruder count — load_palette_colors uses the same
        // source (include_mixed=true), so Manual combo items and target rows never drift apart
        // when filament_colour has fallen out of sync with filament_presets.
        if (auto* plater = wxGetApp().plater())
            m_physical_colors = plater->get_extruder_colors_from_plater_config(nullptr, false);
        // Read multi-color data for dual-color filament rendering.
        // Always initialise to match m_physical_colors.size() so downstream
        // index access (m_physical_multi_colors[j]) is safe even when the
        // project_config lacks either option.
        {
            auto& proj_cfg = wxGetApp().preset_bundle->project_config;
            const size_t n = m_physical_colors.size();
            if (const auto* mc_opt = proj_cfg.option<ConfigOptionStrings>("filament_multi_colors"))
                m_physical_multi_colors = mc_opt->values;
            m_physical_multi_colors.resize(n);          // default "" → solid fallback
            if (const auto* mode_opt = proj_cfg.option<ConfigOptionInts>("filament_colour_mode"))
                m_physical_color_modes = mode_opt->values;
            m_physical_color_modes.resize(n);           // default 0  → Segment fallback
        }
    }
    // Default the manual-mode filament count to the number of physical filaments,
    // capped at 4 (the max supported slots) and floored at 2 (the min).
    //   >4 filaments -> 4, 3 -> 3, 2 -> 2.
    {
        const size_t n = m_physical_colors.size();
        m_manual_filament_count = n > 4 ? 4 : (n >= 2 ? static_cast<int>(n) : 2);
    }
    if (wxGetApp().plater()) {
        auto& plates = wxGetApp().plater()->get_partplate_list();
        m_tray_count = plates.get_plate_count();
        if (m_tray_count < 1) m_tray_count = 1;
        // Default the dialog to the plate the user currently has selected in the plater
        // (multi-plate UX: opening the dialog should reflect the plate the user is looking
        // at, not always plate 1). get_curr_plate_index() is 0-based; m_tray_index is
        // 1-based, so add 1. Clamp to the valid [1, m_tray_count] range defensively —
        // m_current_plate could be -1 (no selection) or out of range in edge cases.
        const int curr = plates.get_curr_plate_index();
        if (curr >= 0 && curr < m_tray_count)
            m_tray_index = curr + 1;
        else
            m_tray_index = 1;
    }
    // Pre-warm FilamentColorLibrary on the main thread. EnsureLoaded() is NOT thread-safe
    // (_loaded is an unlocked bool, and it performs file I/O + JSON parse), so it MUST run
    // before the worker thread in launch_background_match() starts. After warm-up, the
    // worker's only call into the library is the read-only FindFilamentByName (a map lookup),
    // which is safe to run concurrently with the main thread. See load_full_spectrum_colors().
    FilamentColorLibrary::Instance().EnsureLoaded();
    // Match targets = the project's FULL palette (physical filament_colour + enabled mixed
    // display_colors), NOT just the model's painted volumes. See load_palette_colors.
    load_palette_colors();
    build_ui();
    set_match_buttons_state(false);
    m_btn_start_match->Enable(!m_model_colors.empty() && m_physical_colors.size() >= 2);

    // Default to recommended mode — show CMYK card
    if (m_recommended_card) {
        m_recommended_card->Show(true);
        m_recommended_card->Layout();
    }

    // X must signal cancellation like Cancel/Stop Matching — the default close
    // handler would EndModal(wxID_CANCEL) without setting m_cancel_requested.
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { cancel_batch_match(); EndModal(wxID_CANCEL); });

    // Generate thumbnails for plates that don't have slicing data yet
    wxWeakRef<wxWindow> weak_self(this);
    CallAfter([weak_self]() {
        if (!weak_self) return;
        auto* self = static_cast<MixedFilamentBatchDialog*>(weak_self.get());
        // Force thumbnail generation for all plates using the existing 3D canvas
        // (works without slicing — uses offscreen FBO rendering)
        wxGetApp().plater()->update_all_plate_thumbnails(true);
        self->build_preview_panels();
    });
}

MixedFilamentBatchDialog::~MixedFilamentBatchDialog()
{
    m_cancel_requested->store(true);
    if (m_worker_thread.joinable())
        m_worker_thread.join();
    m_destroyed->store(true);
}

void MixedFilamentBatchDialog::on_dpi_changed(const wxRect& /*suggested_rect*/)
{
    SetClientSize(FromDIP(500), FromDIP(700));
    Layout();
    Refresh();
}

// Convert ThumbnailData (RGBA pixels, OpenGL FBO = bottom-up) to wxBitmap (top-down)
// Blends alpha against the panel background (#E7E7E7)
static wxBitmap thumbnail_to_bitmap(const ThumbnailData& data, int max_w, int max_h)
{
    if (!data.is_valid() || data.width == 0 || data.height == 0)
        return wxNullBitmap;

    // Panel background the thumbnail will sit on. Resolved once via
    // darkModeColorFor so the blended bg follows the active theme — the bitmap
    // must not bake in a hardcoded light bg, otherwise in dark mode the cached
    // thumbnail renders as a light-gray patch on the (now dark) panel. The
    // hex #E7E7E7 maps to #54545B in dark mode (see StateColor gDarkColors).
    const wxColour bg_color = StateColor::darkModeColorFor(wxColour(231, 231, 231));
    const float    bg_r     = bg_color.Red();
    const float    bg_g     = bg_color.Green();
    const float    bg_b     = bg_color.Blue();

    wxImage img(data.width, data.height, false);
    img.InitAlpha();
    unsigned char* d  = img.GetData();
    unsigned char* a  = img.GetAlpha();
    if (!d || !a) return wxNullBitmap;

    // OpenGL FBO is bottom-up; wxImage is top-down — flip Y
    for (unsigned int y = 0; y < data.height; ++y) {
        unsigned int src_y = data.height - 1 - y;
        for (unsigned int x = 0; x < data.width; ++x) {
            size_t si = (static_cast<size_t>(src_y) * data.width + x) * 4;
            size_t di = (static_cast<size_t>(y)       * data.width + x) * 3;
            unsigned char r   = data.pixels[si + 0];
            unsigned char g   = data.pixels[si + 1];
            unsigned char b   = data.pixels[si + 2];
            unsigned char alpha = data.pixels[si + 3];
            // Blend with panel background (theme-aware #E7E7E7 / dark #54545B)
            // for low-alpha pixels
            if (alpha < 255) {
                float t = alpha / 255.0f;
                r = static_cast<unsigned char>(r * t + bg_r * (1.0f - t));
                g = static_cast<unsigned char>(g * t + bg_g * (1.0f - t));
                b = static_cast<unsigned char>(b * t + bg_b * (1.0f - t));
            }
            d[di + 0] = r;
            d[di + 1] = g;
            d[di + 2] = b;
            a[static_cast<size_t>(y) * data.width + x] = 255;
        }
    }

    double scale = std::min(double(max_w) / data.width, double(max_h) / data.height);
    int w = std::max(1, int(data.width * scale));
    int h = std::max(1, int(data.height * scale));

    return wxBitmap(img.Scale(w, h, wxIMAGE_QUALITY_HIGH));
}

void MixedFilamentBatchDialog::build_preview_panels()
{
    auto* plater = wxGetApp().plater();
    if (!plater) return;

    auto& plates = plater->get_partplate_list();
    const int count = plates.get_plate_count();
    if (count < 1) return;

    // Panel sizes from layout (set in build_ui). RoundedPreviewPanel renders its own
    // thumbnail + badge in a paint handler, so there are no wxStaticBitmap / badge child
    // windows to create here — we only need the client size to render thumbnails at the
    // right resolution.
    const int orig_w = std::max(FromDIP(ORIG_PREVIEW_DIP), m_preview_orig_panel->GetClientSize().GetWidth());
    const int orig_h = std::max(FromDIP(ORIG_PREVIEW_DIP), m_preview_orig_panel->GetClientSize().GetHeight());
    const int match_w = std::max(FromDIP(MATCH_PREVIEW_DIP), m_preview_match_panel->GetClientSize().GetWidth());
    const int match_h = std::max(FromDIP(MATCH_PREVIEW_DIP), m_preview_match_panel->GetClientSize().GetHeight());

    // Pre-render all plate ORIGINAL thumbnails into the Iso bucket. The match buckets
    // start empty — until a match completes (m_match_completed), refresh_previews() leaves
    // the After-Match panel on its placeholder rather than showing the original render.
    // Other viewpoint buckets stay empty and are filled lazily by render_*_thumb_for_plate()
    // on first access (plate->thumbnail_data is itself an iso render, so only Iso is seeded).
    for (auto& bucket : m_thumb_cache_by_view)  bucket.resize(count);
    for (auto& bucket : m_match_cache_by_view) bucket.resize(count);
    auto& iso_orig = m_thumb_cache_by_view[static_cast<size_t>(ThumbnailView::Iso)];
    for (int i = 0; i < count; ++i) {
        const PartPlate* plate = plates.get_plate(i);
        if (!plate) continue;
        iso_orig[i] = thumbnail_to_bitmap(plate->thumbnail_data, orig_w, orig_h);
    }

    // Adjust match preview to use its own panel dimensions
    // (orig thumbnail is shared cache, displayed at match panel size via wxStaticBitmap stretch)
    refresh_previews();
}

void MixedFilamentBatchDialog::refresh_previews()
{
    const int idx = m_tray_index - 1;
    auto& orig  = orig_cache();
    auto& match = match_cache();
    if (idx < 0 || idx >= static_cast<int>(orig.size())) return;

    // Original side: lazily render the current plate at m_view on first access (every
    // viewpoint other than Iso starts empty in build_preview_panels). The original side
    // follows the user's viewpoint selection (confirmed product behavior).
    if (idx < static_cast<int>(orig.size()) && !orig[idx].IsOk())
        render_original_thumb_for_plate(idx);
    if (m_preview_orig_panel && idx < static_cast<int>(orig.size()) && orig[idx].IsOk())
        static_cast<RoundedPreviewPanel*>(m_preview_orig_panel)->set_bitmap(orig[idx]);

    // Lazy-render match thumbnail for current plate on demand
    if (m_match_completed && !m_match_colors.empty() &&
        idx < static_cast<int>(match.size()) && !match[idx].IsOk()) {
        render_match_thumb_for_plate(idx);
    }

    // After-Match panel: only push a real bitmap once a match has completed. Before that
    // (initial open, re-match in progress) we leave the rounded bitmap unset so
    // RoundedPreviewPanel renders its placeholder — avoids showing the original render
    // on the After-Match side.
    if (m_preview_match_panel && m_match_completed &&
        idx < static_cast<int>(match.size()) && match[idx].IsOk())
        static_cast<RoundedPreviewPanel*>(m_preview_match_panel)->set_bitmap(match[idx]);
    else if (m_preview_match_panel && !m_match_completed)
        static_cast<RoundedPreviewPanel*>(m_preview_match_panel)->set_bitmap(wxNullBitmap);
}

void MixedFilamentBatchDialog::reset_match_preview()
{
    // Reset after-match preview to the no-match (placeholder) state. Clear ALL viewpoint
    // buckets (adversarial review item 7): a re-match must invalidate stale matched
    // thumbnails across every viewpoint, not just the current one — otherwise switching
    // viewpoints later would surface leftovers from the previous match. refresh_previews()
    // sees m_match_completed=false and leaves the After-Match panel on its placeholder.
    m_match_colors.clear();
    for (auto& match_bucket : m_match_cache_by_view) {
        for (auto& bmp : match_bucket) bmp = wxNullBitmap;
    }
    refresh_previews();
}

void MixedFilamentBatchDialog::rebuild_match_thumb_cache()
{
    if (!m_match_completed || m_result.mappings.empty()) return;

    auto* plater = wxGetApp().plater();
    if (!plater) return;

    // Build ColorRGBA vector from current filament_colour config (once, reused by lazy renders).
    // Physical filaments first, then virtual mixed-filament display colors appended
    // to match the indexing used by update_colors_by_extruder() in the normal 3D view.
    m_match_colors.clear();
    {
        auto* pb = wxGetApp().preset_bundle;
        if (pb) {
            auto* co = pb->project_config.option<ConfigOptionStrings>("filament_colour");
            if (co) {
                for (size_t i = 0; i < co->values.size(); ++i) {
                    // Same dual-color derivation as load_palette_colors; prevents an
                    // empty filament_colour on a dual-color slot rendering black.
                    const std::string hex = slot_match_color(
                        i < m_physical_multi_colors.size() ? m_physical_multi_colors[i] : std::string(),
                        i < m_physical_color_modes.size()  ? m_physical_color_modes[i]  : 0,
                        co->values[i]);
                    unsigned char rgba[4] = {};
                    BitmapCache::parse_color4(hex, rgba);
                    m_match_colors.push_back({
                        float(rgba[0]) / 255.f,
                        float(rgba[1]) / 255.f,
                        float(rgba[2]) / 255.f,
                        float(rgba[3]) / 255.f,
                    });
                }
            }
            // Append virtual mixed-filament display colors so that volumes
            // with extruder_ids pointing to mixed filaments resolve correctly.
            for (const std::string& dc : pb->mixed_filaments.display_colors()) {
                if (dc.empty()) continue;
                unsigned char rgba[4] = {};
                BitmapCache::parse_color4(dc, rgba);
                m_match_colors.push_back({
                    float(rgba[0]) / 255.f,
                    float(rgba[1]) / 255.f,
                    float(rgba[2]) / 255.f,
                    float(rgba[3]) / 255.f,
                });
            }
        }
    }
    // Apply match mappings by EXTRUDER ID, mirroring apply_batch_match_to_model's
    // src->target color effect (MixedColorMatchHelpers.cpp:1829-1835). The preview
    // runs BEFORE the model is modified, so the render pipeline still indexes this
    // vector by the ORIGINAL extruder id (render_match_thumb_for_plate sets
    // vol->color = m_match_colors[vol->extruder_id-1]; GLVolume::simple_render,
    // 3DScene.cpp:573, reads extruder_colors[idx-1] for painted facets where idx is
    // the facet's original extruder id). Therefore we substitute the SOURCE slots
    // with the matched color — visually equivalent to what confirm produces once
    // the model's extruder ids are remapped to target.
    //
    // The old loop matched slots by wxColour == source_color and `break`-ed on the
    // first hit, so when two extruder slots share one source color (e.g. two cubes
    // both painted red but on different extruder ids) only the first slot was
    // substituted -> the second cube rendered as the original model. Iterating
    // every source_extruder_ids entry fixes that. Pinned by the [batch_preview]
    // tests (tests/libslic3r/test_mixed_filament.cpp, build_match_preview_colors).
    for (const auto& mapping : m_result.mappings) {
        const ColorRGBA matched_rgba = {
            float(mapping.matched_color.Red())   / 255.f,
            float(mapping.matched_color.Green()) / 255.f,
            float(mapping.matched_color.Blue())  / 255.f,
            1.0f,
        };
        for (unsigned int src_eid : mapping.source_extruder_ids) {
            if (src_eid == 0) continue;
            const size_t idx = static_cast<size_t>(src_eid - 1);
            if (idx >= m_match_colors.size())
                m_match_colors.resize(idx + 1, {0.5f, 0.5f, 0.5f, 1.0f}); // ensure_slot pad
            m_match_colors[idx] = matched_rgba;
        }
    }

    // Invalidate all cached match bitmaps across every viewpoint (review item 7); only the
    // current plate is rendered now, others fill lazily on view/plate navigation.
    const int count = plater->get_partplate_list().get_plate_count();
    for (auto& bucket : m_match_cache_by_view) {
        bucket.clear();
        bucket.resize(count);
    }

    // Immediately render the currently displayed plate
    render_match_thumb_for_plate(m_tray_index - 1);
}

void MixedFilamentBatchDialog::render_match_thumb_for_plate(int plate_idx)
{
    auto& match = match_cache();
    if (plate_idx < 0 || plate_idx >= static_cast<int>(match.size())) return;
    if (m_match_colors.empty()) return;

    auto* plater = wxGetApp().plater();
    if (!plater) return;
    auto* canvas = plater->get_view3D_canvas3D();
    if (!canvas) return;

    const int match_w = std::max(FromDIP(MATCH_PREVIEW_DIP), m_preview_match_panel->GetClientSize().GetWidth());
    const int match_h = std::max(FromDIP(MATCH_PREVIEW_DIP), m_preview_match_panel->GetClientSize().GetHeight());

    // Save original volume colors, apply matched colors by extruder_id
    const auto& volumes = canvas->get_volumes();
    std::vector<ColorRGBA> saved_colors;
    saved_colors.reserve(volumes.volumes.size());
    for (GLVolume* vol : volumes.volumes) {
        saved_colors.push_back(vol->color);
        if (vol->extruder_id > 0 && static_cast<size_t>(vol->extruder_id - 1) < m_match_colors.size())
            vol->color = m_match_colors[vol->extruder_id - 1];
    }

    // RAII guard: restore original volume colors on scope exit (normal + exception)
    struct ColorRestoreGuard {
        const GLVolumeCollection& vols;
        std::vector<ColorRGBA>    saved;
        ~ColorRestoreGuard() {
            for (size_t i = 0; i < saved.size() && i < vols.volumes.size(); ++i)
                vols.volumes[i]->color = saved[i];
        }
    } guard{volumes, std::move(saved_colors)};

    ThumbnailsParams tp = { {}, false, true, true, true, plate_idx };
    ThumbnailData td;
    // Named-viewpoint overload (ThumbnailView arg). Original side also follows m_view.
    canvas->render_thumbnail(td,
        static_cast<unsigned int>(match_w), static_cast<unsigned int>(match_h),
        tp, canvas->get_volumes(), m_match_colors,
        Camera::EType::Ortho, m_view, false, false);
    match[plate_idx] = thumbnail_to_bitmap(td, match_w, match_h);
}

void MixedFilamentBatchDialog::render_original_thumb_for_plate(int plate_idx)
{
    // Render the ORIGINAL (un-matched) model for the given plate at the current viewpoint,
    // into the current viewpoint's original bucket. Mirrors render_match_thumb_for_plate
    // but skips the color swap: it builds the real extruder-color vector (physical + virtual
    // mixed display colours, same indexing as update_colors_by_extruder) with NO match
    // mapping applied, then renders offscreen with a local camera (does not disturb the
    // main 3D view). The original side follows the user's viewpoint (confirmed product
    // behavior — previously this was hardcoded to top view).
    auto& orig = orig_cache();
    if (plate_idx < 0 || plate_idx >= static_cast<int>(orig.size())) return;

    auto* plater = wxGetApp().plater();
    if (!plater) return;
    auto* canvas = plater->get_view3D_canvas3D();
    if (!canvas) return;

    const int orig_w = std::max(FromDIP(ORIG_PREVIEW_DIP), m_preview_orig_panel->GetClientSize().GetWidth());
    const int orig_h = std::max(FromDIP(ORIG_PREVIEW_DIP), m_preview_orig_panel->GetClientSize().GetHeight());

    std::vector<ColorRGBA> orig_colors;
    auto* pb = wxGetApp().preset_bundle;
    if (pb) {
        auto* co = pb->project_config.option<ConfigOptionStrings>("filament_colour");
        if (co) {
            for (size_t i = 0; i < co->values.size(); ++i) {
                // Same dual-color derivation as load_palette_colors; prevents an
                // empty filament_colour on a dual-color slot rendering black.
                const std::string hex = slot_match_color(
                    i < m_physical_multi_colors.size() ? m_physical_multi_colors[i] : std::string(),
                    i < m_physical_color_modes.size()  ? m_physical_color_modes[i]  : 0,
                    co->values[i]);
                unsigned char rgba[4] = {};
                BitmapCache::parse_color4(hex, rgba);
                orig_colors.push_back({
                    float(rgba[0]) / 255.f, float(rgba[1]) / 255.f,
                    float(rgba[2]) / 255.f, float(rgba[3]) / 255.f });
            }
        }
        for (const std::string& dc : pb->mixed_filaments.display_colors()) {
            if (dc.empty()) continue;
            unsigned char rgba[4] = {};
            BitmapCache::parse_color4(dc, rgba);
            orig_colors.push_back({
                float(rgba[0]) / 255.f, float(rgba[1]) / 255.f,
                float(rgba[2]) / 255.f, float(rgba[3]) / 255.f });
        }
    }
    if (orig_colors.empty()) return;

    ThumbnailsParams tp = { {}, false, true, true, true, plate_idx };
    ThumbnailData td;
    // Named-viewpoint overload (ThumbnailView arg) — original side follows m_view.
    canvas->render_thumbnail(td,
        static_cast<unsigned int>(orig_w), static_cast<unsigned int>(orig_h),
        tp, canvas->get_volumes(), orig_colors,
        Camera::EType::Ortho, m_view, false, false);
    orig[plate_idx] = thumbnail_to_bitmap(td, orig_w, orig_h);
}

// ---------------------------------------------------------------------------
// Data loading
// ---------------------------------------------------------------------------

void MixedFilamentBatchDialog::load_model_colors()
{
    // NOTE: legacy path — enumerates target colors from the model's painted MODEL_PART volumes.
    // The ctor now calls load_palette_colors() instead (full project palette: physical
    // filament_colour + enabled mixed display_colors). This function is kept for reference /
    // future use; no current caller. Do not delete without confirming nothing else depends on it.
    m_model_colors.clear();

    auto* plater = wxGetApp().plater();
    if (!plater) return;

    PresetBundle* pb = wxGetApp().preset_bundle;
    if (!pb) return;

    const size_t num_physical = pb->filament_presets.size();
    ConfigOptionStrings* co = pb->project_config.option<ConfigOptionStrings>("filament_colour");
    if (!co || co->values.empty()) return;

    // Collect all extruder IDs actually painted on the model's volumes,
    // NOT the palette index.  load_model_colors used to enumerate the
    // get_extruder_colors_from_plater_config() palette and use its array
    // index as the extruder_id, which misses (a) config-level extruder
    // assignments on volumes with no MMU paint, and (b) old mixed-filament
    // virtual IDs that are still painted on triangles but whose display
    // color may have been deduplicated against the physical palette.
    //
    // We now walk every MODEL_PART volume's get_extruders() (MMU paint
    // chunked facet data + config extruder_id), look up each extruder's
    // hex colour from the physical palette or the mixed-filament
    // display_color table, deduplicate by hex, and ACCUMULATE extruder
    // IDs per colour so every extruder that a given colour is painted with
    // lands in source_extruder_ids.  This guarantees that
    // apply_batch_match_to_model can remap every painted extruder,
    // including virtual mixed IDs from a prior batch match.

    // Map hex → accumulated extruder_ids (unordered, insert-order for log stability)
    std::vector<std::pair<std::string, std::vector<unsigned int>>> hex_to_eids;

    for (const ModelObject* mo : wxGetApp().model().objects) {
        for (const ModelVolume* mv : mo->volumes) {
            if (!mv || mv->type() != ModelVolumeType::MODEL_PART) continue;

            for (int eid : mv->get_extruders()) {
                if (eid < 1 || eid > int(MAXIMUM_EXTRUDER_NUMBER)) continue;

                std::string color_hex;
                if (size_t(eid - 1) < co->values.size()) {
                    // Physical filament colour
                    color_hex = co->values[size_t(eid - 1)];
                } else {
                    // Virtual mixed filament — look up its display_color
                    const MixedFilament* mf = pb->mixed_filaments.mixed_filament_from_id(
                        static_cast<unsigned int>(eid), num_physical);
                    if (mf && !mf->display_color.empty())
                        color_hex = mf->display_color;
                }
                if (color_hex.empty()) continue;

                // Normalize hex for dedup
                wxColour c;
                if (!try_parse_color_match_hex(color_hex, c)) continue;
                const wxString hex_norm = normalize_color_match_hex(color_hex);

                // Accumulate extruder IDs per (normalized) hex value
                auto it = std::find_if(hex_to_eids.begin(), hex_to_eids.end(),
                    [&](const auto& p) { return p.first == hex_norm; });
                if (it != hex_to_eids.end()) {
                    it->second.push_back(static_cast<unsigned int>(eid));
                } else {
                    hex_to_eids.push_back({hex_norm.ToStdString(), {static_cast<unsigned int>(eid)}});
                }
            }
        }
    }

    // If no model volumes provided extruders, fall back to the palette-index
    // enumeration so the dialog still opens with a useful colour list.
    if (hex_to_eids.empty()) {
        const auto all_colors = plater->get_extruder_colors_from_plater_config(nullptr, true);
        for (size_t i = 0; i < all_colors.size(); ++i) {
            const std::string& hex = all_colors[i];
            if (hex.empty()) continue;
            wxColour c;
            if (!try_parse_color_match_hex(hex, c)) continue;
            hex_to_eids.push_back({normalize_color_match_hex(hex).ToStdString(), {static_cast<unsigned int>(i + 1)}});
        }
    }

    for (size_t idx = 0; idx < hex_to_eids.size(); ++idx) {
        const std::string& hex = hex_to_eids[idx].first;
        const auto& eids      = hex_to_eids[idx].second;
        wxColour c;
        try_parse_color_match_hex(hex, c); // already validated above

        m_model_colors.push_back({
            static_cast<unsigned int>(m_model_colors.size() + 1),
            c, hex,
            eids
        });
    }

    // Product spec: cap at 64 distinct colors. This loop is the dedup source (each push is
    // a unique validated color), so the cap applies to unique colors. Drop the tail beyond
    // kMaxColors. The warning is deferred to build_ui because m_warning_panel is still null
    // here — the ctor runs load_model_colors before build_banners/build_ui, so calling
    // display_warning now would hit the early `if (!m_warning_panel) return` and be lost.
    while (m_model_colors.size() > kMaxColors) {
        m_model_colors.pop_back();
        m_pending_64_color_warning = true;
    }
}

// Enumerate the match target set from the project's FULL palette instead of from the model's
// painted volumes (the legacy load_model_colors path above, which is no longer called by the
// ctor — kept here for reference / future use).
//
// Source: Plater::get_extruder_colors_from_plater_config(nullptr, /*include_mixed=*/true),
// which returns [physical filament_colour hexes...][enabled mixed-filament display_colors...].
// The 1-based extruder id is the palette index + 1, matching mixed_filament_from_id's
// "physical+1 onwards is virtual" convention, so apply_batch_match_to_model's source_extruder_ids
// line up with the real (physical|virtual) extruder space.
//
// Palette colors the model never uses still produce a legend row, but apply stays a no-op for
// them: apply_batch_match_to_model walks model volumes' extruders and only remaps ids that
// actually appear, so an unused palette id never triggers a virtual-filament allocation.
//
// Dedup / validation / 64-color cap mirror load_model_colors exactly (same helpers, same
// m_pending_64_color_warning deferred-display contract — m_warning_panel is null here).
void MixedFilamentBatchDialog::load_palette_colors()
{
    m_model_colors.clear();

    auto* plater = wxGetApp().plater();
    if (!plater) return;

    // Map hex → accumulated extruder_ids (insert-order for stable legend ordering)
    std::vector<std::pair<std::string, std::vector<unsigned int>>> hex_to_eids;

    const auto all_colors = plater->get_extruder_colors_from_plater_config(nullptr, true);
    for (size_t i = 0; i < all_colors.size(); ++i) {
        // Dual-color slots: filament_colour may be empty or hold a placeholder while
        // the real colours live in filament_multi_colors. Derive the effective primary
        // so such slots enter the match with their actual colour. Only the physical
        // section carries multi-colour data; mixed display colours appended by
        // get_extruder_colors_from_plater_config are used verbatim (index past physical).
        std::string hex = all_colors[i];
        if (i < m_physical_multi_colors.size() && !m_physical_multi_colors[i].empty())
            hex = slot_match_color(m_physical_multi_colors[i],
                                   i < m_physical_color_modes.size() ? m_physical_color_modes[i] : 0,
                                   hex);
        if (hex.empty()) continue;
        wxColour c;
        if (!try_parse_color_match_hex(hex, c)) continue;
        const wxString hex_norm = normalize_color_match_hex(hex);

        auto it = std::find_if(hex_to_eids.begin(), hex_to_eids.end(),
            [&](const auto& p) { return p.first == hex_norm; });
        if (it != hex_to_eids.end()) {
            it->second.push_back(static_cast<unsigned int>(i + 1));
        } else {
            hex_to_eids.push_back({hex_norm.ToStdString(), {static_cast<unsigned int>(i + 1)}});
        }
    }

    for (size_t idx = 0; idx < hex_to_eids.size(); ++idx) {
        const std::string& hex = hex_to_eids[idx].first;
        const auto&        eids = hex_to_eids[idx].second;
        wxColour c;
        try_parse_color_match_hex(hex, c); // already validated above

        m_model_colors.push_back({
            static_cast<unsigned int>(m_model_colors.size() + 1),
            c, hex,
            eids
        });
    }

    // Same 64-color cap + deferred-warning contract as load_model_colors (see comment there).
    while (m_model_colors.size() > kMaxColors) {
        m_model_colors.pop_back();
        m_pending_64_color_warning = true;
    }
}

// The Full Spectrum preset name shown in the name column of every recommended-card row.
// Per product spec the name stays the preset label across all four slots; only the swatch
// color varies. Falls back to the raw name when the preset bundle is unavailable.
static wxString get_full_spectrum_preset_label()
{
    const std::string preset_name = full_spectrum_preset_name();
    if (auto* pb = wxGetApp().preset_bundle) {
        if (auto* p = pb->filaments.find_preset(preset_name))
            return wxString::FromUTF8(p->label(false));
    }
    return wxString::FromUTF8(preset_name);
}

// Load the real recommended-mode palette colors from the Full Spectrum filament preset
// (filaments_colours.json via FilamentColorLibrary). Returns the list of validated hex colors
// (one per single-color SKU); empty on any failure so the caller falls back to the canonical
// CMYW palette. NOTE: only the swatch COLORS come from here — the name column is the preset
// label (see get_full_spectrum_preset_label), NOT the per-color name.
//
// Safety review closure (harness: input-validation, thread-safety):
//   - (N) hex validation: every hex is re-checked via try_parse_color_match_hex; invalid
//     values are skipped with a warning (defense-in-depth on top of FilamentColorLibrary's
//     own NormalizeFilamentHexColor).
//   - Thread safety: FilamentColorLibrary::EnsureLoaded() is NOT thread-safe (_loaded is an
//     unlocked bool). The dialog ctor pre-warms it on the main thread; this function only
//     does a read-only FindFilamentByName afterwards, so it is safe to call from the worker
//     thread in launch_background_match. EnsureLoaded() here is a cheap no-op after warm-up.
//   - (HIGH) distinct logging: each failure path emits its own BOOST_LOG_TRIVIAL(warning).
static std::vector<std::string> load_full_spectrum_colors()
{
    if (!FilamentColorLibrary::Instance().EnsureLoaded()) {
        BOOST_LOG_TRIVIAL(warning) << "MixedFilamentBatchDialog: FilamentColorLibrary not loaded, fallback to canonical palette";
        return {};
    }
    FilamentColorInfo info;
    if (!FilamentColorLibrary::Instance().FindFilamentByName(full_spectrum_preset_name(), info)) {
        BOOST_LOG_TRIVIAL(warning) << "MixedFilamentBatchDialog: Full Spectrum preset not found in color library, fallback";
        return {};
    }

    std::vector<std::string> result;
    for (const FilamentColorItem& item : info.colors) {
        // Only single-color SKUs qualify as palette candidates; skip dual-color / gradient
        // entries (e.g. PLA Silk's Sunset Ember) so they don't pollute the palette.
        if (item.colorData.colors.size() != 1)
            continue;
        const std::string& hex = item.colorData.colors[0];
        // (N) Re-validate hex: FilamentColorLibrary normalises format, but double-check the
        // color is wxColour-constructible before it flows into get_extruder_color_icon /
        // recommend_best_filament_combo.
        wxColour parsed;
        if (!try_parse_color_match_hex(wxString::FromUTF8(hex.c_str()), parsed)) {
            BOOST_LOG_TRIVIAL(warning) << "MixedFilamentBatchDialog: invalid hex '" << hex << "' in Full Spectrum, skipped";
            continue;
        }
        result.push_back(hex);
    }
    return result;
}

// Full Spectrum palette as raw FilamentColorItems — same single-color-SKU filter and hex
// re-validation as load_full_spectrum_colors(), but keeps each item's colorNames map so the
// caller (build_recommended_card's tooltip) can show the localized per-color name. Returns
// empty on any failure; callers fall back to positional defaults. NOT thread-safe beyond the
// EnsureLoaded warm-up done in the ctor (see load_full_spectrum_colors's safety note).
static std::vector<FilamentColorItem> load_full_spectrum_items()
{
    if (!FilamentColorLibrary::Instance().EnsureLoaded()) return {};
    FilamentColorInfo info;
    if (!FilamentColorLibrary::Instance().FindFilamentByName(full_spectrum_preset_name(), info)) return {};

    std::vector<FilamentColorItem> result;
    for (const FilamentColorItem& item : info.colors) {
        if (item.colorData.colors.size() != 1) continue;
        wxColour parsed;
        if (!try_parse_color_match_hex(wxString::FromUTF8(item.colorData.colors[0].c_str()), parsed))
            continue;
        result.push_back(item);
    }
    return result;
}

// Look up a name from a FilamentColorItem's colorNames map for a specific language key.
// On hit, writes the decoded name into `out` and returns true; on miss, returns false and
// leaves `out` untouched. The JSON keys are short codes ("zh_CN","en"); callers try the full
// locale, then the base language, then a fallback.
//
// NOTE: must NOT return `&wxString::FromUTF8(...)` — that yields a pointer to a temporary
// wxString, which is destroyed at the semicolon, leaving a dangling pointer (UB / crash on
// deref). The bool+out-param form keeps ownership with the caller.
static bool color_name_for_lang(const FilamentColorItem& item, const wxString& key, wxString& out)
{
    auto it = item.colorNames.find(into_u8(key));
    if (it == item.colorNames.end()) return false;
    out = wxString::FromUTF8(it->second.c_str());
    return true;
}

// The ENGLISH color name — always present in colorNames (it's the SKU's canonical name in
// filaments_colours.json) regardless of UI locale. Used as a STABLE identity for TD family
// matching (FULL_SPECTRUM_TD is keyed by EN substrings: cyan/magenta/yellow/...). Falls back
// to whatever name is present, then to a positional "F<n>" label.
static wxString english_color_name(const FilamentColorItem& item, int position)
{
    wxString s;
    if (color_name_for_lang(item, "en", s)) return s;
    if (!item.colorNames.empty())
        return wxString::FromUTF8(item.colorNames.begin()->second.c_str());
    return wxString::Format("F%d", position + 1);
}

// Pick a localized color name from a FilamentColorItem's colorNames map, honouring the app's
// current language. Falls back to English, then to the SKU, then to the positional label.
static wxString localized_color_name(const FilamentColorItem& item, int position)
{
    // current_language_code() returns e.g. "zh_CN" / "en" / "en_US". Try the exact code first,
    // then the base language (strip region suffix), then English.
    const wxString lang = wxGetApp().current_language_code();
    wxString s;
    if (color_name_for_lang(item, lang, s)) return s;
    const wxString base = lang.BeforeFirst('_');
    if (color_name_for_lang(item, base, s)) return s;
    if (color_name_for_lang(item, wxString("en"), s)) return s;
    return english_color_name(item, position);
}

// Resolve the TD family for a filament color by matching the ENGLISH color name against the
// canonical-family substrings in FULL_SPECTRUM_TD (e.g. "Semi-Translucent Cyan" → family "C").
// Returns nullptr if no family matches. Case-insensitive substring match on the EN name so it
// tolerates adjectives ("Semi-Translucent") and prefix/suffix variation.
//
// Why English: the EN name is the SKU's canonical identity in filaments_colours.json and is
// always present regardless of UI locale, so family detection is locale-independent and stable.
static const TdEntry* resolve_td_family(const wxString& english_name)
{
    wxString lower = english_name.Lower();
    for (const TdEntry& e : FULL_SPECTRUM_TD)
        if (lower.find(e.family) != wxString::npos) return &e;
    return nullptr;
}

// Build the hover tooltip string for one recommended-mode row. Two lines:
//   <preset label>          e.g. "Snapmaker PLA Full Spectrum"
//   <color name> TD : <val> e.g. "Translucent Cyan TD : 5.5"
// The color name is localized; TD is resolved by the color's family via its English name
// (see resolve_td_family), NOT by palette position — so the TD stays bound to the right color
// even when the match reorders the palette. When TD is unknown (color family not in
// FULL_SPECTRUM_TD) the value shows "-".
static wxString make_recommended_tooltip(const wxString& localized_name, const wxString& english_name)
{
    const TdEntry* td = resolve_td_family(english_name);
    const wxString td_disp = td ? wxString::Format("%.1f", td->value) : wxString("-");
    return wxString::Format("%s\n%s %s : %s",
        get_full_spectrum_preset_label(),
        localized_name,
        _L("TD"),
        td_disp);
}

void MixedFilamentBatchDialog::update_recommended_card()
{
    if (!m_recommended_card) return;
    const auto& colors = m_result.recommended_physical_colors;
    if (colors.size() < 4) return;

    // NOTE: the name column shows the preset label (Snapmaker PLA Full Spectrum …) on every
    // row, NOT the per-color name — per product spec only the swatch color varies per row.
    // So no hex→name lookup is needed for the LABEL; the tooltip, however, shows the per-color
    // name and DOES need to resolve colors[i] → the right FilamentColorItem.

    // Re-load items and index them by HEX so each row resolves the correct color identity.
    // This is essential after a match: launch_background_match's ΔE fallback can REORDER the
    // palette, so colors[i] may not sit at position i in the preset's natural order. Looking
    // up by hex (rather than positional items[i]) binds the tooltip's name + TD to the actual
    // color on the swatch, not its grid slot. Hex comparison is case-insensitive to tolerate
    // "#08abfb" vs "#08ABFB" between config and library.
    const std::vector<FilamentColorItem> items = load_full_spectrum_items();
    std::unordered_map<std::string, const FilamentColorItem*> by_hex;
    by_hex.reserve(items.size());
    for (const FilamentColorItem& item : items) {
        if (item.colorData.colors.size() == 1) {
            std::string h = item.colorData.colors[0];
            std::transform(h.begin(), h.end(), h.begin(),
                           [](unsigned char ch) { return std::tolower(ch); });
            by_hex.emplace(std::move(h), &item);
        }
    }
    auto find_item = [&](const std::string& hex) -> const FilamentColorItem* {
        std::string h = hex;
        std::transform(h.begin(), h.end(), h.begin(),
                       [](unsigned char ch) { return std::tolower(ch); });
        auto it = by_hex.find(h);
        return it != by_hex.end() ? it->second : nullptr;
    };

    for (int i = 0; i < 4; ++i) {
        // Update swatch bitmap
        if (m_recommended_swatches[i]) {
            wxBitmap* icon = get_extruder_color_icon(
                colors[i], std::to_string(i + 1),
                FromDIP(20), FromDIP(20));
            if (icon)
                m_recommended_swatches[i]->SetBitmap(*icon);
        }

        // Update label text — the preset name (same on all four rows).
        if (m_recommended_labels[i]) {
            m_recommended_labels[i]->SetLabel(get_full_spectrum_preset_label());
        }

        // Refresh the row tooltip to reflect the now-rendered swatch color. Resolve the color
        // identity by HEX (colors[i]) so the name + TD match the actual swatch even if the
        // palette was reordered by the match. Falls back to a positional label if the hex is
        // unknown (e.g. library reload failed / a color outside the Full Spectrum preset).
        if (i < static_cast<int>(colors.size())) {
            const FilamentColorItem* item = find_item(colors[i]);
            const wxString ename = item ? english_color_name(*item, i)
                                        : wxString::Format("F%d", i + 1);
            const wxString cname = item ? localized_color_name(*item, i) : ename;
            const wxString tip = make_recommended_tooltip(cname, ename);
            if (m_recommended_swatches[i]) m_recommended_swatches[i]->SetToolTip(tip);
            if (m_recommended_labels[i])   m_recommended_labels[i]->SetToolTip(tip);
            wxWindow* row = m_recommended_swatches[i] ? m_recommended_swatches[i]->GetParent() : nullptr;
            if (row) row->SetToolTip(tip);
        }
    }

    m_recommended_card->Layout();
}

// ---------------------------------------------------------------------------
// UI — aligned to Figma design (node 27535:68094 "auto match")
// ---------------------------------------------------------------------------

void MixedFilamentBatchDialog::build_ui()
{
    SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F8F7F7")));
    m_root = new wxBoxSizer(wxVERTICAL);

    // No in-content title strip: the native OS title bar (set in the ctor via wxCAPTION)
    // already shows "Mixed Color Match". Adding a second header here would duplicate it.
    build_mode_row();
    // Error/warning banners sit BELOW the mode row (between mode row and scrolled content)
    // so toggling them doesn't shift the mode selector the user just interacted with.
    build_banners();

    // Page-level scroller (mirrors MixedFilamentDialog): spans full dialog width so the
    // scrollbar sits at the right edge; mode row / progress / footer stay fixed.
    m_scrolled_content = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_scrolled_content->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F8F7F7")));
    m_scrolled_content->SetScrollRate(0, FromDIP(8));
    m_scrolled_content->Bind(wxEVT_CHILD_FOCUS, [](wxChildFocusEvent&) {});
    auto* scroll_sizer = new wxBoxSizer(wxVERTICAL);
    build_manual_card(*scroll_sizer);
    build_recommended_card(*scroll_sizer);
    // 12px vertical gap between cards (only one filament-config card is visible per mode).
    scroll_sizer->AddSpacer(FromDIP(12));
    build_preview_card(*scroll_sizer);
    scroll_sizer->AddSpacer(FromDIP(12));
    build_mapping_card(*scroll_sizer);
    m_scrolled_content->SetSizer(scroll_sizer);
    // Width floor must match the dialog width or cards overflow horizontally (MultiMachineManagerPage).
    m_scrolled_content->SetMinSize(wxSize(FromDIP(500), FromDIP(80)));
    // 12px top gap separates the scrolled content from whatever sits above (mode row, or the
    // last visible banner). Applied here rather than as wxBOTTOM on the banners/mode row so a
    // banner sits flush against the mode row when shown.
    m_root->Add(m_scrolled_content, 1, wxEXPAND | wxTOP, FromDIP(12));

    // Footer (Cancel/Confirm) above progress: Figma spec has the match-progress bar pinned
    // to the dialog's bottom edge, with the action buttons sitting just above it. Order
    // matters here — VERTICAL sizer renders in insertion order, so footer is added first.
    // (Progress bar + label live inside build_footer's panel; no separate build_progress step.)
    build_footer();

    SetSizer(m_root);
    // Set the client area explicitly. Original code used SetSize(540, 680) which includes
    // OS title bar + borders on wxMSW (~17px), so SetSize(500) yields a ~483px client area
    // and the 460px cards sit at uneven ~11px margins instead of the intended 20px.
    // SetClientSize(500) makes the client area exactly 500 → cards get (500-460)/2 = 20px.
    // Layout() then stretches the sizer to fill the 500×700 client area. (We do NOT use
    // SetMin/MaxClientSize==value to lock: m_scrolled_content's SetMinSize height floor of
    // 80px makes the sizer's computed best size much smaller than 700, so Fit() collapses
    // the window; SetClientSize forces the size directly, bypassing that computation.)
    SetClientSize(FromDIP(500), FromDIP(700));
    Layout();

    update_add_remove_buttons();
    // Initialize plate-nav arrow enabled state. Without this the arrows inherit
    // ScalableButton's default (Enabled=true) and stay clickable even when there's
    // nowhere to navigate — most visible when there's a single plate (both arrows
    // should be disabled) but also wrong for multi-plate (prev stays enabled at plate 1).
    // update_nav_arrow_state is otherwise only called from combo/nav callbacks, so the
    // initial state was never set.
    update_nav_arrow_state();

    // Deferred from load_model_colors: if the model had >64 distinct colors, the extras were
    // dropped there. m_warning_panel is now created (build_banners ran above in build_ui),
    // so the advisory can finally be shown.
    if (m_pending_64_color_warning)
        display_warning(_L("Filament color limit reached (64 colors). Colors over the limit have been removed."));
}

void MixedFilamentBatchDialog::build_banners()
{
    // Error banner
    m_error_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_error_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FDE8E8")));
    m_error_panel->Hide();
    auto* es = new wxBoxSizer(wxHORIZONTAL);
    ScalableBitmap ebmp(m_error_panel, "error_icon_red_exclamation", 14);
    auto* e_icon = new wxStaticBitmap(m_error_panel, wxID_ANY, ebmp.bmp());
    // §17: wxStaticBitmap does not inherit the panel bg on wxMSW — the icon SVG has
    // transparent pixels that would show the system color instead of #FDE8E8.
    e_icon->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FDE8E8")));
    es->Add(e_icon, 0, wxALL, FromDIP(8));
    m_error_text = new Label(m_error_panel, Label::Body_12, wxEmptyString, LB_AUTO_WRAP);
    m_error_text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#D32F2F")));
    m_error_text->SetMaxSize(wxSize(FromDIP(480), -1));
    es->Add(m_error_text, 1, wxALL, FromDIP(8));
    m_error_panel->SetSizer(es);
    m_root->Add(m_error_panel, 0, wxEXPAND);

    // Warning banner — mirrors MixedFilamentDialog's banner 1:1 (same bg, icon, margins,
    // tab-traversal flag) so the two dialogs read as the same family.
    m_warning_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                   wxBORDER_NONE | wxTAB_TRAVERSAL);
    m_warning_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFF3EB")));
    m_warning_panel->Hide();
    auto* ws = new wxBoxSizer(wxHORIZONTAL);
    ScalableBitmap wbmp(m_warning_panel, "icon_warning_triangle", 14);
    auto* w_icon = new wxStaticBitmap(m_warning_panel, wxID_ANY, wbmp.bmp());
    // §17: see e_icon above — transparent SVG pixels need the panel bg to render correctly on wxMSW.
    w_icon->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFF3EB")));
    ws->Add(w_icon, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    ws->AddSpacer(FromDIP(4));
    m_warning_text = new Label(m_warning_panel, Label::Body_12, wxEmptyString, LB_AUTO_WRAP);
    m_warning_text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#FF842D")));
    m_warning_text->SetMaxSize(wxSize(FromDIP(480), -1));
    ws->Add(m_warning_text, 1, wxALL, FromDIP(8));
    m_warning_panel->SetSizer(ws);
    // No bottom margin on the banners: the gap to the scrolled content below is applied as wxTOP
    // on m_scrolled_content, so the banner sits flush against the mode row above it. wxBoxSizer
    // collapses both the window and its border when the window is hidden, so idle state (both
    // banners hidden) adds no whitespace.
    m_root->Add(m_warning_panel, 0, wxEXPAND);
}

void MixedFilamentBatchDialog::build_mode_row()
{
    // Host panel for the mode row so the strip renders on a solid white background
    // instead of the dialog's #F8F7F7 (matches the filament-config cards' bg).
    m_mode_row_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_mode_row_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    {
        auto* lbl = new wxStaticText(m_mode_row_panel, wxID_ANY, _L("Match Mode"));
        lbl->SetFont(Label::Body_14);
        lbl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
        lbl->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        // Figma Container: row px=20; label→combo gap=8. Put the 20px row padding on the
        // sizer (wxLEFT|wxRIGHT) so it applies once to the whole row, and the 8px label→combo
        // gap on the label's right side only — avoids the old double-counted left padding.
        row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    }
    m_method_combo = new ComboBox(m_mode_row_panel, wxID_ANY, wxEmptyString,
                                   wxDefaultPosition, wxSize(FromDIP(149), FromDIP(30)),
                                   0, nullptr, wxCB_READONLY);
    // Figma: combo text = 14px (same as the "Match Mode" label).
    m_method_combo->SetFont(Label::Body_14);
    m_method_combo->Append(_L("Auto"));
    m_method_combo->Append(_L("Manual"));
    m_method_combo->SetSelection(0);
    m_method_combo->Bind(wxEVT_COMBOBOX, &MixedFilamentBatchDialog::on_method_changed, this);
    // Seed the tooltip for the default (Auto) mode — on_method_changed keeps it in sync
    // on every subsequent switch.
    update_method_combo_tooltip();
    row->Add(m_method_combo, 0, wxALIGN_CENTER_VERTICAL);

    row->AddStretchSpacer();

    // Segmented Start / Re-match tabs (80x28). Active tab is teal (#009688), inactive gray
    // (#d9d9d9); set_match_buttons_state drives which is enabled based on match progress.
    auto make_tab = [this, panel = m_mode_row_panel](Button*& slot, const wxString& label) {
        auto* btn = new Button(panel, label);
        btn->SetMinSize(wxSize(FromDIP(80), FromDIP(28)));
        btn->SetCornerRadius(FromDIP(4));
        btn->SetBorderWidth(0);
        // Figma: tab label = 12px (Body_12).
        btn->SetFont(Label::Body_12);
        // #FEFEFE (not #FFFFFF): dark map keeps it white; #FFFFFF maps to the dark bg.
        btn->SetBackgroundColor(StateColor(
            std::pair(wxColour("#DFDFDF"), static_cast<int>(StateColor::Disabled)),
            std::pair(wxColour("#009688"), static_cast<int>(StateColor::Normal))));
        btn->SetTextColor(StateColor(
            std::pair(wxColour("#6B6A6A"), static_cast<int>(StateColor::Disabled)),
            std::pair(wxColour("#FEFEFE"), static_cast<int>(StateColor::Normal))));
        slot = btn;
        return btn;
    };
    // Figma: two tabs separated by gap=8 (provided by Start tab's wxRIGHT), with NO margin
    // after the last tab — the 20px right padding lives on the row sizer (wxRIGHT below).
    row->Add(make_tab(m_btn_start_match, _L("Start Matching")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    row->Add(make_tab(m_btn_rematch, _L("Rematch")), 0, wxALIGN_CENTER_VERTICAL);

    m_btn_start_match->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { start_batch_match(); });
    m_btn_rematch->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { start_batch_match(); });

    // Wrap the horizontal row with 10px vertical padding inside the white panel so the
    // strip has breathing room above/below the controls (panel height = 10 + content + 10),
    // matching how the footer panel pads its own contents.
    auto* outer = new wxBoxSizer(wxVERTICAL);
    // Top hairline divider (#F0F0F0, theme-aware) — same technique as the footer's top_line,
    // so the strip reads as a separate section sitting flush under the title bar.
    {
        auto* top_line = new wxPanel(m_mode_row_panel, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
        top_line->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F0F0F0")));
        outer->Add(top_line, 0, wxEXPAND);
    }
    outer->AddSpacer(FromDIP(10));
    // Figma Container: px=20. Applied here on the row so left/right edges are symmetric and
    // the label/combo/tabs sit at the same horizontal insets as the cards' contents.
    outer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(20));
    outer->AddSpacer(FromDIP(10));
    m_mode_row_panel->SetSizer(outer);
    // Full-width like the footer panel (no left/right margin) so the white strip spans the
    // dialog content area edge-to-edge. No top/bottom margin — strip sits flush under the title
    // bar and directly above the error/warning banners (build_banners runs next). The 12px gap
    // to whatever appears below (banner, or scrolled content when banners are hidden) is applied
    // as wxTOP on m_scrolled_content so the banner sits flush against the mode row when shown.
    m_root->Add(m_mode_row_panel, 0, wxEXPAND);
}

void MixedFilamentBatchDialog::build_manual_card(wxBoxSizer& parent)
{
    auto* card = new StaticBox(m_scrolled_content, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    card->SetCornerRadius(FromDIP(4));
    card->SetBorderWidth(FromDIP(1));
    card->SetBorderColorNormal(StateColor::darkModeColorFor(wxColour("#F0F0F0")));
    card->SetBackgroundColor(StateColor(std::pair(wxColour("#FFFFFF"), static_cast<int>(StateColor::Normal))));
    // All four cards share the same fixed width + centered alignment so their edges line up
    // with consistent margins — mirrors MixedFilamentDialog's fixed-width (325) cards.
    card->SetMinSize(wxSize(FromDIP(CARD_WIDTH_DIP), -1));
    card->SetMaxSize(wxSize(FromDIP(CARD_WIDTH_DIP), -1));
    card->Hide();
    m_manual_card = card;
    auto* s = new wxBoxSizer(wxVERTICAL);

    // Title row + add/remove (same pattern as MixedFilamentDialog)
    {
        auto* title_row = new wxBoxSizer(wxHORIZONTAL);
        auto* lbl = new wxStaticText(card, wxID_ANY, _L("Filament Setup"));
        lbl->SetFont(Label::Body_14);
        // §17/§81: wxStaticText does not inherit the card's bg on wxMSW and may be
        // overridden by the GTK theme — set fg+bg explicitly via darkModeColorFor.
        lbl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
        lbl->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        title_row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);
        title_row->AddStretchSpacer();

        m_btn_remove_filament = new ScalableButton(card, wxID_ANY, "icon_minus");
        m_btn_remove_filament->SetToolTip(_L("Remove filament"));
        m_btn_remove_filament->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            if (m_manual_filament_count > 2) {
                --m_manual_filament_count;
                m_manual_row_panels[m_manual_filament_count]->Hide();
                on_manual_selection_changed();
                update_add_remove_buttons();
                relayout_scrolled_content();
            }
        });
        title_row->Add(m_btn_remove_filament, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));

        m_btn_add_filament = new ScalableButton(card, wxID_ANY, "icon_plus");
        m_btn_add_filament->SetToolTip(_L("Add filament"));
        m_btn_add_filament->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            // Cap at the number of physical filaments — you can't pick more rows than the
            // spools the printer actually has — and never exceed the 4-row UI bound.
            const int max_rows = std::min<int>(4, static_cast<int>(m_physical_colors.size()));
            if (m_manual_filament_count < max_rows) {
                m_manual_row_panels[m_manual_filament_count]->Show();
                ++m_manual_filament_count;
                on_manual_selection_changed();
                update_add_remove_buttons();
                relayout_scrolled_content();
            }
        });
        title_row->Add(m_btn_add_filament, 0, wxALIGN_CENTER_VERTICAL);
        s->Add(title_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));
    }

    // No divider between title and grid — Figma spec relies on whitespace alone.
    // (The previous 1px #F3F4F6 wxPanel divider rendered as a stray dark line on wxMSW
    // because wxPanel doesn't reliably honour SetBackgroundColour inside a StaticBox;
    // see §137 of the wxWidgets pitfalls checklist.)
    s->AddSpacer(FromDIP(10)); // title-to-grid gap per Figma spec

    // 2-column grid of filament rows — a single preset-name combo per row whose dropdown
    // items carry a numbered color icon (the filament number lives in that icon, not a label).
    PresetBundle* pb = wxGetApp().preset_bundle;
    const std::vector<std::string>& fps = pb ? pb->filament_presets : std::vector<std::string>();
    auto* grid = new wxFlexGridSizer(2, FromDIP(12), FromDIP(12));
    grid->AddGrowableCol(0, 1);
    grid->AddGrowableCol(1, 1);
    for (int i = 0; i < 4; ++i) {
        auto* panel = new wxPanel(card, wxID_ANY);
        // wxPanel/StaticText don't inherit the card's bg — set white explicitly so the
        // row doesn't show the dialog's #F8F7F7 through (same as MixedFilamentDialog rows).
        panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        // Pin a fixed column width so the 2×2 slots stay equal regardless of how long the
        // selected filament's name is. wxFlexGridSizer sizes columns by content (best size)
        // first, then distributes leftover via AddGrowableCol(0/1, 1) — without a pinned
        // base width, slots 1/2 and 3/4 drift apart when names differ in length.
        panel->SetMinSize(wxSize(FromDIP(FILAMENT_COL_WIDTH_DIP), -1));
        panel->SetMaxSize(wxSize(FromDIP(FILAMENT_COL_WIDTH_DIP), -1));
        auto* r = new wxBoxSizer(wxHORIZONTAL);

        // §68: read-only selection would normally call for wxChoice, but this
        // custom ComboBox carries a numbered color icon (SetIcon in
        // set_manual_combo_icon) that wxChoice cannot render, so ComboBox is
        // intentional here — do not "fix" to wxChoice.
        auto* cb = new ComboBox(panel, wxID_ANY, wxEmptyString,
                                 wxDefaultPosition, wxSize(-1, FromDIP(30)),
                                 0, nullptr, wxCB_READONLY);
        for (size_t j = 0; j < m_physical_colors.size(); ++j) {
            wxString name;
            if (pb && j < fps.size()) {
                const Preset* preset = pb->filaments.find_preset(fps[j]);
                if (preset) name = from_u8(preset->label(false));
            }
            if (name.empty()) name = wxString::Format("F%zu", j + 1);
            const std::string&  multi_colors  = m_physical_multi_colors[j];
            const FilamentColorMode color_mode = FilamentColorModeFromConfig(m_physical_color_modes[j]);
            wxBitmap* icon = FilamentColorUtils::GetFilamentColorIcon(multi_colors, color_mode, m_physical_colors[j],
                std::to_string(j + 1), FromDIP(20), FromDIP(20));
            cb->Append(name, icon ? icon->ConvertToImage() : wxNullImage);
        }
        if (m_filament_selections[i] >= 0 && m_filament_selections[i] < static_cast<int>(m_physical_colors.size()))
            cb->SetSelection(m_filament_selections[i]);
        else if (!m_physical_colors.empty())
            cb->SetSelection(0);
        // Combined arrow+badge icon so the combo keeps its drop-down arrow alongside the
        // numbered color swatch (ComboBox hides the arrow when an item image is set).
        set_manual_combo_icon(i, cb->GetSelection());
        cb->Bind(wxEVT_COMBOBOX, [this, i](wxCommandEvent&) {
            if (m_filament_combo[i]) {
                // Record the user's selection so the failed/cancelled re-match restore
                // gate (input_intact) can detect that the manual input changed.
                m_filament_selections[i] = m_filament_combo[i]->GetSelection();
                set_manual_combo_icon(i, m_filament_combo[i]->GetSelection());
            }
            on_manual_selection_changed();
        });
        r->Add(cb, 1, wxALIGN_CENTER_VERTICAL);
        m_filament_combo[i] = cb;
        panel->SetSizer(r);
        if (i >= m_manual_filament_count) panel->Hide();
        m_manual_row_panels[i] = panel;
        grid->Add(panel, 0, wxEXPAND);
    }
    s->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    card->SetSizer(s);
    // Horizontal margin only; the card-to-card vertical gap is added at the build_ui call site.
    // 12px pairs with CARD_WIDTH_DIP (459 = 500 − 2×12 − 17 scrollbar) so cards stay centered.
    parent.Add(card, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, FromDIP(12));
}

void MixedFilamentBatchDialog::build_recommended_card(wxBoxSizer& parent)
{
    auto* card = new StaticBox(m_scrolled_content, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    card->SetCornerRadius(FromDIP(4));
    card->SetBorderWidth(FromDIP(1));
    card->SetBorderColorNormal(StateColor::darkModeColorFor(wxColour("#F0F0F0")));
    card->SetBackgroundColor(StateColor(std::pair(wxColour("#FFFFFF"), static_cast<int>(StateColor::Normal))));
    // Same fixed width + centering as the manual card (see build_manual_card).
    card->SetMinSize(wxSize(FromDIP(CARD_WIDTH_DIP), -1));
    card->SetMaxSize(wxSize(FromDIP(CARD_WIDTH_DIP), -1));
    card->Hide();
    m_recommended_card = card;
    auto* s = new wxBoxSizer(wxVERTICAL);

    // Title — "Filament Setup"; colors are now driven by the Full Spectrum
    // preset.
    {
        auto* lbl = new wxStaticText(card, wxID_ANY, _L("Filament Setup"));
        lbl->SetFont(Label::Body_14);
        // §17/§81: explicit fg+bg so the label matches the card on wxMSW and
        // resists GTK theme override.
        lbl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
        lbl->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        s->Add(lbl, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(16));
    }
    // No divider between title and grid — matches build_manual_card (Figma spec relies on
    // whitespace alone; the old 1px wxPanel divider showed as a stray dark line on wxMSW,
    // see §137).
    s->AddSpacer(FromDIP(10)); // title-to-grid gap per Figma spec

    // Load the real palette colors from the Full Spectrum preset (filaments_colours.json).
    // Falls back to the canonical palette when the preset is missing or yields <4 validated
    // single-color SKUs. Only the swatch COLORS come from here; the name column is the preset
    // label (see get_full_spectrum_preset_label). See load_full_spectrum_colors() for the
    // safety review (hex validation, thread safety, distinct logging).
    std::vector<std::string> palette = load_full_spectrum_colors();
    if (palette.size() < 4) {
        palette.clear();
        for (const std::string& c : FULL_SPECTRUM_FALLBACK_COLORS)
            palette.push_back(c);
    }
    const int fill_count = std::min<int>(4, static_cast<int>(palette.size()));

    // Also load the raw FilamentColorItems (same filter/validation) so the per-row hover
    // tooltip can show the localized color NAME. Loaded in lock-step with `palette` (same
    // preset, same single-color-SKU filter, same order) so palette[i] ↔ items[i]. If the
    // loader fails (library not loaded / preset missing) items is empty and the tooltip
    // builder falls back to the positional label "F<i+1>".
    const std::vector<FilamentColorItem> items = load_full_spectrum_items();

    // 2x2 grid: numbered swatch (20x20) + bordered name field. update_recommended_card()
    // refreshes swatches after a match reflects the palette order chosen by
    // recommend_best_filament_combo.
    auto* grid = new wxFlexGridSizer(2, FromDIP(12), FromDIP(12));
    grid->AddGrowableCol(0, 1);
    grid->AddGrowableCol(1, 1);
    for (int i = 0; i < 4; ++i) {
        // Per Figma (node 28325:94417 "Container"): the WHOLE row is a bordered container
        // (1px #dbdbdb, sharp corners — no rounded-* class, height 30, horizontal padding 9px)
        // holding swatch + name together. The previous implementation bordered only the name
        // `field`, leaving the numbered swatch sitting outside the border — the most visible
        // mismatch with Figma. Sharp corners: the spec shows a plain `border border-[#dbdbdb]`
        // with no `rounded-*` utility, so SetCornerRadius is 0 (StaticBox's default is 0, but
        // we set it explicitly for clarity against future edits).
        auto* panel = new StaticBox(card, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        panel->SetCornerRadius(FromDIP(0));
        panel->SetBorderWidth(FromDIP(1));
        panel->SetBorderColorNormal(StateColor::darkModeColorFor(wxColour("#DBDBDB")));
        panel->SetBackgroundColor(StateColor(std::pair(wxColour("#FFFFFF"), static_cast<int>(StateColor::Normal))));
        // Pin a fixed column width so the 2×2 slots stay equal — see build_manual_card for
        // the wxFlexGridSizer rationale (keeps recommended + manual rows aligned on switch).
        // Height 30 matches Figma; pinning min+max keeps the four rows visually uniform.
        panel->SetMinSize(wxSize(FromDIP(FILAMENT_COL_WIDTH_DIP), FromDIP(30)));
        panel->SetMaxSize(wxSize(FromDIP(FILAMENT_COL_WIDTH_DIP), FromDIP(30)));
        auto* r = new wxBoxSizer(wxHORIZONTAL);
        // Numbered color swatch (20×20, stored for later update). Guard with fill_count so a
        // short palette can never read out of bounds (the fallback above always yields 4).
        // wxLEFT=9 is the bordered container's left inset (Figma: px=9).
        if (i < fill_count) {
            wxBitmap* icon = get_extruder_color_icon(palette[i], std::to_string(i + 1), FromDIP(20), FromDIP(20));
            if (icon) {
                auto* sw = new wxStaticBitmap(panel, wxID_ANY, *icon);
                // §17: wxStaticBitmap does not inherit the panel bg. The icon is opaque so it
                // covers the control, but set the bg explicitly to match the card and resist
                // GTK theme override.
                sw->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
                m_recommended_swatches[i] = sw;
                r->Add(sw, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(9));
            }
        }
        // Name — the preset label (same on every row; only the swatch color varies, per product
        // spec). 14px per Figma, with wxST_ELLIPSIZE_END so a long preset name ("Snapmaker PLA
        // Full Spectrum @U1 0.4 nozzle") truncates instead of wrapping and breaking the row.
        // The name sits directly in the bordered row (no separate bordered `field`); wxLEFT=8
        // is the swatch→name gap (Figma: gap 8), wxRIGHT=8 the trailing inset (Figma 9 − 1px).
        auto* name = new wxStaticText(panel, wxID_ANY, get_full_spectrum_preset_label(),
                                       wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
        name->SetFont(Label::Body_14);
        name->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
        name->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        m_recommended_labels[i] = name;
        r->Add(name, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(8));
        panel->SetSizer(r);
        // Per-row hover tooltip: per-color NAME (localized) + COLOR hex + TD value (product spec).
        // TD is resolved by the color's ENGLISH family name (cyan→C, magenta→M, ...), NOT by
        // palette position — so it stays bound to the right color even when a match reorders the
        // palette. The tooltip is set on the row panel AND its children (swatch, name): wx child
        // windows do NOT inherit the parent tooltip, so without setting it on each child, hovering
        // directly over the swatch/name shows nothing. update_recommended_card re-applies it after
        // a match.
        if (i < fill_count) {
            const wxString ename = (i < static_cast<int>(items.size()))
                ? english_color_name(items[i], i)
                : wxString::Format("F%d", i + 1);
            const wxString cname = (i < static_cast<int>(items.size()))
                ? localized_color_name(items[i], i)
                : ename;
            const wxString tip = make_recommended_tooltip(cname, ename);
            panel->SetToolTip(tip);
            if (m_recommended_swatches[i]) m_recommended_swatches[i]->SetToolTip(tip);
            name->SetToolTip(tip);
        }
        grid->Add(panel, 0, wxEXPAND);
    }
    s->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    card->SetSizer(s);
    // Horizontal margin only; the card-to-card vertical gap is added at the build_ui call site.
    // 12px pairs with CARD_WIDTH_DIP (459 = 500 − 2×12 − 17 scrollbar) so cards stay centered.
    parent.Add(card, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, FromDIP(12));
}

void MixedFilamentBatchDialog::build_preview_card(wxBoxSizer& parent)
{
    m_preview_card = new StaticBox(m_scrolled_content, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_preview_card->SetCornerRadius(FromDIP(4));
    m_preview_card->SetBorderWidth(FromDIP(1));
    m_preview_card->SetBorderColorNormal(StateColor::darkModeColorFor(wxColour("#F0F0F0")));
    m_preview_card->SetBackgroundColor(StateColor(std::pair(wxColour("#FFFFFF"), static_cast<int>(StateColor::Normal))));
    // Same fixed width + centering as the filament-config cards.
    m_preview_card->SetMinSize(wxSize(FromDIP(CARD_WIDTH_DIP), -1));
    m_preview_card->SetMaxSize(wxSize(FromDIP(CARD_WIDTH_DIP), -1));
    auto* s = new wxBoxSizer(wxVERTICAL);

    // Dual previews: Original (fixed 180x180) + After Match (fixed 227x227), both rounded.
    // RoundedPreviewPanel paints the rounded background + thumbnail + corner mask + badge in
    // one wxBG_STYLE_PAINT handler (codebase's proven ImageGrid pattern); see the class doc
    // for why this replaces the earlier StaticBox + child wxStaticBitmap approach.
    {
        auto* prow = new wxBoxSizer(wxHORIZONTAL);
        m_preview_orig_panel = new RoundedPreviewPanel(m_preview_card, 180, 8, _L("Original"));
        // Top-align: the smaller (180h) original lines up with the 227h match panel; 20 DIP gap per Figma.
        prow->Add(m_preview_orig_panel, 0, wxALIGN_TOP | wxRIGHT, FromDIP(20));

        m_preview_match_panel = new RoundedPreviewPanel(m_preview_card, 227, 8, _L("Matched"));
        prow->Add(m_preview_match_panel, 0, wxALIGN_TOP);
        s->Add(prow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));
    }

    // Divider between previews and controls.
    // NOTE: build_manual_card / build_recommended_card dropped their 1px wxPanel
    // dividers because a 1px wxPanel rendered as a stray dark line on wxMSW
    // (§17: a wxPanel does not reliably honour SetBackgroundColour inside a
    // coloured container). This one is KEPT because it separates two visually
    // distinct sections (previews vs. controls) and its #F3F4F6 fill is wrapped
    // in darkModeColorFor. If it ever renders incorrectly on Windows, remove it
    // and rely on the spacing already in the sizer (AddSpacer pattern), as the
    // other two cards do.
    //
    // 8 DIP gap above/below via AddSpacer: divider needs wxLEFT|wxRIGHT=16, and
    // wxBoxSizer's single border value can't express 8-v / 16-h in one flag.
    s->AddSpacer(FromDIP(8));
    {
        auto* divider = new wxPanel(m_preview_card, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
        divider->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F3F4F6")));
        s->Add(divider, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
    }
    s->AddSpacer(FromDIP(8));

    // Centered controls: Plate (prev arrow + label + combo + next arrow) | View (label + combo)
    {
        auto* crow = new wxBoxSizer(wxHORIZONTAL);
        m_btn_tray_prev = new ScalableButton(m_preview_card, wxID_ANY, "filament_picker_left_arrow");
        m_btn_tray_prev->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_tray_nav(-1); });
        crow->Add(m_btn_tray_prev, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));

        auto* plate_lbl = new wxStaticText(m_preview_card, wxID_ANY, _L("Plate"));
        plate_lbl->SetFont(Label::Body_12);
        plate_lbl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#4A4A4A")));
        // wxStaticText doesn't inherit the card's white bg — set it explicitly so the label
        // doesn't show the dialog's #F8F7F7 through (same as the other card labels above).
        plate_lbl->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        crow->Add(plate_lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));

        m_tray_combo = new ComboBox(m_preview_card, wxID_ANY, wxEmptyString,
                                     wxDefaultPosition, wxSize(FromDIP(56), FromDIP(24)),
                                     0, nullptr, wxCB_READONLY);
        for (int i = 1; i <= m_tray_count; ++i)
            m_tray_combo->Append(wxString::Format("%02d", i));
        // Reflect the plate the user has selected in the plater (set in the ctor from
        // get_curr_plate_index). Falls back to 0 when m_tray_index is out of range
        // (defensive — ctor clamps it, but guard anyway).
        m_tray_combo->SetSelection(m_tray_index >= 1 && m_tray_index <= m_tray_count
                                       ? m_tray_index - 1 : 0);
        m_tray_combo->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) {
            m_tray_index = m_tray_combo->GetSelection() + 1;
            update_nav_arrow_state();
            refresh_previews();
        });
        crow->Add(m_tray_combo, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));

        m_btn_tray_next = new ScalableButton(m_preview_card, wxID_ANY, "filament_picker_right_arrow");
        m_btn_tray_next->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_tray_nav(1); });
        crow->Add(m_btn_tray_next, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(48));

        auto* view_lbl = new wxStaticText(m_preview_card, wxID_ANY, _L("View"));
        view_lbl->SetFont(Label::Body_12);
        view_lbl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#4A4A4A")));
        // Same as plate_lbl above — explicit white bg so the label matches the card.
        view_lbl->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        crow->Add(view_lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));

        m_view_combo = new ComboBox(m_preview_card, wxID_ANY, wxEmptyString,
                                     wxDefaultPosition, wxSize(FromDIP(130), FromDIP(24)),
                                     0, nullptr, wxCB_READONLY);
        // Order MUST match kComboToView[] in the header — on_view_changed maps by index.
        m_view_combo->Append(_L("Isometric"));   // 0 -> Iso
        m_view_combo->Append(_L("Top-Front"));   // 1 -> TopFront
        m_view_combo->Append(_L("Left"));        // 2 -> Left
        m_view_combo->Append(_L("Right"));       // 3 -> Right
        m_view_combo->Append(_L("Top"));         // 4 -> Top
        m_view_combo->Append(_L("Bottom"));      // 5 -> Bottom
        m_view_combo->Append(_L("Front"));       // 6 -> Front
        m_view_combo->Append(_L("Rear"));        // 7 -> Rear
        m_view_combo->SetSelection(0);
        m_view_combo->Bind(wxEVT_COMBOBOX, &MixedFilamentBatchDialog::on_view_changed, this);
        crow->Add(m_view_combo, 0, wxALIGN_CENTER_VERTICAL);

        // wxTOP dropped: 8 DIP gap to divider is provided by the AddSpacer above.
        s->Add(crow, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    }

    m_preview_card->SetSizer(s);
    // 12px margin pairs with CARD_WIDTH_DIP (see build_manual_card).
    parent.Add(m_preview_card, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, FromDIP(12));
}

void MixedFilamentBatchDialog::build_mapping_card(wxBoxSizer& parent)
{
    m_mapping_card = new StaticBox(m_scrolled_content, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_mapping_card->SetCornerRadius(FromDIP(4));
    m_mapping_card->SetBorderWidth(FromDIP(1));
    m_mapping_card->SetBorderColorNormal(StateColor::darkModeColorFor(wxColour("#F0F0F0")));
    m_mapping_card->SetBackgroundColor(StateColor(std::pair(wxColour("#FFFFFF"), static_cast<int>(StateColor::Normal))));
    // Same fixed width + centering as the filament-config cards.
    m_mapping_card->SetMinSize(wxSize(FromDIP(CARD_WIDTH_DIP), -1));
    m_mapping_card->SetMaxSize(wxSize(FromDIP(CARD_WIDTH_DIP), -1));
    auto* cs = new wxBoxSizer(wxVERTICAL);

    // Title row: label
    {
        auto* tr = new wxBoxSizer(wxHORIZONTAL);
        auto* lbl = new wxStaticText(m_mapping_card, wxID_ANY, _L("Color Mapping"));
        lbl->SetFont(Label::Body_14);
        lbl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
        lbl->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        tr->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
        cs->Add(tr, 0, wxTOP | wxLEFT | wxRIGHT, FromDIP(16));
    }
    // 10px gap between the title row and the legend grid (no divider — see commit history).
    cs->AddSpacer(FromDIP(10));
    // Grid host (populated by update_mapping_legend). Fixed-col wxGridSizer is Mac-safe;
    // wxWrapSizer miscomputes its height on macOS (same rationale as MixedFilamentDialog).
    m_legend_panel = new wxPanel(m_mapping_card, wxID_ANY);
    m_legend_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
    m_legend_sizer = new wxGridSizer(LEGEND_GRID_COLS, FromDIP(10), FromDIP(10));
    m_legend_panel->SetSizer(m_legend_sizer);
    cs->Add(m_legend_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    m_mapping_card->SetSizer(cs);
    // Horizontal margin 12 (pairs with CARD_WIDTH_DIP); bottom 12 — same gap as between cards
    // above, so the last card sits at a uniform distance from the footer.
    parent.Add(m_mapping_card, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, FromDIP(12));
    parent.AddSpacer(FromDIP(12));
}

void MixedFilamentBatchDialog::build_footer()
{
    // Footer is a single white panel hosting, top-to-bottom:
    //   1) hairline divider
    //   2) Match progress bar + label (shown only while matching — sits ABOVE the buttons)
    //   3) Cancel / Confirm buttons (pinned to the dialog's bottom edge)
    auto* btn_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    btn_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
    auto* panel_sizer = new wxBoxSizer(wxVERTICAL);

    auto* top_line = new wxPanel(btn_panel, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    top_line->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F0F0F0")));
    panel_sizer->Add(top_line, 0, wxEXPAND);

    // Progress block (gauge + Stop Matching, hidden when idle). The 44-DIP spacer pins the row
    // height so the button centers with 8 DIP padding top and bottom. The gap to Cancel/Confirm is
    // outside the block so it survives the idle collapse.
    {
        auto* block = new wxBoxSizer(wxVERTICAL);
        auto* progress_row = new wxBoxSizer(wxHORIZONTAL);
        progress_row->Add(0, FromDIP(44), 0);
        m_progress_bar = new wxGauge(btn_panel, wxID_ANY, 100, wxDefaultPosition, wxSize(FromDIP(200), FromDIP(6)),
                                      wxGA_HORIZONTAL | wxGA_SMOOTH);
        m_progress_bar->SetMinSize(wxSize(FromDIP(200), FromDIP(6)));
        m_progress_bar->SetValue(0);
        m_progress_bar->Hide();
        progress_row->Add(m_progress_bar, 1, wxALIGN_CENTER_VERTICAL);
        m_btn_stop_match = new Button(btn_panel, _L("Stop Matching"));
        m_btn_stop_match->SetMinSize(wxSize(FromDIP(96), FromDIP(28)));
        m_btn_stop_match->SetCornerRadius(FromDIP(4));
        m_btn_stop_match->SetBorderWidth(FromDIP(1));
        m_btn_stop_match->SetFont(Label::Body_14);
        m_btn_stop_match->SetBorderColorNormal(wxColour("#DBDBDB"));
        m_btn_stop_match->SetBackgroundColorNormal(wxColour("#F8F7F7"));
        m_btn_stop_match->SetTextColorNormal(wxColour("#242424"));
        m_btn_stop_match->Hide();
        m_btn_stop_match->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { cancel_batch_match(); });
        progress_row->Add(m_btn_stop_match, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(18));
        block->Add(progress_row, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(20));
        m_progress_divider = new wxPanel(btn_panel, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
        m_progress_divider->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F0F0F0")));
        m_progress_divider->Hide();
        block->Add(m_progress_divider, 0, wxEXPAND);
        panel_sizer->Add(block, 0, wxEXPAND);
        block->ShowItems(false);
        m_progress_block = block;
    }
    panel_sizer->AddSpacer(FromDIP(12));

    auto* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_btn_cancel_match = new Button(btn_panel, _L("Cancel"));
    m_btn_cancel_match->SetMinSize(wxSize(-1, FromDIP(36)));
    m_btn_cancel_match->SetCornerRadius(FromDIP(4));
    m_btn_cancel_match->SetBorderWidth(FromDIP(1));
    m_btn_cancel_match->SetBorderColorNormal(wxColour("#D1D5DC"));
    m_btn_cancel_match->SetBackgroundColorNormal(wxColour("#FFFFFF"));
    m_btn_cancel_match->SetTextColorNormal(wxColour("#242424"));

    m_btn_confirm = new Button(btn_panel, _L("Confirm"));
    m_btn_confirm->SetMinSize(wxSize(-1, FromDIP(36)));
    m_btn_confirm->SetCornerRadius(FromDIP(4));
    m_btn_confirm->SetBorderWidth(0);
    // #FEFEFE (not #FFFFFF): stays white in dark mode; #FFFFFF maps to the dark bg.
    m_btn_confirm->SetBackgroundColor(StateColor(
        std::pair(wxColour("#DFDFDF"), static_cast<int>(StateColor::Disabled)),
        std::pair(wxColour("#019687"), static_cast<int>(StateColor::Normal))));
    m_btn_confirm->SetTextColor(StateColor(
        std::pair(wxColour("#6B6A6A"), static_cast<int>(StateColor::Disabled)),
        std::pair(wxColour("#FEFEFE"), static_cast<int>(StateColor::Normal))));

    btn_sizer->Add(m_btn_cancel_match, 1, wxRIGHT, FromDIP(8));
    btn_sizer->Add(m_btn_confirm, 1);
    panel_sizer->Add(btn_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(20));
    panel_sizer->AddSpacer(FromDIP(12));

    // Cancel shows a "Discard Match" confirmation before closing — but only when there's an
    // actual match result to lose. The PRD (6.6) secondary check exists so the user doesn't
    // discard a completed match by accident; "No" returns to the dialog with the current
    // state (results, preview, legend) intact. When there's no result yet (idle / matching /
    // failed), closing is lossless, so we skip the prompt and close directly. The gate mirrors
    // the Confirm-button enable condition (set_match_buttons_state): a discardable result
    // exists iff m_match_completed && m_result.success.
    m_btn_cancel_match->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        // Idempotent: align Cancel with Stop Matching. join() in the destructor
        // still waits for the worker's next checkpoint (per-combo/per-color).
        cancel_batch_match();
        if (!m_match_completed || !m_result.success) {
            EndModal(wxID_CANCEL);
            return;
        }
        RichMessageDialog confirm(this,
            _L("Are you sure you want to discard this match? The current configuration will not be saved."),
            _L("Discard Matching"),
            wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION);
        confirm.SetYesNoLabels(_L("Discard"), _L("Cancel"));
        auto result = confirm.ShowModal();
        if (result == wxID_YES) {
            EndModal(wxID_CANCEL);
        } else {
            // The confirm dialog is a modal child: while it ran, Cancel never lost
            // keyboard focus (mouseDown did SetFocus on click). After dismissal the
            // focus stays on Cancel, which on Windows renders as a focus indicator
            // that looks like a stuck "pressed" state. Hand focus to Confirm — it is
            // enabled in this branch (m_match_completed && m_result.success) — so the
            // dialog returns to a clean visual.
            if (m_btn_confirm && m_btn_confirm->IsEnabled()) m_btn_confirm->SetFocus();
        }
    });
    m_btn_confirm->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        // The slot-overflow advisory is already shown as a red banner at match completion
        // (see handle_batch_match_result), so there is nothing to re-check here — Confirm
        // proceeds directly. Overflow, if any, is dropped silently at apply time by
        // add_batch_custom_filaments.
        EndModal(wxID_OK);
    });

    btn_panel->SetSizer(panel_sizer);
    m_root->Add(btn_panel, 0, wxEXPAND);
}

// ---------------------------------------------------------------------------
// View + plate navigation
// ---------------------------------------------------------------------------

void MixedFilamentBatchDialog::on_view_changed(wxCommandEvent&)
{
    // Map the combo selection to a ThumbnailView via kComboToView (review item 3: decouple
    // from Append order). Guard against invalid indices defensively.
    const int sel = m_view_combo ? m_view_combo->GetSelection() : 0;
    if (sel >= 0 && static_cast<size_t>(sel) < sizeof(kComboToView) / sizeof(kComboToView[0]))
        m_view = kComboToView[sel];
    update_view();
}

void MixedFilamentBatchDialog::update_view()
{
    // Just re-render the current plate through the normal lazy path. The match buckets
    // are keyed by viewpoint and their content stays valid across viewpoint switches
    // (m_match_colors is unchanged), so we must NOT clear them here — clearing on every
    // switch would discard already-rendered bitmaps and force re-rendering each plate
    // every time the user toggles back to a previously visited viewpoint (N×switch
    // re-renders instead of N total). Full-bucket invalidation belongs in
    // reset_match_preview / rebuild_match_thumb_cache, which fire when the match result
    // itself changes.
    refresh_previews();
}

void MixedFilamentBatchDialog::on_tray_nav(int delta)
{
    int next = m_tray_index + delta;
    if (next < 1 || next > m_tray_count) return;
    m_tray_index = next;
    if (m_tray_combo) m_tray_combo->SetSelection(m_tray_index - 1);
    update_nav_arrow_state();
    refresh_previews();
}

void MixedFilamentBatchDialog::update_nav_arrow_state()
{
    if (m_btn_tray_prev) m_btn_tray_prev->Enable(m_tray_index > 1);
    if (m_btn_tray_next) m_btn_tray_next->Enable(m_tray_index < m_tray_count);
}

// ---------------------------------------------------------------------------
// State management
// ---------------------------------------------------------------------------

void MixedFilamentBatchDialog::display_warning(const wxString& msg)
{
    if (!m_warning_panel || !m_warning_text || !m_error_panel) return;
    m_error_panel->Hide();
    m_warning_panel->Show();
    // The auto-wrap label's width is pinned at construction (SetMaxSize), so
    // SetLabel already sees a valid width and a single Layout is correct on macOS.
    m_warning_text->SetLabel(msg);
    Layout();
}

void MixedFilamentBatchDialog::set_error(const wxString& msg)
{
    if (!m_error_panel || !m_error_text || !m_warning_panel) return;
    m_warning_panel->Hide();
    m_error_panel->Show();
    m_error_text->SetLabel(msg);
    if (m_btn_confirm) m_btn_confirm->Disable();
    Layout();
}

void MixedFilamentBatchDialog::display_error_advisory(const wxString& msg)
{
    // Like set_error (red #FDE8E8 banner) but does NOT disable Confirm — used when the
    // situation is a serious advisory the user may still choose to proceed past (e.g. slot
    // overflow will silently drop some mixes at apply time). Mirrors display_warning's
    // mechanics (hide the other banner, set label, single Layout).
    if (!m_error_panel || !m_error_text || !m_warning_panel) return;
    m_warning_panel->Hide();
    m_error_panel->Show();
    m_error_text->SetLabel(msg);
    Layout();
}

void MixedFilamentBatchDialog::set_match_buttons_state(bool matching)
{
    m_btn_start_match->Enable(!matching && !m_match_completed);
    m_btn_rematch->Enable(!matching && m_match_completed);
    // Cancel always closes the dialog — keep it enabled in every state so the user can
    // bail out at any time (idle / matching / match-completed-pending-confirm).
    m_btn_cancel_match->Enable(true);
    m_btn_confirm->Enable(!matching && m_match_completed && m_result.success);

    // Lock upper-region controls while a match runs. start_batch_match() snapshots the match
    // inputs on the UI thread and the worker receives them by value, so this is not a thread-
    // safety measure — it keeps the combos consistent with the inputs the in-flight match used,
    // and stops mid-match edits from mutating m_tray_index / m_manual_filament_count, which the
    // post-match restore re-reads via update_nav_arrow_state() / update_add_remove_buttons().
    // Cancel stays enabled (set above) so the user can still bail out.
    //
    // Enabling a hidden combo (e.g. m_filament_combo[] in RECOMMENDED mode) is a harmless no-op.
    if (m_method_combo) m_method_combo->Enable(!matching);
    if (m_tray_combo)   m_tray_combo->Enable(!matching);
    if (m_view_combo)   m_view_combo->Enable(!matching);
    for (int i = 0; i < 4; ++i)
        if (m_filament_combo[i]) m_filament_combo[i]->Enable(!matching);
    // Button-type controls have state-dependent enable conditions (tray arrows depend on
    // m_tray_index; add/remove depend on m_manual_filament_count). Disable them directly while
    // matching, and on restore delegate to the existing helpers so the boundary conditions are
    // recomputed correctly — NOT a blanket Enable(!matching), which would leave e.g. the prev
    // arrow enabled at plate 1.
    if (matching) {
        if (m_btn_tray_prev)       m_btn_tray_prev->Disable();
        if (m_btn_tray_next)       m_btn_tray_next->Disable();
        if (m_btn_remove_filament) m_btn_remove_filament->Disable();
        if (m_btn_add_filament)    m_btn_add_filament->Disable();
    } else {
        update_nav_arrow_state();    // prev/next re-enabled per m_tray_index
        update_add_remove_buttons(); // add/remove re-enabled per m_manual_filament_count
    }

    if (matching) {
        m_progress_bar->Show();
        m_progress_bar->SetValue(0);
        if (m_btn_stop_match)   m_btn_stop_match->Show();
        if (m_progress_divider) m_progress_divider->Show();
        // Re-show the block's spacers/sub-sizers (collapsed by the ShowItems(false) below).
        if (m_progress_block)   m_progress_block->ShowItems(true);
    } else {
        m_progress_bar->Hide();
        if (m_btn_stop_match)   m_btn_stop_match->Hide();
        if (m_progress_divider) m_progress_divider->Hide();
        // Collapse the whole block — widgets + the 12 DIP gap — to 0 so the idle
        // footer stays 61 DIP (button 36 + 12/12 padding + 1px hairline).
        if (m_progress_block)   m_progress_block->ShowItems(false);
    }
    // Footer height changes with the progress row visibility — re-layout so Cancel/Confirm
    // stay pinned to the bottom edge and there's no stale gap or clipping.
    Layout();
}

void MixedFilamentBatchDialog::on_method_changed(wxCommandEvent&)
{
    int sel = m_method_combo->GetSelection();
    MatchingMethod new_method = (sel == 1) ? MANUAL : RECOMMENDED;
    // No-op if the user re-selected the already-active mode: wxComboBox fires COMBOBOX
    // even for a re-pick of the current item (notably on wxMSW, clicking the open list's
    // selected row). Re-running the refresh pipeline here would force needless card
    // Show/Layout/tooltip updates and briefly hide the error/warning banners even though
    // nothing actually changed.
    if (new_method == m_matching_method)
        return;
    m_matching_method = new_method;
    m_error_panel->Hide();
    m_warning_panel->Hide();
    if (m_match_completed && new_method == MANUAL) check_manual_recipe_ratio();
    if (m_manual_card)
        m_manual_card->Show(m_matching_method == MANUAL);
    if (m_recommended_card)
        m_recommended_card->Show(m_matching_method == RECOMMENDED);
    // Re-apply combo icons: set_manual_combo_icon was called during build
    // while the card was hidden (SetIcon may not render on a hidden window).
    if (m_matching_method == MANUAL) {
        for (int i = 0; i < 4; ++i)
            if (m_filament_combo[i])
                set_manual_combo_icon(i, m_filament_combo[i]->GetSelection());
    }
    // Card visibility changed — the filament-config card swaps between manual and recommended,
    // whose content heights differ, so re-layout the scrolled region (not just the card itself)
    // to keep the preview / Color Mapping cards in a stable position.
    relayout_scrolled_content();
    update_mapping_legend();
    set_match_buttons_state(false);
    // Refresh the combo's tooltip to describe the now-active mode (hover hint).
    update_method_combo_tooltip();
    Layout();
}

void MixedFilamentBatchDialog::update_method_combo_tooltip()
{
    if (!m_method_combo) return;
    // Mode-specific hover hint on the Match Mode combo. Each mode gets a one-line
    // description of what it does, so hovering the combo explains the choice without
    // adding a permanent subtitle row to the UI.
    m_method_combo->SetToolTip(m_matching_method == MANUAL
        ? _L("Manually select filaments from the current list for color mixing.")
        : wxString::Format(_L("Automatically uses official CMYG filaments for color mixing. The mix ratio for each color is limited to %d%%–%d%%."),
             kMinComponentPercent, kMaxComponentPercent));
}

void MixedFilamentBatchDialog::on_manual_selection_changed()
{
    // Preserve the previous match result; only Start/Re-match clears it.
    set_match_buttons_state(false);
    // Re-evaluate the post-match ratio advisory only when a previous match is still on
    // screen. check_manual_recipe_ratio() internally Hides the banner first, then
    // re-evaluates against m_result.mappings (the still-shown result) and may re-show it.
    // There is no pre-match ratio guard; ratio warnings come only from this post-match check.
    //
    // Note: when no match is shown (m_match_completed == false), a pre-existing banner —
    // e.g. the 64-color limit advisory from display_warning at build_ui — is intentionally
    // NOT cleared here. It persists until the next Start/Re-match (which Hides all banners).
    // This is acceptable: the advisory is still contextually relevant while the user is
    // picking filaments for the over-limit model.
    if (m_match_completed)
        check_manual_recipe_ratio();
}

void MixedFilamentBatchDialog::check_manual_recipe_ratio()
{
    // Manual-mode post-match ratio guard: after a match completes, scan every non-pure
    // recipe for any single component above kMaxComponentPercent and list ALL such
    // over-threshold colors in the warning banner (gap doc case 13).
    //
    // The IDs reported here are the TARGET filament ids (target_filament_id) — i.e. the
    // numbers shown on the right-hand badge of each Color Mapping row — NOT the recipe
    // component (source physical) ids. A user reading "the mix ratio for {5} is too high"
    // looks for the row labelled "5" in the mapping list, which is the target id. Reporting
    // the underlying component id instead would point at a label the list never displays.
    if (m_warning_panel) m_warning_panel->Hide();
    if (m_matching_method != MANUAL) return;
    if (!m_match_completed || m_result.mappings.empty()) return;

    // Overflow outranks the ratio hint (the two banners are mutually exclusive — see
    // handle_batch_match_result). This path is also reached from on_manual_selection_changed
    // after a match that already surfaced the overflow error: without this guard the
    // display_warning call below would hide that error and replace it with the ratio hint.
    // m_result is unchanged by a combo edit (no re-match), so an overflow predicted at match
    // time still holds here — re-show the advisory and skip the ratio evaluation entirely.
    if (try_show_slot_overflow_advisory()) return;

    // NOTE: size by m_physical_colors.size(). In manual mode, launch_background_match's
    // manual-remap branch rewrites recipe component ids to project-wide 1-based indices, so
    // expand_color_match_recipe_weights must be sized against the FULL physical palette.
    // A too-small count reads out-of-range indices as 0 weight and silently misses the
    // over-threshold component.
    const size_t num_physical = m_physical_colors.size();
    if (num_physical == 0) return;

    // Collect EVERY over-threshold occurrence (target_filament_id — legend badge). No dedup
    // needed: is_pure_recipe mappings are skipped below, and the rest get unique ascending
    // ids from next_virtual_id++ (assign_batch_virtual_filament_ids), preserved through
    // merge_duplicate_recipe_mappings — so each reported id is already unique & in order.
    // Skip target_filament_id == 0 (unmapped/invisible row — no badge to report).
    wxString id_list;
    bool first = true;
    for (const ColorMappingEntry& e : m_result.mappings) {
        if (e.is_pure_recipe) continue;
        if (!e.recipe.valid) continue;
        if (e.target_filament_id == 0) continue;
        const auto weights = expand_color_match_recipe_weights(e.recipe, num_physical);
        bool over = false;
        for (size_t i = 0; i < weights.size() && i < num_physical; ++i) {
            if (weights[i] > kMaxComponentPercent) { over = true; break; }
        }
        if (!over) continue;
        if (!first) id_list += ", ";
        id_list += wxString::Format("%u", e.target_filament_id);
        first = false;
    }
    if (id_list.empty()) return;

    display_warning(wxString::Format(
        _L("The mix ratios for %s in the Color Mapping list are outside the recommended %d"
           "%%–%d%% range. Adjust the mix ratios manually for better results."),
        id_list, kMinComponentPercent, kMaxComponentPercent));
}

void MixedFilamentBatchDialog::set_manual_combo_icon(int row, int filament_idx)
{
    // Compose a single transparent icon holding [drop_down arrow] + [numbered color badge]
    // and set it as the combo's left icon via SetIcon(). This mirrors
    // MixedFilamentDialog::set_combo_combined_icon: ComboBox only renders ONE icon on its
    // left — when the selected dropdown item carries an image (our numbered color swatch),
    // the native drop-down arrow is suppressed, so we bake both into one image.
    if (row < 0 || row >= 4) return;
    ComboBox* cb = m_filament_combo[row];
    if (!cb) return;
    if (filament_idx < 0 || filament_idx >= static_cast<int>(m_physical_colors.size())) return;

    const int pad = FromDIP(8), arr_w = FromDIP(8), badge_w = FromDIP(20), h = FromDIP(20), gap = FromDIP(8), text_gap = FromDIP(8);
    const int total_w = pad + arr_w + gap + badge_w + text_gap;
    wxImage  img(total_w, h, true);
    img.InitAlpha();
    memset(img.GetAlpha(), 0, total_w * h);

    auto set_rgba = [&](int x, int y, unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
        if (x < 0 || x >= total_w || y < 0 || y >= h) return;
        int pos = y * total_w + x;
        img.GetData()[pos * 3]     = r;
        img.GetData()[pos * 3 + 1] = g;
        img.GetData()[pos * 3 + 2] = b;
        img.GetAlpha()[pos]        = a;
    };

    // Arrow: paste SVG (transparent background, only arrow pixels opaque)
    ScalableBitmap ab(cb, "drop_down", arr_w);
    if (ab.bmp().IsOk()) {
        wxImage aimg = ab.bmp().ConvertToImage();
        if (!aimg.HasAlpha()) aimg.InitAlpha();
        int ax = pad, ay = (h - aimg.GetHeight()) / 2;
        for (int y = 0; y < aimg.GetHeight() && ay + y < h; ++y)
            for (int x = 0; x < aimg.GetWidth() && ax + x < total_w; ++x) {
                unsigned char* s = aimg.GetData() + (y * aimg.GetWidth() + x) * 3;
                unsigned char a = aimg.HasAlpha() ? *(aimg.GetAlpha() + y * aimg.GetWidth() + x) : 255;
                if (a > 0) set_rgba(ax + x, ay + y, s[0], s[1], s[2], a);
            }
    }

    // Badge: use GetFilamentColorIcon for multi-color aware rendering (numbered color swatch, opaque)
    const int bx = pad + arr_w + gap;
    const std::string&  multi_colors  = m_physical_multi_colors[filament_idx];
    const FilamentColorMode color_mode = FilamentColorModeFromConfig(m_physical_color_modes[filament_idx]);
    wxBitmap* badge_bmp = FilamentColorUtils::GetFilamentColorIcon(multi_colors, color_mode,
        m_physical_colors[filament_idx],
        std::to_string(filament_idx + 1), FromDIP(20), FromDIP(20));
    if (badge_bmp) {
        wxImage bimg = badge_bmp->ConvertToImage();
        int by = (h - bimg.GetHeight()) / 2;
        for (int y = 0; y < bimg.GetHeight() && by + y < h; ++y)
            for (int x = 0; x < bimg.GetWidth() && bx + x < total_w; ++x) {
                unsigned char* s = bimg.GetData() + (y * bimg.GetWidth() + x) * 3;
                set_rgba(bx + x, by + y, s[0], s[1], s[2], 255);
            }
    }

    cb->SetIcon(wxBitmap(img));
    // SetIcon triggers Rescale→messureSize which recalculates height; re-lock to 30
    cb->SetMinSize(wxSize(-1, FromDIP(30)));
    cb->SetMaxSize(wxSize(-1, FromDIP(30)));
}

void MixedFilamentBatchDialog::update_add_remove_buttons()
{
    // Keep both buttons always visible and gray them out at their limits (remove at min=2,
    // add at the physical-filament cap, never beyond the 4-row UI bound) instead of hiding
    // them — makes the available action discoverable. The add cap tracks the live physical
    // spool count: you can't configure more rows than the printer actually holds.
    const int max_rows = std::min<int>(4, static_cast<int>(m_physical_colors.size()));
    if (m_btn_remove_filament) m_btn_remove_filament->Enable(m_manual_filament_count > 2);
    if (m_btn_add_filament)    m_btn_add_filament->Enable(m_manual_filament_count < max_rows);
}

void MixedFilamentBatchDialog::relayout_scrolled_content()
{
    if (!m_scrolled_content) return;
    // Layout() re-arranges children within the existing virtual size; FitInside() additionally
    // re-computes the virtual (scrollable) extent from the children's current best sizes. Both
    // are needed after a content-height change: without FitInside the scrolled window keeps the
    // old virtual height, so cards below the change (preview, Color Mapping) drift upward or
    // leave a dangling scrollable tail — visible as the Color Mapping card jumping when an
    // add/remove filament row alters the manual card's height.
    m_scrolled_content->Layout();
    m_scrolled_content->FitInside();
    m_scrolled_content->Refresh();
}

bool MixedFilamentBatchDialog::predict_slot_overflow() const
{
    // Predict whether confirming this match would push the project past
    // MAXIMUM_FILAMENT_NUMBER (64). The AUTHORITATIVE gate is
    // add_batch_custom_filaments (MixedFilament.cpp:1951-1957): it computes
    //   current_total = total_filaments(n) = n + enabled_mixed
    // then drops every batch entry once current_total reaches 64 (assigns id 0
    // -> the model region keeps its original colour, i.e. colour is LOST).
    //
    // n = apply-time physical base:
    //   recommended -> max(4, current_physical)  (target_count, Plater.cpp:2419)
    //   manual      -> current_physical          (colors_vec = full filament_colour,
    //                  Plater.cpp:2466)
    // enabled_mixed = current enabled_count() (auto_generate is off; manual mode does
    //   not touch m_mixed before add_batch; recommended mode's set_num_filaments would
    //   rebuild it only if auto_generate were on).
    // new_mixed_rows = non-pure, non-in-place mappings (one slot each), mirroring the
    //   batch_entries build at Plater.cpp:2576-2579.
    const auto* pb = wxGetApp().preset_bundle;
    if (pb == nullptr) return false;
    const size_t current_physical = m_physical_colors.size();
    if (current_physical == 0) return false;

    const bool recom = m_result.is_recommended_mode;
    const size_t n = recom ? std::max<size_t>(4, current_physical) : current_physical;

    size_t new_mixed_rows = 0;
    for (const auto& m : m_result.mappings)
        if (!m.is_pure_recipe && !m.in_place_edited) ++new_mixed_rows;

    const size_t enabled_mixed = pb->mixed_filaments.enabled_count();
    const size_t post_apply_total = n + enabled_mixed + new_mixed_rows;
    return post_apply_total > MAXIMUM_FILAMENT_NUMBER;
}

bool MixedFilamentBatchDialog::try_show_slot_overflow_advisory()
{
    // Single source of truth for the overflow banner + the "overflow wins over ratio"
    // priority. Returns true (and shows the red advisory) when applying m_result would
    // exceed the 64-slot cap; otherwise returns false and leaves the banners as they are
    // so the caller is free to surface the lower-priority ratio hint.
    if (!predict_slot_overflow()) return false;
    display_error_advisory(
        _L("Color mapping list limit reached (max 64 colors). Excess colors will be removed."));
    return true;
}

// ---------------------------------------------------------------------------
// Match
// ---------------------------------------------------------------------------

void MixedFilamentBatchDialog::start_batch_match()
{
    if (m_match_running) return;
    if (m_model_colors.empty()) {
        set_error(_L("No model detected. Import a multi-color model to continue."));
        return;
    }
    // Recommended (auto) mode relies on the Full Spectrum filament preset, whose
    // palette is validated per-nozzle. Gate on whether a Full Spectrum preset
    // actually exists for the *current* nozzle diameter (instead of a hard-coded
    // 0.4) -- shipping a new nozzle variant just needs its preset JSON, and this
    // opens up automatically. When no preset exists, block up front and direct
    // the user to Manual mode or a nozzle change. Manual mode itself is
    // unaffected (user picks filaments from the current list, palette-agnostic).
    if (m_matching_method == RECOMMENDED && !full_spectrum_preset_exists_for_current_nozzle()) {
        RichMessageDialog dlg(this,
            _L("Automatic color mixing matching is not supported for the current nozzle diameter. Please use manual matching or replace with a smaller-size nozzle."),
            _L("Color Mixing Match"), wxOK);
        dlg.SetOKLabel(_L("Got it"));
        dlg.CentreOnScreen();
        dlg.ShowModal();
        return;
    }
    m_match_running = true;
    m_error_panel->Hide();
    m_warning_panel->Hide();
    // Don't clear m_result — preserve it across failed/cancelled matches so the user can
    // retry without losing context. Only the success branch in handle_batch_match_result
    // overwrites m_result.
    m_match_completed = false;
    reset_match_preview();
    set_match_buttons_state(true);
    launch_background_match();
}

void MixedFilamentBatchDialog::cancel_batch_match()
{
    m_cancel_requested->store(true);
}

void MixedFilamentBatchDialog::launch_background_match()
{
    if (m_worker_thread.joinable()) m_worker_thread.join();
    m_cancel_requested->store(false);

    const auto model_colors = m_model_colors;
    std::vector<std::string> active_colors;
    std::vector<unsigned int> manual_full_ids; // 1-based indices into m_physical_colors
    if (m_matching_method == MANUAL) {
        for (int i = 0; i < m_manual_filament_count; ++i) {
            int sel = m_filament_combo[i] ? m_filament_combo[i]->GetSelection() : i;
            if (sel >= 0 && sel < static_cast<int>(m_physical_colors.size())) {
                // Same dual-color derivation as load_palette_colors: the combo badge
                // shows the multi-colour pair, so the selected slot contributes its
                // effective primary (filament_colour may be empty for dual-color slots).
                active_colors.push_back(slot_match_color(
                    sel < static_cast<int>(m_physical_multi_colors.size()) ? m_physical_multi_colors[sel] : std::string(),
                    sel < static_cast<int>(m_physical_color_modes.size())  ? m_physical_color_modes[sel]  : 0,
                    m_physical_colors[sel]));
                manual_full_ids.push_back(static_cast<unsigned int>(sel + 1)); // 1-based
            }
        }
        if (active_colors.size() < 2) {
            active_colors = m_physical_colors;
            // Fallback to the full physical palette: derive each slot the same way so a
            // dual-color slot with an empty filament_colour is not misrepresented.
            for (size_t i = 0; i < m_physical_colors.size(); ++i)
                active_colors[i] = slot_match_color(
                    i < m_physical_multi_colors.size() ? m_physical_multi_colors[i] : std::string(),
                    i < m_physical_color_modes.size()  ? m_physical_color_modes[i]  : 0,
                    m_physical_colors[i]);
            manual_full_ids.clear();
            for (size_t i = 0; i < m_physical_colors.size(); ++i)
                manual_full_ids.push_back(static_cast<unsigned int>(i + 1));
        }
    }
    const auto manual_colors     = std::move(active_colors);
    auto manual_full_ids_c = std::move(manual_full_ids);
    const auto all_physical      = m_physical_colors;

    const auto matching_method = m_matching_method;
    // Physical palette for recommended mode. Loaded from the real Full Spectrum preset
    // (filaments_colours.json) on the main thread here, then captured by value into the
    // worker lambda below — so the worker never touches FilamentColorLibrary. Falls back to
    // FULL_SPECTRUM_FALLBACK_COLORS when the preset is missing or yields <4 validated colors.
    // See load_full_spectrum_colors() for hex validation + distinct logging.
    std::vector<std::string> preset_colors;
    {
        std::vector<std::string> palette = load_full_spectrum_colors();
        if (palette.size() >= 4) {
            preset_colors = std::move(palette);
        } else {
            BOOST_LOG_TRIVIAL(warning) << "launch_background_match: preset colors <4, fallback to canonical palette";
            preset_colors = FULL_SPECTRUM_FALLBACK_COLORS;
        }
    }
    // Use enabled_count() (skips deleted/disabled) to match the virtual ID
    // numbering scheme used by mixed_index_from_filament_id() and
    // add_batch_custom_filaments(), both of which count only enabled entries.
    const size_t existing_mixed_count = wxGetApp().preset_bundle
        ? wxGetApp().preset_bundle->mixed_filaments.enabled_count()
        : size_t(0);

    auto destroyed = m_destroyed;
    auto cancel_token = m_cancel_requested;
    auto progress_bar = m_progress_bar;

    m_worker_thread = std::thread([this, model_colors, manual_colors, all_physical,
                                    preset_colors, matching_method, existing_mixed_count,
                                    destroyed, cancel_token, progress_bar,
                                    manual_full_ids = std::move(manual_full_ids_c)]()
    {
        std::vector<std::string> physical_colors;
        if (matching_method == MANUAL) {
            physical_colors = manual_colors;
        } else {
            if (preset_colors.size() >= 4) {
                // Combo search passes max=100 here: this stage only picks WHICH 4 preset
                // colors form the palette; the per-component 70% cap is enforced later in
                // the Pass-2 batch_match_model_colors call (match_max=kMaxComponentPercent).
                auto best = recommend_best_filament_combo(model_colors, preset_colors, 15, 100, cancel_token);
                // recommend_best_filament_combo returns {} for BOTH "no valid combo" and
                // "cancelled" — distinguish here by re-checking the token. On cancel, skip
                // the fallback palette + Pass-2 match (hundreds of ms of wasted work) and
                // deliver a cancelled result directly so handle_batch_match_result shows no
                // error banner and restores the prior result. Mirrors the cancel handling
                // in batch_match_model_colors (error_code = 2).
                if (cancel_token->load()) {
                    // User cancellation (Stop Matching) — not an error. error_message is
                    // intentionally empty: handle_batch_match_result treats error_code==2 as a
                    // silent rollback and never displays it.
                    BatchMatchResult cancelled;
                    cancelled.success    = false;
                    cancelled.error_code = 2;
                    wxGetApp().CallAfter([this, destroyed, result = std::move(cancelled)]() mutable {
                        if (destroyed->load()) return;
                        handle_batch_match_result(result);
                    });
                    return;  // exit worker lambda — skip Pass-2 match + merge
                }
                if (best.empty()) {
                    physical_colors.assign(preset_colors.begin(), preset_colors.begin() + std::min<size_t>(4, preset_colors.size()));
                } else {
                    // Restore original preset_colors order (the function returns the
                    // chosen subset sorted by ΔE); keeps the palette order stable and
                    // stays correct when the candidate set grows beyond 4.
                    std::vector<std::string> remaining = best;
                    for (const std::string& c : preset_colors) {
                        auto it = std::find(remaining.begin(), remaining.end(), c);
                        if (it != remaining.end()) {
                            physical_colors.push_back(c);
                            remaining.erase(it);
                            if (physical_colors.size() >= 4) break;
                        }
                    }
                    if (physical_colors.size() < 4)
                        physical_colors = std::move(best);  // fallback
                }
            } else {
                physical_colors = all_physical;
            }
        }

        BatchMatchResult result;
        result.success = true;
        std::vector<ModelColorEntry> unmatched_colors;

        // Build palette of all existing filament colors (physical + mixed)
        const size_t num_physical = physical_colors.size();
        std::vector<wxColour> existing_palette;
        std::vector<unsigned int> existing_ids;
        for (size_t i = 0; i < physical_colors.size(); ++i) {
            wxColour c;
            if (try_parse_color_match_hex(physical_colors[i], c)) {
                existing_palette.push_back(c);
                existing_ids.push_back(static_cast<unsigned int>(i + 1));
            }
        }
        // Recommended mode: only use the recommended Full Spectrum physical colors as the
        // Pass-1 reuse palette. Existing mixed filaments are NOT included because
        // (a) their display_colors reference the old physical palette that is about
        // to be replaced, and (b) a passthrough recipe targeting a virtual filament
        // ID (e.g. component_a=5) would be silently clamped in add_batch_custom_filaments.

        // Pass 1: map each model color to closest existing filament. Per case 11 / PRD 6.2.5,
        // a model color within ΔE<1 of an existing filament is mapped directly to it (pure
        // filament preferred over a mix) — strict <1 matches the product spec.
        constexpr double K_REUSE_THRESHOLD = 1.0;
        for (const auto& mc : model_colors) {
            double best_de = std::numeric_limits<double>::max();
            size_t best_idx = 0;
            for (size_t j = 0; j < existing_palette.size(); ++j) {
                double de = color_delta_e00(mc.color, existing_palette[j]);
                if (de < best_de) { best_de = de; best_idx = j; }
            }
            if (best_de < K_REUSE_THRESHOLD && best_idx < existing_ids.size()) {
                // Direct mapping to existing filament
                ColorMappingEntry mapping;
                mapping.model_color_index   = mc.color_index;
                mapping.source_color         = mc.color;
                mapping.target_filament_id   = existing_ids[best_idx];
                mapping.matched_color        = existing_palette[best_idx];
                mapping.delta_e              = best_de;
                mapping.is_pure_recipe       = (existing_ids[best_idx] <= static_cast<unsigned int>(num_physical));
                mapping.pure_delta_e         = best_de;
                mapping.merged_model_indices = {mc.color_index};
                mapping.source_extruder_ids  = mc.extruder_ids;
                mapping.recipe.valid         = true;
                mapping.recipe.component_a  = existing_ids[best_idx];
                mapping.recipe.mix_b_percent = 0;
                mapping.recipe.preview_color = existing_palette[best_idx];
                mapping.recipe.delta_e       = best_de;
                result.mappings.push_back(mapping);
            } else {
                unmatched_colors.push_back(mc);
            }
        }

        // Pass 2: batch-match remaining colors.  batch_match_model_colors uses a
        // return-code model (result.success / error_code) and its whole call chain
        // has no throw sites, so no exception handling is needed here.
        //
        // Per-component weight bounds: min is shared (kMinComponentPercent); max is
        // mode-dependent — MANUAL allows >70% (advisory via check_manual_recipe_ratio).
        //
        // check_compatible is false for BOTH modes. RECOMMENDED needs none: its
        // physical_colors come from a single Full Spectrum preset (one PLA spool,
        // multiple colours), so every pair is same-material by construction — a
        // compatibility filter would be a no-op. Passing false also keeps this
        // worker thread off preset_bundle: build_compatibility_matrix would read
        // preset_bundle->filament_presets unsynchronized from here, racing the UI
        // thread (data-race UB). The slice gate (Plater::has_incompatible_mixed_
        // filament_in_use) still blocks genuinely incompatible mixes at slice time.
        const int match_min = kMinComponentPercent; // shared floor for both modes
        const int match_max = (matching_method == MANUAL) ? 100 : kMaxComponentPercent;
        if (!unmatched_colors.empty()) {
            auto sub_result = batch_match_model_colors(unmatched_colors, physical_colors, match_min, match_max, cancel_token,
                [progress_bar, destroyed](int done, int total) {
                    if (progress_bar && !destroyed->load()) {
                        wxGetApp().CallAfter([progress_bar, done, total, destroyed]() {
                            if (destroyed->load()) return;
                            if (total > 0) progress_bar->SetValue(done * 100 / total);
                        });
                    }
                },
                /*check_compatible=*/ false);
            if (sub_result.success) {
                // Offset virtual IDs: start after all existing filaments
                assign_batch_virtual_filament_ids(sub_result, physical_colors.size(), existing_mixed_count);
                // Merge
                result.mappings.insert(result.mappings.end(),
                    sub_result.mappings.begin(), sub_result.mappings.end());
            } else {
                result.success = false;
                result.error_message = sub_result.error_message;
                result.error_code = sub_result.error_code;
            }
        }

        // Manual mode: remap recipe component IDs from the user-selected subset
        // back to their original 1-based indices in the project filament_colour list.
        // Without this, add_batch_custom_filaments() interprets subset-relative
        // indices (e.g. "1,2" for a 2-filament manual selection) as project-level
        // indices, creating mixed filaments that reference the wrong physical spools.
        const bool need_manual_remap = (matching_method == MANUAL
            && !manual_full_ids.empty()
            && manual_full_ids.size() == physical_colors.size()
            && physical_colors.size() != all_physical.size());
        if (need_manual_remap) {

            // Build lookup: subset_idx(1-based) → project_idx(1-based)
            std::vector<unsigned int> remap(physical_colors.size() + 1, 0);
            for (size_t i = 0; i < manual_full_ids.size(); ++i)
                remap[i + 1] = manual_full_ids[i];

            auto remap_id = [&](unsigned int& id) {
                if (id > 0 && id < remap.size() && remap[id] > 0)
                    id = remap[id];
            };

            for (auto& mapping : result.mappings) {
                remap_id(mapping.target_filament_id);
                remap_id(mapping.recipe.component_a);
                remap_id(mapping.recipe.component_b);
                if (!mapping.recipe.gradient_component_ids.empty()) {
                    auto ids = MixedFilamentManager::decode_gradient_component_ids(
                        mapping.recipe.gradient_component_ids);
                    for (auto& id : ids) remap_id(id);
                    mapping.recipe.gradient_component_ids =
                        MixedFilamentManager::encode_gradient_component_ids(ids);
                }
            }

            // After remapping, pure-recipe component_a values may now be > the
            // old subset-size threshold used by assign_batch_virtual_filament_ids.
            // Re-evaluate is_pure_recipe against the full project physical count.
            const unsigned int full_phys = static_cast<unsigned int>(all_physical.size());
            for (auto& mapping : result.mappings) {
                if (mapping.is_pure_recipe)
                    mapping.is_pure_recipe = (mapping.recipe.component_a <= full_phys);
            }

            // Reassign virtual filament IDs using the full project physical count
            // so they don't collide with remapped pure-recipe IDs.
            unsigned int next_vid = static_cast<unsigned int>(all_physical.size()
                                                   + existing_mixed_count + 1);
            for (auto& mapping : result.mappings) {
                if (!mapping.is_pure_recipe)
                    mapping.target_filament_id = next_vid++;
            }
        }

        // Merge mappings whose mixed-filament recipes are byte-identical so they share one
        // virtual slot instead of each creating a duplicate row (e.g. two identical model
        // colors, or two distinct colors whose recipes happen to match). MUST run after both
        // ID-assignment phases (assign_batch_virtual_filament_ids + optional manual remap)
        // so the survivor keeps the final target_filament_id; the manual-remap reassign loop
        // above unconditionally rewrites target ids, so any earlier merge would be clobbered.
        // Pure-recipe mappings are skipped by the merge (they target existing physicals and
        // never allocate a new slot). source_extruder_ids are unioned so apply still covers
        // every source extruder.
        result.mappings = merge_duplicate_recipe_mappings(result.mappings);

        if (result.success && result.mappings.empty()) {
            result.success = false;
            result.error_message = "No valid recipes found for any model color";
            result.error_code = 1;
        }
        if (result.success) {
            // avg ΔE over the (post-merge) mapping set. We do NOT merge by matched_color
            // (that would collapse distinct source colors whose recipes produce similar
            // output shades, losing per-source traceability); merge_duplicate_recipe_mappings
            // above only collapses byte-identical recipes, which is lossless for ΔE.
            double sum_de = 0.0;
            for (const auto& m : result.mappings) sum_de += m.delta_e;
            result.avg_delta_e = sum_de / double(result.mappings.size());
            // Use full project filament indices for manual mode
            if (need_manual_remap) {
                result.selected_physical_ids = manual_full_ids;
            } else {
                for (size_t i = 1; i <= physical_colors.size(); ++i)
                    result.selected_physical_ids.push_back(static_cast<unsigned int>(i));
            }
            if (matching_method == RECOMMENDED) {
                result.is_recommended_mode = true;
                result.recommended_physical_colors = physical_colors;
            }
        }

        wxGetApp().CallAfter([this, destroyed, result = std::move(result)]() mutable {
            if (destroyed->load()) return;
            handle_batch_match_result(result);
        });
    });
}

void MixedFilamentBatchDialog::handle_batch_match_result(const BatchMatchResult& result)
{
    if (m_destroyed->load()) return;
    m_match_running = false;
    m_progress_bar->SetValue(100);
    m_progress_bar->Hide();

    if (!result.success) {
        // User cancellation is not an error — don't show the error banner.
        const bool cancelled = (result.error_code == 2);
        if (!cancelled) {
            set_error(wxString::FromUTF8(result.error_message));
        }
        // Restore the prior result ONLY when the user-facing input is unchanged since
        // that result was produced (same mode + same manual filament selections).
        // Otherwise we'd surface a preview built for a different input — e.g. after
        // toggling Recommended↔Manual or changing a combo selection. m_result itself
        // is preserved across failed matches (start_batch_match no longer clears it),
        // so this gate is what prevents a stale result from being shown.
        const bool input_intact = (m_matching_method == m_last_result_method
            && (m_matching_method != MANUAL
                || (m_manual_filament_count == m_last_result_manual_count
                    && std::equal(m_filament_selections,
                                  m_filament_selections + m_manual_filament_count,
                                  m_last_result_selections))));
        if (m_result.success && input_intact) {
            // rebuild_match_thumb_cache is needed because reset_match_preview already
            // cleared the thumb buckets at the start of this match attempt.
            m_match_completed = true;
            rebuild_match_thumb_cache();
            refresh_previews();
        }
        set_match_buttons_state(false);
        Layout();
        return;
    }

    m_match_completed = false;
    m_result = result;
    // Record the user-facing input that produced this result, so a later failed/cancelled
    // re-match can tell whether the prior preview is still valid to restore.
    m_last_result_method = m_matching_method;
    m_last_result_manual_count = m_manual_filament_count;
    for (int i = 0; i < m_manual_filament_count; ++i)
        m_last_result_selections[i] = m_filament_selections[i];
    m_match_completed = true;
    update_mapping_legend();
    rebuild_match_thumb_cache();
    if (result.is_recommended_mode)
        update_recommended_card();
    refresh_previews();
    // Post-match advisories — both run here so the relevant banner is in place when
    // Confirm lights up:
    //   • Slot overflow (data-loss): surfaced as a non-blocking red error banner. Runs at
    //     match completion (not at confirm time) so the user sees the risk before deciding
    //     to Confirm. Confirm stays enabled; overflow is dropped silently at apply time.
    //   • Ratio hint ("single component > 70%", gap doc case 13): warning banner.
    // The two banners are mutually exclusive and overflow is the more serious condition, so
    // it wins — try_show_slot_overflow_advisory centralizes that priority (also honoured by
    // check_manual_recipe_ratio on the re-evaluation paths).
    if (!try_show_slot_overflow_advisory())
        check_manual_recipe_ratio();
    // Defer the button-state flip until AFTER refresh_previews() has rendered and
    // pushed the After-Match bitmap, so Confirm/Re-match light up in lockstep with
    // the preview — not a frame earlier. m_match_completed stays true throughout so
    // rebuild_match_thumb_cache / refresh_previews see the new result (their internal
    // gate still depends on it; only the user-facing buttons wait).
    set_match_buttons_state(false);  // after m_match_completed=true so confirm enables
    BOOST_LOG_TRIVIAL(info) << "Batch match: " << result.mappings.size()
                            << " mappings, avg DeltaE=" << result.avg_delta_e;
    Layout();
}

// ---------------------------------------------------------------------------
// Legend (matches prototype: src-swatch → badge-number  ΔE-badge)
// ---------------------------------------------------------------------------

void MixedFilamentBatchDialog::update_mapping_legend()
{
    // Freeze the legend panel for the whole clear+rebuild so wxMSW doesn't repaint after
    // every child create/destroy (up to 64 rows × 4 sub-windows = hundreds of partial
    // paints → visible flicker). RAII guarantees Thaw on every exit path, including
    // exceptions (harness cpp-wxwidgets §132). Held for the whole function: Refresh()
    // calls issued while frozen are coalesced and flushed at Thaw (the destructor at
    // scope exit), so the rebuilt card repaints in one pass. m_legend_panel is the direct
    // parent of every new row window, so freezing it (which propagates to children) is
    // the minimal effective scope — same idiom as ConfigWizard / PresetComboBoxes.
    wxWindowUpdateLocker no_updates(m_legend_panel);
    // Clear(true) deletes row windows immediately (vs deferred Destroy), so their
    // pixel regions are released before repaint — same pattern as rebuild_legend.
    m_legend_sizer->Clear(true);

    if (!m_result.mappings.empty()) {
        for (const auto& mapping : m_result.mappings) {
            // Source colours for this mapping's legend rows. A pure recipe maps one model
            // colour 1:1 onto a physical filament — always a single row. A mixed recipe
            // may be shared by several model colours (byte-identical recipes are folded
            // into one mapping by merge_duplicate_recipe_mappings); each merged colour
            // gets its own legend row so nothing is hidden (e.g. the light-blue of a
            // {1,3,5,4} run used to disappear under the first source colour). Recover the
            // merged colours via merged_model_indices -> m_model_colors; indices are
            // sorted+unique after a merge and m_model_colors is hex-deduped, so colours
            // here are already distinct — the find-dedup below is belt-and-suspenders.
            std::vector<wxColour> src_colors;
            if (mapping.is_pure_recipe) {
                src_colors.push_back(mapping.source_color.IsOk() ? mapping.source_color : wxColour(128, 128, 128));
            } else {
                for (unsigned int ci : mapping.merged_model_indices) {
                    if (ci < 1 || ci > static_cast<unsigned int>(m_model_colors.size()))
                        continue;
                    const wxColour& c = m_model_colors[ci - 1].color;
                    if (!c.IsOk() || std::find(src_colors.begin(), src_colors.end(), c) != src_colors.end())
                        continue;
                    src_colors.push_back(c);
                }
                if (src_colors.empty())
                    src_colors.push_back(mapping.source_color.IsOk() ? mapping.source_color : wxColour(128, 128, 128));
            }
            for (const wxColour& row_color : src_colors) {
                // Bordered item box (white bg, #dbdbdb border) holding
                // [source swatch 20] -> [arrow icon] -> [target swatch 28 with filament number].
                // One row per source colour: merged mappings render each merged colour as its
                // own row (all pointing at the same shared target slot), which keeps every
                // matched colour visible in the legend. Per-row ΔE is hover-only (native
                // tooltip) to keep the row clean.
                // Figma (node 27590:61488 light / 27646:130023 dark): square corners (no radius),
                // 4px internal padding, justify-between layout (source left, target right, arrow
                // centered in the remaining space), plain ams_arrow (no circle frame).
                auto* item = new StaticBox(m_legend_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
                item->SetCornerRadius(FromDIP(0));
                item->SetBorderWidth(FromDIP(1));
                item->SetBorderColorNormal(StateColor::darkModeColorFor(wxColour("#DBDBDB")));
                item->SetBackgroundColor(StateColor(std::pair(wxColour("#FFFFFF"), static_cast<int>(StateColor::Normal))));
                auto* s = new wxBoxSizer(wxHORIZONTAL);

                // Source swatch (20x20, square corners)
                // For pure-recipe mappings that target a single physical filament,
                // render the source as a dual-color segment swatch when the physical
                // filament has multi-color data — so a bicolor filament row shows
                // its two colors side-by-side instead of a single averaged swatch.
                wxBitmap* src_bmp = nullptr;
                {
                    const unsigned int phy_id  = mapping.recipe.component_a;
                    const bool         is_pure = mapping.is_pure_recipe && phy_id >= 1 && phy_id <= m_physical_multi_colors.size();
                    const std::string& multi   = is_pure ? m_physical_multi_colors[phy_id - 1] : std::string();
                    const auto         parts   = FilamentColorUtils::SplitMultiColors(multi);
                    if (parts.size() > 1) {
                        std::vector<wxColour> dual_colors;
                        dual_colors.reserve(parts.size());
                        for (const auto& h : parts) {
                            wxColour c;
                            try_parse_color_match_hex(h, c);
                            if (c.IsOk())
                                dual_colors.push_back(c);
                        }
                        if (dual_colors.size() > 1) {
                            const int mode_val    = (phy_id >= 1 && phy_id <= m_physical_color_modes.size()) ?
                                                        m_physical_color_modes[phy_id - 1] :
                                                        0;
                            bool      is_gradient = (FilamentColorModeFromConfig(mode_val) == FilamentColorMode::Gradient);
                            src_bmp = get_color_block_bitmap_cached(dual_colors, is_gradient, FromDIP(20), FromDIP(20), wxEmptyString,
                                                                    wxNullColour);
                        }
                    }
                    if (!src_bmp) {
                        // Non-dual (or non-pure) rows: render the row's own source colour.
                        // Pure single-colour rows use mapping.source_color == row_color;
                        // merged rows use the merged colour of the current split row.
                        ColorBlockParams src_params;
                        src_params.mode        = ColorBlockParams::Solid;
                        src_params.solid_color = row_color.IsOk() ? row_color : wxColour(128, 128, 128);
                        src_params.width       = FromDIP(20);
                        src_params.height      = FromDIP(20);
                        src_bmp                = get_color_block_bitmap_cached(src_params);
                    }
                }
                auto* src_bmp_ctrl = new wxStaticBitmap(item, wxID_ANY, *src_bmp);
                s->Add(src_bmp_ctrl, 0, wxALIGN_CENTER_VERTICAL);

                // Stretch: source pinned left, target pinned right, arrow centered between them
                // (Figma's justify-between with the arrow as the middle flex item).
                s->AddStretchSpacer(1);

                // Arrow icon — plain right arrow (line + chevron, no circle frame), matching
                // Figma's stroke-only arrow between the swatches. Two theme variants:
                //   mixed_filament_mapping_right_arrow.svg       (#333333, light mode)
                //   mixed_filament_mapping_right_arrow_dark.svg  (#939495, dark mode)
                // Selected at build time via wxGetApp().dark_mode() — same pattern as
                // AmsMappingPopup's mode_string switch. NOTE: legend rows are rebuilt on every
                // match, so a theme switch while the dialog is open won't recolor existing rows
                // until the next refresh_previews/match cycle.
                const bool        is_dark    = wxGetApp().dark_mode();
                const std::string arrow_name = is_dark ? "mixed_filament_mapping_right_arrow_dark" : "mixed_filament_mapping_right_arrow";
                ScalableBitmap    arrow_bmp(item, arrow_name, 16);
                auto*             arrow = new wxStaticBitmap(item, wxID_ANY, arrow_bmp.bmp());
                // §17: stroke-only arrow SVG has transparent pixels; without matching the item's
                // white bg they would render with the system color on wxMSW.
                arrow->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
                s->Add(arrow, 0, wxALIGN_CENTER_VERTICAL);

                s->AddStretchSpacer(1);

                // Target swatch with filament number (28x28, square corners)
                ColorBlockParams badge;
                badge.mode        = ColorBlockParams::Solid;
                badge.solid_color = mapping.matched_color.IsOk() ? mapping.matched_color : wxColour(128, 128, 128);
                badge.width       = FromDIP(28);
                badge.height      = FromDIP(28);
                badge.label       = wxString::Format("%u", mapping.target_filament_id);
                auto* tgt_bmp     = new wxStaticBitmap(item, wxID_ANY, *get_color_block_bitmap_cached(badge));
                s->Add(tgt_bmp, 0, wxALIGN_CENTER_VERTICAL);

                // 4px internal padding (Figma p-[4px]): wrap the content sizer in an outer sizer
                // that adds the padding, since wxWindow::SetSizer has no border param.
                auto* outer = new wxBoxSizer(wxHORIZONTAL);
                outer->Add(s, 1, wxEXPAND | wxALL, FromDIP(4));
                item->SetSizer(outer);
                // ΔE revealed on hover. wx tooltips are per-window (they do NOT inherit from the
                // parent), and the swatches/arrow are independent child windows — so we must set
                // the same tip on every child too, otherwise hovering a swatch shows nothing and
                // only the gaps between children trigger the row's tip.
                // Grade bands per PRD 6.2.5 — open intervals: <kDeltaEGoodMax Good,
                // kDeltaEGoodMax≤ΔE<kDeltaEFairMax Fair, ≥kDeltaEFairMax Poor. Strict < so a value
                // exactly on the boundary lands in the worse band (4.0→Fair, 8.0→Poor).
                // Per-row ΔE: a merged mapping only keeps the survivor's delta_e, so each split
                // row recomputes its own ΔE = color_delta_e00(row_color, matched_color) — the
                // same definition the matcher used (matched_color == recipe preview colour).
                // Pure and non-merged rows keep mapping.delta_e (identical value, no change).
                const double   row_delta_e = (!mapping.is_pure_recipe && mapping.matched_color.IsOk()) ?
                                                 color_delta_e00(row_color, mapping.matched_color) :
                                                 mapping.delta_e;
                const wxString grade       = (row_delta_e < kDeltaEGoodMax) ? _L("Good") :
                                             (row_delta_e < kDeltaEFairMax) ? _L("Fair") :
                                                                              _L("Poor");
                // Per copy spec, tooltip format: "Color Difference: {Level} (ΔE={X})".
                // The ΔE glyph needs a font with Greek coverage; wx's default UI font on all
                // supported platforms (Win10+, macOS, mainstream Linux) has it.
                const wxString tip = wxString::Format(_L("Color Difference: %s (\u0394E=%.1f)"), grade, row_delta_e);
                item->SetToolTip(tip);
                src_bmp_ctrl->SetToolTip(tip);
                arrow->SetToolTip(tip);
                tgt_bmp->SetToolTip(tip);
                m_legend_sizer->Add(item, 0, wxEXPAND | wxALL, FromDIP(3));
            } // one legend row per source colour (merged mappings render N rows)
        }
    }
    // Card grows with content (no inner scroller); re-layout the legend panel + card so they
    // reflect the new row count, then re-layout the scrolled region so its virtual (scrollable)
    // extent tracks the new card height and downstream positioning stays correct. The Layout()
    // and Refresh() calls run while still frozen — they coalesce, and the whole card repaints
    // in one pass when no_updates Thaws at function exit.
    m_legend_panel->Layout();
    m_mapping_card->Layout();
    m_mapping_card->Refresh();
    relayout_scrolled_content();
}

}} // namespace Slic3r::GUI
