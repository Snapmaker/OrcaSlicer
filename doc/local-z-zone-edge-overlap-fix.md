# Local-Z: stopping an independent row from printing into a neighbouring colour zone

## Mixed filament phase 4

The implementation lives in `src/libslic3r/LocalZ/LocalZPlanner.cpp` and `LocalZPlan.hpp`. Paint segmentation supplies the physical filament for each nominal island, including islands that begin their filament sequence partway up the object. The planner aligns the incoming masks with repaired layers before using them. Perimeter generation looks up each interval by its raft-aware layer ID, and G-code emission uses the phase 4 clipper and seam placer.

The six regression scenarios are in `tests/libslic3r/test_local_z_plan.cpp`. Run `libslic3r_tests "[LocalZ][Gradient]"` after building the Release test target. The fixture uses the nozzle's minimum layer height.

The two regression cases passed all six sections and 509 assertions in the Windows Release build. The full `libslic3r_tests` run had 463 passes, one skip and three expected failures.

For the reference-model check, the CLI sliced scratch copies of `Cubeedge.3mf` at a nominal layer height of 0.16 mm. The four exports combined 0.20 and 0.30 mm gradient cycles with mix subdivision enabled and disabled. Collinear, axis-aligned outer-wall segments on different print planes had no overlaps exceeding 0.01 mm in both length and depth in any export.

## Original porting notes

The following describes the source branch, including its paths, configuration names, and reference measurements. The accompanying patch is part of the original archive.

- **Developed on:** SnapmakerOrca-FullSpectrum, branch `main` at `cc152a84dd`, as an uncommitted working-tree change.
- **Exact code:** `local-z-zone-edge-overlap-fix.patch` (output of `git diff`, 7 files, about 1000 added lines of which 400 are tests).
- **This document:** what the change does, why it is built this way, and everything needed to re-implement it by hand where the patch does not apply.

Try the patch first:

```sh
git apply --3way --check local-z-zone-edge-overlap-fix.patch
git apply --3way local-z-zone-edge-overlap-fix.patch
```

The patch was made with `core.autocrlf=true`, so it contains LF line endings. The source files in the working tree are CRLF; keep them that way when editing by hand.

---

## 1. Terms

| Term | Meaning |
|---|---|
| Nominal layer | An ordinary layer of the object, `Layer`, height `layer_height`. |
| Row | A mixed-filament definition that Local-Z prints as its own stack of thin passes. Gradients are always rows. Other mixes are rows only with `dithering_local_z_mode` on. |
| Pass | One `SubLayerPlan`: a print plane (`print_z`), a height (`flow_height`), and per-extruder masks. |
| Independent row | A row whose pass heights do not add up to the nominal layer height (`dithering_local_z_independent_layer_height`, or a gradient with its own `dithering_local_z_gradient_layer_height`). It keeps a Z cursor across layers. |
| Cursor | Top of the last pass a row has planned (`IndependentCadenceState::z_cursor`). |
| Lag | How far the cursor is below the bottom of the layer being planned. |
| Zone | Anything a row can sit next to: another row, or a painted area printed at the nominal layer height (a solid colour, or a mix that is not subdivided). The second kind is called a nominal zone here. |
| Base | The ordinary, non-Local-Z extrusions of a layer. Nominal zones are printed as base. |
| Tip | The part of a zone's layer that the row covers on the next layer, where the zone's edge steps back. |

---

## 2. How Local-Z works, as far as this change depends on it

### Planner (`build_local_z_plan()` in `src/libslic3r/PrintObjectSlice.cpp`)

For every nominal layer it creates one `LocalZInterval` and a contiguous range of `SubLayerPlan`s (`first_sublayer_idx`, `sublayer_count`).

In an *independent interval* every active row runs this loop:

```cpp
while (true) {
    const double next_z = state.z_cursor + cadence_pass->height;
    if (next_z > interval.z_hi + EPSILON)
        break;
    append_independent_pass(...);   // mask = row_state_masks[row_idx] of THIS layer
    state.z_cursor = next_z;
    ...
}
```

Two consequences:

1. A pass belongs to the layer that contains its **top**. Its bottom may lie in the layer below.
2. When the next pass does not fit, the cursor stays below the layer top. On the next layer the row resumes below that layer's bottom: it lags.

A pass always uses the row's mask of the layer it is planned on.

Passes carry `dependency_group == row index + 1`. Fixed-colour passes and the cut passes added by this change use `mixed_definitions.size() + 1`.

Nominal zones are **not** in the planner. They come from the multi-material painting (`mm_segmentation`) and are printed as base. A non-subdivided mix alternates its physical filament by layer through `MixedFilamentManager::effective_painted_region_filament_id()`.

### G-code stage (`GCode::process_layer()` in `src/libslic3r/GCode.cpp`)

- Every pass prints the layer's **own perimeters** (`LayerRegion::perimeters`), clipped by the pass mask grown by `LOCAL_Z_PERIMETER_MASK_EXPAND_MM` (0.10 mm), at the pass's `print_z` and `flow_height`.
- The base prints everything outside `interval.managed_masks ∪ all pass masks`, grown by `LOCAL_Z_BASE_MASK_EXPAND_MM` (0.04 mm).
- All passes of a layer are printed before its base.
- **All passes of a layer share one set of perimeters.** Before this change there was no way for one pass to have a different outline than the others.
- Infill is only split by Local-Z when `dithering_local_z_infill` is on (the GUI turns it on together with layer subdivision).

---

## 3. The defect

A gradient with 0.14 + 0.06 mm passes over 0.16 mm layers shares no print planes with the layers around it. Side view of one step of a slanted zone edge, zone on the right:

```
            gradient                  zone
 layer N    ┌──────────────────────┐  ┌────────────
 pass P1    │ c .. c+h  (mask of N)│  │ layer N
            │              ████████│  └────────────
 ───────────┤              ████████┌──────────────── T = top of layer N-1
 layer N-1  │ passes of N-1│  tip  │ layer N-1
            └──────────────┴───────┴────────────────
                           B(N-1)  B(N)
```

`P1` is the first pass planned on layer N. It starts at the cursor `c`, below `T`, and uses the row mask of layer N, which reaches to `B(N)`. Between `B(N-1)` and `B(N)` the zone's layer N-1 already fills up to `T`. `P1` is printed into it by `T - c`, up to a full nominal layer.

Two situations have this problem:

1. **The zone ends.** The whole island is gone on layer N, and the row covers all of it.
2. **The zone continues and its edge steps back** (a slanted edge). The row covers a strip one step wide on every layer.

Where a zone *grows* instead, the row's passes of layer N-1 end at the old edge and the zone's layer N reaches over them. That leaves a gap of the lag height and no overlap. It was already like that, looks like an ordinary seam, and is not changed.

---

## 4. What the change does

**Rule A, the zone ends:** the zone's last layer is cut down, as a whole island, to where the row resumes.

- What lies above the cursor is not printed.
- The pass or nominal layer that straddles the cursor is printed as a shorter pass `[bottom, cursor]`. A nominal layer becomes a pass of the physical filament that layer would have used.
- If less than half the minimum sublayer height would remain, nothing is printed.
- The row's pass keeps its height and shape.

**Rule B, the zone continues:** the zone is left exactly as painted, and nothing changes height. The one pass that starts below the top of the tip stops short of the tip.

- That pass is planned without the tip. It ends at `B(N-1)`, where the row's passes of the layer below end too.
- Its walls are generated a **second time**, for the row's region without the tip, so the pass turns inwards and ends in an ordinary seam. They are not clipped.
- The next pass of the row starts above `T` and covers the tip. A gap of `c + h - T` remains between the tip and that pass, one step long. This mirrors the joint at a growing edge.

Only the first pass a row prints after resuming can start below a tip, so at most one pass per row and layer is affected.

**Left as before:** strips narrower than 0.2 mm, adaptive (non-planar) rows, unpainted regions, and infill unless infill subdivision is on.

---

## 5. Data model

`src/libslic3r/Print.hpp`, in `struct SubLayerPlan`:

```cpp
    // Areas of the row's region that this pass leaves out, because it would be printed into what is there already
    // (the tip of a colour zone at a step of its edge). They are not part of the masks above. The walls of this
    // pass come from LayerRegion::local_z_alt_perimeters, which are generated for the region without these areas,
    // so that the pass ends in an ordinary seam where it stops short.
    ExPolygons              held_back_masks;
```

`src/libslic3r/Layer.hpp`, in `class LayerRegion`, after `perimeters`:

```cpp
    // The perimeters of this region without the areas that Local-Z passes hold back on this layer
    // (SubLayerPlan::held_back_masks), for the passes that hold them back. Same structure as perimeters.
    // Empty when nothing is held back in this region.
    ExtrusionEntityCollection   local_z_alt_perimeters;
```

Both headers are included widely; expect a near-full rebuild.

---

## 6. Planner changes (`PrintObjectSlice.cpp`)

### 6.1 Give the planner the painted segmentation

`build_local_z_plan()` gets one more parameter, after `segmentation`:

```cpp
                               // The painted segmentation as it will be applied to the layers, or empty when nothing is painted.
                               const std::vector<std::vector<ExPolygons>> &paint_segmentation,
```

Call sites in `PrintObject::slice_volumes()`:

- the painted path passes `mm_segmentation` (after `apply_mixed_surface_indentation()` and `apply_mixed_component_surface_offsets()`, before `apply_mm_segmentation()`);
- the whole-object-only path passes an empty `std::vector<std::vector<ExPolygons>>()`.

The planner's own `segmentation` argument cannot be used for this: it leaves out physical colours and non-subdivided mixes.

### 6.2 Nominal zones of a layer

A lambda inside `build_local_z_plan()`, defined before the layer loop:

```cpp
    const float nominal_zone_base_layer_height = std::max<float>(0.01f, float(print_object.config().layer_height.value));
    const auto  nominal_zones_on_layer         = [&](const size_t nominal_layer_id) {
        LocalZNominalZones zones;
        const Layer       *nominal_layer = nominal_layer_id < paint_segmentation.size() ? print_object.get_layer(int(nominal_layer_id)) : nullptr;
        if (nominal_layer == nullptr)
            return zones;
        for (size_t channel_idx = 1; channel_idx < paint_segmentation[nominal_layer_id].size(); ++channel_idx) {
            const ExPolygons &channel_masks = paint_segmentation[nominal_layer_id][channel_idx];
            if (channel_masks.empty())
                continue;
            const unsigned int channel_id  = segmentation_channel_filament_id(channel_idx);
            unsigned int       filament_id = channel_id;
            if (mixed_mgr.is_mixed(channel_id, num_physical)) {
                const int mixed_idx = mixed_mgr.mixed_index_from_filament_id(channel_id, num_physical);
                if (mixed_idx < 0 || size_t(mixed_idx) >= mixed_definitions.size() || row_uses_local_z[size_t(mixed_idx)] != 0 ||
                    mixed_definitions[size_t(mixed_idx)].behavior.surface_bias.perimeter_modulation)
                    continue;
                filament_id = mixed_mgr.effective_painted_region_filament_id(channel_id, num_physical, int(nominal_layer_id),
                                                                             float(nominal_layer->print_z), float(nominal_layer->height),
                                                                             float(preferred_a), float(preferred_b),
                                                                             nominal_zone_base_layer_height);
            }
            if (filament_id >= 1 && filament_id <= num_physical)
                zones.push_back({filament_id, channel_masks});
        }
        return zones;
    };
```

Mixes that are rows (`row_uses_local_z`) are skipped here because they are handled through their passes. Mixes with perimeter modulation are skipped as well.

The filament must be resolved exactly as `apply_mm_segmentation()` and the tool ordering resolve it, with the same arguments. If the target branch resolves it differently, copy that call.

### 6.3 Helper types

Placed before `build_local_z_plan()`:

```cpp
// What one independent Local-Z row printed on a layer, for the rows that take its place on the next layer.
struct LocalZIndependentRowFootprint
{
    ExPolygons masks;
    // Top of the row's stack of passes. Not known for adaptive rows.
    double     top { 0.0 };
    bool       top_known { false };
};

// A painted zone that is printed at the nominal layer height on one layer.
struct LocalZNominalZone
{
    // Physical filament it is printed with on that layer, 1 based.
    unsigned int filament { 0 };
    ExPolygons   islands;
};
using LocalZNominalZones = std::vector<LocalZNominalZone>;

constexpr double LOCAL_Z_PASS_MASK_GROWTH_MM = 0.10;   // == LOCAL_Z_PERIMETER_MASK_EXPAND_MM in GCode.cpp
constexpr double LOCAL_Z_BASE_MASK_GROWTH_MM = 0.04;   // == LOCAL_Z_BASE_MASK_EXPAND_MM in GCode.cpp

// The strips that one zone, which continues beside them, gives up to a resuming row: steps of the zone's edge.
struct LocalZZoneSteps
{
    // Top of what the zone printed there on the previous layer.
    double     owner_top { 0. };
    ExPolygons strips;
};

// The tip of a zone that a row has taken over and has not printed a pass over yet. z: the top of the tip.
struct LocalZPendingTip
{
    ExPolygons masks;
    double     z { 0. };
};
```

The two growth constants duplicate values that are local to a function in `GCode.cpp`. If the target branch has different values there, use those.

### 6.4 The taken-over area

The area a lagging row covers on the current layer that it did not own on the previous one, where another zone reaches above the row's cursor:

```cpp
struct LocalZTakenOverArea
{
    // Without the parts narrower than twice LOCAL_Z_PASS_MASK_GROWTH_MM.
    ExPolygons masks;
    // Everything that changes hands, narrow parts included.
    ExPolygons all;
    float      growth { 0.f };

    bool empty() const { return masks.empty(); }

    // The part of one zone island that is taken over.
    // ends: the island ends on this layer, nothing is left of it beside the taken-over area. Otherwise the island
    // continues and gives up a step of its edge.
    ExPolygons under(const ExPolygon &island, bool &ends) const
    {
        const ExPolygons whole{island};
        ExPolygons       taken = intersection_ex(whole, masks);
        ends = false;
        if (taken.empty())
            return taken;
        // Leftovers narrower than LOCAL_Z_PASS_MASK_GROWTH_MM do not count.
        ExPolygons rest = diff_ex(whole, all);
        if (!rest.empty())
            rest = opening_ex(rest, 0.5f * growth);
        ends = rest.empty();
        return taken;
    }
};

template<typename NominalZonesOnLayer>
static LocalZTakenOverArea local_z_area_taken_over_by_resuming_row(const std::vector<LocalZInterval>                &intervals,
                                                                   const size_t                                      row_idx,
                                                                   const ExPolygons                                 &row_masks,
                                                                   const std::vector<LocalZIndependentRowFootprint> &previous_rows,
                                                                   const double                                      row_z_cursor,
                                                                   NominalZonesOnLayer                               nominal_zones_on_layer)
{
    LocalZTakenOverArea taken;
    if (intervals.empty() || row_idx >= previous_rows.size() || row_masks.empty())
        return taken;
    const LocalZInterval &previous_interval = intervals.back();
    if (!previous_interval.independent_layer_height || previous_interval.z_hi <= row_z_cursor + EPSILON)
        return taken;

    // Everything the other rows and the nominal zones printed above the cursor on the previous layer.
    ExPolygons other_zones;
    for (size_t other_idx = 0; other_idx < previous_rows.size(); ++other_idx) {
        const LocalZIndependentRowFootprint &other = previous_rows[other_idx];
        if (other_idx != row_idx && other.top_known && other.top > row_z_cursor + EPSILON)
            append(other_zones, other.masks);
    }
    for (const LocalZNominalZone &nominal : nominal_zones_on_layer(intervals.size() - 1))
        append(other_zones, nominal.islands);
    if (other_zones.empty())
        return taken;

    // The part of it this row covers now and did not own on the previous layer.
    const ExPolygons &own_previous_masks = previous_rows[row_idx].masks;
    taken.all = own_previous_masks.empty() ? row_masks : diff_ex(row_masks, own_previous_masks);
    if (!taken.all.empty())
        taken.all = intersection_ex(other_zones, taken.all);
    taken.growth = float(scale_(LOCAL_Z_PASS_MASK_GROWTH_MM));
    if (!taken.all.empty())
        taken.masks = opening_ex(taken.all, taken.growth);
    return taken;
}
```

`intervals` holds only the layers below the current one at this point (the current interval is pushed at the end of the layer loop), so `intervals.back()` is the previous layer and `intervals.size() - 1` is its index.

`under()` is the one place that decides between Rule A and Rule B. An island **ends** only when nothing at least 0.1 mm wide is left of it outside the taken-over area. An island that keeps even a narrow core continues, and gets Rule B. Do not loosen this: an earlier version called an island ended when less than about 0.4 mm was left, and it cut down zone apexes that still had a layer above them.

### 6.5 Steps of zones that continue

```cpp
static std::vector<LocalZZoneSteps> local_z_zone_steps_under_resuming_row(const LocalZTakenOverArea                        &taken,
                                                                          const size_t                                      row_idx,
                                                                          const std::vector<LocalZIndependentRowFootprint> &previous_rows,
                                                                          const double                                      row_z_cursor,
                                                                          const LocalZNominalZones                         &previous_nominal_zones,
                                                                          const double                                      previous_layer_top)
{
    std::vector<LocalZZoneSteps> steps;
    if (taken.empty())
        return steps;
    const auto collect = [&taken, &steps](const double owner_top, const ExPolygons &islands) {
        LocalZZoneSteps zone_steps;
        zone_steps.owner_top = owner_top;
        for (const ExPolygon &island : islands) {
            bool ends = false;
            if (taken.under(island, ends).empty() || ends)
                continue;
            // All of what changes hands along this island, with the parts narrower than a wall.
            append(zone_steps.strips, intersection_ex(ExPolygons{island}, taken.all));
        }
        if (!zone_steps.strips.empty())
            steps.emplace_back(std::move(zone_steps));
    };
    for (size_t other_idx = 0; other_idx < previous_rows.size(); ++other_idx) {
        const LocalZIndependentRowFootprint &other = previous_rows[other_idx];
        if (other_idx != row_idx && other.top_known && other.top > row_z_cursor + EPSILON)
            collect(other.top, other.masks);
    }
    for (const LocalZNominalZone &nominal : previous_nominal_zones)
        collect(previous_layer_top, nominal.islands);
    return steps;
}
```

The strips use `taken.all`, not `taken.masks`. The held-back area becomes the outline of a region that walls are generated for, so it must not leave slivers along the edge.

### 6.6 Rule A: `cut_local_z_zones_under_resuming_row()`

Signature:

```cpp
template<typename NominalZonesOnLayer>
static void cut_local_z_zones_under_resuming_row(std::vector<SubLayerPlan>                        &plans,
                                                 std::vector<LocalZInterval>                      &intervals,
                                                 const size_t                                      row_idx,
                                                 const ExPolygons                                 &row_masks,
                                                 const std::vector<LocalZIndependentRowFootprint> &previous_rows,
                                                 const double                                      row_z_cursor,
                                                 const double                                      min_pass_height,
                                                 const size_t                                      num_physical,
                                                 NominalZonesOnLayer                               nominal_zones_on_layer);
```

The full body is in the patch. What it does:

1. Compute the taken-over area. Return if it is empty.
2. Walk the layers downwards from the previous one, for as long as `interval.independent_layer_height` holds and `interval.z_hi > row_z_cursor`. With passes taller than a layer the cursor can be more than one layer down.
3. **Passes of other rows** on that layer (`split_interval`, not `adaptive_nonplanar`, `dependency_group` in `1..rows` and not the resuming row's, `print_z > row_z_cursor`, and a non-empty mask): split the mask's islands into those that end and those that stay.
   - If the pass bottom is at least `min_pass_height` below the cursor, add a copy of the pass with `z_hi = print_z = row_z_cursor`, `flow_height = row_z_cursor - bottom`, the ended islands as its mask, and the **same** `dependency_group` and `dependency_order` (passes of one order are chained by `print_z` downstream). Clear `base_masks` and `held_back_masks` on the copy.
   - Keep the remaining islands in the original pass, or erase the pass when none remain.
4. **Nominal zones** of that layer: collect the islands that end.
   - Append them to `interval.managed_masks`, grown by `LOCAL_Z_PASS_MASK_GROWTH_MM - LOCAL_Z_BASE_MASK_GROWTH_MM` (0.06 mm). Without this growth the base gives way 0.04 mm past the mask while the cut pass is clipped 0.10 mm past it, and a wall running along the edge of the island is printed by both.
   - If `row_z_cursor - interval.z_lo >= min_pass_height`, add a pass: `layer_id` of that interval, `split_interval = true`, `z_lo = interval.z_lo`, `z_hi = print_z = row_z_cursor`, `flow_height = row_z_cursor - interval.z_lo`, `dependency_group = previous_rows.size() + 1`, `dependency_order = filament - 1`, both mask vectors sized `num_physical`, and the islands in `painted_masks_by_extruder[filament - 1]`.
   - If it is not printable, the islands are still in `managed_masks`, so that layer is simply left out there.
5. If the layer changed: insert the new passes at the end of the layer's range, sort the range by `(print_z, dependency_group, dependency_order)`, renumber `pass_index`, set `interval.sublayer_count`, lower `interval.sublayer_height` to the thinnest new pass, and shift `first_sublayer_idx` of **every later interval** by the change in count.

`min_pass_height` is passed as `0.5 * min_sublayer_height`.

### 6.7 State kept across layers

Next to `row_independent_states`:

```cpp
    // What every independent row printed on the previous layer. Empty after a layer that is not independent.
    std::vector<LocalZIndependentRowFootprint> previous_row_footprints(mixed_definitions.size());
    // The zone tips every independent row has taken over and not printed a pass over yet. Its next pass starts
    // below their tops, so it stops short of them.
    std::vector<std::vector<LocalZPendingTip>> row_pending_tips(mixed_definitions.size());
```

Both are reset where a layer is **not** an independent interval (right after the existing reset of `row_independent_states`):

```cpp
        for (LocalZIndependentRowFootprint &footprint : previous_row_footprints)
            footprint = LocalZIndependentRowFootprint{};
        for (std::vector<LocalZPendingTip> &pending_tips : row_pending_tips)
            pending_tips.clear();
```

### 6.8 Inside the independent branch of the layer loop

Before the row loop:

```cpp
            std::vector<LocalZIndependentRowFootprint> row_footprints(mixed_definitions.size());
            // Where every row resumed on this layer, and how far below the layer bottom that is. -1: the row does
            // not run a stack of its own here.
            std::vector<double> row_resume_z(mixed_definitions.size(), 0.);
            std::vector<double> row_lag(mixed_definitions.size(), -1.);
```

In the row loop:

- Adaptive rows: record only `row_footprints[row_idx].masks = row_state_masks[row_idx];` and leave `top_known` false. They `continue` before anything else, so their `row_lag` stays -1.
- Other rows, right after the block that resets `state` when the row starts or restarts, and before any pass is appended:

```cpp
                const size_t row_first_plan_idx = independent_plans.size();
                row_resume_z[row_idx] = state.z_cursor;
                row_lag[row_idx]      = std::max(0., interval.z_lo - state.z_cursor);
```

- At the end of the row's iteration, after the remainder pass logic:

```cpp
                LocalZIndependentRowFootprint &footprint = row_footprints[row_idx];
                footprint.masks     = row_state_masks[row_idx];
                footprint.top       = state.z_cursor;
                footprint.top_known = true;
                for (size_t plan_idx = row_first_plan_idx; plan_idx < independent_plans.size(); ++plan_idx)
                    footprint.top = std::max(footprint.top, independent_plans[plan_idx].print_z);
```

After the row loop, **before** the fixed-colour passes are appended and before `independent_plans` is sorted and moved into `plans`:

```cpp
            if (layer_id > 0) {
                const LocalZInterval &previous_interval = intervals.back();
                for (size_t row_idx = 0; row_idx < row_lag.size(); ++row_idx) {
                    std::vector<LocalZPendingTip> &pending_tips = row_pending_tips[row_idx];
                    if (row_lag[row_idx] < 0.) {
                        pending_tips.clear();
                        continue;
                    }
                    if (row_lag[row_idx] > EPSILON) {
                        const LocalZTakenOverArea taken =
                            local_z_area_taken_over_by_resuming_row(intervals, row_idx, row_state_masks[row_idx], previous_row_footprints,
                                                                    row_resume_z[row_idx], nominal_zones_on_layer);
                        for (LocalZZoneSteps &steps :
                             local_z_zone_steps_under_resuming_row(taken, row_idx, previous_row_footprints, row_resume_z[row_idx],
                                                                   nominal_zones_on_layer(layer_id - 1), previous_interval.z_hi))
                            pending_tips.push_back({std::move(steps.strips), steps.owner_top});
                    }
                    if (pending_tips.empty())
                        continue;

                    // The first pass of the row on this layer, if it has one.
                    const auto first_pass = std::find_if(independent_plans.begin(), independent_plans.end(), [row_idx](const SubLayerPlan &plan) {
                        return plan.split_interval && plan.dependency_group == row_idx + 1;
                    });
                    if (first_pass == independent_plans.end())
                        continue;
                    const double pass_z_lo = first_pass->print_z - first_pass->flow_height;
                    ExPolygons   held_back;
                    for (const LocalZPendingTip &tip : pending_tips)
                        if (tip.z > pass_z_lo + EPSILON)
                            append(held_back, intersection_ex(tip.masks, row_state_masks[row_idx]));
                    pending_tips.clear();
                    if (held_back.empty())
                        continue;
                    held_back = union_ex(held_back);
                    for (ExPolygons &masks : first_pass->painted_masks_by_extruder)
                        if (!masks.empty()) {
                            masks = diff_ex(masks, held_back);
                            break;
                        }
                    first_pass->held_back_masks = std::move(held_back);
                }
            }

            if (layer_id > 0)
                for (size_t row_idx = 0; row_idx < row_lag.size(); ++row_idx)
                    if (row_lag[row_idx] > EPSILON)
                        cut_local_z_zones_under_resuming_row(plans, intervals, row_idx, row_state_masks[row_idx], previous_row_footprints,
                                                             row_resume_z[row_idx], 0.5 * min_sublayer_height, num_physical,
                                                             nominal_zones_on_layer);
            interval.first_sublayer_idx = plans.size();
            previous_row_footprints = std::move(row_footprints);
```

Points that are easy to get wrong:

- **Tips are pending, not immediate.** A row with passes taller than a layer may plan no pass on the layer where it takes a tip over. The tip waits in `row_pending_tips` until the row's next pass, which may be one or more layers later. It is intersected with the row's mask of *that* layer.
- **Only the first pass is held back.** `independent_plans` is still in emission order here, so the first plan of the row's group is its lowest one. Every later pass of the row on this layer starts at or above the top of the layer below, so it is above every tip.
- **The held-back area stays the row's.** Only the pass's own mask shrinks. `row_state_masks`, the footprint and `interval.managed_masks` are untouched, so the base stays out of the tip and the next layer does not see the tip as taken over again.
- **`interval.first_sublayer_idx = plans.size()` is required.** The cut function inserts and erases plans of earlier layers, which moves the index this layer's plans will start at.
- **The cut function runs for every lagging row, after all rows have planned their passes.** It only touches earlier layers.

---

## 7. A second set of walls (`src/libslic3r/Layer.cpp`)

Clipping a pass at the edge of the tip is not acceptable: the layer's perimeters turn inwards at `B(N)`, so a pass cut off at `B(N-1)` ends bluntly on the outer wall line and loses its inward wall. This was tried and rejected (section 11).

Instead, `Layer::make_perimeters()` generates the perimeters of the affected region group a second time, from slices without the held-back areas:

```cpp
// The areas that Local-Z passes hold back on a layer.
static ExPolygons local_z_held_back_masks(const Layer &layer)
{
    ExPolygons masks;
    const PrintObject *object = layer.object();
    if (object == nullptr)
        return masks;
    const std::vector<LocalZInterval> &intervals = object->local_z_intervals();
    const std::vector<SubLayerPlan>   &plans     = object->local_z_sublayer_plan();
    const size_t                       layer_id  = size_t(layer.id());
    const auto interval = std::find_if(intervals.begin(), intervals.end(),
                                       [layer_id](const LocalZInterval &candidate) { return candidate.layer_id == layer_id; });
    if (interval == intervals.end())
        return masks;
    const size_t end_idx = std::min(plans.size(), interval->first_sublayer_idx + interval->sublayer_count);
    for (size_t plan_idx = interval->first_sublayer_idx; plan_idx < end_idx; ++plan_idx)
        append(masks, plans[plan_idx].held_back_masks);
    if (masks.size() > 1)
        masks = union_ex(masks);
    return masks;
}

// owner holds the perimeters of the group.
static void make_local_z_alt_perimeters(LayerRegion &owner, const SurfaceCollection &group_slices, const LayerRegionPtrs &group,
                                        const ExPolygons &held_back)
{
    owner.local_z_alt_perimeters.clear();
    if (held_back.empty() || owner.perimeters.empty())
        return;

    SurfaceCollection alt_slices;
    bool              changed = false;
    for (const Surface &surface : group_slices.surfaces) {
        ExPolygons rest = diff_ex(ExPolygons{surface.expolygon}, held_back);
        if (rest.size() != 1 || std::abs(rest.front().area() - surface.expolygon.area()) > 1.)
            changed = true;
        if (!rest.empty())
            alt_slices.append(rest, surface);
    }
    if (!changed)
        return;

    ExtrusionEntityCollection perimeters = std::move(owner.perimeters);
    ExtrusionEntityCollection thin_fills = std::move(owner.thin_fills);
    SurfaceCollection         alt_fill_surfaces;
    ExPolygons                alt_fill_no_overlap;
    owner.make_perimeters(alt_slices, group, &alt_fill_surfaces, &alt_fill_no_overlap);
    owner.local_z_alt_perimeters = std::move(owner.perimeters);
    owner.perimeters             = std::move(perimeters);
    owner.thin_fills             = std::move(thin_fills);
}
```

In `Layer::make_perimeters()`:

```cpp
    const ExPolygons local_z_held_back = local_z_held_back_masks(*this);
    for (LayerRegion *layerm : m_regions)
        layerm->local_z_alt_perimeters.clear();
```

and one call after each of the two existing `make_perimeters` calls:

```cpp
    // single region
    (*layerm)->make_perimeters((*layerm)->slices, {*layerm}, &(*layerm)->fill_surfaces, &(*layerm)->fill_no_overlap_expolygons);
    (*layerm)->fill_expolygons = to_expolygons((*layerm)->fill_surfaces.surfaces);
    make_local_z_alt_perimeters(**layerm, (*layerm)->slices, {*layerm}, local_z_held_back);

    // merged compatible regions
    layerm_config->make_perimeters(new_slices, layerms, &fill_surfaces, &fill_no_overlap);
    make_local_z_alt_perimeters(*layerm_config, new_slices, layerms, local_z_held_back);
```

This relies on `LayerRegion::make_perimeters()` writing only to `this->perimeters`, `this->thin_fills` and its two output arguments. That holds on the source branch, including the image-map branch inside it. **Check it on the target branch.** If the function has other side effects there, the save-and-restore above is not enough.

The fill surfaces of the second run are thrown away. Only the walls differ; infill still comes from the first run.

One set per region is enough even with several rows on a layer: a held-back area lies inside the region of the row that holds it back, so removing all held-back areas of the layer from every region changes each region only where its own row needs it.

The planner runs in the slicing step and the walls in the perimeter step, so the plan exists by then, and anything that invalidates slicing regenerates both.

---

## 8. G-code stage (`GCode.cpp`, `process_layer()`)

A pass with `held_back_masks` takes its walls of a region from `local_z_alt_perimeters` when that region has them. Everything else is unchanged: other passes, the base, infill.

1. The body of the existing per-pass loop (clip one island's extrusions to the pass mask, check heights, hand the result to the pass's extruders and islands) moves into a lambda, defined once per region before the `for (entity_type ...)` loop:

```cpp
                const auto pass_uses_alt_perimeters = [layerm](const LocalZPassBucket& pass_bucket) {
                    return !pass_bucket.plan->held_back_masks.empty() && !layerm->local_z_alt_perimeters.entities.empty();
                };
                const auto append_local_z_pass_extrusions = [&](const ExtrusionEntityCollection&             source,
                                                                LocalZPassBucket&                            pass_bucket,
                                                                const ObjectByExtruder::Island::Region::Type entity_type) {
                    // former body of: for (LocalZPassBucket& pass_bucket : ...) { for (pass_extruder_id ...) { ... } }
                    // with *extrusions replaced by source
                };
```

2. The per-pass loop becomes:

```cpp
                            for (LocalZPassBucket& pass_bucket : local_z_ctx->pass_buckets) {
                                if (pass_bucket.plan == nullptr)
                                    continue;
                                // A pass that holds areas back gets its walls from the perimeters generated without them, below.
                                if (entity_type == ObjectByExtruder::Island::Region::PERIMETERS && pass_uses_alt_perimeters(pass_bucket))
                                    continue;
                                append_local_z_pass_extrusions(*extrusions, pass_bucket, entity_type);
                            }
```

3. After the loop over the region's ordinary perimeter islands, still inside the `entity_type` loop:

```cpp
                    if (entity_type == ObjectByExtruder::Island::Region::PERIMETERS && local_z_ctx != nullptr &&
                        !local_z_ctx->mixed_masks_union.empty())
                        for (const ExtrusionEntity* ee : layerm->local_z_alt_perimeters.entities) {
                            const auto* extrusions = static_cast<const ExtrusionEntityCollection*>(ee);
                            if (extrusions->entities.empty())
                                continue;
                            for (LocalZPassBucket& pass_bucket : local_z_ctx->pass_buckets)
                                if (pass_bucket.plan != nullptr && pass_uses_alt_perimeters(pass_bucket))
                                    append_local_z_pass_extrusions(*extrusions, pass_bucket, entity_type);
                        }
```

No change is needed in the wipe-tower planning (`collect_local_z_wipe_tower_toolchanges()` in `Print.cpp`) or in the tool ordering: both look at which passes exist and which extruders their masks name, and that is the same as before for Rule B. Rule A adds passes through the ordinary plan list, which those already read.

---

## 9. Tests (`tests/fff_print/test_print.cpp`)

One shared checker, `check_local_z_zone_under_gradient(ZoneKind, bool stepped_edge)`, and two test cases with three sections each (zone is a subdivided mix, a mix printed one filament per layer, a solid colour). Tags: `[PrintObject][MixedFilament][LocalZ][Gradient]`.

Setup: a 20 x 20 x 6 mm box whose side walls are split at z = 3.1 mm, printed with a whole-object gradient (mixed filament 4, 0.30 mm cadence) over 0.24 mm layers. The lower part of one wall is painted. With both lower facets painted the zone is a rectangle that ends below the object's top (Rule A). With one facet painted it is a triangle that narrows by about 1.5 mm per layer (Rule B). `mmu_segmented_region_max_width` is 1 mm so the zone is shallow, as on a thin-walled model.

- **"Local-Z ends a colour zone early under the gradient pass that takes its place"**: the gradient resumes inside the zone's last layer; the zone's printed top equals where the gradient starts, within half the minimum sublayer height; a nominal zone keeps its layer's filament; no wall is printed into another.
- **"Local-Z stops the gradient pass short of the step of a colour zone edge it would be printed into"**:
  - at least three gradient passes have `held_back_masks`, and their own mask does not cover them;
  - the layer of each has `local_z_alt_perimeters`;
  - each such pass has, in the G-code at its `print_z`, a wall segment running into the object just before the edge the zone has on the layer **below** (the seam);
  - no wall changes height between two consecutive extruding moves;
  - every pass is planned once (no shortened copies below the zone's top);
  - walls of the zone's own filament are all a full layer high, and nothing of the zone's region is in `managed_masks` (the zone is neither cut nor clipped);
  - no wall is printed into another.

The overlap check rasterises wall centrelines from `GCodeProcessorResult::moves` on a 0.05 mm grid, gives every bead the Z range `[z - height, z]`, and counts cells where a bead starts more than 0.01 mm below the top of the bead under it. The limit is 1.5 mm of wall, which only seams reach.

Four other cases in `fff_print_tests` fail on the source branch with or without this change: three in `test_bed_temperature.cpp` and "Persistent image maps generate geometry-first perimeter path modulation V2".

---

## 10. Porting checklist

1. Add `SubLayerPlan::held_back_masks` (`Print.hpp`) and `LayerRegion::local_z_alt_perimeters` (`Layer.hpp`).
2. `PrintObjectSlice.cpp`: helper types and functions (6.3 to 6.6) before `build_local_z_plan()`.
3. `build_local_z_plan()`: new parameter and both call sites (6.1), `nominal_zones_on_layer` (6.2), the two state vectors and their reset (6.7), the per-row recording and the block after the row loop (6.8).
4. `Layer.cpp`: the two helpers and the three insertions in `Layer::make_perimeters()` (7).
5. `GCode.cpp`: the lambda refactor and the second loop (8).
6. Tests (9) and the user documentation paragraph in `doc/fullspectrum-simplified-gradients-and-surface-paint.md` under "Where the cadence runs".
7. Build and run the checks in section 12.

---

## 11. What to re-check on the target branch

The change leans on these properties of the source branch. If one does not hold, the logic needs adjusting, not just the patch context.

| Assumption | Where it matters |
|---|---|
| An independent row plans passes `while next_z <= interval.z_hi`, so a pass never rises above its layer and the first pass of a layer is the only one that can start below it. | Rule B holds back only the first pass. If passes may overshoot the layer top, a row can also *lead*, and growing edges get the overlap instead. |
| One `LocalZInterval` is pushed per layer, in order, in both the independent and the other path. | `intervals.back()` and `intervals.size() - 1` are taken as the previous layer. |
| `LocalZInterval::layer_id` is compared with `Layer::id()` in `GCode.cpp`. | `local_z_held_back_masks()` does the same. With raft layers `Layer::id()` is offset; this was not tested. |
| `dependency_group == row index + 1` for row passes, and `mixed_definitions.size() + 1` for fixed colours. | Finding a row's passes; the group of cut nominal passes. |
| Passes are chained per `(layer, dependency_group)` in the order `(dependency_order, print_z, pass_index)`. | A shortened copy keeps the order of the pass it is cut from. |
| Pass masks are grown by 0.10 mm and base exclusion by 0.04 mm in the G-code stage. | The 0.06 mm growth of cut nominal islands in `managed_masks`; the 0.2 mm minimum strip width. |
| `Layer::lslices` is **not** filled in while the planner runs. | Do not use it there. Take outlines from `layerm->slices` if one is ever needed. |
| `LayerRegion::make_perimeters()` writes only to `perimeters`, `thin_fills` and its outputs. | The second wall generation. |
| The planner runs after `mm_segmentation` is final and before `apply_mm_segmentation()`. | `nominal_zones_on_layer` reads what will be applied. |
| Nominal zones resolve their filament per layer with `effective_painted_region_filament_id()` and the arguments shown in 6.2. | A cut nominal layer must be printed in the filament the layer would have used. |

---

## 12. Verifying

Build and test (Windows, from the repository root):

```sh
cmake --build build --config Release --target Snapmaker_Orca fff_print_tests -- -m -v:minimal
build/tests/fff_print/Release/fff_print_tests.exe "[LocalZ]"
```

Expected: all 8 Local-Z cases pass.

Headless slice of a project, for inspecting G-code without the GUI:

```sh
build/src/Release/snapmaker-orca.exe --debug 2 --allow-newer-file --datadir <dir> \
    --slice 1 --outputdir <out> --export-3mf out.3mf <project.3mf>
```

The planner dump is written to `<dir>/SVG/local-z-plan-obj-N.json`. The CLI rejects GUI projects whose `wall_filament`, `sparse_infill_filament` or `solid_infill_filament` is 0 or whose `raft_first_layer_expansion` is -1; patch a scratch copy of the project (set them to 1, 1, 1 and 2).

Reference numbers from the test project used during development (`Cubeedge.3mf`: a 27 mm tall block with a whole-object gradient and painted zones with slanted edges), 0.16 mm layers:

| Setting | Wall printed into another wall, before | After |
|---|---|---|
| gradient 0.2, subdivide off | 19.5 mm, up to 0.16 mm deep | 0.0 mm |
| gradient 0.2, subdivide on | 44.3 mm, up to 0.16 mm deep | 0.0 mm |
| gradient 0.3, subdivide off | 19.9 mm, up to 0.16 mm deep | 0.0 mm |
| gradient 0.3, subdivide on | 44.8 mm, up to 0.16 mm deep | 0.0 mm |

"Before" is a build with Rule A only. With subdivide off the number of continuous wall runs (1354), the tool changes (489) and the estimated time (3h06m) are the same before and after: Rule B adds no extrusions and removes none.

In the preview, at a slanted zone edge:

- zone layers end in regular steps, each turning inwards;
- the gradient pass that crosses a layer boundary ends one step earlier than the passes above it, at the same place as the passes below, also turning inwards;
- above each zone tip there is a small gap under the next gradient pass.

---

## 13. Approaches that were tried and rejected

All of these were built and looked at in the G-code preview before the current design. Do not reintroduce them.

| Approach | Why it was rejected |
|---|---|
| Leave the overlap. | The row is printed up to a full layer into the zone. |
| Raise the bottom of the row's pass over the whole taken-over area, zone tops included (split into two passes of different height). | Long notches in the underside of the gradient; "layers that suddenly change layer height". |
| Cut the zone's tip down to the cursor at every step, as its own short pass. | Thin slivers a fraction of a millimetre long on every layer, each a separate extrusion. An early version also printed a wall twice because base exclusion (0.04) and pass clip (0.10) disagreed. |
| Give the tip up: take it out of the zone's layer by mask. | The zone's outer wall is clipped and ends without turning inwards, and its inward wall is removed, leaving a hole in that wall line. |
| Move the step to a neighbouring layer where the planes line up, by editing the painted regions. | Seams are fine, but steps bunch up (two or three layers end at the same place, then a jump): a jagged edge. With passes taller than a layer many steps find no layer that lines up. |
| Print the row's pass thinner only over the tip, inside the same wall (a path whose height changes on the way). | On the outer wall the thin part is about 0.05 mm long before the wall turns inwards; with pointed bead ends it reads as a wedge. |

The general lessons: a wall must end where the perimeter generator ended it, so change the region and regenerate rather than clip; do not change a bead's height along its length; do not move painted edges; do not add extrusions shorter than a few line widths.

---

## 14. Limits and things not tested

- **The gap above a tip** is as tall as the part of the held-back pass above the tip, so anything from almost nothing to almost a full pass. With 0.14 + 0.06 mm passes over 0.16 mm layers it was 0.01 to 0.08 mm in the inspected region.
- **Strips narrower than 0.2 mm** are still overlapped, as before. That is the case along steep edges.
- **A bead cannot be split lengthwise.** Where an edge moves sideways along a wall by less than a line width, the centreline decides which side owns the wall.
- **`dithering_local_z_whole_objects`** ("Full domain for all mixes") with fixed-colour passes was not tested. Physical colours are then printed as fixed passes and also listed by `nominal_zones_on_layer`, so Rule A may add a cut pass next to a fixed pass.
- **Raft layers**, several objects or instances, and the classic wall generator were not tested. The unit tests use the default wall generator.
- **Checked in the G-code**, by the unit tests and by scripted measurements on the reference cube. The preview was judged by eye on that cube only.
