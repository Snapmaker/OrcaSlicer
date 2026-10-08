#ifndef slic3r_ImageFill_hpp_
#define slic3r_ImageFill_hpp_

// Image Fill: put a picture on a part by choosing, per facet, the best of the filaments the
// user allowed - and writing the answer as ordinary MMU painting.
//
// Spec: docs/superpowers/specs/2026-09-07-imagemap-phase2-imagefill.md
// Plan: docs/superpowers/specs/2026-09-07-imagemap-edgeslicer-plan.md, Phase 2.
//
// The governing constraint, from the plan's section 5.4: **this feature adds no ObjectBase
// member anywhere**. Image bytes live in ImageAssetStore, a plain value member of Model keyed
// by content hash; the per-part annotation rides two new keys on the config objects that
// already exist. Bar A's "; model label id" check is the automated gate on that.
//
// Everything below is wxWidgets-free so it can be unit tested and so the CLI/import paths
// share it. The dialog is src/slic3r/GUI/ImageFillDialog.{cpp,hpp}.

#include "libslic3r.h"
#include "Point.hpp"
#include "TriangleMesh.hpp"
#include "TriangleSelector.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Slic3r {

class ModelObject;
class ModelVolume;

// ---------------------------------------------------------------------------------------------
// 1. The image asset store
// ---------------------------------------------------------------------------------------------

// One image, addressed by the SHA-256 of its encoded bytes. `bytes` is always a PNG: the GUI
// normalises whatever the user picked (JPEG included) through wxImage before it gets here, so
// what the store holds is what goes into the 3MF and what the hash is taken over - which is why
// the round trip can be bit-identical rather than merely equivalent.
struct ImageAsset
{
    std::string          sha256;   // 64 lowercase hex chars; the key
    std::vector<uint8_t> bytes;    // the encoded PNG, verbatim

    // Decoded lazily by ImageAssetStore::pixels(); never serialised.
    mutable unsigned                     width  = 0;
    mutable unsigned                     height = 0;
    mutable std::vector<uint8_t>         rgb;      // 3 bytes per pixel, row-major, top row first
    mutable bool                         decoded = false;
    mutable bool                         decode_failed = false;

    bool empty() const { return bytes.empty(); }
};

// SHA-256 of a byte range, as 64 lowercase hex characters.
std::string image_fill_sha256_hex(const uint8_t *data, size_t len);
inline std::string image_fill_sha256_hex(const std::vector<uint8_t> &v)
{
    return image_fill_sha256_hex(v.data(), v.size());
}

// A content-addressed set of images. Held BY VALUE on Model - it is not an ObjectBase and
// consumes no global id. Copying a Model copies the store; two parts using the same picture
// share one entry, because the key is the content.
class ImageAssetStore
{
public:
    // Stores `bytes` (which must be a PNG) and returns its hash. Storing the same bytes twice
    // is a no-op that returns the same hash, which is the whole point of the content key.
    std::string add(std::vector<uint8_t> bytes);
    const ImageAsset *find(const std::string &sha256) const;
    bool              contains(const std::string &sha256) const { return find(sha256) != nullptr; }
    void              clear() { m_assets.clear(); }
    bool              empty() const { return m_assets.empty(); }
    size_t            size() const { return m_assets.size(); }

    // Decode on demand. Returns nullptr when the asset is missing or is not a PNG this build
    // can decode (PNGReadWrite handles 8-bit RGB/RGBA only - see the spec's "not done" list).
    // Safe to call from several threads at once; the returned asset is complete and immutable.
    const ImageAsset *pixels(const std::string &sha256) const;

    const std::map<std::string, ImageAsset> &assets() const { return m_assets; }
    std::map<std::string, ImageAsset>       &assets() { return m_assets; }

    // Drop every asset no `keep` names. Called by the 3MF writer so a project does not carry
    // pictures nothing points at any more.
    void retain(const std::vector<std::string> &keep);

private:
    std::map<std::string, ImageAsset> m_assets;
};

// ---------------------------------------------------------------------------------------------
// 2. Projections
// ---------------------------------------------------------------------------------------------

enum class ImageFillProjection : int {
    // The image lies in the plane perpendicular to `axis` and is stretched over the part's
    // bounding box in the other two axes.
    Planar = 0,
    // The image wraps once around `axis`: u is the turn angle, v runs along the axis.
    Cylindrical = 1,
    // The mesh's own UVs, supplied by the caller. This is the glTF/GLB import path.
    MeshUV = 2,
    // TRI-PLANAR. Every facet takes the axis of its normal's dominant component and is projected
    // flat along that axis onto the box face it belongs to, so ONE image lands on all six sides
    // of a box. The choice is made from the FACET normal, once per original facet, so a facet
    // never switches face part-way across itself and a curved part is blended by the dominant
    // axis with no mid-facet seams. `axis` and `axis_negative` are ignored: every face is its own
    // axis. See image_fill_box_face() for the orientation rule.
    Box = 3,
};

enum class ImageFillAxis : int { X = 0, Y = 1, Z = 2 };

// WHICH facets a projection is allowed to paint. A projection is a direction, not a solid: a
// planar projection along Z is a picture held above the part and shone down at it, so it lands on
// what faces up and on nothing else. Painting every facet - which is what Phase 2 shipped - puts
// the image on the far face and smears it down the sides, because a side facet's centroid still
// has an (x, y) and so still samples a pixel. That is the bug this enum fixes.
enum class ImageFillFaces : int {
    // Only facets that face the projection: normal . direction > 0. For a cylinder, only the
    // facets whose normal points AWAY from the axis. The default, and what the user expects.
    Facing = 0,
    // Facing and far facets both: |normal . direction| > 0. The image passes through the part and
    // lands on the back as well, seen from behind - so it reads mirrored there, and "Mirror
    // horizontally" is the way to change that. Facets parallel to the direction are still skipped.
    Through = 1,
    // Every facet, parallel ones included. Phase 2's original behaviour, kept so the importers,
    // the tests and an old params string can still ask for it; NOT offered in the dialog, because
    // "the image is smeared down every side" is not something anyone chose on purpose.
    All = 2,
};

// A facet counts as parallel to the projection - and so is never painted by Facing or Through -
// when the cosine between its unit normal and the projection direction is no bigger than this.
// A cube's side facets under a planar projection along Z score exactly 0.
static constexpr float IMAGE_FILL_FACING_EPS = 1e-4f;

// A two- or three-stop linear gradient, used when no image is chosen. Cheap to store (it is
// three colours and a direction), so it needs no asset and no 3MF payload at all.
struct ImageFillGradient
{
    bool                 enabled = false;
    std::array<float, 3> stop_a{0.f, 0.f, 0.f};
    std::array<float, 3> stop_b{1.f, 1.f, 1.f};
    std::array<float, 3> stop_c{1.f, 1.f, 1.f};
    bool                 three_stop = false;
    // 0 = along u, 1 = along v, 2 = radial from the centre of the uv square.
    int                  direction = 1;

    std::array<float, 3> sample(float u, float v) const;
};

// Everything one application of Image Fill needs, and exactly what round-trips through the
// `image_fill_params` config key. Serialised as `k=v;k=v` so it survives the 3MF's
// <metadata key="image_fill_params" value="..."> without any new 3MF element.
struct ImageFillParams
{
    std::string         asset = "";                              // sha256, or empty for a gradient
    ImageFillGradient   gradient;
    ImageFillProjection projection = ImageFillProjection::Planar;
    ImageFillAxis       axis       = ImageFillAxis::Z;
    // Which side of `axis` the projection comes from. false = the + side (a planar projection
    // along Z lands on what faces up); true = the - side. Ignored by MeshUV, and for a
    // cylindrical projection it selects outward-facing (false) or inward-facing (true) surfaces.
    bool                axis_negative = false;
    // Which facets the projection may paint. See ImageFillFaces.
    ImageFillFaces      faces  = ImageFillFaces::Facing;
    bool                flip_u = false;
    bool                flip_v = false;
    // Box only. false (the default) gives each face the same u the PLANAR projection of that
    // face's axis would give it, so the +X, -Y and +Z faces agree with Planar X, Y and Z exactly
    // - and the opposite three (-X, +Y, -Z) read MIRRORED from outside, because you are looking
    // at that projection from behind. true flips u on exactly those three, so text reads
    // correctly from outside on all six. Ignored by every other projection.
    bool                box_mirror = false;
    // 1-based filament ids the solver may choose from. Empty means "every loaded filament",
    // which the caller resolves before calling.
    std::vector<int>    allowed;
    // 1-based id painted where the image is fully transparent or where a facet is outside the
    // selection. 0 leaves those facets unpainted, i.e. the part's own filament.
    int                 background = 0;
    // Uniform subdivision depth, 0..IMAGE_FILL_MAX_SUBDIVISION. Every original facet becomes
    // 4^depth leaves, so the depth is uniform across the volume by construction and the result
    // has no T-joints (see the spec's section on subdivision).
    int                 subdivision = 2;
    // When > 0 the depth is derived from this target facet edge length in millimetres and
    // `subdivision` is only the cap. 0 uses `subdivision` as given.
    float               detail_mm = 0.f;
    // Restrict the fill to facets already painted with this 1-based id (a face selection made
    // with the existing MMU paint tool). 0 = the whole part.
    int                 selection_state = 0;

    std::string to_string() const;
    static bool from_string(const std::string &s, ImageFillParams &out);
    bool        operator==(const ImageFillParams &r) const { return to_string() == r.to_string(); }
};

static constexpr int IMAGE_FILL_MAX_SUBDIVISION = 6;
// A hard ceiling on leaves per volume, so a dense mesh at depth 6 cannot allocate for ever.
// The depth is reduced until the product fits; the dialog reports the depth actually used.
static constexpr size_t IMAGE_FILL_MAX_LEAVES = 4000000;

// The one projection function, shared by every caller. `p` is a point in the volume's own mesh
// space; `box` is the mesh-space box the projection is normalised over. Returns false when the
// point has no sensible image coordinate (a cylindrical projection exactly on the axis, or a Box
// projection asked without the facet normal it needs to pick a face).
//
// `facet_normal` is the ORIGINAL facet's normal - need not be normalised - and is read only by
// ImageFillProjection::Box, which cannot choose a face without it. Passing it per leaf of one
// facet is what keeps a facet from switching face part-way across itself.
bool image_fill_project(const ImageFillParams &params, const BoundingBoxf3 &box, const Vec3f &p,
                        float &u, float &v, const Vec3f *facet_normal = nullptr);

// Which face of an axis-aligned box a facet belongs to, from its normal's dominant component:
//
//   0 = +X   1 = -X   2 = +Y   3 = -Y   4 = +Z   5 = -Z
//
// (the same numbering the box-face tests use). A tie goes to the LOWER axis index - X, then Y,
// then Z - so a facet at exactly 45 degrees between two axes still lands somewhere definite and
// the same way on every run. Returns -1 only for a degenerate, zero-length normal.
//
// THE ORIENTATION RULE, which this face number decides:
//
//   * v - "up" in the image - is +Z on the four SIDE faces (dominant axis X or Y) and +Y on the
//     TOP and the BOTTOM (dominant axis Z). Always, in both mirror modes.
//   * u - "right" in the image - is the remaining world axis, and by default runs in its POSITIVE
//     direction on every face. That makes the +X, -Y and +Z faces identical to a planar
//     projection along X, Y and Z, and leaves -X, +Y and -Z reading mirrored from outside.
//   * ImageFillParams::box_mirror flips u on those three faces, which is exactly u = v x n for
//     every face - the condition for the image to read the right way round from outside.
//
//     face  |  u (image right)        |  v (image up)   |  u when box_mirror
//     ------+-------------------------+-----------------+-------------------
//      +X   |  +Y                     |  +Z             |  +Y  (unchanged)
//      -X   |  +Y                     |  +Z             |  -Y
//      +Y   |  +X                     |  +Z             |  -X
//      -Y   |  +X                     |  +Z             |  +X  (unchanged)
//      +Z   |  +X                     |  +Y             |  +X  (unchanged)
//      -Z   |  +X                     |  +Y             |  -X
//
// flip_u and flip_v still apply on top, globally, as they do for every other projection.
int image_fill_box_face(const Vec3f &normal);

// The unit direction a planar projection travels along: +axis, or -axis when axis_negative.
Vec3f image_fill_direction(const ImageFillParams &params);

// Does the projection land on this facet? `normal` need not be normalised; `centroid` is the
// facet's centre in the same mesh space as `box`, and is what a cylindrical projection needs to
// know which way "outward" is for this facet. Always true for MeshUV (the UVs decide coverage),
// for ImageFillFaces::All, and for Box - under a tri-planar projection every facet faces its own
// axis by construction, so there is nothing for the faces rule to exclude and `faces` is
// meaningless there (the dialog disables it).
//
// The rule, in one line: Facing is normal . direction > IMAGE_FILL_FACING_EPS, Through is
// |normal . direction| > IMAGE_FILL_FACING_EPS, and a facet parallel to the direction - a cube's
// sides under a planar projection along Z, a cylinder's end caps under a cylindrical one - is
// painted by neither.
bool image_fill_face_is_painted(const ImageFillParams &params, const BoundingBoxf3 &box,
                                const Vec3f &normal, const Vec3f &centroid);

// ---------------------------------------------------------------------------------------------
// 3. The per-facet answer
// ---------------------------------------------------------------------------------------------

// The palette an image is quantised to before the solver is asked anything. The plan's phase 1
// note is explicit that a solve costs real time, so the image is reduced to at most
// `max_colors` representatives first and the solver is asked once per representative.
struct ImageFillPalette
{
    std::vector<std::array<float, 3>> colors;   // sRGB 0..1
    std::vector<size_t>               counts;   // how many samples landed on each
    // Per palette entry, the 1-based filament id the solver chose. Filled by image_fill_solve().
    std::vector<int>                  filament;
};

// Quantise `samples` (sRGB 0..1) to at most `max_colors` representatives. Deterministic: a
// fixed 5-bit-per-channel bucket pass followed by a frequency-ordered merge, no randomness and
// no iteration count, so two runs on the same image give the same palette in the same order.
ImageFillPalette image_fill_quantise(const std::vector<std::array<float, 3>> &samples, size_t max_colors);

// Ask the phase 1 colour solver which of `filament_colors` (parallel to `filament_ids`, both
// 1-based-id-ordered) each palette entry should be printed with.
//
// Phase 2 writes ONE filament id per facet, so the solver is called with
// ColorSolverConstraints{max_components = 1}: the constraint says "a facet can only be one
// filament" and the solver answers "which one", measured in Oklab with the chroma-dependent
// axis weights - which is the point of using it instead of ObjColorMatch's CIE76 pass. Phase 3
// lifts the constraint and the same call returns mixes.
void image_fill_solve(ImageFillPalette &palette, const std::vector<std::array<float, 3>> &filament_colors,
                      const std::vector<int> &filament_ids);

// ---------------------------------------------------------------------------------------------
// 4. Applying it
// ---------------------------------------------------------------------------------------------

struct ImageFillResult
{
    bool                     ok = false;
    std::string              error;
    int                      subdivision_used = 0;
    size_t                   facets_painted   = 0;   // leaves that got a filament id
    size_t                   leaves_total     = 0;
    std::vector<int>         filaments_used;         // ascending 1-based ids actually written
    // What the dialog draws as the preview: one colour per palette entry, and the share of the
    // surface each covers.
    ImageFillPalette         palette;
    TriangleSelector::TriangleSplittingData painting;
};

// Texture coordinates for the MeshUV projection: THREE per triangle, in that triangle's own
// vertex order, so the array is indexed by face and not by vertex. Per face rather than per
// vertex on purpose - a glTF exporter splits a vertex at every UV seam, and the importer has to
// weld those vertices back together before the mesh is printable, which destroys a per-vertex UV
// array but leaves a per-face one exactly right. Empty for the other projections.
using ImageFillUVs = std::vector<std::array<Vec2f, 3>>;

// The service. Samples `mesh` through the projection, quantises, solves, and returns the
// painting to write into ModelVolume::mmu_segmentation_facets.
//
// `existing` is the volume's current painting; it is read for `selection_state` and is
// otherwise replaced. `filament_colors` / `filament_ids` are the allowed palette, already
// filtered by ImageFillParams::allowed.
ImageFillResult image_fill_compute(const indexed_triangle_set                &mesh,
                                   const TriangleSelector::TriangleSplittingData &existing,
                                   const ImageFillParams                     &params,
                                   const ImageAssetStore                     &assets,
                                   const std::vector<std::array<float, 3>>   &filament_colors,
                                   const std::vector<int>                    &filament_ids,
                                   const ImageFillUVs                        &uvs = {});

// The convenience wrapper the dialog, the object-list menu item and the glTF importer all use:
// compute, write mmu_segmentation_facets, and record the params on volume->config so a reload
// can re-open the dialog with the same settings.
ImageFillResult image_fill_apply(ModelVolume &volume, const ImageFillParams &params,
                                 const ImageAssetStore                   &assets,
                                 const std::vector<std::array<float, 3>> &filament_colors,
                                 const std::vector<int>                  &filament_ids,
                                 const ImageFillUVs                      &uvs = {});

// Read back what image_fill_apply recorded, so the dialog can re-open on the same settings.
bool image_fill_params_of(const ModelVolume &volume, ImageFillParams &out);

// ---------------------------------------------------------------------------------------------
// 5. Per-face colours - the path the glTF importer takes
// ---------------------------------------------------------------------------------------------

// One colour per triangle of `mesh` (the glTF importer's centroid texture samples, or an OBJ's
// mtl face colours), turned into MMU painting through the same quantise + solve the projection
// path uses. This is what replaces Model.cpp's paint_volume_from_face_colors + ObjColorMatch's
// CIE76 nearest-single matcher for the texture case.
//
// `face_colors` holds 4^`depth` entries per triangle, in image_fill_subdivide's leaf order -
// so `depth` 0 is the plain "one colour per triangle" case the OBJ and glTF importers have
// always had, and a positive depth is the same importer sampling its texture per sub-facet. The
// quantise, the solve and the bitstream are the same ones the projection path uses; only the
// sampling happens elsewhere, because the texture and the pre-weld UVs live in the reader.
// Facets whose colour lands on `background`
// are left unpainted, exactly as the old path skipped ids <= 1.
ImageFillResult image_fill_from_face_colors(const indexed_triangle_set              &mesh,
                                            const std::vector<std::array<float, 3>> &face_colors,
                                            const std::vector<std::array<float, 3>> &filament_colors,
                                            const std::vector<int>                  &filament_ids,
                                            int                                      background = 0,
                                            int                                      depth      = 0);

// ---------------------------------------------------------------------------------------------
// 6. Subdivision, exposed for the tests
// ---------------------------------------------------------------------------------------------

// The leaves of a uniform depth-`depth` subdivision of one triangle, in the order
// TriangleSelector's own tree produces them, with their three mesh-space corners.
// A leaf's state is written by the caller; the encoder below turns (depth, states) into the
// bitstream TriangleSelector::deserialize reads.
struct ImageFillLeaf
{
    Vec3f a, b, c;
    Vec3f centroid() const { return (a + b + c) / 3.f; }
};
void image_fill_subdivide(const Vec3f &a, const Vec3f &b, const Vec3f &c, int depth,
                          std::vector<ImageFillLeaf> &out);

// Encode a uniform-depth subdivision with one state per leaf into TriangleSelector's bitstream.
// `states` holds 4^depth entries per original triangle, in image_fill_subdivide's order.
// Original triangles whose leaves are all state 0 are omitted, exactly as serialize() omits them.
//
// `selected` and `existing`, when both given, make this a MERGE rather than a replacement: an
// original triangle the caller did not select keeps whatever `existing` said about it, copied
// bit-for-bit out of its bitstream. That is what makes "apply the image only to these faces" leave
// the rest of the part's painting alone instead of erasing it. `selected` must have one entry per
// original triangle.
TriangleSelector::TriangleSplittingData image_fill_encode(
    size_t n_original_triangles, int depth, const std::vector<int> &states,
    const std::vector<bool>                       *selected = nullptr,
    const TriangleSelector::TriangleSplittingData *existing = nullptr);

// How deep to go so that a triangle whose longest edge is `max_edge_mm` ends up with edges of
// about `detail_mm`, capped at `cap` and at IMAGE_FILL_MAX_LEAVES over `n_triangles`.
int image_fill_depth_for_detail(float max_edge_mm, float detail_mm, int cap, size_t n_triangles);

// ---------------------------------------------------------------------------------------------
// 7. Phase 3, step 2: the image row sampler - one fill segment, N samples along it
// ---------------------------------------------------------------------------------------------
//
// Spec: docs/superpowers/specs/2026-09-07-imagemap-edgeslicer-plan.md, Phase 3. Everything below
// stays outside the slicing pipeline: it is called by nothing in Fill.cpp, LayerRegion.cpp or
// ToolOrdering.cpp yet - that wiring is a later step. What is here answers one question a caller
// in that pipeline will eventually ask: "what colour does the image put at this point on this
// fill segment", at extrusion-width resolution instead of facet resolution.

// The pixel a decoded (u, v) reads, top row first, clamped, nearest-neighbour. Exposed so
// image_fill_sample_segment() and image_fill_compute()'s facet painting read the SAME function -
// which is what makes a segment lying in a facet's own plane sample identically to that facet's
// own leaf centroids: both end up computing image_fill_project() then this, on the same inputs.
std::array<float, 3> image_fill_sample_pixel(const ImageAsset &asset, float u, float v);

// One sample along a fill segment.
struct ImageRowSample
{
    float                s = 0.f;    // arc length from the segment's start, same units as p0/p1
    Vec3f                pos = Vec3f(0.f, 0.f, 0.f);
    // false where the projection declines a colour at this point - the same cases
    // image_fill_project() itself declines (a cylindrical projection exactly on the axis, a Box
    // projection off a degenerate facet normal), or where there is neither an image nor an
    // enabled gradient to sample.
    bool                 has_color = false;
    std::array<float, 3> color{0.f, 0.f, 0.f};   // sRGB 0..1, meaningful only when has_color
};

// Samples the image (or the gradient, when `params.asset` is empty) at evenly spaced points
// along the straight segment `p0` -> `p1`, both in the volume's own mesh space (their z already
// carries the layer height a caller in the real pipeline would supply). Reuses
// image_fill_project() - the same function image_fill_compute() calls per leaf centroid - so a
// segment lying in a facet's plane gets EXACTLY the facet painter's u, v at any point on that
// plane, and therefore exactly its colour: this is what "a segment on the top face maps where
// phase 2's facet painting maps it" means, made precise enough to test.
//
// `facet_normal` is the plane the segment lies in - the caller's job to supply, exactly as
// image_fill_compute() computes it once per original facet and reuses it for every leaf. It is
// read only by a Box projection (to choose a face) and by the `faces` culling rule, which this
// function does NOT apply - a fill segment was already placed on a facet the caller decided is
// part of the fill, so "is this facet facing the projection" is not this function's question to
// re-ask; it always returns a colour when image_fill_project() succeeds, for every projection
// except MeshUV, which a bare position cannot resolve without the triangle it came from (that is
// the caller's job in the real pipeline, not this pure function's - see the header note above).
//
// There are always at least 1 sample; for a non-degenerate segment there are at least 2 (both
// endpoints), spaced ~`spacing_mm` apart with the endpoints always exact even when that shortens
// the last interval - so two segments sharing an endpoint always agree on that shared point's
// colour, which matters once fill segments are stitched into a fill line.
std::vector<ImageRowSample> image_fill_sample_segment(const ImageFillParams &params, const BoundingBoxf3 &box,
                                                      const ImageAssetStore &assets, const Vec3f &p0, const Vec3f &p1,
                                                      const Vec3f &facet_normal, float spacing_mm);

// ---------------------------------------------------------------------------------------------
// 8. Phase 3, step 3: the XY split with dither - sampled colours become filament runs
// ---------------------------------------------------------------------------------------------

// One run of one filament id along a sampled fill segment, in the same arc-length units
// (`ImageRowSample::s`) the sampler above reports.
struct ImageRowRun
{
    float s0 = 0.f;          // start, inclusive, mm from the segment's start
    float s1 = 0.f;          // end, inclusive, mm from the segment's start
    int   filament_id = 0;   // one of candidate_ids, verbatim

    float length() const { return s1 - s0; }
};

// Turns a sampled colour ramp into a sequence of single-filament runs by 1-D error-diffusion
// dithering against the row's own reachable colours (`candidate_colors`, parallel to
// `candidate_ids` - phase 1's solver output for an ImageWeighted row's allowed filaments, or
// simply the row's fixed physical pair; either way, plain (id, colour) pairs is all this
// function needs to know about a "reachable mix").
//
// Per sample i, in order:
//   target         = colors[i] + carried_error                 (carried_error starts at 0)
//   chosen         = the candidate in candidate_colors nearest `target` (squared sRGB distance -
//                    the same space image_fill_compute's own nearest-palette lookup measures in)
//   carried_error <- target - candidate_colors[chosen]          (passed to sample i + 1)
//
// That is "dithering between the two or three nearest reachable mixes" made concrete: choosing
// the single nearest candidate to a running target and carrying the leftover error forward is
// exactly Floyd-Steinberg's rule generalised from two grey levels to a small palette, and it
// finds the same answer a fixed local shortlist would - the nearest candidate to any target is,
// by definition, always inside whatever "nearest few" shortlist a caller could have precomputed,
// so nothing is lost by searching the whole list fresh each time, and nothing about the search
// depends on iteration order or randomness: two calls on identical input produce identical
// output. A sample with `has_color == false` extends the previous sample's run without updating
// the carried error (there is no colour there to diffuse); the first such sample, if there is no
// previous one, takes `candidate_ids.front()`.
//
// Consecutive samples that dither to the same id merge into one run. A run shorter than
// `min_run_len_mm` - shorter than the nozzle can print - is folded into whichever neighbour run's
// own candidate colour is closer to its own (the edge run has only one neighbour), repeated until
// every run meets the floor or only one run is left; adjacent runs merged down to the same id are
// then joined, so the output never has two consecutive entries with the same filament_id.
std::vector<ImageRowRun> image_fill_dither_segment(const std::vector<ImageRowSample>       &samples,
                                                   const std::vector<std::array<float, 3>> &candidate_colors,
                                                   const std::vector<int>                  &candidate_ids,
                                                   float                                     min_run_len_mm);

} // namespace Slic3r

#endif // slic3r_ImageFill_hpp_
