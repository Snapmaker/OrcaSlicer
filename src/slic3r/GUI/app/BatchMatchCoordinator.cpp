#include "BatchMatchCoordinator.hpp"

#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <wx/wupdlock.h>

#include <boost/log/trivial.hpp>

#include "../GUI_App.hpp"
#include "../I18N.hpp"
#include "../MsgDialog.hpp"                // RichMessageDialog
#include "../MixedColorMatchHelpers.hpp"     // is_filament_compatible 等
#include "../MixedFilamentBatchDialog.hpp"   // apply_batch_match_to_model / MixedFilamentBatchDialog
#include "../Plater.hpp"
#include "../GLCanvas3D.hpp"                 // 阶段 I：批匹配后刷新 MMU 涂色 gizmo 的内存副本
#include "../Gizmos/GLGizmosManager.hpp"
#include "../Gizmos/GLGizmoMmuSegmentation.hpp"
#include "../GUI_ObjectList.hpp"   // H2 3.1 顶部瘦身：ObjectList 完整型（原经 Plater.hpp→Search.hpp 传递链）
#include "../PresetComboBoxes.hpp"           // PlaterPresetComboBox
#include "../Sidebar.hpp"
#include "../Widgets/ProgressDialog.hpp"     // ProgressDialog
#include "../Mixing/MixingBatchHelpers.hpp"   // extract_batch_kept_sets
#include "libslic3r/Model.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {
namespace GUI {
namespace app {

BatchMatchCoordinator::BatchMatchCoordinator(Sidebar& sidebar)
    : m_sidebar(sidebar)
{
}

void BatchMatchCoordinator::run_batch_match()
{
        if (!wxGetApp().preset_bundle) return;
        // No loaded model → batch match has nothing to map. Surface as a confirmation
        // dialog (not an error — these are "please do X first" hints, not failures)
        // BEFORE opening the dialog, using the same RichMessageDialog style as the
        // filament-sync prompts (caption "Color Mixing Match", "Got it" button, screen-centred).
        if (wxGetApp().plater()->model().objects.empty()) {
            RichMessageDialog dlg(&m_sidebar,
                _L("No model detected. Import a multi-color model to continue."),
                _L("Color Mixing Match"), wxOK);
            dlg.SetOKLabel(_L("Got it"));
            dlg.CentreOnScreen();
            dlg.ShowModal();
            return;
        }
        ConfigOptionStrings* co = wxGetApp().preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
        const std::vector<std::string> colors = co ? co->values : std::vector<std::string>{};
        if (colors.size() < 2) {
            RichMessageDialog dlg(&m_sidebar,
                _L("Color Mixing Match is unavailable when only one filament is added to Filament Management."),
                _L("Color Mixing Match"), wxOK);
            dlg.SetOKLabel(_L("Got it"));
            dlg.CentreOnScreen();
            dlg.ShowModal();
            return;
        }
        MixedFilamentBatchDialog dlg(&m_sidebar);
        if (dlg.ShowModal() != wxID_OK) return;
        const BatchMatchResult& result = dlg.GetResult();
        if (!result.success) return;

        // Applying batch-match results involves palette rewrite, mixed-filament
        // creation, painting remap and cleanup — all synchronous on the main
        // thread. Show a determinate progress dialog so the user sees real
        // progress instead of "not responding". Profiling showed ~96% of apply
        // time is the physical-filament deletion loop inside cleanup_unused_, so
        // the bar is mapped by time share: quick pre-steps → 0..5, the deletion
        // loop → 5..95 (reported back per-deletion via on_progress), the final UI
        // refresh → 95..100. wxWidgets pitfall (§70): Update() pumps the event
        // loop and its return value reflects skip/close state; there is no abort
        // button here so it's consumed but not acted on. wxPD_APP_MODAL blocks
        // interaction with the main window while the apply runs.
        ProgressDialog progress(_L("Loading"),
                                _L("Updating the Filaments and Color Mixing list..."),
                                100, find_toplevel_parent(&m_sidebar),
                                wxPD_APP_MODAL | wxPD_AUTO_HIDE);
        const wxString kMsg = _L("Updating the Filaments and Color Mixing list...");
        auto set_progress = [&](int pct) {
            // Return value intentionally not acted on (no wxPD_CAN_ABORT button),
            // but consumed to satisfy §70 ("check the return value of Update()").
            (void) progress.Update(pct, kMsg);
        };
        set_progress(1);

        auto& mgr = wxGetApp().preset_bundle->mixed_filaments;

        // Snapshot the dialog-time virtual-id -> stable_id mapping BEFORE any
        // palette or filament-count change.  The dialog captured each mapping's
        // source_extruder_ids ({slot index + 1}) against exactly this
        // enumeration; stable_ids survive the auto_generate rebuild that
        // set_num_filaments runs in recommended mode, so this table lets the
        // in-place / translate steps below re-find existing mixed rows by
        // IDENTITY.  (Display colors are refreshed to the NEW palette before
        // those steps run, so a color key would silently never match there.)
        std::unordered_map<unsigned int, uint64_t> dialog_vid_to_sid;
        const size_t dialog_num_physical = wxGetApp().preset_bundle->filament_presets.size();
        {
            unsigned int vid = static_cast<unsigned int>(dialog_num_physical) + 1;
            for (const MixedFilament& mf : mgr.mixed_filaments()) {
                if (!mf.enabled || mf.deleted) continue;
                dialog_vid_to_sid[vid++] = mf.stable_id;
            }
        }

        // NOTE: deliberately NO take_snapshot() here. The UndoRedo stack only
        // captures the Model (object painting), not the preset_bundle palette
        // or mixed_filaments. Snapshotting batch match would make Ctrl+Z revert
        // painting while leaving the expanded recommended palette in place → silent
        // wrong colors. Re-enable only after UndoRedo covers preset_bundle.
        // For recommended mode, apply the matched palette colors to the first 4 physical slots.
        // < 4 filaments: expand to 4.  >= 4 filaments: keep every slot THROUGH
        // this stage (virtual ids / add_batch compute over the full slot
        // space).  The batch match is a project-wide re-plan of the filament
        // system, so cleanup_unused_filaments_after_batch_match below removes
        // whichever physical slots the match did not select
        // (result.selected_physical_ids = {1..4} in recommended mode).
        std::vector<std::string> colors_vec;
        if (result.is_recommended_mode && result.recommended_physical_colors.size() >= 4) {
            const auto& cm = result.recommended_physical_colors;
            auto* pb = wxGetApp().preset_bundle;
            auto* fc = pb->project_config.option<ConfigOptionStrings>("filament_colour");

            const size_t current_count = pb->filament_presets.size();
            const size_t target_count  = std::max<size_t>(4, current_count);

            // Build full palette: matched colors in slots 1-4, original in 5+.
            colors_vec = fc ? fc->values : std::vector<std::string>{};
            colors_vec.resize(target_count);

            // Recommended-mode colors are plain single-color filaments.
            // Clear any left-over dual-color / gradient metadata on the
            // first 4 slots so the UI renders the new palette instead of
            // the old multi-color swatches.  (filament_multi_colors is
            // indexed the same as filament_colour; a non-empty entry
            // takes precedence over the single-color value.)
            auto* multi_colors = pb->project_config.option<ConfigOptionStrings>("filament_multi_colors");
            auto* color_modes  = pb->project_config.option<ConfigOptionInts>("filament_colour_mode");
            if (multi_colors)
                multi_colors->values.resize(target_count);
            if (color_modes)
                color_modes->values.resize(target_count);
            for (size_t i = 0; i < 4 && i < cm.size(); ++i) {
                colors_vec[i] = cm[i];
                if (multi_colors)
                    multi_colors->values[i].clear();
                if (color_modes)
                    color_modes->values[i] = 0;
            }

            // Write before set_num_filaments so auto_generate sees the matched palette.
            if (fc) fc->values = colors_vec;

            // Snapshot the old mixed list BEFORE set_num_filaments clears
            // custom entries, so we can build a proper old→new remap that
            // covers the existing painted virtual IDs (review item R3).
            const std::vector<MixedFilament> old_mixed_snapshot = pb->mixed_filaments.mixed_filaments();

            pb->set_num_filaments(static_cast<unsigned int>(target_count), std::vector<std::string>{});

            // Restore custom entries that were wiped by clear_custom_entries
            // in update_multi_material_filament_presets.  add_batch_custom_
            // filaments below will manage the batch-matched rows; we just
            // need to keep the pre-existing custom rows alive so their
            // stable_ids survive into the in-place edit block above and the
            // translation step below.
            {
                const std::string saved = pb->project_config.opt_string("mixed_filament_definitions");
                if (!saved.empty())
                    pb->mixed_filaments.load_custom_entries(saved, colors_vec);
            }

            // Build a remap from old→new virtual IDs so on_filaments_change
            // correctly remaps existing painted mixed-IDs before
            // apply_batch_match_to_model runs.  A physical-count change shifts
            // every mixed row's virtual id even when the rows are structurally
            // identical (operator== ignores display_color), so run the remap
            // whenever the count changes, not only when the row list differs.
            if (current_count != target_count ||
                old_mixed_snapshot != pb->mixed_filaments.mixed_filaments()) {
                pb->update_mixed_filament_id_remap(
                    old_mixed_snapshot, current_count, target_count);
            }

            // Remap config-level extruder references (object/volume/layer) with
            // the remap just built: the triangle remap inside on_filaments_change
            // only touches mmu_segmentation_facets, so without this a config
            // entry on a mixed row would stay on its stale pre-expansion id
            // (e.g. vid 3 → 5 on a 2→4 physical-count expansion). Scoped to this
            // batch-match branch only — other paths that build a remap (row
            // delete/enable, manual add/remove) are intentionally left unchanged.
            // Copy, don't borrow: last_filament_id_remap() returns a reference
            // into PresetBundle's buffer, which on_filaments_change below
            // consumes (moves + clears). A copy keeps this block independent
            // of that consume ordering.
            const std::vector<unsigned int> batch_remap = pb->last_filament_id_remap();
            if (!batch_remap.empty()) {
                const size_t total_filaments = target_count + pb->mixed_filaments.enabled_count();
                // NONE is the default: a stale id NOT in the remap must not
                // stay identity, because after a 2-to-4 renumbering the old
                // mixed id 3 would alias onto physical slot 3 and silently
                // reassign the object.  The loop overlays the actual remap on
                // top of the NONE-filled baseline.
                EnforcerBlockerStateMap batch_state_map;
                batch_state_map.fill(EnforcerBlockerType::NONE);
                for (size_t i = 1; i < batch_state_map.size(); ++i) {
                    const unsigned int mapped = i < batch_remap.size() ? batch_remap[i] : 0;
                    if (mapped == 0 || mapped >= batch_state_map.size() || mapped > total_filaments)
                        continue;  // stays NONE: deleted/expired row, config falls back to default
                    batch_state_map[i] = EnforcerBlockerType(mapped);
                }
                auto remap_config_extruder = [&batch_state_map](ModelConfig& cfg) {
                    if (!cfg.has("extruder"))
                        return;
                    const int eid = cfg.extruder();
                    if (eid <= 0 || static_cast<size_t>(eid) >= batch_state_map.size())
                        return;
                    const unsigned int mapped = static_cast<unsigned int>(batch_state_map[static_cast<size_t>(eid)]);
                    if (mapped == 0) {
                        // Deleted/expired row: revert to default.  Set the key to
                        // 0 rather than erasing it — a missing "extruder" would
                        // make cfg.extruder() (opt_int, nullptr on absent key) a
                        // dangling dereference for any unprotected reader, and an
                        // explicit 0 matches the out-of-range normalization in
                        // update_filament_values_for_items (GUI_ObjectList.cpp).
                        // Objects/volumes with extruder 0 resolve to "default"
                        // via ModelVolume::extruder_id()'s inherit-from-object
                        // fallback, so inheriting children stay "default" too.
                        cfg.set("extruder", 0);
                    } else if (mapped != static_cast<unsigned int>(eid)) {
                        cfg.set_key_value("extruder", new ConfigOptionInt(static_cast<int>(mapped)));
                    }
                };
                for (ModelObject* mo : wxGetApp().model().objects) {
                    remap_config_extruder(mo->config);
                    for (ModelVolume* mv : mo->volumes)
                        remap_config_extruder(mv->config);
                    for (auto& lr : mo->layer_config_ranges)
                        remap_config_extruder(lr.second);
                }
            }

            // Write a Full Spectrum preset into each of slots 1-4, per the FAMILY the
            // user selected in that slot's palette dropdown (phase 2 multi-family:
            // e.g. PLA in slots 1-2, PETG in 3-4). When a slot's family has no
            // selectable preset, the slot KEEPS its current preset — per spec §5.1
            // ("Configured filaments will be used instead", the same promise the
            // Confirm-time note in MixedFilamentBatchDialog makes). The
            // default-family single preset is only used for legacy results that
            // carry no per-slot family info (pre-phase-2 behavior).
            for (size_t i = 0; i < std::min<size_t>(static_cast<size_t>(kFullSpectrumSlotCount), target_count); ++i) {
                std::string preset_name;
                if (i < result.recommended_physical_family_names.size()) {
                    preset_name = find_selectable_full_spectrum_family_preset(result.recommended_physical_family_names[i]);
                    // Family not selectable: leave empty on purpose — the slot keeps
                    // the user's configured preset (§5.1). Do NOT substitute the
                    // default family here: the user explicitly chose this family.
                } else {
                    // Legacy result without per-slot family info: the pre-phase-2
                    // single default-family preset, all-or-nothing per slot.
                    const Preset* fs_preset = pb->filaments.find_preset(full_spectrum_preset_name());
                    if (fs_preset != nullptr && fs_preset->is_visible && fs_preset->is_compatible)
                        preset_name = fs_preset->name;
                }
                if (!preset_name.empty())
                    pb->set_filament_preset(i, preset_name);
            }

            wxGetApp().plater()->on_filaments_change(static_cast<int>(target_count));
        } else {
            ConfigOptionStrings* co = wxGetApp().preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
            colors_vec = co ? co->values : std::vector<std::string>{};
        }

        BatchMatchResult model_result = result; // mutable copy — used for in-place edits + apply/cleanup

        // In-place recipe update for EXISTING custom mixed filaments: overwrite
        // the row's recipe with the new-palette match (mapping.recipe is already
        // the best new-palette approximation of that model color), keeping
        // stable_id / virtual id / painting.  This avoids creating a duplicate
        // new mixed for colors the user already has and — because the virtual id
        // and painting are untouched — sidesteps the cascade/erase that would
        // otherwise lose painting when a component physical is dropped.  Rows
        // are matched by IDENTITY (dialog-time source id -> stable_id -> current
        // row), NOT by display color: recommended mode refreshes display colors
        // before this block runs.  Auto rows never match (their vids resolve to
        // rows with custom == false; auto_generate owns them).
        {
            const size_t cur_num_physical = colors_vec.size();
            bool any_in_place = false;
            for (auto& mapping : model_result.mappings) {
                if (mapping.is_pure_recipe || mapping.in_place_edited) continue;
                const MixedColorMatchRecipeResult& r = mapping.recipe;
                // Only write recipes add_batch would accept: valid, in-range,
                // distinct components.  Anything else falls through to the
                // add_batch path, which clamps and validates on its own.
                if (!r.valid) continue;
                if (r.component_a < 1 || r.component_a > cur_num_physical
                    || r.component_b < 1 || r.component_b > cur_num_physical
                    || r.component_a == r.component_b) continue;
                // Resolve the dialog-time source id to an existing row identity.
                uint64_t sid = 0;
                for (unsigned int src : mapping.source_extruder_ids) {
                    auto it = dialog_vid_to_sid.find(src);
                    if (it != dialog_vid_to_sid.end() && it->second != 0) {
                        sid = it->second;
                        break;
                    }
                }
                if (sid == 0) continue; // no existing mixed row behind this color
                // Locate that row in the CURRENT mixed list; record its vid.
                MixedFilament* target     = nullptr;
                unsigned int   target_vid = 0;
                unsigned int   vid        = static_cast<unsigned int>(cur_num_physical) + 1;
                for (MixedFilament& mf : mgr.mixed_filaments()) {
                    if (!mf.enabled || mf.deleted) continue;
                    if (mf.stable_id == sid) { target = &mf; target_vid = vid; break; }
                    ++vid;
                }
                // Only custom rows can be rewritten in place.  Auto rows are
                // owned by auto_generate; in this project auto mixed filaments
                // are off by default and not painted, so they never reach here.
                // If auto painting is ever enabled, revisit the R3 invariant
                // documented in cleanup_unused_filaments_after_batch_match.
                if (target == nullptr || !target->custom) continue;
                target->component_a                = r.component_a;
                target->component_b                = r.component_b;
                target->mix_b_percent              = r.mix_b_percent;
                target->gradient_component_ids     = r.gradient_component_ids;
                target->gradient_component_weights = r.gradient_component_weights;
                target->distribution_mode          = r.gradient_component_ids.empty()
                    ? int(MixedFilament::Simple) : int(MixedFilament::LayerCycle);
                target->gradient_enabled           = !r.gradient_component_ids.empty();
                target->manual_pattern.clear();   // MATCH recipes carry no manual pattern
                target->ratio_a = 1;              // MATCH recipes are 1:1 (equal-ratio blend)
                target->ratio_b = 1;
                target->ui_mode = 2;              // MATCH
                // Keep the mapping in the list, flagged: batch_entries and the
                // assigned-id fixup skip it, apply no-ops on it (source ==
                // target after translation below), and extract_batch_kept_sets
                // keeps the row via target_filament_id — so the edited row is
                // not deleted by the cleanup, regardless of whether it is painted.
                mapping.in_place_edited    = true;
                mapping.target_filament_id = target_vid;
                any_in_place = true;
            }
            // Refresh swatches so they reflect the new recipes even when the
            // batch below ends up empty (add_batch skips refresh in that case).
            if (any_in_place)
                mgr.refresh_display_colors(colors_vec);
        }

        // Translate mixed-slot source ids from the dialog epoch to the CURRENT
        // epoch.
        {
            const unsigned int cur_num_physical = static_cast<unsigned int>(colors_vec.size());
            for (auto& mapping : model_result.mappings) {
                std::vector<unsigned int> translated;
                translated.reserve(mapping.source_extruder_ids.size());
                for (unsigned int src : mapping.source_extruder_ids) {
                    if (src <= static_cast<unsigned int>(dialog_num_physical)) {
                        translated.push_back(src); // physical slot: identity
                        continue;
                    }
                    auto it = dialog_vid_to_sid.find(src);
                    if (it == dialog_vid_to_sid.end() || it->second == 0) continue;
                    unsigned int cur_vid = 0;
                    unsigned int vid     = cur_num_physical + 1;
                    for (const MixedFilament& mf : mgr.mixed_filaments()) {
                        if (!mf.enabled || mf.deleted) continue;
                        if (mf.stable_id == it->second) { cur_vid = vid; break; }
                        ++vid;
                    }
                    if (cur_vid != 0) translated.push_back(cur_vid);
                }
                mapping.source_extruder_ids = std::move(translated);
            }
        }

        std::vector<MixedFilamentBatchEntry> batch_entries;
        batch_entries.reserve(model_result.mappings.size());
        for (const auto& mapping : model_result.mappings) {
            // Skip condition MUST stay identical to the assigned-id fixup below —
            // `k` aligns mappings to assigned_ids by construction order.
            if (mapping.is_pure_recipe || mapping.in_place_edited) continue;
            MixedFilamentBatchEntry entry;
            entry.component_a     = mapping.recipe.component_a;
            entry.component_b     = mapping.recipe.component_b;
            entry.mix_b_percent   = mapping.recipe.mix_b_percent;
            entry.manual_pattern  = mapping.recipe.manual_pattern;
            entry.gradient_component_ids     = mapping.recipe.gradient_component_ids;
            entry.gradient_component_weights = mapping.recipe.gradient_component_weights;
            entry.distribution_mode = mapping.recipe.gradient_component_ids.empty()
                ? int(MixedFilament::Simple) : int(MixedFilament::LayerCycle);
            entry.display_color = mapping.matched_color.GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
            batch_entries.push_back(std::move(entry));
        }
        std::vector<unsigned int> assigned_ids;
        set_progress(3);
        mgr.add_batch_custom_filaments(batch_entries, colors_vec, &assigned_ids);

        // Replace dialog-computed target ids with the actual virtual ids assigned by
        // add_batch_custom_filaments.
        {
            size_t k = 0;
            size_t dropped = 0;
            for (auto& mapping : model_result.mappings) {
                // MUST mirror the batch_entries skip condition above (k-alignment).
                if (mapping.is_pure_recipe || mapping.in_place_edited) continue;
                const unsigned int vid = (k < assigned_ids.size()) ? assigned_ids[k] : 0u;
                ++k;
                if (vid != 0u) {
                    mapping.target_filament_id = vid;
                } else {
                    mapping.target_filament_id = 0;
                    mapping.source_extruder_ids.clear();
                    ++dropped;
                }
            }
            if (dropped > 0)
                BOOST_LOG_TRIVIAL(warning)
                    << "Batch match: " << dropped << " recipe(s) dropped (cap "
                    << "or invalid components); regions left on original filament.";
        }
        // Apply matched recipes to model painting data
        set_progress(5);
        apply_batch_match_to_model(model_result);

        // Remove physical/mixed filaments left unreferenced by the match. This is
        // ~96% of apply time (per-deletion combo rebuild). Map the deletion loop's
        // (current, total) onto 5..95 so the bar advances steadily and reflects
        // real remaining work, not just a spinner.
        cleanup_unused_filaments_after_batch_match(
            model_result,
            [&set_progress](int current, int total) {
                // Map current/total ∈ [1,total] to 5..95. total==0 can't happen
                // here (the loop only runs when redundant_physical is non-empty),
                // but guard anyway to avoid divide-by-zero if the contract changes.
                const int span = (total > 0) ? (90 * current / total) : 0;
                set_progress(5 + span);
            });

        // The painting gizmo keeps an in-memory editing copy of the painting
        // (m_triangle_selectors) and data_changed() only reloads it when the
        // extruder count changes; a same-count palette rewrite leaves it stale.
        // The match just rewrote mmu_segmentation_facets in the model, so force a
        // re-deserialize on every canvas where the gizmo is active (each canvas
        // owns its own gizmo manager). A stale copy would keep rendering the old
        // mapping and its next update_model_object() would write that stale copy
        // back over the applied match.
        {
            Plater* batch_plater = wxGetApp().plater();
            for (GLCanvas3D* cnv : { batch_plater->get_view3D_canvas3D(), batch_plater->get_assmeble_canvas3D() }) {
                if (cnv == nullptr) continue;
                GLGizmosManager& gizmos_mgr = cnv->get_gizmos_manager();
                if (gizmos_mgr.get_current_type() != GLGizmosManager::EType::MmSegmentation) continue;
                if (auto* mmu_gizmo = dynamic_cast<GLGizmoMmuSegmentation*>(gizmos_mgr.get_gizmo(GLGizmosManager::EType::MmSegmentation)))
                    mmu_gizmo->refresh_from_model();
            }
        }

        // cleanup already serializes; only panel refresh needed.
        set_progress(95);
        m_sidebar.update_mixed_filament_panel(false);
        m_sidebar.update_ui_from_settings();
        m_sidebar.update_dynamic_filament_list();
        // Refresh the object list filament column so every row shows the
        // post-remap extruder ID.  Without this the list still displays the
        // old physical-slot IDs because the earlier on_filaments_change
        // refresh ran BEFORE apply_batch_match_to_model.
        m_sidebar.obj_list()->update_objects_list_filament_column(colors_vec.size());
        // Force-refresh combo swatches in case filament count stayed the same
        // (Sidebar::on_filaments_change early-returns in that case).
        std::vector<PlaterPresetComboBox*>& fcombos = m_sidebar.combos_filament();
        for (size_t i = 0; i < fcombos.size(); ++i) {
            if (fcombos[i]) fcombos[i]->update();
        }
        // No undo snapshot in this apply path: mark dirty so close-without-save prompts
        // instead of silently dropping the match result.
        wxGetApp().plater()->update_project_dirty_from_presets();
        // §70: wxPD_AUTO_HIDE only fires at 100%, so reach 100 here for a clean
        // dismiss (otherwise the bar visibly aborts when the dialog leaves scope).
        set_progress(100);
}

void BatchMatchCoordinator::cleanup_unused_filaments_after_batch_match(const BatchMatchResult &match_result,
                                                                      std::function<void(int, int)> on_progress)
{
    PresetBundle *pb = wxGetApp().preset_bundle;
    if (pb == nullptr || pb->filament_presets.empty()) return;

    const size_t num_physical = pb->filament_presets.size();
    std::vector<unsigned int> kept_physical, kept_mixed;
    extract_batch_kept_sets(match_result, num_physical, kept_physical, kept_mixed);

    // R3 guard: force-keep mixed rows still painted on the model so
    // compute_redundant_filaments does not flag them via !in_kept_set; otherwise
    // the row is deleted and its former virtual id re-aliases to the next
    // survivor → silent wrong color.
    // LIMITATION: does NOT save a painted row whose components reference a
    // deleted physical — cascade still flags it and remove_physical_filament
    // erases it unconditionally; its stranded painting then resolves via
    // build_filament_id_remap's pair-fallback (possibly a wrong survivor).
    for (const ModelObject *mo : wxGetApp().model().objects) {
        for (const ModelVolume *mv : mo->volumes) {
            if (mv->type() != ModelVolumeType::MODEL_PART) continue;
            for (int eid : mv->get_extruders()) {
                if (eid > static_cast<int>(num_physical))
                    kept_mixed.push_back(static_cast<unsigned int>(eid));
            }
        }
        // R3b: config-level references pin a mixed row just like painting does.
        // The object-list filament column writes object/volume/layer "extruder",
        // which get_extruders() (triangle data) never sees — the gap the guard
        // comment above acknowledges. Without this, a row referenced only from
        // config is flagged redundant and deleted, leaving the config on a stale
        // virtual id that re-aliases to a renumbered survivor (silent wrong
        // colour).
        auto guard_config_ref = [&kept_mixed, num_physical](const ModelConfig &cfg) {
            if (cfg.has("extruder") && cfg.extruder() > static_cast<int>(num_physical))
                kept_mixed.push_back(static_cast<unsigned int>(cfg.extruder()));
        };
        guard_config_ref(mo->config);
        for (const ModelVolume *mv : mo->volumes)
            guard_config_ref(mv->config);
        for (const auto &lr : mo->layer_config_ranges)
            guard_config_ref(lr.second);
    }

    RedundantFilamentSet red = compute_redundant_filaments(
        num_physical, kept_physical, kept_mixed,
        pb->mixed_filaments.mixed_filaments());

    if (red.redundant_physical.empty() && red.redundant_mixed.empty()) {
        // No deletions needed, but add_batch_custom_filaments only calls
        // refresh_display_colors — persist the new mixed definitions here.
        if (auto *opt = pb->project_config.option<ConfigOptionString>("mixed_filament_definitions"))
            opt->value = pb->mixed_filaments.serialize_custom_entries();
        return;
    }

    // Capture the stable_id of each redundant mixed row BEFORE any deletion.
    // Virtual ids and indices are valid now, but each delete_filament below
    // rebuilds m_mixed (remove_physical_filament erases cascade rows and
    // renumbers survivors), so cached *indices* would go stale and mark the
    // wrong rows (or run past the new vector end).  stable_id survives that
    // rebuild — it is the identity key build_filament_id_remap relies on —
    // so we re-match by stable_id against the post-deletion m_mixed below.
    auto &mfs = pb->mixed_filaments.mixed_filaments();
    std::vector<uint64_t> redundant_mixed_stable_ids;
    redundant_mixed_stable_ids.reserve(red.redundant_mixed.size());
    for (unsigned int redundant_id : red.redundant_mixed) {
        int idx = pb->mixed_filaments.mixed_index_from_filament_id(redundant_id, num_physical);
        if (idx >= 0 && static_cast<size_t>(idx) < mfs.size()) {
            const uint64_t sid = mfs[idx].stable_id;
            if (sid != 0) {
                redundant_mixed_stable_ids.push_back(sid);
            } else {
                // Every live row is allocated a stable_id; 0 means a row
                // bypassed allocation and cannot be tracked through deletion.
                BOOST_LOG_TRIVIAL(warning)
                    << "Batch match cleanup: redundant mixed row " << redundant_id
                    << " has stable_id 0; it will not be removed.";
            }
        }
    }

    // Delete physical filaments FIRST.  This triggers on_filaments_delete →
    // build_filament_id_remap while all mixed entries are still enabled, so
    // the remap table covers the full pre-cleanup virtual-ID space and
    // surviving mixed entries' model painting data is correctly remapped.
    // Do NOT use merge_mixed_filament — on_filaments_delete would remap
    // model triangles away from the correct ids already set by apply.
    // Cascade rows (components reference a deleted physical) are erased here
    // by remove_physical_filament, so they need no explicit marking below.
    //
    // Batch optimization: skip per-deletion painting remap + panel rebuild +
    // GL render during the loop, then apply ONE composite painting remap and
    // ONE panel rebuild at the end.  This replaces K × TriangleSelector
    // reconstruction + K × panel rebuild + K × render with 1 of each.
    if (!red.redundant_physical.empty()) {
        // Snapshot pre-deletion mixed state for the composite remap.
        const std::vector<MixedFilament> old_mixed_snapshot = pb->mixed_filaments.mixed_filaments();
        const size_t old_num_physical = num_physical;
        const size_t new_num_physical = red.new_num_physical;

        // INVARIANT: red.redundant_physical must be strictly DESCENDING.
        // delete_filament does middle-deletion repacking, so only descending
        // order guarantees the kept head ids (1..new_num_physical) keep their
        // identity — the precondition build_filament_id_remap(..., deleting_
        // filament=false) bakes into the composite remap (tail-truncation).
        // If this ever flips to ascending, painting is silently remapped to
        // wrong colours. Source: compute_redundant_filaments @ MixedFilament.cpp.
        // The check is enforced at RUNTIME (not just assert) because the failure
        // mode is silent wrong-colour mapping and assert() is compiled out under
        // NDEBUG (Release builds). On violation we fall back to per-deletion
        // updates (skip_update=false) so each deletion's own remap keeps colours
        // correct, instead of the batched composite path that depends on the order.
        bool descending_ok = true;
        for (size_t i = 1; i < red.redundant_physical.size(); ++i) {
            if (red.redundant_physical[i] >= red.redundant_physical[i - 1]) {
                BOOST_LOG_TRIVIAL(error)
                    << "redundant_physical descending invariant violated at index " << i
                    << " (" << red.redundant_physical[i] << " >= " << red.redundant_physical[i - 1]
                    << "); falling back to per-deletion update to avoid silent wrong-colour mapping";
                descending_ok = false;
                break;
            }
        }
        assert(descending_ok);

        if (descending_ok) {
            // RAII: bump the single batch-deletion flag for the duration of the
            // loop so on_filaments_delete (Sidebar + Plater) skip per-deletion panel
            // rebuild and painting remap. Exception- and reentrancy-safe.
            Plater *plater = wxGetApp().plater();
            Plater::BatchPhysicalDeletionGuard guard(*plater);
            // Freeze the sidebar for the duration of the deletion loop only. This
            // is the KEY to keeping apply fast (~10.7s) instead of slow (~29.9s):
            // without it, each progress.Update's internal YieldFor(UI) flushes the
            // paints that on_filaments_delete/Layout enqueued, and processing those
            // paints triggers further Layout cascades — a positive feedback loop
            // that inflates per-deletion time ~3.5x (measured 80ms→280ms). Freezing
            // suppresses both the paints and the cascades, so yield finds an empty
            // paint queue and stays cheap, while delete work runs at baseline speed
            // (harness wxwidgets-3-1-5 §132). Scoped to a nested block so the RAII
            // Thaw fires right after the loop — BEFORE the composite remap and panel
            // rebuild below, which need the sidebar unfrozen to repaint the final
            // state. §132: call Layout()+Refresh() after Thaw when children were
            // removed while frozen (we destroyed N combos), otherwise the region
            // repaints blank.
            const int total = static_cast<int>(red.redundant_physical.size());
            {
                wxWindowUpdateLocker freeze_sidebar(&m_sidebar);
                // Report (current, total) per deletion so the caller can drive a
                // real progress bar. Each Update internally YieldFor(UI|USER_INPUT),
                // keeping the window responsive (harness §119/§70) AND advancing
                // the bar. With the sidebar frozen these yields process only the
                // progress dialog's own paint (parented to MainFrame, outside this
                // frozen subtree), so the bar animates while the sidebar stays
                // visually frozen until the Thaw just below.
                for (int i = 0; i < total; ++i) {
                    m_sidebar.delete_filament(red.redundant_physical[i] - 1, -1,
                                    /*skip_dependency_check=*/true,
                                    /*skip_update=*/true);
                    if (on_progress) on_progress(i + 1, total);
                }
            } // freeze_sidebar Thaws here
            // §132: children (combos) were destroyed while frozen — re-layout then
            // force a repaint so the final state fills in instead of leaving the
            // region blank until the next paint event.
            m_sidebar.Layout();
            m_sidebar.Refresh();

            // ONE composite painting remap: build old→new from the snapshot, apply
            // to every volume.  Equivalent to K sequential single-deletion remaps
            // (remap is a pure pointwise state_map lookup, composable).
            // Pass kept_physical so the batch remap maps physical ids by their
            // position in the surviving set (kept-aware), not by the
            // tail-truncation assumption (survivors == {1..new_num}). The
            // assumption only holds when the palette is head-rewritten
            // (recommended mode); for non-contiguous manual selections like
            // [2,6,8,10] tail-truncation maps survivors to NONE and loses
            // painting. kept-aware is a no-op when kept == {1..new_num}.
            pb->update_mixed_filament_id_remap(old_mixed_snapshot, old_num_physical, new_num_physical,
                                                size_t(-1), kept_physical);
            const std::vector<unsigned int> composite_remap = pb->consume_last_filament_id_remap();
            if (!composite_remap.empty()) {
                EnforcerBlockerStateMap state_map;
                for (size_t i = 0; i < state_map.size(); ++i)
                    state_map[i] = EnforcerBlockerType(i);
                const size_t total_filaments = new_num_physical + pb->mixed_filaments.enabled_count();
                for (size_t i = 1; i < composite_remap.size() && i < state_map.size(); ++i) {
                    const unsigned int mapped = composite_remap[i];
                    if (mapped == 0 || mapped >= state_map.size() || mapped > total_filaments)
                        state_map[i] = EnforcerBlockerType::NONE;
                    else
                        state_map[i] = EnforcerBlockerType(mapped);
                }
                for (ModelObject* mo : wxGetApp().model().objects)
                    for (ModelVolume* mv : mo->volumes)
                        if (mv->type() == ModelVolumeType::MODEL_PART)
                            // Pass total_filaments (physical + mixed), NOT new_num_physical —
                            // remap_enforcer_block_types clips any state > max_type to NONE,
                            // and remapped mixed virtual ids (N+1..total) must survive.
                            mv->remap_extruder_ids(total_filaments, state_map);
            }
        } else {
            // Safe fallback: delete one by one with full per-deletion update so each
            // deletion's own painting remap runs. Slower but colour-correct.
            for (unsigned int redundant_id : red.redundant_physical)
                m_sidebar.delete_filament(redundant_id - 1, -1,
                                /*skip_dependency_check=*/true,
                                /*skip_update=*/false);
        }
    }

    // Deleting mixed rows renumbers every higher survivor down by one (virtual
    // ids enumerate over *enabled* rows). Painted facets still hold the old ids,
    // so without this remap they resolve to the wrong row — the merged-slot
    // regression. Computed in the T2 epoch (rows still enabled), applied after
    // the mark loop in T3.
    std::vector<unsigned int> mixed_deletion_remap;
    if (!redundant_mixed_stable_ids.empty()) {
        const size_t cur_num_physical = pb->filament_presets.size();
        const std::unordered_set<uint64_t> to_delete(redundant_mixed_stable_ids.begin(),
                                                      redundant_mixed_stable_ids.end());
        // Enumerate enabled rows in order to find each redundant row's T2 vid.
        std::vector<unsigned int> deleted_t2_vids;
        unsigned int vid = static_cast<unsigned int>(cur_num_physical) + 1;
        for (const MixedFilament& mf : mfs) {
            if (!mf.enabled || mf.deleted) continue;
            if (mf.stable_id != 0 && to_delete.count(mf.stable_id) > 0)
                deleted_t2_vids.push_back(vid);
            ++vid;
        }
        const size_t t2_total = cur_num_physical + pb->mixed_filaments.enabled_count();
        mixed_deletion_remap = MixedFilamentManager::build_mixed_deletion_painting_remap(
            cur_num_physical, t2_total, deleted_t2_vids);
    }

    // Mark the redundant mixed rows that survived the physical deletions
    // (the non-cascade ones).  Match by stable_id against the CURRENT m_mixed
    // — `mfs` still refers to the same (now-rebuilt) vector object.
    //
    // DESIGN INVARIANT: every row that reaches here is guaranteed to carry NO
    // painted model region, so deleting it cannot strand painting.  This holds
    // under two rules the confirm flow enforces upstream:
    //   (1) mixed-color sources are rewritten in place (in_place_edited): the
    //       row keeps its virtual id, painting is untouched, and the row is
    //       kept (not redundant) — it never reaches this loop;
    //   (2) physical-color sources get a NEW mixed row + an apply() remap that
    //       moves their painting onto it BEFORE cleanup runs, so by the time
    //       the source physical is deleted it has no painting left to strand.
    // The R3 guard above (get_extruders scan) force-keeps a painted row that no
    // mapping's source matched.  That keep is not absolute: a cascade row
    // (components reference a deleted physical) is still erased by
    // remove_physical_filament — see the LIMITATION note on the R3 guard above.
    //
    // Removing the R3 guard, or enabling auto mixed rows (auto_generate) and
    // painting them, would broaden this invariant breach → a deleted row's
    // virtual id re-aliases to the next survivor → silent wrong colour (see
    // test "resolve: deleted middle mixed's virtual id re-aliases to a
    // survivor").
    const std::unordered_set<uint64_t> to_mark(redundant_mixed_stable_ids.begin(),
                                               redundant_mixed_stable_ids.end());
    for (auto &mf : mfs) {
        if (mf.stable_id != 0 && to_mark.count(mf.stable_id) > 0) {
            mf.deleted = true;
            mf.enabled = false;
        }
    }

    // Apply the mixed-deletion remap now that the rows are marked (T3 epoch).
    // Mirrors the physical-deletion composite remap above.
    if (!mixed_deletion_remap.empty()) {
        EnforcerBlockerStateMap state_map;
        for (size_t i = 0; i < state_map.size(); ++i)
            state_map[i] = EnforcerBlockerType(i);
        const size_t t3_total = pb->filament_presets.size() + pb->mixed_filaments.enabled_count();
        for (size_t i = 1; i < mixed_deletion_remap.size() && i < state_map.size(); ++i) {
            const unsigned int mapped = mixed_deletion_remap[i];
            if (mapped == 0 || mapped >= state_map.size() || mapped > t3_total)
                state_map[i] = EnforcerBlockerType::NONE;
            else
                state_map[i] = EnforcerBlockerType(mapped);
        }
        for (ModelObject* mo : wxGetApp().model().objects)
            for (ModelVolume* mv : mo->volumes)
                if (mv->type() == ModelVolumeType::MODEL_PART)
                    mv->remap_extruder_ids(t3_total, state_map);

        // Config "extruder" references (object/volume/layer) must follow the same
        // remap as painting — remap_extruder_ids above touches triangle data only,
        // so a config entry on a mixed row would stay on its stale pre-deletion id
        // (the config side of the merged-slot regression). In the table, physical
        // ids are identity, a deleted row maps to 0 (default), a survivor keeps
        // its shifted id, and out-of-range/absent keys are left untouched. EPOCH
        // NOTE: config values are naive-decremented per physical deletion while
        // the table is cascade-aware (T2) — consistent only with no cascade removal
        // (the batch-match flow's case); if a cascade ever fires, skip via
        // cascade_mixed_count == 0 or normalise first.
        auto remap_config_extruder = [&mixed_deletion_remap](ModelConfig &cfg) {
            if (!cfg.has("extruder")) return;
            const int old = cfg.extruder();
            if (old <= 0) return;
            const size_t idx = static_cast<size_t>(old);
            if (idx >= mixed_deletion_remap.size()) return;
            const unsigned int mapped = mixed_deletion_remap[idx];
            if (mapped == 0) {
                // Deleted row: revert to default.  Set the key to 0 rather than
                // erasing it — a missing "extruder" would make cfg.extruder()
                // (opt_int, nullptr on absent key) a dangling dereference for
                // any unprotected reader (GUI_ObjectList's delete path writes 0
                // to layer ranges too).  Objects/volumes with extruder 0 resolve
                // to "default" via ModelVolume::extruder_id()'s inherit-from-
                // object fallback, so inheriting children stay "default".
                cfg.set("extruder", 0);
            } else if (mapped != static_cast<unsigned int>(old)) {
                cfg.set_key_value("extruder", new ConfigOptionInt(static_cast<int>(mapped)));
            }
        };
        for (ModelObject* mo : wxGetApp().model().objects) {
            remap_config_extruder(mo->config);
            for (ModelVolume* mv : mo->volumes)
                remap_config_extruder(mv->config);
            for (auto &lr : mo->layer_config_ranges)
                remap_config_extruder(lr.second);
        }
    }

    // Final authoritative serialization of the post-cleanup state (it must
    // include the `deleted` markings above).  Each delete_filament call above
    // also serialized once inside update_multi_material_filament_presets — that
    // per-iteration write is the carrier of the physical-deletion round-trip
    // and is NOT skippable.
    if (auto *opt = pb->project_config.option<ConfigOptionString>("mixed_filament_definitions"))
        opt->value = pb->mixed_filaments.serialize_custom_entries();

    // Rebuild panels once (skipped per-deletion in the loop above). The object-list
    // refresh performs the single final Plater update through its model sync path.
    m_sidebar.update_mixed_filament_panel();
    m_sidebar.update_color_mix_panel();
    m_sidebar.obj_list()->update_objects_list_filament_column(pb->filament_presets.size());
    m_sidebar.obj_list()->refresh_layer_range_filament_items();
}

} // namespace app
} // namespace GUI
} // namespace Slic3r
