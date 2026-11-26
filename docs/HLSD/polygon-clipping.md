# Polygon Clipping — High Level Design

## Purpose and scope

Almost every stage of slicing works on 2D regions: slices, perimeters, infill
areas, bridges, supports and brims are all produced by boolean operations and
offsets on polygons. libslic3r does this through two interfaces, both built on
the Clipper2 library vendored in `deps_src/clipper2`:

- `ClipperUtils` (`src/libslic3r/ClipperUtils.hpp`) takes and returns Slic3r
  geometry: `Polygon(s)`, `ExPolygon(s)`, `Polyline(s)`, `Lines` and
  `Surfaces`. It provides unions, intersections, differences and xor, closed
  and open offsets, morphological opening and closing, variable width offsets
  and polyline clipping.
- `ClipperZUtils` (`src/libslic3r/ClipperZUtils.hpp`) clips paths whose
  vertices carry a Z value, which callers use to tag vertices with a source
  index or an extrusion width.

No other code calls Clipper2.

`ClipperUtils` declares its own `JoinType`, `EndType`, `PolyFillType` and
`ClipType` enums and maps them to Clipper2's. Every call builds its own
Clipper2 objects and shares no state, so slicing threads can clip
concurrently.

Clipping is one of the largest costs of slicing, and nearly all of it goes
through `ClipperUtils`. The layer is therefore designed for throughput as much
as for predictable geometry.

## Vendored Clipper2

`deps_src/clipper2` builds the static target `Clipper2`. It carries six
changes to the upstream sources that must be carried over when Clipper2 is
updated. The namespace switch sits at the top of every header and source, and
the other patches are marked with `Orca:` comments.

| Change | Files | Why |
| --- | --- | --- |
| Z build in its own namespace and order-independent header | all headers and sources, `clipper2_z.cpp`, `clipper2_z.hpp` | The library is compiled a second time with `USINGZ` in namespace `Clipper2Lib_Z`, so the 2D and the Z variants link into one binary. `clipper2_z.hpp` saves and restores all Clipper2 sub-header include guards so `clipper.h` and `clipper2_z.hpp` can be included in either order in the same translation unit. |
| Engine nodes from tbbmalloc | `clipper.engine.h`, `clipper.engine.cpp` | Vertices, active edges, output points and records, local minima and `PolyTree` nodes are allocated one by one. `CLIPPER2_NODE_ALLOCATOR` routes them through `scalable_malloc`, because the default heap does not scale when all slicing threads clip at once. |
| Cyclic split-owner recursion guard | `clipper.engine.cpp` | `CheckSplitOwner()` marks `split->recursive_split = outrec` before recursing into emptied split `OutRec` lists so cyclic references cannot overflow the stack. |
| Restore `end_type_` after 2-point `Joined` fallback | `clipper.offset.cpp` | `DoGroupOffset()` restores `end_type_ = group.end_type` after each path so a degenerate 2-point `Joined` path does not turn subsequent closed paths in the same group into open paths. |
| Exact area sign & non-collinear zero-area shrink | `clipper.offset.cpp` | `GetLowestClosedPathInfo()` uses a local-origin 128-bit signed area check so distant paths do not cancel, and `DoGroupOffset()` flips negative offsets only for collinear degenerate paths rather than self-intersecting zero-area paths (bowties). |
| Concave joins at the edge crossing | `clipper.offset.cpp` | For closed paths, a concave corner is joined at the crossing of the two offset edges when that point lies within half of both adjacent edges. The upstream 3-point loop makes inward offsets of dense curves very slow to union. |
| Rounded arc steps | `clipper.offset.cpp` | Round joins use the rounded number of steps, not the ceiling, which keeps the vertex count of round offsets that the rest of the code is tuned for. |

## ClipperUtils semantics

The callers of `ClipperUtils` rely on a fixed set of behaviours. Where
Clipper2 behaves differently by default, the wrapper adjusts it.

### Booleans

- The fill rule is non-zero unless the function takes a `PolyFillType`. One
  rule applies to both subject and clip; Clipper2 has no per-operand rule.
- Collinear vertices are removed from the result. Clipper2 keeps them by
  default, so every boolean sets `PreserveCollinear(false)`.
- Outer contours are CCW and holes are CW. Standard Clipper2 booleans may emit
  vertex-self-touching or vertex-on-edge touching contours; `simplify_polygons()`,
  `simplify_polygons_ex()`, and `polygons_simplify(..., strictly_simple = true)`
  split contours at repeated vertices and vertex-on-edge touches into strictly
  simple loops and locally re-nest holes (with padded bounding boxes and interior
  point verification).
- `ExPolygons`, `top_level_expolygons()`, `top_level_islands()`, and
  `union_pt_chained_outside_in()` consume `PolyTree64` directly on the fast
  path when node orientations match depth parity, and rebuild exact containment
  via an AABB tree if Clipper2 emits misnested nodes. An island inside a hole
  becomes an `ExPolygon` of its own.
- `ApplySafetyOffset::Yes` grows the clip polygons by `ClipperSafetyOffset`
  before an intersection or a difference, so that edges shared by subject and
  clip do not leave slivers. For `ExPolygon`, `ExPolygons`, `Surfaces`, and
  `SurfacesPtr` clips this goes through `c2_offset_expolygons()` to normalize
  contour/hole orientations first.
- Open polylines are clipped per source path against an indexed clip edge set,
  preserving source direction and vertex order. `intersection_pl` / `diff_pl`
  and `clip_extrusion` follow Clipper1's exact boundary semantics (resolving
  boundary-grazing intervals through a Clipper1-equivalent open-path scanline
  pass in `OpenClipEdgeData::clip_path`, so `intersection` and `diff` remain
  complementary across corners, rotations, and horizontal runs), while
  `Algorithm::split_line` uses boundary-inclusive containment.

### Offsets

- Before offsetting, input vertices closer than
  `ClipperOffsetShortestEdgeFactor` × |delta| to the previously kept vertex
  are dropped. This bounds the work on dense contours, and the error it
  introduces is far below the offset distance. Polyline `offset()` overloads
  accept an optional `shortest_edge_factor` parameter (set to `0.` in
  `Format/svg.cpp` stroke offsetting so short terminal segments that survive
  SVG path parsing keep their cap orientation).
- The miter limit is at least 2. For `jtRound`, a positive `miterLimit`
  argument is the arc tolerance, and `0.25` is used otherwise; in both cases
  it is capped at |delta| / 4. Other joins use the smaller of `0.25` and
  |delta| / 4 for round end caps.
- A single `Polygon` keeps its orientation: a CCW polygon grows with a
  positive delta, a CW polygon is a hole and shrinks.
- `Polygons` follow the same rule per path. Non-CW batches run through
  `c2_offset_ccw_paths_isolated()`, which fast-batches all provably clean
  convex contours (`c2_is_clean_convex_offset_impl`) and isolates only non-clean
  or zero-area paths whose offset-influence boxes overlap another path (so
  concave self-intersections cannot cancel neighboring islands). Mixed CCW/CW
  inputs run as one Clipper2 group only when every path is clean-convex for its
  effective offset direction, every CW path is strictly enclosed with positive
  winding inside non-touching CCW contours, and no same-sign paths overlap
  within offset influence range; otherwise each path is offset on its own and
  the results are united.
- `ExPolygons` and `Surfaces` normalize contour (CCW) and hole (CW)
  orientations, fast-batching clean-convex holeless contours and clean-convex
  holed entities whose holes stay strictly inside without pairwise
  interference, while isolating non-clean holed entities (and isolating their
  holes by influence bounding box on both positive and negative offsets) before
  subtraction and union. Two-step `offset2()` / `offset2_ex()` calls preserve
  the intermediate `PolyTree64` hierarchy across both positive and negative
  second deltas.
- Only truly collinear degenerate paths are dropped under a negative offset;
  self-intersecting zero-area paths (e.g. bowties) retain their
  positive-winding lobes.
- Polyline offsets use the requested end type. Clipper2 already unites the
  result, so no further union is done.

### Coordinate range

Clipper2 computes intersections and slopes in doubles, which hold integers
exactly only up to 2^53 (about 9e15 units, 9,000 km). Geometry passed to
`ClipperUtils` must stay well inside that range; near the int64 limit the
results shift by hundreds of units. This is why the arrange `InfiniteBed` is a
box of ±2^50 units around its centre rather than libnest2d's infinite box,
which reaches ±2.3e18.

## ClipperZUtils

`ZPoint` is a `Vec3crd`, and a `ZPath` is a vector of them.
`clip_zpaths()` runs one boolean (defaulting to `pftNonZero`, with an optional
`PolyFillType` parameter) on the Clipper2 Z build. The subject may be open, the
clip is closed, and the result lists the closed paths before the open ones.
`clip_zpaths_polytree()` executes a boolean into a `PolyTree64`, repairs any
misnested nodes if orientation disagrees with depth parity, and invokes a
callback for each closed contour with its true nesting depth (depth 0 for
top-level outer contours, odd depths for holes, even depths > 0 for islands).

The Z of each output vertex follows these rules:

- An input vertex keeps its Z.
- An intersection that lies on an end point of one of the two crossing edges
  takes that end point's Z, preferring the subject edge.
- Any other intersection gets its Z from the callback, which receives both
  crossing edges, the subject edge first.

Clipper2 calls the callback only when it creates an output vertex at an
intersection, not for every crossing it processes. A callback that records
intersections, like `ClipperZIntersectionVisitor`, therefore sees only those.

The users are:

| User | Z carries |
| --- | --- |
| `Brim.cpp` (`get_top_level_objects_with_brim`) | island index + 1 via `clip_zpaths_polytree(..., pftEvenOdd)` so only top-level outer contours get an outer brim |
| `Algorithm::wave_seeds()` (region expansion) | source and boundary index; intersections get a negative index into the visitor's list of crossing pairs |
| `Algorithm::split_line()` | index of the source vertex; an intersection gets the negated index of its source edge, so the pieces can be put back in path order |
| `PerimeterGenerator` overhang and top-surface clipping of Arachne walls | extrusion width, interpolated along the edge at intersections (and recovered on 2-point paths clipped at clip vertices) |
| Tree support anchors in `SupportCommon` | index of the source contour, -1 at intersections |
| `extrusion_paths_append()` | extrusion width, turned into extrusion paths |

## Testing

`tests/libslic3r/test_clipper_utils.cpp`, `test_clipper_offset.cpp`,
`test_polygon.cpp`, `test_region_expansion.cpp`, and `test_svg.cpp` cover the
wrapper's booleans, `PolyTree64` nesting repair, orientation and offset rules,
strictly simple splitting, 128-bit point-in-polygon, wave-seed recovery, and
SVG short-stroke preservation. The perimeter, support, brim, and wipe-tower
users are exercised by the slicing tests in `tests/fff_print` and
`tests/sla_print`.
