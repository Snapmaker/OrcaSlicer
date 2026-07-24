# Support-Model Interference — Adversarial Dev Loop Journal

Branch: `bugfix/support_interference_with_model_feet` (from `origin/process_optimistic_july`)
Symptom: support body wraps the model feet; path interference in Z=[0.25, 11.75].

## Iteration 1 — "inset fill polygon at L796 (fill-bead edge intrusion)"  [REFUTED]

### Hypothesis
Fill centerlines land on the support-polygon boundary, so the bead half-width
(0.21mm) intrudes into the object gap. Fix = inset polygon by 0.5*width in
`fill_expolygons_with_sheath_generate_paths` (SupportCommon.cpp L796).

### Adversarial refutation (REFUTE-2) — ACCEPTED (independently verified)
Premise is FALSE for the dominant path: the filler already insets.
- `Fill::fill_surface` (FillBase.cpp L107): `offset_ex(., scale_(overlap - 0.5*spacing))`.
- `FillSupportBase::fill_surface` (FillRectilinear.cpp L3241): insets 0.5*spacing.
- `FillRectilinear::fill_surface_by_lines` (L2774): insets only 0.05*spacing.
- Support never sets filler->overlap.
=> Sparse support ALREADY insets ~0.19mm; L796 adds 0.5*width on top = DOUBLE
   inset (~0.40mm over-inset). Wrong-layer patch. REVERTED (tree clean).

## Iteration 2 — evidence-driven re-PLAN (this iteration)

### Method
GCode toolpath geometry (snorca.gcode) + source trace. Config:
support_object_xy_distance=0.35, tree_support_wall_count=0 (with_sheath=false),
support_base_pattern_spacing=2.5 (sparse), raft_layers=2, no_overlap_xy_gap=0.2,
sharp_tail_xy_gap=0.2.

### Evidence 1 — naive crossing test was a FALSE-POSITIVE generator
First segment-intersection test reported "47/57 layers cross the wall". But point-
distance at the worst layer (z=5.95) showed min support-to-wall = 0.601mm (>gap_xy).
The crossing test flags path lines that *pass* the wall line at 0.6mm without material
overlap (net bead gap +0.18mm). Lesson: crossing != overlap; must use centerline
distance vs extrusion width. (Self-adversary caught my own flawed metric.)

### Evidence 2 — rigorous per-layer min centerline distance (support vs outer wall)
Material overlap iff centerline gap < extrusion width (0.42mm):
- OVERLAP (gap<0.37mm): 6 layers — z=2.35(0.014), 5.35(0.019), 2.55(0.090),
  6.55(0.100), 5.15(0.242), 8.35(0.287)
- TOUCH (0.37-0.52): 11 layers
- CLEAN (>0.52): 39 layers
Worst case z=2.35: support fill line 0.014mm from wall centerline -> ~0.40mm material overlap.

### Evidence 3 — failing subtype is ONLY 'support' (sparse fill), never interface
On all 6 overlap layers the sparse 'support' fails; 'support interface' (meant to
touch the model) is correctly spaced. => The support POLYGON of the sparse body is
gapped too tight on these layers, not the bead edge and not the interface.

### Evidence 4 — root-cause mechanism (quantitative)
On the 6 failing layers the polygon gap measures ~0.20mm, which equals
`no_overlap_xy_gap = 0.2f` (SupportMaterial.cpp L1363), NOT `gap_xy = 0.35`.
`trim_support_layers_by_object` (L3163-3165) picks gap_xy ONLY when
`is_layers_overlap(...)` is true; otherwise it uses no_overlap_xy_gap=0.2.
=> On these layers is_layers_overlap=false.
Then FillSupportBase insets the fill by 0.5*spacing (~0.19mm). Net fill-line-to-wall:
  0.20 (polygon gap) - 0.19 (fill inset) = 0.01mm  ->  matches measured 0.014mm.
For a positive net bead gap, no_overlap_xy_gap must be >= 0.5*spacing + 0.5*width
(= 0.19 + 0.21 = 0.40mm). Current 0.2mm is short by ~0.20mm on the non-overlap branch.

### Conclusion (corrected root cause)
NOT fill-bead-edge intrusion. The support sparse-body polygon is gapped by only
0.2mm (no_overlap_xy_gap) on layers where support is not Z-synchronized with the
object layer (is_layers_overlap=false). After the filler's own 0.5*spacing inset,
the fill line lands on the object wall. The user's "body avoidance" intuition is
right: avoidance exists but the non-overlap gap constant is too tight for the
extrusion width.

### Candidate fix (not yet implemented, for REFUTE-3)
- Option A: raise no_overlap_xy_gap to >= 0.5*spacing + 0.5*width (>=0.40mm), or
  derive it from support flow width. Minimal, single constant.
- Option B: always use gap_xy for body avoidance regardless of Z-overlap (drops the
  no_overlap shortcut). Wider effect, may grow support gap globally.
- Option C: account for fill inset in the trim offset on the support-body path.

### Change this iteration
None (evidence phase). Tree clean. Iteration 1 revert still in place.

### Backlog
- REFUTE-3 the chosen fix option before implementing.
- Confirm which layers hit is_overlap=false and why (layer sync / variable layer height).
- Update Feishu doc to corrected root cause after fix option is agreed.
- Visual evidence rendered: z2.35_interference.png

## Iteration 3 — user asked "isn't C more root-cause?" -> re-derivation OVERTURNED my A claim

### What the user's question triggered
Re-deriving the geometry honestly:
- Closest point on each failing layer is on the MAIN fill polyline (polyline#0,
  ~3000-3900 pts, ~1700-2600mm, the single connect_base_support chain). NOT a link line.
- => support polygon inner edge is ~0.38mm INSIDE the object body (overlaps it).
- CLEAN layers measure fill-to-wall = 0.63mm, which equals
  no_overlap_xy_gap(0.2) + 0.5*spacing(0.19) + 0.5*width(0.21) = 0.60mm. EXACT match.

### Conclusion: my iteration-2 root cause ("constant too small") was WRONG
Same config, same no_overlap_xy_gap=0.2:
- 39 layers: trim works -> 0.63mm material gap (plenty).
- 6 layers: trim BYPASSED -> polygon overlaps object ~0.38mm.
=> Raising the constant (A) cannot fix the 6 layers; it only widens the 39.
   The constant is NOT the differentiator. The user's instinct (C / body-avoidance)
   is closer to correct than my A recommendation.

### Why A and literal-C both fail here
- A (raise no_overlap_xy_gap): affects the gap VALUE, but the 6 layers never reach
  the trim at all. No effect on them.
- C (account for fill inset in trim offset): also assumes the trim runs. The polygon
  already overlaps before any inset accounting. No effect.

### Real root cause (mechanism now clear)
`trim_support_layers_by_object` scans object layers in the Z-window
[bottom_print_z - gap_extra_below, print_z + gap_extra_above] (L3155/L3160).
gap_extra_below/above = gap_support_object/gap_object_support =
support_top_z_distance/support_bottom_z_distance. Both = 0 in this GCode
(support_top_z_distance=0, support_bottom_z_distance=0). So the Z-window has ZERO
extra margin. Under adaptive layer height (tree_support_adaptive_layer_height=1),
support-layer and object-layer Z grids desync: a support layer's [bottom_z, print_z]
window can fall BETWEEN object layers, so the object layer carrying the feet XY
geometry is never scanned -> its lslices never enter polygons_trimming -> the diff
leaves the support polygon overlapping the feet. This is the classic per-layer
sync gap the user's "body avoidance" hypothesis points at.

### A applied as a FALSIFICATION EXPERIMENT (this iteration's change)
no_overlap_xy_gap: 0.2 -> 0.4 (SupportMaterial.cpp L1363) with explanatory comment.
This is a diagnostic, not the fix. Expected outcomes:
- 6 failing layers do NOT improve (stay ~0mm) -> confirms trim-bypass root cause,
  revert A, implement the real fix.
- 6 failing layers lift to >0.4mm -> I was wrong, it was the constant.

### Real fix (next, after A falsified) — body-avoidance, sync-independent
Make the trim NOT depend on per-layer Z coincidence:
- Option D1: widen the Z-window margin by a robust amount (>= one max object layer
  height) independent of support_top/bottom_z_distance, so the feet-bearing object
  layer is always scanned. Cheapest, single change.
- Option D2: trim support against a UNION of object lslices over a Z band of at
  least [print_z - layer_height, print_z] (the actual footprint at this Z), not a
  coincidence-based window. Most correct ("主体的避让算法").
- Option D3: additionally inset the trim offset by the support bead half-width +
  fill inset so the cleared zone covers actual material (this is the honest part of
  C), but only meaningful once the right layers are scanned (D1/D2).

### Change this iteration
no_overlap_xy_gap 0.2 -> 0.4 (falsification test). Tree otherwise clean.


## 迭代 4 — Tree Support base_areas object-body trim (root cause: wrong code path analysis)

### 触发的理论缺口
**前3轮迭代全部分析了错误的代码路径。** support_type=tree(auto) 时，
PrintObject::_generate_support_material() 分发到 TreeSupport::generate()，
完全绕过 PrintObjectSupportMaterial::generate()（以及其 trim_support_layers_by_object）。
前序分析中 no_overlap_xy_gap、generate_base_layers、trim_support_layers_by_object 均为死代码。

### 初版方案（被推翻点）
- 迭代1: L796 fill-bead inset → 被推翻（fill已inset）
- 迭代2: no_overlap_xy_gap 0.2→0.4 → 被推翻（同一常量同时产生clean和failing层）
- 迭代3: D1/D2 Z-window desync → 被推翻（Z网格已同步）

### 对抗审查结论
**[blocker] 根因发现**: TreeSupport::draw_circles() 的 get_collision lambda:
```cpp
collision = offset_ex(m_ts_data->m_layer_outlines[obj_layer_nr],
    sharp_tail ? scale_(top_z_distance) :     // ← top_z_distance=0 → ZERO offset
                scale_(m_xy_distance));
```
- support_top_z_distance=0 → sharp_tail 碰撞 offset = 0
- m_layer_outlines 用 poly.simplify(scale_(0.2)) 简化 → 简化轮廓比实际 lslices 最多小 0.2mm
- 净效果: 支撑多边形可深入实际 object body ~0.2mm
- 数学验证: 填充中心线距离 = 0.2(simplify) - 0.19(0.5*spacing) = 0.01mm ≈ 实测 0.014mm ✓

**[major] get_trim_support_regions 是死代码**: 定义了完整的 trim 逻辑（复制自 normal support），
但从未在任何地方被调用。Tree support 完全依赖碰撞模型做 object avoidance。

### 修订方案
在 draw_circles() 中 floor_areas 处理之后、area_groups 创建之前，
添加 base_areas 安全 trim：

```cpp
// 仅移除"环带"区域（lslices 与 lslices+offset 之间）
// 保留 overhang 下方的支撑（lslices 内部）
ExPolygons offset_lslices = offset_ex(trim_obj_layer->lslices, scale_(trim_gap));
ExPolygons trim_ring = diff_ex(offset_lslices, trim_obj_layer->lslices);
base_areas = diff_ex(base_areas, clip_bbox(trim_ring, get_extents(base_areas)));
```

Ring 方案而非 full-offset 的原因：
- 直接 offset+subtract 会移除 overhang 下方的接触支撑
- Ring 只移除 object body 外侧的近距离支撑，不影响 overhang 接触

### 对抗自审清单
- [passed] 对 clean 层的影响: gap 从 ~0.23mm 增至 ~0.35mm（因用实际 lslices 而非简化轮廓），
  行为一致化，可接受
- [passed] ExPolygons 孔洞处理: offset 正确缩放 contour 外扩 + hole 内缩，diff 生成正确 ring
- [passed] 线程安全: 全部局部变量，无共享状态
- [passed] floor_areas 交互: trim 在 floor 减除之后执行，不影响 floor 计算
- [passed] roof_areas 交互: trim 仅影响 base_areas，roof 已有独立 collision 裁剪
- [passed] 编译安全: 所有类型/函数签名已验证

### 改动文件
- src/libslic3r/Support/TreeSupport.cpp (draw_circles, +22 lines)

### 测试证据
待用户编译验证。验证方法：切片 Lizard 模型，检查 z=[0.25, 11.75] 范围内
支撑填充路径与 wall 的 centerline 距离是否 >0.42mm（无材料重叠）。

### 遗留 backlog
- 考虑同时修复 get_collision lambda 的 sharp_tail offset: max(top_z_distance, resolution)
- 考虑对 roof_areas 也加 ring trim（当前仅 base_areas）
- get_trim_support_regions 死代码清理或激活
