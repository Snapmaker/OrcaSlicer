#include "MixedFilamentBatchDialog.hpp"
#include "MixedFilamentBadge.hpp"
#include "MixedColorMatchHelpers.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "PresetBundle.hpp"
#include "libslic3r/Preset.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/StaticBox.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "wxExtensions.hpp"
#include "Plater.hpp"
#include "PartPlate.hpp"
#include "BitmapCache.hpp"

#include <unordered_map>

#include <wx/scrolwin.h>
#include <wx/dcbuffer.h>
#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI {

// Columns for the Color Mapping legend grid. Fixed (not growable) so each mapping
// item keeps its natural width and the card height is computed correctly on macOS
// (same rationale as MixedFilamentDialog's fixed-col swatch grid).
static constexpr int LEGEND_GRID_COLS = 4;

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
// This is the codebase's proven pattern for "rounded image preview" (mirrors
// ImageGrid.cpp's createShadowBorder + renderContent1): a single wxBG_STYLE_PAINT
// handler draws (1) rounded bg, (2) thumbnail, (3) a cached ARGB corner mask that
// paints the four corners opaque so the square bitmap reads as rounded, (4) the
// badge. Avoids StaticBox + child windows — on MSW StaticBox composites its
// rounded background in a memory DC and erases both the rounded corners (when an
// opaque child wxStaticBitmap covers them) and absolutely-positioned child
// panels (StepMeshDialog.cpp:92-115 documents the same dead-end).
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
        // wxBG_STYLE_PAINT + wxAutoBufferedPaintDC: the whole panel is painted in one
        // handler (background + image + mask + badge), flicker-free. Same idiom as
        // MixedColorMatchPanel::StripedPreviewPanel.
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        // Fixed square size via SetMinSize + FromDIP (the codebase's robust pattern for
        // fixed-size preview areas, e.g. MixedFilamentDialog's m_strip_panel).
        SetMinSize(wxSize(FromDIP(side_dip), FromDIP(side_dip)));
        Bind(wxEVT_PAINT, &RoundedPreviewPanel::on_paint, this);
        // The corner mask depends on the panel size + DPI, so rebuild it when those change.
        // GetClientSize() is 0 during construction; the first wxEVT_SIZE rebuilds it.
        Bind(wxEVT_DPI_CHANGED, [this](wxDPIChangedEvent& e) {
            SetMinSize(wxSize(FromDIP(m_side_dip), FromDIP(m_side_dip)));
            rebuild_corner_mask();
            // Re-rasterize placeholder at the new DPI (ScalableBitmap convention, same as
            // PrintingTaskPanel::msw_rescale rescaling monitor_placeholder).
            if (m_placeholder_loaded) m_placeholder.msw_rescale();
            Refresh();
            e.Skip();
        });
        Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
            rebuild_corner_mask();
            Refresh();
            e.Skip();
        });
    }

    // Store the source thumbnail (kept as-is). The square bitmap is drawn centered in
    // on_paint, then overlaid with the corner mask (see rebuild_corner_mask) which paints
    // bg-colored corners over the bitmap's square corners. No alpha baking — the mask uses
    // a color key for transparency (MSW-safe).
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
    wxBitmap m_src_bmp;     // unscaled source thumbnail (for re-baking on size/DPI change)
    wxBitmap m_bmp;         // square thumbnail drawn centered in on_paint
    wxBitmap m_corner_mask; // ARGB mask: transparent inside rounded path, opaque bg corners
    // mixed_filament_preview_placeholder.svg; shown until a real thumbnail is set.
    // Loaded via ScalableBitmap — the codebase's standard SVG-load convention, same as
    // StatusPanel's monitor_placeholder / Auxiliary's placeholder_excel/pdf/txt. msw_rescale()
    // re-rasterizes on DPI change so Retina gets @2x automatically (see DPI_CHANGED handler).
    // px_cnt is the *rasterization* target; on_paint still draws the bitmap scaled to ~60% of
    // the panel's shorter side so the mountains/sun read centered with margins like a real
    // render.
    ScalableBitmap m_placeholder;
    bool           m_placeholder_loaded = false;

    // Build the corner mask: a panel-sized opaque wxBitmap whose four corners (outside the
    // rounded path) are filled with the panel bg color, and whose rounded-rect interior is a
    // "magic" key color declared transparent via wxImage's color mask. Drawn over the square
    // thumbnail in on_paint (DrawBitmap useMask=true), the key-color interior drops out and
    // only the bg-colored corners remain — hiding the bitmap's square corners so the preview
    // reads as rounded.
    //
    // Why color mask (not alpha): MSW's wxBitmap(wxImage) corrupts alpha=0 pixels (their RGB
    // becomes black), and wxCOMPOSITION_DESTINATION_OUT is unavailable in this wx build. The
    // color-mask path (wxImage::SetMaskColour → wxBitmap) is wxWidgets' oldest, most portable
    // transparency mechanism and needs no alpha channel. The key color #FF00FF never appears
    // in the bg-colored corners.
    void rebuild_corner_mask()
    {
        const wxSize sz = GetClientSize();
        if (sz.x <= 0 || sz.y <= 0) return;
        const int radius = FromDIP(m_radius_dip);
        const wxColour bg = StateColor::darkModeColorFor(wxColour(245, 245, 245));
        const wxColour key(255, 0, 255); // mask key — transparent where it appears

        wxImage img(sz);
        // Fill entire panel with bg (becomes the corner fill).
        img.SetRGB(wxRect({0, 0}, sz), bg.Red(), bg.Green(), bg.Blue());
        // Overpaint the rounded-rect interior with the key color (will be masked transparent).
        wxBitmap tmp(img);
        {
            wxMemoryDC memdc;
            memdc.SelectObject(tmp);
            memdc.SetBrush(wxBrush(key));
            memdc.SetPen(*wxTRANSPARENT_PEN);
            memdc.DrawRoundedRectangle(wxRect(sz), radius);
            memdc.SelectObject(wxNullBitmap);
        }
        img = tmp.ConvertToImage();
        img.SetMaskColour(key.Red(), key.Green(), key.Blue());
        img.SetMask(true);
        m_corner_mask = wxBitmap(std::move(img));
    }

    void on_paint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize sz = GetClientSize();
        const int radius = FromDIP(m_radius_dip);

        // 1) Rounded background (#F5F5F5, theme-aware). Shows through the corner mask's
        //    transparent center and any letterbox bars around the centered thumbnail.
        dc.SetBrush(wxBrush(StateColor::darkModeColorFor(wxColour(245, 245, 245))));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRoundedRectangle(wxRect(sz), radius);

        // 2) Square thumbnail, centered, scaled to fit (preserve aspect ratio). The corners
        //    will be covered by the mask in step 3 so the result reads as rounded.
        //    Falls back to the mixed-filament placeholder SVG when no real thumbnail has
        //    been set yet (e.g. Before-Match state on the After-Match panel, or while plate
        //    thumbnails are still being generated). The placeholder goes through the same
        //    corner mask so it reads as rounded like a real render. Loaded via ScalableBitmap
        //    on first paint — the codebase's standard SVG-load convention (see m_placeholder).
        if (!m_bmp.IsOk() && !m_placeholder_loaded) {
            // px_cnt is the SVG's native height in DIP (110x60). ScalableBitmap applies
            // FromDIP internally, so the bitmap is rasterized at 110x60 physical pixels on
            // 100% DPI and @2x on Retina — drawn 1:1 centered, exactly as the SVG is authored.
            m_placeholder = ScalableBitmap(this, "mixed_filament_preview_placeholder", 60);
            m_placeholder_loaded = true;
        }
        const wxBitmap& draw_bmp = m_bmp.IsOk() ? m_bmp :
            (m_placeholder_loaded ? m_placeholder.bmp() : wxNullBitmap);
        if (draw_bmp.IsOk()) {
            const int bw = draw_bmp.GetWidth();
            const int bh = draw_bmp.GetHeight();
            if (bw > 0 && bh > 0) {
                // Both paths draw 1:1 pixels (DrawBitmap does not scale): the real thumbnail is
                // pre-scaled to the panel in thumbnail_to_bitmap; the placeholder is rasterized
                // at its natural size by ScalableBitmap. Just center it.
                dc.DrawBitmap(draw_bmp, (sz.x - bw) / 2, (sz.y - bh) / 2, false);
            }
        }

        // 3) Corner mask overlay — bg-colored corners hide the square bitmap's corners; the
        //    key-color interior is masked transparent so the thumbnail shows through.
        //    useMask=true applies the color mask (set in rebuild_corner_mask).
        if (m_corner_mask.IsOk())
            dc.DrawBitmap(m_corner_mask, 0, 0, true);

        // 4) Badge: square-cornered label, top-left, with white text. Drawn here (not a
        // child window) so StaticBox-style compositing can't erase it.
        // Style: bg (147,147,147) — the opaque equivalent of rgba(0,0,0,0.40) over the
        // #F5F5F5 panel bg (no alpha blending needed; a plain wxDC DrawRectangle handles
        // it). Square corners (no border-radius); the preview image keeps radius 8.
        dc.SetFont(Label::Body_12);
        const wxSize text_sz = dc.GetTextExtent(m_badge_label);
        const int pad_x = FromDIP(8);
        const int pad_y = FromDIP(4);
        const int inset = FromDIP(8);
        wxRect badge(inset, inset, text_sz.x + pad_x * 2, text_sz.y + pad_y * 2);
        dc.SetBrush(wxBrush(wxColour(147, 147, 147)));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(badge);
        dc.SetTextForeground(*wxWHITE);
        dc.DrawText(m_badge_label, badge.x + pad_x, badge.y + pad_y);
    }
};

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

MixedFilamentBatchDialog::MixedFilamentBatchDialog(wxWindow* parent)
    : DPIDialog(parent, wxID_ANY, _L("Mixed Color Match"),
                wxDefaultPosition, wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX)
{
    SetSize(FromDIP(540), FromDIP(680));
    SetMinClientSize(wxSize(FromDIP(500), FromDIP(560)));
    CentreOnScreen();

    if (wxGetApp().preset_bundle) {
        ConfigOptionStrings* co = wxGetApp().preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
        if (co) m_physical_colors = co->values;
    }
    // Default the manual-mode filament count to the number of physical filaments,
    // capped at 4 (the max supported slots) and floored at 2 (the min).
    //   >4 filaments -> 4, 3 -> 3, 2 -> 2.
    {
        const size_t n = m_physical_colors.size();
        m_manual_filament_count = n > 4 ? 4 : (n >= 2 ? static_cast<int>(n) : 2);
    }
    if (wxGetApp().plater()) {
        m_tray_count = wxGetApp().plater()->get_partplate_list().get_plate_count();
        if (m_tray_count < 1) m_tray_count = 1;
    }
    load_model_colors();
    build_ui();
    set_match_buttons_state(false);
    m_btn_start_match->Enable(!m_model_colors.empty() && m_physical_colors.size() >= 2);

    // Default to recommended mode — show CMYK card
    if (m_recommended_card) {
        m_recommended_card->Show(true);
        m_recommended_card->Layout();
    }

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
    SetSize(FromDIP(540), FromDIP(680));
    Layout();
    Refresh();
}

// Convert ThumbnailData (RGBA pixels, OpenGL FBO = bottom-up) to wxBitmap (top-down)
// Blends alpha against the panel background (#F5F5F5)
static wxBitmap thumbnail_to_bitmap(const ThumbnailData& data, int max_w, int max_h)
{
    if (!data.is_valid() || data.width == 0 || data.height == 0)
        return wxNullBitmap;

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
            // Blend with panel background (#F5F5F5) for low-alpha pixels
            if (alpha < 255) {
                float t = alpha / 255.0f;
                r = static_cast<unsigned char>(r * t + 245 * (1.0f - t));
                g = static_cast<unsigned char>(g * t + 245 * (1.0f - t));
                b = static_cast<unsigned char>(b * t + 245 * (1.0f - t));
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
    // (initial open, re-match in progress) we leave m_bmp unset so RoundedPreviewPanel
    // renders its placeholder — avoids showing the original render on the After-Match side.
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
                for (const std::string& hex : co->values) {
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
    // Apply match mappings: find which original extruder slot each
    // source_color belongs to, and replace it with the matched_color.
    // This keeps the array indexed by the ORIGINAL extruder ID, which is
    // what the render pipeline reads (extruder_colors[extruder_id-1]).
    for (const auto& mapping : m_result.mappings) {
        wxColour src   = mapping.source_color;
        bool     found = false;
        for (size_t i = 0; i < m_match_colors.size(); ++i) {
            wxColour slot_color(
                static_cast<unsigned char>(m_match_colors[i].r() * 255.f + 0.5f),
                static_cast<unsigned char>(m_match_colors[i].g() * 255.f + 0.5f),
                static_cast<unsigned char>(m_match_colors[i].b() * 255.f + 0.5f));
            if (slot_color == src) {
                m_match_colors[i] = {
                    float(mapping.matched_color.Red())   / 255.f,
                    float(mapping.matched_color.Green()) / 255.f,
                    float(mapping.matched_color.Blue())  / 255.f,
                    1.0f,
                };
                found = true;
                break;
            }
        }
        // If source_color is not in any physical slot (virtual extruder /
        // mixed filament from a prior match), add a new slot.
        if (!found) {
            size_t slot = (mapping.target_filament_id > 0)
                ? static_cast<size_t>(mapping.target_filament_id - 1)
                : m_match_colors.size();
            if (slot >= m_match_colors.size())
                m_match_colors.resize(slot + 1, {0.5f, 0.5f, 0.5f, 1.0f});
            m_match_colors[slot] = {
                float(mapping.matched_color.Red())   / 255.f,
                float(mapping.matched_color.Green()) / 255.f,
                float(mapping.matched_color.Blue())  / 255.f,
                1.0f,
            };
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
    auto* canvas = plater->canvas3D();
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
    auto* canvas = plater->canvas3D();
    if (!canvas) return;

    const int orig_w = std::max(FromDIP(ORIG_PREVIEW_DIP), m_preview_orig_panel->GetClientSize().GetWidth());
    const int orig_h = std::max(FromDIP(ORIG_PREVIEW_DIP), m_preview_orig_panel->GetClientSize().GetHeight());

    std::vector<ColorRGBA> orig_colors;
    auto* pb = wxGetApp().preset_bundle;
    if (pb) {
        auto* co = pb->project_config.option<ConfigOptionStrings>("filament_colour");
        if (co) {
            for (const std::string& hex : co->values) {
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
    m_model_colors.clear();

    auto* plater = wxGetApp().plater();
    if (!plater) return;

    const auto all_colors = plater->get_extruder_colors_from_plater_config(nullptr, true);

    for (size_t i = 0; i < all_colors.size(); ++i) {
        const std::string& hex = all_colors[i];
        if (hex.empty()) continue;
        wxColour c;
        if (!try_parse_color_match_hex(hex, c)) continue;

        // all_colors is indexed by filament_id-1: [physical colors..., then mixed
        // display colors]. filament_id = i+1 for every slot — physical OR mixed.
        // For a mixed slot, i+1 is the virtual id its regions are painted with, so
        // binding it here lets apply_batch_match_to_model re-match and re-map
        // existing mixed-painted regions to the new target. Leaving it empty (the
        // old behavior) stranded those regions: never re-matched, then erased by
        // cleanup when a component physical was dropped.
        m_model_colors.push_back({
            static_cast<unsigned int>(m_model_colors.size() + 1),
            c, hex,
            std::vector<unsigned int>{static_cast<unsigned int>(i + 1)}
        });
    }
}

void MixedFilamentBatchDialog::update_recommended_card()
{
    if (!m_recommended_card) return;
    const auto& colors = m_result.recommended_physical_colors;
    if (colors.size() < 4) return;

    // Color name lookup — maps from hex in CMYG palette back to a
    // human-readable label.  Covers the 4 canonical colors.
    static const std::unordered_map<std::string, wxString> kColorName = {
        {"#08ABFB", _L("Blue")},
        {"#D93B90", _L("Magenta")},
        {"#F9ED3D", _L("Yellow")},
        {"#9199A4", _L("Gray")},
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

        // Update label text
        if (m_recommended_labels[i]) {
            auto it = kColorName.find(colors[i]);
            m_recommended_labels[i]->SetLabel(
                it != kColorName.end()
                    ? it->second
                    : wxString::Format("F%d", int(i + 1)));
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
    build_banners();
    build_mode_row();

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
    m_scrolled_content->SetMinSize(wxSize(FromDIP(540), FromDIP(80)));
    m_root->Add(m_scrolled_content, 1, wxEXPAND);

    // Footer (Cancel/Confirm) above progress: Figma spec has the match-progress bar pinned
    // to the dialog's bottom edge, with the action buttons sitting just above it. Order
    // matters here — VERTICAL sizer renders in insertion order, so footer is added first.
    // (Progress bar + label live inside build_footer's panel; no separate build_progress step.)
    build_footer();

    SetSizer(m_root);
    Layout();
    SetSize(FromDIP(540), FromDIP(680));

    update_add_remove_buttons();
}

void MixedFilamentBatchDialog::build_banners()
{
    // Error banner
    m_error_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_error_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FDE8E8")));
    m_error_panel->Hide();
    auto* es = new wxBoxSizer(wxHORIZONTAL);
    ScalableBitmap ebmp(m_error_panel, "error_icon_red_exclamation", 14);
    es->Add(new wxStaticBitmap(m_error_panel, wxID_ANY, ebmp.bmp()), 0, wxALL, FromDIP(8));
    m_error_text = new Label(m_error_panel, Label::Body_12, wxEmptyString, LB_AUTO_WRAP);
    m_error_text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#D32F2F")));
    m_error_text->SetMaxSize(wxSize(FromDIP(480), -1));
    es->Add(m_error_text, 1, wxALL, FromDIP(8));
    m_error_panel->SetSizer(es);
    m_root->Add(m_error_panel, 0, wxEXPAND);

    // Warning banner
    m_warning_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_warning_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFF3EB")));
    m_warning_panel->Hide();
    auto* ws = new wxBoxSizer(wxHORIZONTAL);
    ScalableBitmap wbmp(m_warning_panel, "icon_warning_triangle", 14);
    ws->Add(new wxStaticBitmap(m_warning_panel, wxID_ANY, wbmp.bmp()), 0, wxALL, FromDIP(8));
    m_warning_text = new Label(m_warning_panel, Label::Body_12, wxEmptyString, LB_AUTO_WRAP);
    m_warning_text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#FF842D")));
    m_warning_text->SetMaxSize(wxSize(FromDIP(480), -1));
    ws->Add(m_warning_text, 1, wxALL, FromDIP(8));
    m_warning_panel->SetSizer(ws);
    m_root->Add(m_warning_panel, 0, wxEXPAND);
}

void MixedFilamentBatchDialog::build_mode_row()
{
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    {
        auto* lbl = new wxStaticText(this, wxID_ANY, _L("Match Mode"));
        lbl->SetFont(Label::Body_14);
        lbl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
        row->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(8));
    }
    m_method_combo = new ComboBox(this, wxID_ANY, wxEmptyString,
                                   wxDefaultPosition, wxSize(FromDIP(149), FromDIP(30)),
                                   0, nullptr, wxCB_READONLY);
    m_method_combo->Append(_L("Auto Match"));
    m_method_combo->Append(_L("Manual Select"));
    m_method_combo->SetSelection(0);
    m_method_combo->Bind(wxEVT_COMBOBOX, &MixedFilamentBatchDialog::on_method_changed, this);
    row->Add(m_method_combo, 0, wxALIGN_CENTER_VERTICAL);

    row->AddStretchSpacer();

    // Segmented Start / Re-match tabs (80x28). Active tab is teal (#009688), inactive gray
    // (#d9d9d9); set_match_buttons_state drives which is enabled based on match progress.
    auto make_tab = [this](Button*& slot, const wxString& label) {
        auto* btn = new Button(this, label);
        btn->SetMinSize(wxSize(FromDIP(80), FromDIP(28)));
        btn->SetCornerRadius(FromDIP(4));
        btn->SetBorderWidth(0);
        btn->SetBackgroundColor(StateColor(
            std::pair(wxColour("#D9D9D9"), static_cast<int>(StateColor::Disabled)),
            std::pair(wxColour("#009688"), static_cast<int>(StateColor::Normal))));
        btn->SetTextColor(StateColor(
            std::pair(wxColour("#8F8F8F"), static_cast<int>(StateColor::Disabled)),
            std::pair(wxColour("#FFFFFF"), static_cast<int>(StateColor::Normal))));
        slot = btn;
        return btn;
    };
    row->Add(make_tab(m_btn_start_match, _L("Start Matching")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    row->Add(make_tab(m_btn_rematch, _L("Re-match")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

    m_btn_start_match->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { start_batch_match(); });
    m_btn_rematch->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { start_batch_match(); });

    m_root->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, FromDIP(10));
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
    card->SetMinSize(wxSize(FromDIP(459), -1));
    card->SetMaxSize(wxSize(FromDIP(459), -1));
    card->Hide();
    m_manual_card = card;
    auto* s = new wxBoxSizer(wxVERTICAL);

    // Title row + add/remove (same pattern as MixedFilamentDialog)
    {
        auto* title_row = new wxBoxSizer(wxHORIZONTAL);
        auto* lbl = new wxStaticText(card, wxID_ANY, _L("Filament Config"));
        lbl->SetFont(Label::Body_14);
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
                m_manual_card->Layout();
                m_manual_card->GetParent()->Layout();
            }
        });
        title_row->Add(m_btn_remove_filament, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));

        m_btn_add_filament = new ScalableButton(card, wxID_ANY, "icon_plus");
        m_btn_add_filament->SetToolTip(_L("Add filament"));
        m_btn_add_filament->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            if (m_manual_filament_count < 4) {
                m_manual_row_panels[m_manual_filament_count]->Show();
                ++m_manual_filament_count;
                on_manual_selection_changed();
                update_add_remove_buttons();
                m_manual_card->Layout();
                m_manual_card->GetParent()->Layout();
            }
        });
        title_row->Add(m_btn_add_filament, 0, wxALIGN_CENTER_VERTICAL);
        s->Add(title_row, 0, wxEXPAND | wxALL, FromDIP(16));
    }

    // Divider
    {
        auto* divider = new wxPanel(card, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
        divider->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F3F4F6")));
        s->Add(divider, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
    }

    // 2-column grid of filament selectors (ComboBox rows with color icons)
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
        auto* r = new wxBoxSizer(wxHORIZONTAL);
        auto* filament_lbl = new wxStaticText(panel, wxID_ANY, wxString::Format(_L("Filament %d"), i + 1),
                                               wxDefaultPosition, wxSize(FromDIP(40), -1));
        filament_lbl->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        r->Add(filament_lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
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
            wxBitmap* icon = get_extruder_color_icon(m_physical_colors[j], std::to_string(j + 1), FromDIP(20), FromDIP(20));
            cb->Append(name, icon ? icon->ConvertToImage() : wxNullImage);
        }
        if (m_filament_selections[i] >= 0 && m_filament_selections[i] < static_cast<int>(m_physical_colors.size()))
            cb->SetSelection(m_filament_selections[i]);
        else if (!m_physical_colors.empty())
            cb->SetSelection(0);
        cb->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { on_manual_selection_changed(); });
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
    parent.Add(card, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, FromDIP(16));
}

void MixedFilamentBatchDialog::build_recommended_card(wxBoxSizer& parent)
{
    auto* card = new StaticBox(m_scrolled_content, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    card->SetCornerRadius(FromDIP(4));
    card->SetBorderWidth(FromDIP(1));
    card->SetBorderColorNormal(StateColor::darkModeColorFor(wxColour("#F0F0F0")));
    card->SetBackgroundColor(StateColor(std::pair(wxColour("#FFFFFF"), static_cast<int>(StateColor::Normal))));
    // Same fixed width + centering as the manual card (see build_manual_card).
    card->SetMinSize(wxSize(FromDIP(459), -1));
    card->SetMaxSize(wxSize(FromDIP(459), -1));
    card->Hide();
    m_recommended_card = card;
    auto* s = new wxBoxSizer(wxVERTICAL);

    // Title
    {
        auto* lbl = new wxStaticText(card, wxID_ANY, _L("Filament Config"));
        lbl->SetFont(Label::Body_14);
        lbl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
        lbl->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        s->Add(lbl, 0, wxALL, FromDIP(16));
    }
    // Divider
    {
        auto* divider = new wxPanel(card, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
        divider->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F3F4F6")));
        s->Add(divider, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
    }

    // 2x2 grid: numbered CMYG swatch (20x20) + bordered name field. Initially populated
    // with the canonical CMYG order; update_recommended_card() refreshes swatches/names
    // after a match reflects the palette order chosen by recommend_best_filament_combo.
    static const std::vector<std::pair<std::string, wxString>> CMYG_ENTRIES = {
        {"#08ABFB", _L("Blue")},
        {"#D93B90", _L("Magenta")},
        {"#F9ED3D", _L("Yellow")},
        {"#9199A4", _L("Gray")},
    };
    auto* grid = new wxFlexGridSizer(2, FromDIP(12), FromDIP(12));
    grid->AddGrowableCol(0, 1);
    grid->AddGrowableCol(1, 1);
    for (int i = 0; i < 4; ++i) {
        auto* panel = new wxPanel(card, wxID_ANY);
        // wxPanel doesn't inherit the card's bg — set white explicitly (see build_manual_card).
        panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        auto* r = new wxBoxSizer(wxHORIZONTAL);
        // Numbered color swatch (stored for later update)
        wxBitmap* icon = get_extruder_color_icon(CMYG_ENTRIES[i].first, std::to_string(i + 1), FromDIP(20), FromDIP(20));
        if (icon) {
            auto* sw = new wxStaticBitmap(panel, wxID_ANY, *icon);
            m_recommended_swatches[i] = sw;
            r->Add(sw, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        }
        // Bordered name field (StaticBox wrapper, border #dbdbdb — same technique as
        // MixedFilamentDialog's pattern-input wrapper)
        auto* field = new StaticBox(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        field->SetCornerRadius(FromDIP(4));
        field->SetBorderWidth(FromDIP(1));
        field->SetBorderColorNormal(StateColor::darkModeColorFor(wxColour("#DBDBDB")));
        field->SetBackgroundColor(StateColor(std::pair(wxColour("#FFFFFF"), static_cast<int>(StateColor::Normal))));
        field->SetMinSize(wxSize(-1, FromDIP(24)));
        auto* fs = new wxBoxSizer(wxHORIZONTAL);
        auto* name = new wxStaticText(field, wxID_ANY, CMYG_ENTRIES[i].second);
        name->SetFont(Label::Body_13);
        name->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
        name->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        fs->Add(name, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(9));
        field->SetSizer(fs);
        m_recommended_labels[i] = name;
        r->Add(field, 1, wxALIGN_CENTER_VERTICAL | wxEXPAND);
        panel->SetSizer(r);
        grid->Add(panel, 0, wxEXPAND);
    }
    s->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    card->SetSizer(s);
    // Horizontal margin only; the card-to-card vertical gap is added at the build_ui call site.
    parent.Add(card, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, FromDIP(16));
}

void MixedFilamentBatchDialog::build_preview_card(wxBoxSizer& parent)
{
    m_preview_card = new StaticBox(m_scrolled_content, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_preview_card->SetCornerRadius(FromDIP(4));
    m_preview_card->SetBorderWidth(FromDIP(1));
    m_preview_card->SetBorderColorNormal(StateColor::darkModeColorFor(wxColour("#F0F0F0")));
    m_preview_card->SetBackgroundColor(StateColor(std::pair(wxColour("#FFFFFF"), static_cast<int>(StateColor::Normal))));
    // Same fixed width + centering as the filament-config cards.
    m_preview_card->SetMinSize(wxSize(FromDIP(459), -1));
    m_preview_card->SetMaxSize(wxSize(FromDIP(459), -1));
    auto* s = new wxBoxSizer(wxVERTICAL);

    // Dual previews: Original (fixed 180x180) + After Match (fixed 227x227), both rounded.
    // RoundedPreviewPanel paints the rounded background + thumbnail + corner mask + badge in
    // one wxBG_STYLE_PAINT handler (codebase's proven ImageGrid pattern); see the class doc
    // for why this replaces the earlier StaticBox + child wxStaticBitmap approach.
    {
        auto* prow = new wxBoxSizer(wxHORIZONTAL);
        m_preview_orig_panel = new RoundedPreviewPanel(m_preview_card, 180, 8, _L("Original Model"));
        // Top-align so the smaller (180h) original lines up with the 227h match panel at the
        // top; the 47px difference shows as empty space below the original.
        prow->Add(m_preview_orig_panel, 0, wxALIGN_TOP | wxRIGHT, FromDIP(12));

        m_preview_match_panel = new RoundedPreviewPanel(m_preview_card, 227, 8, _L("After Match"));
        prow->Add(m_preview_match_panel, 0, wxALIGN_TOP);
        s->Add(prow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));
    }

    // Divider between previews and controls
    {
        auto* divider = new wxPanel(m_preview_card, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
        divider->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F3F4F6")));
        s->Add(divider, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));
    }

    // Centered controls: Plate (prev arrow + label + combo + next arrow) | View (label + combo)
    {
        auto* crow = new wxBoxSizer(wxHORIZONTAL);
        m_btn_tray_prev = new ScalableButton(m_preview_card, wxID_ANY, "filament_picker_left_arrow");
        m_btn_tray_prev->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_tray_nav(-1); });
        crow->Add(m_btn_tray_prev, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));

        auto* plate_lbl = new wxStaticText(m_preview_card, wxID_ANY, _L("Plate"));
        plate_lbl->SetFont(Label::Body_12);
        plate_lbl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#4A4A4A")));
        crow->Add(plate_lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));

        m_tray_combo = new ComboBox(m_preview_card, wxID_ANY, wxEmptyString,
                                     wxDefaultPosition, wxSize(FromDIP(56), FromDIP(24)),
                                     0, nullptr, wxCB_READONLY);
        for (int i = 1; i <= m_tray_count; ++i)
            m_tray_combo->Append(wxString::Format("%02d", i));
        m_tray_combo->SetSelection(0);
        m_tray_combo->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) {
            m_tray_index = m_tray_combo->GetSelection() + 1;
            update_nav_arrow_state();
            refresh_previews();
        });
        crow->Add(m_tray_combo, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));

        m_btn_tray_next = new ScalableButton(m_preview_card, wxID_ANY, "filament_picker_right_arrow");
        m_btn_tray_next->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_tray_nav(1); });
        crow->Add(m_btn_tray_next, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(24));

        auto* view_lbl = new wxStaticText(m_preview_card, wxID_ANY, _L("View"));
        view_lbl->SetFont(Label::Body_12);
        view_lbl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#4A4A4A")));
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

        s->Add(crow, 0, wxALIGN_CENTER_HORIZONTAL | wxALL, FromDIP(16));
    }

    m_preview_card->SetSizer(s);
    parent.Add(m_preview_card, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, FromDIP(16));
}

void MixedFilamentBatchDialog::build_mapping_card(wxBoxSizer& parent)
{
    m_mapping_card = new StaticBox(m_scrolled_content, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_mapping_card->SetCornerRadius(FromDIP(4));
    m_mapping_card->SetBorderWidth(FromDIP(1));
    m_mapping_card->SetBorderColorNormal(StateColor::darkModeColorFor(wxColour("#F0F0F0")));
    m_mapping_card->SetBackgroundColor(StateColor(std::pair(wxColour("#FFFFFF"), static_cast<int>(StateColor::Normal))));
    // Same fixed width + centering as the filament-config cards.
    m_mapping_card->SetMinSize(wxSize(FromDIP(459), -1));
    m_mapping_card->SetMaxSize(wxSize(FromDIP(459), -1));
    auto* cs = new wxBoxSizer(wxVERTICAL);

    // Title row: label + info icon
    {
        auto* tr = new wxBoxSizer(wxHORIZONTAL);
        auto* lbl = new wxStaticText(m_mapping_card, wxID_ANY, _L("Color Mapping"));
        lbl->SetFont(Label::Body_14);
        lbl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
        tr->Add(lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
        m_mapping_info_icon = new ScalableButton(m_mapping_card, wxID_ANY, "info");
        m_mapping_info_icon->SetToolTip(_L("Hover a row to see its color difference."));
        tr->Add(m_mapping_info_icon, 0, wxALIGN_CENTER_VERTICAL);
        cs->Add(tr, 0, wxALL, FromDIP(16));
    }
    // Divider
    {
        auto* divider = new wxPanel(m_mapping_card, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
        divider->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F3F4F6")));
        cs->Add(divider, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
    }
    // Grid host (populated by update_mapping_legend). Fixed-col wxGridSizer is Mac-safe;
    // wxWrapSizer miscomputes its height on macOS (same rationale as MixedFilamentDialog).
    m_legend_panel = new wxPanel(m_mapping_card, wxID_ANY);
    m_legend_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
    m_legend_sizer = new wxGridSizer(LEGEND_GRID_COLS, FromDIP(10), FromDIP(10));
    m_legend_panel->SetSizer(m_legend_sizer);
    cs->Add(m_legend_panel, 0, wxEXPAND | wxALL, FromDIP(16));
    m_mapping_card->SetSizer(cs);
    parent.Add(m_mapping_card, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
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
    panel_sizer->AddSpacer(FromDIP(12));

    // Progress row (added before the button row so it renders ABOVE the buttons). Hidden by
    // default; set_match_buttons_state(true) reveals it. Parented to btn_panel and inside
    // the footer's VERTICAL sizer so the white footer bg extends under it and the whole
    // footer grows/shrinks as a unit when the progress row is shown/hidden.
    auto* progress_row = new wxBoxSizer(wxHORIZONTAL);
    m_progress_bar = new wxGauge(btn_panel, wxID_ANY, 100, wxDefaultPosition, wxSize(FromDIP(200), FromDIP(8)),
                                  wxGA_HORIZONTAL | wxGA_SMOOTH);
    m_progress_bar->SetValue(0);
    m_progress_bar->Hide();
    progress_row->Add(m_progress_bar, 1, wxALIGN_CENTER_VERTICAL);
    panel_sizer->Add(progress_row, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(20));
    panel_sizer->AddSpacer(FromDIP(12));

    auto* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_btn_cancel_match = new Button(btn_panel, _L("Cancel"));
    m_btn_cancel_match->SetMinSize(wxSize(-1, FromDIP(36)));
    m_btn_cancel_match->SetCornerRadius(FromDIP(4));
    m_btn_cancel_match->SetBorderWidth(FromDIP(1));
    m_btn_cancel_match->SetBorderColorNormal(StateColor::darkModeColorFor(wxColour("#D9D9D9")));
    m_btn_cancel_match->SetBackgroundColorNormal(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
    m_btn_cancel_match->SetTextColorNormal(StateColor::darkModeColorFor(wxColour("#242424")));

    m_btn_confirm = new Button(btn_panel, _L("Confirm"));
    m_btn_confirm->SetMinSize(wxSize(-1, FromDIP(36)));
    m_btn_confirm->SetCornerRadius(FromDIP(4));
    m_btn_confirm->SetBorderWidth(0);
    m_btn_confirm->SetBackgroundColor(StateColor(
        std::pair(wxColour("#DFDFDF"), static_cast<int>(StateColor::Disabled)),
        std::pair(wxColour("#009688"), static_cast<int>(StateColor::Normal))));
    m_btn_confirm->SetTextColor(StateColor(
        std::pair(wxColour("#6B6A6A"), static_cast<int>(StateColor::Disabled)),
        std::pair(wxColour("#FFFFFF"), static_cast<int>(StateColor::Normal))));

    btn_sizer->Add(m_btn_cancel_match, 1, wxRIGHT, FromDIP(8));
    btn_sizer->Add(m_btn_confirm, 1);
    panel_sizer->Add(btn_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(20));
    panel_sizer->AddSpacer(FromDIP(12));

    m_btn_cancel_match->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
    m_btn_confirm->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); });

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

void MixedFilamentBatchDialog::set_match_buttons_state(bool matching)
{
    m_btn_start_match->Enable(!matching && !m_match_completed);
    m_btn_rematch->Enable(!matching && m_match_completed);
    // Cancel always closes the dialog — keep it enabled in every state so the user can
    // bail out at any time (idle / matching / match-completed-pending-confirm).
    m_btn_cancel_match->Enable(true);
    m_btn_confirm->Enable(!matching && m_match_completed && m_result.success);
    if (matching) {
        m_progress_bar->Show();
        m_progress_bar->SetValue(0);
    } else {
        m_progress_bar->Hide();
    }
}

void MixedFilamentBatchDialog::on_method_changed(wxCommandEvent&)
{
    int sel = m_method_combo->GetSelection();
    m_matching_method = (sel == 1) ? MANUAL : RECOMMENDED;
    // Preserve the previous match result; only Start/Re-match clears it.
    m_error_panel->Hide();
    m_warning_panel->Hide();
    if (m_manual_card)
        m_manual_card->Show(m_matching_method == MANUAL);
    if (m_recommended_card)
        m_recommended_card->Show(m_matching_method == RECOMMENDED);
    // Force parent re-layout since card visibility changed
    if (m_matching_method == MANUAL && m_manual_card) {
        m_manual_card->Layout();
        m_manual_card->GetParent()->Layout();
    }
    if (m_matching_method == RECOMMENDED && m_recommended_card) {
        m_recommended_card->Layout();
        m_recommended_card->GetParent()->Layout();
    }
    update_mapping_legend();
    set_match_buttons_state(false);
    Layout();
}

void MixedFilamentBatchDialog::on_manual_selection_changed()
{
    // Preserve the previous match result; only Start/Re-match clears it.
    set_match_buttons_state(false);
}

void MixedFilamentBatchDialog::update_add_remove_buttons()
{
    // Mirror MixedFilamentDialog: hide the remove button at the minimum (2)
    // and the add button at the maximum (4) instead of leaving them visible.
    if (m_btn_remove_filament) m_btn_remove_filament->Show(m_manual_filament_count > 2);
    if (m_btn_add_filament)    m_btn_add_filament->Show(m_manual_filament_count < 4);
}

// ---------------------------------------------------------------------------
// Match
// ---------------------------------------------------------------------------

void MixedFilamentBatchDialog::start_batch_match()
{
    if (m_match_running) return;
    if (m_model_colors.empty()) {
        set_error(_L("No model colors found. Please load a multi-color model."));
        return;
    }
    m_match_running = true;
    m_error_panel->Hide();
    m_warning_panel->Hide();
    // Clear stale legend from previous match before starting new one
    m_result = BatchMatchResult{};
    m_match_completed = false;
    update_mapping_legend();
    m_legend_panel->Layout();
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
                active_colors.push_back(m_physical_colors[sel]);
                manual_full_ids.push_back(static_cast<unsigned int>(sel + 1)); // 1-based
            }
        }
        if (active_colors.size() < 2) {
            active_colors = m_physical_colors;
            manual_full_ids.clear();
            for (size_t i = 0; i < m_physical_colors.size(); ++i)
                manual_full_ids.push_back(static_cast<unsigned int>(i + 1));
        }
    }
    const auto manual_colors     = std::move(active_colors);
    auto manual_full_ids_c = std::move(manual_full_ids);
    const auto all_physical      = m_physical_colors;

    const auto matching_method = m_matching_method;
    // Fixed CMYG palette for recommended mode.
    static const std::vector<std::string> CMYG_COLORS = {
        "#08ABFB",  // blue
        "#D93B90",  // magenta
        "#F9ED3D",  // yellow
        "#9199A4",  // gray
    };
    const std::vector<std::string> preset_colors = CMYG_COLORS;
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
                auto best = recommend_best_filament_combo(model_colors, preset_colors, 15, cancel_token);
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
        // Recommended mode: only use the recommended CMYG physical colors as the
        // Pass-1 reuse palette. Existing mixed filaments are NOT included because
        // (a) their display_colors reference the old physical palette that is about
        // to be replaced, and (b) a passthrough recipe targeting a virtual filament
        // ID (e.g. component_a=5) would be silently clamped in add_batch_custom_filaments.

        // Pass 1: map each model color to closest existing filament
        constexpr double K_REUSE_THRESHOLD = 1.5;
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
        if (!unmatched_colors.empty()) {
            auto sub_result = batch_match_model_colors(unmatched_colors, physical_colors, 15, cancel_token,
                [progress_bar, destroyed](int done, int total) {
                    if (progress_bar && !destroyed->load()) {
                        wxGetApp().CallAfter([progress_bar, done, total, destroyed]() {
                            if (destroyed->load()) return;
                            if (total > 0) progress_bar->SetValue(done * 100 / total);
                        });
                    }
                });
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

        if (result.success && result.mappings.empty()) {
            result.success = false;
            result.error_message = "No valid recipes found for any model color";
            result.error_code = 1;
        }
        if (result.success) {
            // Each model color always gets its own mapping entry — no deduplication.
            // Merging by matched_color would lose distinct model-source colors whose
            // recipes happen to produce similar output shades.
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
        set_error(wxString::FromUTF8(result.error_message));
        // Route through set_match_buttons_state so the Start vs Re-match enable mask
        // follows m_match_completed (false here — Start is enabled, Re-match is not).
        // Hard-coding both Enable(true) left Re-match clickable with no result to re-match.
        set_match_buttons_state(false);
        Layout();
        return;
    }

    m_match_completed = false;
    m_result = result;
    m_match_completed = true;
    set_match_buttons_state(false);  // after m_match_completed=true so confirm enables
    update_mapping_legend();
    rebuild_match_thumb_cache();
    if (result.is_recommended_mode)
        update_recommended_card();
    refresh_previews();
    BOOST_LOG_TRIVIAL(info) << "Batch match: " << result.mappings.size()
                            << " mappings, avg ΔE=" << result.avg_delta_e;
    Layout();
}

// ---------------------------------------------------------------------------
// Legend (matches prototype: src-swatch → badge-number  ΔE-badge)
// ---------------------------------------------------------------------------

void MixedFilamentBatchDialog::update_mapping_legend()
{
    // Clear(true) deletes row windows immediately (vs deferred Destroy), so their
    // pixel regions are released before repaint — same pattern as rebuild_legend.
    m_legend_sizer->Clear(true);

    if (m_result.mappings.empty()) {
        m_legend_sizer->Add(new Label(m_legend_panel,
            _L("No color mappings yet. Click \"Start Match\" to begin.")), 0, wxALL, FromDIP(4));
    } else {
        for (const auto& mapping : m_result.mappings) {
            // Bordered item box (white bg, #dbdbdb border) holding
            // [source swatch 20] -> [arrow icon] -> [target swatch 28 with filament number].
            // Per-item ΔE is hover-only (native tooltip) to keep the row clean.
            auto* item = new StaticBox(m_legend_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
            item->SetCornerRadius(FromDIP(4));
            item->SetBorderWidth(FromDIP(1));
            item->SetBorderColorNormal(StateColor::darkModeColorFor(wxColour("#DBDBDB")));
            item->SetBackgroundColor(StateColor(std::pair(wxColour("#FFFFFF"), static_cast<int>(StateColor::Normal))));
            auto* s = new wxBoxSizer(wxHORIZONTAL);

            // Source swatch (20x20)
            ColorBlockParams src;
            src.mode = ColorBlockParams::Solid;
            src.solid_color = mapping.source_color.IsOk() ? mapping.source_color : wxColour(128, 128, 128);
            src.width  = FromDIP(20);
            src.height = FromDIP(20);
            auto* src_bmp = new wxStaticBitmap(item, wxID_ANY, *get_color_block_bitmap_cached(src));
            s->Add(src_bmp, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));

            // Arrow icon (Figma uses a thin right-pointing arrow between the swatches)
            ScalableBitmap arrow_bmp(item, "filament_picker_right_arrow", 16);
            auto* arrow = new wxStaticBitmap(item, wxID_ANY, arrow_bmp.bmp());
            s->Add(arrow, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));

            // Target swatch with filament number (28x28)
            ColorBlockParams badge;
            badge.mode = ColorBlockParams::Solid;
            badge.solid_color = mapping.matched_color.IsOk() ? mapping.matched_color : wxColour(128, 128, 128);
            badge.width  = FromDIP(28);
            badge.height = FromDIP(28);
            badge.label  = wxString::Format("%u", mapping.target_filament_id);
            auto* tgt_bmp = new wxStaticBitmap(item, wxID_ANY, *get_color_block_bitmap_cached(badge));
            s->Add(tgt_bmp, 0, wxALIGN_CENTER_VERTICAL);

            item->SetSizer(s);
            // ΔE revealed on hover. wx tooltips are per-window (they do NOT inherit from the
            // parent), and the swatches/arrow are independent child windows — so we must set
            // the same tip on every child too, otherwise hovering a swatch shows nothing and
            // only the gaps between children trigger the row's tip.
            const wxString grade = (mapping.delta_e <= 1.0) ? _L("Excellent")
                                  : (mapping.delta_e <= 3.0) ? _L("Good")
                                  : _L("Poor");
            const wxString tip = wxString::Format(_L("Color diff: %s    ΔE = %.1f"), grade, mapping.delta_e);
            item->SetToolTip(tip);
            src_bmp->SetToolTip(tip);
            arrow->SetToolTip(tip);
            tgt_bmp->SetToolTip(tip);
            m_legend_sizer->Add(item, 0, wxEXPAND | wxALL, FromDIP(3));
        }
    }
    // Card grows with content (no inner scroller); re-layout the panel/card and
    // let the outer dialog-level scroller absorb the new height.
    m_legend_panel->Layout();
    m_mapping_card->Layout();
    m_mapping_card->Refresh();
    if (m_scrolled_content) {
        m_scrolled_content->FitInside();
        m_scrolled_content->Refresh();
    }
}

}} // namespace Slic3r::GUI
