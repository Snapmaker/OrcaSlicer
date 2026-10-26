// Phase 1 trimmed copy of upstream TexturePainting.cpp (upstream OrcaSlicer
// main @ 069bd402, 758 lines). Kept here:
//   - decode_texture_image / build_multi_texture_atlas (upstream :36-283)
//   - decode_texture_to_pixels / sample_face_from_uvs / sample_face_from_texture /
//     sample_original_face_colors (upstream :618-756)
// Stubbed until Phase 2 (P2.1 ports TextureToColor/ + TextureDisplacement.hpp):
//   - texture_to_painting / face_colors_to_painting / compute_delta_e /
//     match_clusters_to_filaments / apply_painted_mesh_to_volume (upstream :285-616)
//   - build_tex2color_mesh / extract_painted_mesh (upstream :63-138, only used by
//     the functions above)
#include "TexturePainting.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/core/mat.hpp>
#include <opencv2/core/matx.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <boost/log/trivial.hpp>

#include "Point.hpp"
// Phase 2 (P2.1/P2.2) restores these upstream includes with the full algorithm:
//   #include "TextureToColor/Callbacks.hpp"
//   #include "TextureToColor/TextureToColor.hpp"
//   #include "TextureToColor/ColorUtils.hpp"
//   #include "TextureToColor/TriMesh.hpp"
//   #include "TextureDisplacement.hpp"
#include "Model.hpp"
#include "TriangleMesh.hpp"
#include "TriangleSelector.hpp"
#include "BoundingBox.hpp"

namespace Slic3r {

static cv::Mat decode_texture_image(const TextureImage& img) {
    if (img.data.empty())
        return {};

    // Raw encoded image data (PNG/JPEG) from glTF loader: width == -1
    if (img.width <= 0 || img.height <= 0) {
        std::vector<unsigned char> buf(img.data.begin(), img.data.end());
        cv::Mat raw(1, static_cast<int>(buf.size()), CV_8UC1, buf.data());
        cv::Mat decoded = cv::imdecode(raw, cv::IMREAD_COLOR);
        return decoded;
    }

    int cv_type = (img.channels == 4) ? CV_8UC4 : CV_8UC3;
    std::vector<unsigned char> pixel_buf(img.data.begin(), img.data.end());
    cv::Mat src(img.height, img.width, cv_type, pixel_buf.data());

    cv::Mat bgr;
    if (img.channels == 4)
        cv::cvtColor(src, bgr, cv::COLOR_RGBA2BGR);
    else if (img.channels == 3)
        cv::cvtColor(src, bgr, cv::COLOR_RGB2BGR);
    else
        return {};

    return bgr;
}

// Phase 2 stub — copy from upstream :63-112 with TextureToColor/TriMesh.hpp.
// static void build_tex2color_mesh(
//     const TexturedMesh& textured,
//     tex2color::TriMesh& mesh,
//     std::vector<std::vector<Vec2f>>& uv_coords)

// Phase 2 stub — copy from upstream :114-138 with TextureToColor/TriMesh.hpp.
// static void extract_painted_mesh(
//     const tex2color::TriMesh& color_mesh,
//     const std::vector<std::array<std::size_t,3>>& face_colors,
//     PaintedMesh& painted)

// Build a vertically-stacked atlas from multiple textures and remap per-face UVs.
//
// Sub-textures are laid out left-aligned (x=0) at successive y offsets, with
// atlas_w taken as the maximum width across all sub-textures. UVs must therefore
// be remapped on BOTH axes so that faces belonging to a sub-texture narrower
// than atlas_w sample inside that sub-texture's region (left side of the atlas)
// instead of the right-side zero-padding. Materials that carry only a baseColor
// (no map_Kd / glTF baseColorTexture) get their own 1x1 swatch at the bottom of
// the atlas so their faces sample the correct flat colour rather than being
// silently aliased onto textures[0].
static bool build_multi_texture_atlas(
    const TexturedMesh& textured,
    cv::Mat& out_atlas,
    std::vector<std::vector<Vec2f>>& out_uv_coords)
{
    std::vector<cv::Mat> decoded;
    decoded.reserve(textured.textures.size());
    for (const auto& ti : textured.textures)
        decoded.push_back(decode_texture_image(ti));

    const bool   has_mapping = !textured.material_texture_map.empty();
    const size_t nf          = textured.indices.size();

    auto resolve_tex_idx = [&](int mat_idx) -> int {
        if (!has_mapping || mat_idx < 0
            || static_cast<size_t>(mat_idx) >= textured.material_texture_map.size())
            return -1;
        const int ti = textured.material_texture_map[mat_idx];
        if (ti < 0 || static_cast<size_t>(ti) >= decoded.size() || decoded[ti].empty())
            return -1;
        return ti;
    };

    // Determine atlas width (max width across all textures) and per-texture row offsets.
    int atlas_w = 0;
    int atlas_h = 0;
    std::vector<int> y_offsets(decoded.size(), 0);
    int first_usable_tex = -1;
    for (size_t i = 0; i < decoded.size(); ++i) {
        if (decoded[i].empty()) continue;
        if (first_usable_tex < 0) first_usable_tex = static_cast<int>(i);
        y_offsets[i] = atlas_h;
        atlas_w = std::max(atlas_w, decoded[i].cols);
        atlas_h += decoded[i].rows;
    }
    if (atlas_w == 0 || atlas_h == 0)
        return false;

    // Collect materials that have a baseColor but no usable texture so we can
    // route their faces to a dedicated 1x1 solid swatch instead of aliasing
    // them onto textures[0].
    std::map<int, int>                 mat_solid_y;     // mat_idx -> y row in atlas
    std::map<int, std::array<float,4>> mat_solid_color; // mat_idx -> baseColor (RGBA)
    for (size_t fi = 0; fi < nf; ++fi) {
        const int mat_idx = (fi < textured.material_ids.size()) ? textured.material_ids[fi] : -1;
        if (mat_idx < 0) continue;
        if (resolve_tex_idx(mat_idx) >= 0) continue;
        if (static_cast<size_t>(mat_idx) >= textured.material_colors.size()) continue;
        if (mat_solid_y.find(mat_idx) != mat_solid_y.end()) continue;
        mat_solid_y[mat_idx]     = atlas_h++;
        mat_solid_color[mat_idx] = textured.material_colors[mat_idx];
    }

    out_atlas = cv::Mat::zeros(atlas_h, atlas_w, CV_8UC3);
    for (size_t i = 0; i < decoded.size(); ++i) {
        if (decoded[i].empty()) continue;
        cv::Mat roi = out_atlas(cv::Rect(0, y_offsets[i], decoded[i].cols, decoded[i].rows));
        decoded[i].copyTo(roi);
    }
    for (const auto& kv : mat_solid_color) {
        const auto& c = kv.second;
        // OpenCV stores BGR; baseColor is RGBA in [0,1].
        out_atlas.at<cv::Vec3b>(mat_solid_y[kv.first], 0) = cv::Vec3b(
            static_cast<uchar>(std::clamp(c[2] * 255.f, 0.f, 255.f)),
            static_cast<uchar>(std::clamp(c[1] * 255.f, 0.f, 255.f)),
            static_cast<uchar>(std::clamp(c[0] * 255.f, 0.f, 255.f)));
    }

    out_uv_coords.resize(nf);
    for (size_t fi = 0; fi < nf; ++fi) {
        const int mat_idx = (fi < textured.material_ids.size()) ? textured.material_ids[fi] : -1;
        const int tex_idx = resolve_tex_idx(mat_idx);

        // Pick the atlas region this face samples from.
        int  y_off = 0, x_off = 0, th = atlas_h, tw = atlas_w;
        bool use_solid = false;
        if (tex_idx >= 0) {
            y_off = y_offsets[tex_idx];
            th    = decoded[tex_idx].rows;
            tw    = decoded[tex_idx].cols;
        } else if (mat_idx >= 0 && mat_solid_y.count(mat_idx) > 0) {
            y_off     = mat_solid_y[mat_idx];
            th        = 1;
            tw        = 1;
            use_solid = true;
        } else if (first_usable_tex >= 0) {
            // Last-resort fallback: faces without a material or without any
            // baseColor still need somewhere to sample; the first usable
            // texture preserves legacy behaviour and, with the per-axis
            // remapping below, no longer aliases onto the zero-padded right
            // margin even when sub-textures have unequal widths.
            y_off = y_offsets[first_usable_tex];
            th    = decoded[first_usable_tex].rows;
            tw    = decoded[first_usable_tex].cols;
        }

        out_uv_coords[fi].resize(3);
        for (int vi = 0; vi < 3; ++vi) {
            float u = 0.f, v = 0.f;
            if (textured.has_face_uvs()) {
                int uv_idx = textured.uv_indices[fi][vi];
                if (uv_idx >= 0 && static_cast<size_t>(uv_idx) < textured.uv_coords.size()) {
                    u = textured.uv_coords[uv_idx][0];
                    v = textured.uv_coords[uv_idx][1];
                }
            } else {
                int vtx_idx = textured.indices[fi][vi];
                if (vtx_idx >= 0 && static_cast<size_t>(vtx_idx) < textured.uvs.size()) {
                    u = textured.uvs[vtx_idx][0];
                    v = textured.uvs[vtx_idx][1];
                }
            }
            if (use_solid) {
                // Aim at the centre of the 1x1 swatch so bilinear sampling
                // (in tex2color) cannot drift into neighbouring rows.
                const float u_atlas = (x_off + 0.5f) / static_cast<float>(atlas_w);
                const float v_atlas = (y_off + 0.5f) / static_cast<float>(atlas_h);
                out_uv_coords[fi][vi] = Vec2f(u_atlas, v_atlas);
            } else {
                // Wrap to [0,1) on both axes (OBJ tile UVs may step outside
                // the unit square), then scale by the sub-texture extents so
                // samples land inside its actual region. Without scaling u,
                // any sub-texture narrower than atlas_w would have all its
                // faces sampled from the right-side zero-padding.
                u = u - std::floor(u);
                v = v - std::floor(v);
                const float u_atlas = (x_off + u * tw) / static_cast<float>(atlas_w);
                const float v_atlas = (y_off + v * th) / static_cast<float>(atlas_h);
                out_uv_coords[fi][vi] = Vec2f(u_atlas, v_atlas);
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Phase 1 stubs. Full bodies return in Phase 2: copy from upstream
// TexturePainting.cpp :285-616 once TextureToColor/ and TextureDisplacement.hpp
// are ported (P2.1/P2.2). Signatures match TexturePainting.hpp so callers and
// the dialog compile unchanged.
// ---------------------------------------------------------------------------

// Phase 2: upstream :285-366.
bool texture_to_painting(
    const TexturedMesh& /*textured*/,
    PaintedMesh& /*painted*/,
    const TexturePaintingSettings& /*settings*/,
    PaintProgressCallback /*progress*/,
    PaintCancelCallback /*cancel*/)
{
    return false;
}

// Phase 2: upstream :368-429.
bool face_colors_to_painting(
    const TexturedMesh& /*mesh*/,
    PaintedMesh& /*painted*/,
    const TexturePaintingSettings& /*settings*/,
    PaintProgressCallback /*progress*/,
    PaintCancelCallback /*cancel*/)
{
    return false;
}

// Phase 2: upstream :431-442 (tex2color::color_utils::calc_rgb_color_difference_by_ciede2000).
double compute_delta_e(
    const std::array<std::size_t,3>& /*rgb1*/,
    const std::array<float,4>& /*rgba2*/)
{
    return 0.0;
}

// Phase 2: upstream :444-466.
std::vector<FilamentMatch> match_clusters_to_filaments(
    const std::vector<std::array<std::size_t,3>>& /*cluster_colors*/,
    const std::vector<std::array<float,4>>& /*filament_colors*/,
    const std::vector<std::string>& /*filament_names*/)
{
    return {};
}

// Phase 2: upstream :468-616 (mmu segmentation write-back via TriangleSelector).
bool apply_painted_mesh_to_volume(
    const PaintedMesh& /*painted*/,
    const std::vector<FilamentMatch>& /*matches*/,
    ModelVolume& /*volume*/)
{
    return false;
}

// ---------------------------------------------------------------------------

bool decode_texture_to_pixels(
    const TextureImage& img,
    std::vector<unsigned char>& out_pixels,
    int& out_w, int& out_h)
{
    cv::Mat decoded = decode_texture_image(img);
    if (decoded.empty())
        return false;

    // decoded is BGR, CV_8UC3
    out_w = decoded.cols;
    out_h = decoded.rows;
    size_t nbytes = (size_t)out_w * out_h * 3;
    out_pixels.resize(nbytes);

    if (decoded.isContinuous()) {
        std::memcpy(out_pixels.data(), decoded.data, nbytes);
    } else {
        for (int r = 0; r < out_h; ++r)
            std::memcpy(out_pixels.data() + r * out_w * 3, decoded.ptr(r), out_w * 3);
    }
    return true;
}

// Sample face color from texture using 3 explicit UV values (centroid + bilinear).
static std::array<std::size_t,3> sample_face_from_uvs(
    const cv::Mat& tex,
    const std::array<float,2>& uv0,
    const std::array<float,2>& uv1,
    const std::array<float,2>& uv2)
{
    float cu = (uv0[0] + uv1[0] + uv2[0]) / 3.f;
    float cv_val = (uv0[1] + uv1[1] + uv2[1]) / 3.f;

    cu = cu - std::floor(cu);
    cv_val = cv_val - std::floor(cv_val);

    float fx = cu * (tex.cols - 1);
    float fy = cv_val * (tex.rows - 1);

    int x0 = std::clamp(static_cast<int>(fx), 0, tex.cols - 1);
    int y0 = std::clamp(static_cast<int>(fy), 0, tex.rows - 1);
    int x1 = std::min(x0 + 1, tex.cols - 1);
    int y1 = std::min(y0 + 1, tex.rows - 1);

    float wx = fx - x0;
    float wy = fy - y0;

    const int ch = tex.channels();
    auto sample = [&](int row, int col) -> std::array<float,3> {
        const uchar* ptr = tex.data + row * tex.step[0] + col * ch;
        return {static_cast<float>(ptr[2]), static_cast<float>(ptr[1]), static_cast<float>(ptr[0])};
    };

    auto c00 = sample(y0, x0);
    auto c10 = sample(y0, x1);
    auto c01 = sample(y1, x0);
    auto c11 = sample(y1, x1);

    std::array<std::size_t,3> color;
    for (int i = 0; i < 3; ++i) {
        float top = c00[i] * (1.f - wx) + c10[i] * wx;
        float bot = c01[i] * (1.f - wx) + c11[i] * wx;
        color[i] = static_cast<std::size_t>(std::clamp(top * (1.f - wy) + bot * wy, 0.f, 255.f));
    }
    return color;
}

// Legacy overload: look up UVs from per-vertex array by vertex indices.
static std::array<std::size_t,3> sample_face_from_texture(
    const cv::Mat& tex,
    const std::vector<std::array<float,2>>& uvs,
    const std::array<int,3>& face)
{
    std::array<float,2> uv0 = {0.f, 0.f}, uv1 = {0.f, 0.f}, uv2 = {0.f, 0.f};
    if (face[0] >= 0 && static_cast<size_t>(face[0]) < uvs.size()) uv0 = uvs[face[0]];
    if (face[1] >= 0 && static_cast<size_t>(face[1]) < uvs.size()) uv1 = uvs[face[1]];
    if (face[2] >= 0 && static_cast<size_t>(face[2]) < uvs.size()) uv2 = uvs[face[2]];
    return sample_face_from_uvs(tex, uv0, uv1, uv2);
}

bool sample_original_face_colors(
    const TexturedMesh& textured,
    std::vector<std::array<std::size_t,3>>& out_face_colors)
{
    if (textured.indices.empty())
        return false;

    // Decode all textures up front
    std::vector<cv::Mat> decoded_textures;
    decoded_textures.reserve(textured.textures.size());
    for (const auto& ti : textured.textures) {
        decoded_textures.push_back(decode_texture_image(ti));
    }

    const bool has_mapping = !textured.material_texture_map.empty();
    const size_t nf = textured.indices.size();
    out_face_colors.resize(nf);

    for (size_t fi = 0; fi < nf; ++fi) {
        int mat_idx = (fi < textured.material_ids.size()) ? textured.material_ids[fi] : -1;

        int tex_idx = -1;
        if (has_mapping && mat_idx >= 0 && static_cast<size_t>(mat_idx) < textured.material_texture_map.size())
            tex_idx = textured.material_texture_map[mat_idx];
        else if (!decoded_textures.empty())
            tex_idx = 0; // fallback: single-texture model

        if (tex_idx >= 0 && static_cast<size_t>(tex_idx) < decoded_textures.size()
            && !decoded_textures[tex_idx].empty()) {
            if (textured.has_face_uvs()) {
                const auto& ui = textured.uv_indices[fi];
                auto get_uv = [&](int vi) -> std::array<float,2> {
                    int idx = ui[vi];
                    if (idx >= 0 && static_cast<size_t>(idx) < textured.uv_coords.size())
                        return textured.uv_coords[idx];
                    return {0.f, 0.f};
                };
                out_face_colors[fi] = sample_face_from_uvs(
                    decoded_textures[tex_idx], get_uv(0), get_uv(1), get_uv(2));
            } else {
                out_face_colors[fi] = sample_face_from_texture(
                    decoded_textures[tex_idx], textured.uvs, textured.indices[fi]);
            }
        } else if (has_mapping && mat_idx >= 0
                   && static_cast<size_t>(mat_idx) < textured.material_colors.size()) {
            // No texture — use baseColorFactor as solid color
            const auto& c = textured.material_colors[mat_idx];
            out_face_colors[fi] = {
                static_cast<std::size_t>(std::clamp(c[0] * 255.f, 0.f, 255.f)),
                static_cast<std::size_t>(std::clamp(c[1] * 255.f, 0.f, 255.f)),
                static_cast<std::size_t>(std::clamp(c[2] * 255.f, 0.f, 255.f))
            };
        } else {
            out_face_colors[fi] = {192, 192, 192}; // default gray
        }
    }
    return true;
}

} // namespace Slic3r
