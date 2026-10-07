#include "ImageTrace.hpp"

#include <algorithm>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <limits>
#include <numeric>
#include <sstream>

#include <boost/beast/core/detail/base64.hpp>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <png.h>
#include <jpeglib.h>

#include "nlohmann/json.hpp"

#include "BoundingBox.hpp"
#include "ClipperUtils.hpp"
#include "CodeEmboss.hpp" // create_code_group_id
#include "EmbossShape.hpp"
#include "Model.hpp"
#include "NSVGUtils.hpp"
#include "UntrustedInput.hpp"

namespace Slic3r {

// Named namespace instead of an anonymous one: the file is compiled in unity batches
namespace image_trace_detail {

constexpr const char *META_BEGIN = "<metadata id=\"edgeslicer-imagetrace\">";
constexpr const char *META_END   = "</metadata>";
// Version of stored metadata
constexpr int META_FORMAT = 1;

// Longest side of the image used for decisions, which have to be the same for the preview and
// for the final trace (palette, background, automatic threshold)
constexpr int ANALYSIS_SIDE = 256;
// More rings than this in one level is noise (photo with a bad threshold), refuse early
constexpr size_t MAX_RINGS = 100000;
// Alpha below this is transparent
constexpr uint8_t ALPHA_SOLID = 128;

uint32_t be32(const std::string &d, size_t i)
{
    return (uint32_t(uint8_t(d[i])) << 24) | (uint32_t(uint8_t(d[i + 1])) << 16) | (uint32_t(uint8_t(d[i + 2])) << 8) | uint32_t(uint8_t(d[i + 3]));
}
uint32_t be16(const std::string &d, size_t i) { return (uint32_t(uint8_t(d[i])) << 8) | uint32_t(uint8_t(d[i + 1])); }
uint32_t le16(const std::string &d, size_t i) { return uint32_t(uint8_t(d[i])) | (uint32_t(uint8_t(d[i + 1])) << 8); }
int32_t  le32(const std::string &d, size_t i)
{
    return int32_t(uint32_t(uint8_t(d[i])) | (uint32_t(uint8_t(d[i + 1])) << 8) | (uint32_t(uint8_t(d[i + 2])) << 16) |
                   (uint32_t(uint8_t(d[i + 3])) << 24));
}

long long coord_to_um(coord_t c) { return std::llround(double(c) * SCALING_FACTOR * 1000.); }

std::string um_to_mm_string(long long um)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%s%lld.%03lld", um < 0 ? "-" : "", std::llabs(um) / 1000, std::llabs(um) % 1000);
    return buffer;
}

std::string meta_escape(const std::string &text)
{
    std::string result;
    result.reserve(text.size());
    for (char c : text) {
        switch (c) {
        case '&': result += "&amp;"; break;
        case '<': result += "&lt;"; break;
        case '>': result += "&gt;"; break;
        default: result += c;
        }
    }
    return result;
}

std::string meta_unescape(const std::string &text)
{
    std::string result;
    result.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '&') {
            static const std::pair<const char *, char> entities[] = {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
            bool found = false;
            for (const auto &[name, c] : entities) {
                size_t len = std::char_traits<char>::length(name);
                if (text.compare(i, len, name) == 0) {
                    result += c;
                    i += len - 1;
                    found = true;
                    break;
                }
            }
            if (!found)
                result += text[i];
        } else
            result += text[i];
    }
    return result;
}

std::string base64_encode(const std::string &data)
{
    namespace b64 = boost::beast::detail::base64;
    std::string out(b64::encoded_size(data.size()), '\0');
    out.resize(b64::encode(out.data(), data.data(), data.size()));
    return out;
}

std::string base64_decode(const std::string &text)
{
    namespace b64 = boost::beast::detail::base64;
    std::string out(b64::decoded_size(text.size()), '\0');
    auto [written, read] = b64::decode(out.data(), text.data(), text.size());
    out.resize(written);
    return out;
}

double luminance(const uint8_t *rgb) { return 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2]; }
double luminance(const std::array<double, 3> &c) { return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]; }

double dist2(const std::array<double, 3> &a, const std::array<double, 3> &b)
{
    double dr = a[0] - b[0], dg = a[1] - b[1], db = a[2] - b[2];
    return dr * dr + dg * dg + db * db;
}

cv::Mat to_mat(const TraceImage &image)
{
    // view, not a copy
    return cv::Mat(image.height, image.width, CV_8UC4, const_cast<uint8_t *>(image.rgba.data()));
}

TraceImage from_mat(const cv::Mat &rgba)
{
    TraceImage out;
    out.width  = rgba.cols;
    out.height = rgba.rows;
    out.rgba.resize(size_t(rgba.cols) * size_t(rgba.rows) * 4);
    for (int y = 0; y < rgba.rows; ++y)
        std::memcpy(out.rgba.data() + size_t(y) * size_t(rgba.cols) * 4, rgba.ptr<uint8_t>(y), size_t(rgba.cols) * 4);
    return out;
}

ImageTraceParams sanitize(const ImageTraceParams &in)
{
    ImageTraceParams p = in;
    p.colors     = std::clamp(p.colors, 1, IMAGE_TRACE_MAX_COLORS);
    p.threshold  = std::clamp(p.threshold, 0, 255);
    p.blur       = std::isfinite(p.blur) ? std::clamp(p.blur, 0., 10.) : 1.;
    p.despeckle  = std::isfinite(p.despeckle) ? std::clamp(p.despeckle, 0., 100.) : 0.05;
    p.detail     = std::isfinite(p.detail) ? std::clamp(p.detail, 0.05, 10.) : 0.5;
    p.max_side   = std::clamp(p.max_side, 64, 2048);
    p.width      = std::isfinite(p.width) ? std::clamp(p.width, 0.1, 2000.) : 40.;
    p.depth      = std::isfinite(p.depth) ? std::clamp(p.depth, 0.01, 100.) : 1.;
    p.depth_step = std::isfinite(p.depth_step) ? std::clamp(p.depth_step, 0., 50.) : 0.4;
    if (int(p.shape_from) < 0 || int(p.shape_from) > int(TraceShapeFrom::Brightness))
        p.shape_from = TraceShapeFrom::Auto;
    if (int(p.layout) < 0 || int(p.layout) > int(TraceLayout::Stacked))
        p.layout = TraceLayout::Flat;
    return p;
}

// ---- palette (deterministic k-means over a colour histogram) ---------------------------------

struct Palette
{
    std::vector<std::array<double, 3>> centers;
    std::vector<double>                weights;
};

struct HistBin
{
    double                 weight = 0.;
    std::array<double, 3>  color{0., 0., 0.};
};

int nearest(const std::vector<std::array<double, 3>> &centers, const std::array<double, 3> &c)
{
    int    best   = 0;
    double best_d = std::numeric_limits<double>::max();
    for (size_t i = 0; i < centers.size(); ++i) {
        double d = dist2(centers[i], c);
        if (d < best_d) {
            best_d = d;
            best   = int(i);
        }
    }
    return best;
}

// Lloyd iterations over histogram bins, empty clusters are removed
void lloyd(const std::vector<HistBin> &bins, Palette &pal, int iterations)
{
    for (int it = 0; it < iterations; ++it) {
        std::vector<std::array<double, 3>> sum(pal.centers.size(), {0., 0., 0.});
        std::vector<double>                w(pal.centers.size(), 0.);
        for (const HistBin &b : bins) {
            int k = nearest(pal.centers, b.color);
            for (int c = 0; c < 3; ++c)
                sum[k][c] += b.color[c] * b.weight;
            w[k] += b.weight;
        }
        Palette next;
        bool    changed = false;
        for (size_t k = 0; k < pal.centers.size(); ++k) {
            if (w[k] <= 0.) {
                changed = true;
                continue;
            }
            std::array<double, 3> c{sum[k][0] / w[k], sum[k][1] / w[k], sum[k][2] / w[k]};
            if (dist2(c, pal.centers[k]) > 1e-6)
                changed = true;
            next.centers.push_back(c);
            next.weights.push_back(w[k]);
        }
        pal = std::move(next);
        if (!changed)
            break;
    }
}

std::vector<int> label_pixels(const TraceImage &image, const Palette &pal);

// Remove colours which only blend two other colours along their common edges (anti-aliasing of
// an exported logo): almost all of their pixels touch another colour and the colour lies between
// two other colours. A real colour of the image (even a gradient level) covers areas.
void remove_edge_blends(const TraceImage &image, const std::vector<HistBin> &bins, Palette &pal)
{
    constexpr double MIN_EDGE_FRACTION = 0.6;
    while (pal.centers.size() >= 3) {
        const size_t        k      = pal.centers.size();
        std::vector<int>    labels = label_pixels(image, pal);
        std::vector<double> total(k, 0.), edge(k, 0.);
        const int           W = image.width, H = image.height;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                int l = labels[size_t(y) * size_t(W) + size_t(x)];
                if (l < 0)
                    continue;
                total[size_t(l)] += 1.;
                bool is_edge = false;
                const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
                for (int d = 0; d < 4 && !is_edge; ++d) {
                    int nx = x + dx[d], ny = y + dy[d];
                    if (nx < 0 || ny < 0 || nx >= W || ny >= H)
                        continue;
                    is_edge = labels[size_t(ny) * size_t(W) + size_t(nx)] != l;
                }
                if (is_edge)
                    edge[size_t(l)] += 1.;
            }
        int    worst       = -1;
        double worst_score = 0.;
        for (size_t c = 0; c < k; ++c) {
            if (total[c] <= 0.)
                continue;
            double fraction = edge[c] / total[c];
            if (fraction < MIN_EDGE_FRACTION || fraction <= worst_score)
                continue;
            bool between = false;
            for (size_t a = 0; a < k && !between; ++a)
                for (size_t b = a + 1; b < k && !between; ++b) {
                    if (a == c || b == c)
                        continue;
                    const auto &ca = pal.centers[a], &cb = pal.centers[b], &cc = pal.centers[c];
                    std::array<double, 3> seg{cb[0] - ca[0], cb[1] - ca[1], cb[2] - ca[2]};
                    double len2 = seg[0] * seg[0] + seg[1] * seg[1] + seg[2] * seg[2];
                    if (len2 < 1.)
                        continue;
                    double t = ((cc[0] - ca[0]) * seg[0] + (cc[1] - ca[1]) * seg[1] + (cc[2] - ca[2]) * seg[2]) / len2;
                    if (t < 0.1 || t > 0.9)
                        continue;
                    std::array<double, 3> closest{ca[0] + t * seg[0], ca[1] + t * seg[1], ca[2] + t * seg[2]};
                    between = dist2(cc, closest) <= 0.04 * len2; // within 20 % of the distance
                }
            if (between) {
                worst       = int(c);
                worst_score = fraction;
            }
        }
        if (worst < 0)
            return;
        pal.centers.erase(pal.centers.begin() + worst);
        pal.weights.erase(pal.weights.begin() + worst);
        lloyd(bins, pal, 10);
    }
}

Palette compute_palette(const TraceImage &image, int k)
{
    // 5 bits per channel
    std::vector<HistBin> hist(32 * 32 * 32);
    for (size_t i = 0; i + 3 < image.rgba.size(); i += 4) {
        const uint8_t *p = &image.rgba[i];
        if (p[3] < ALPHA_SOLID)
            continue;
        HistBin &b = hist[(size_t(p[0] >> 3) << 10) | (size_t(p[1] >> 3) << 5) | size_t(p[2] >> 3)];
        b.weight += 1.;
        for (int c = 0; c < 3; ++c)
            b.color[c] += p[c];
    }
    std::vector<HistBin> bins;
    for (HistBin &b : hist)
        if (b.weight > 0.) {
            for (int c = 0; c < 3; ++c)
                b.color[c] /= b.weight;
            bins.push_back(b);
        }
    Palette pal;
    if (bins.empty() || k <= 0)
        return pal;

    // Deterministic seeding: the most common colour first, then the colour farthest from the
    // seeds weighted by its count (k-means++ without randomness)
    size_t first = 0;
    for (size_t i = 1; i < bins.size(); ++i)
        if (bins[i].weight > bins[first].weight)
            first = i;
    pal.centers.push_back(bins[first].color);
    while (int(pal.centers.size()) < k) {
        double best = 0.;
        size_t best_i = 0;
        for (size_t i = 0; i < bins.size(); ++i) {
            double d = std::numeric_limits<double>::max();
            for (const auto &c : pal.centers)
                d = std::min(d, dist2(c, bins[i].color));
            double score = d * bins[i].weight;
            if (score > best) {
                best   = score;
                best_i = i;
            }
        }
        if (best <= 0.)
            break; // less distinct colours than wanted
        pal.centers.push_back(bins[best_i].color);
    }
    lloyd(bins, pal, 50);

    // Merge colours which are only anti-aliasing of edges (tiny share) or almost the same
    const double total = std::accumulate(pal.weights.begin(), pal.weights.end(), 0.);
    constexpr double MIN_SHARE   = 0.004;
    constexpr double MERGE_DIST2 = 24. * 24.;
    for (bool merged = true; merged && pal.centers.size() > 1;) {
        merged = false;
        // smallest cluster under the share limit
        size_t smallest = 0;
        for (size_t i = 1; i < pal.weights.size(); ++i)
            if (pal.weights[i] < pal.weights[smallest])
                smallest = i;
        size_t a = 0, b = 0;
        if (pal.weights[smallest] < MIN_SHARE * total) {
            a = smallest;
            double best = std::numeric_limits<double>::max();
            for (size_t i = 0; i < pal.centers.size(); ++i)
                if (i != a && dist2(pal.centers[i], pal.centers[a]) < best) {
                    best = dist2(pal.centers[i], pal.centers[a]);
                    b    = i;
                }
            merged = true;
        } else {
            double best = MERGE_DIST2;
            for (size_t i = 0; i < pal.centers.size(); ++i)
                for (size_t j = i + 1; j < pal.centers.size(); ++j)
                    if (double d = dist2(pal.centers[i], pal.centers[j]); d < best) {
                        best   = d;
                        a      = i;
                        b      = j;
                        merged = true;
                    }
        }
        if (!merged)
            break;
        // weighted mean into b, remove a
        double wa = pal.weights[a], wb = pal.weights[b];
        for (int c = 0; c < 3; ++c)
            pal.centers[b][c] = (pal.centers[a][c] * wa + pal.centers[b][c] * wb) / std::max(wa + wb, 1e-9);
        pal.weights[b] = wa + wb;
        pal.centers.erase(pal.centers.begin() + a);
        pal.weights.erase(pal.weights.begin() + a);
        lloyd(bins, pal, 10);
    }
    remove_edge_blends(image, bins, pal);
    return pal;
}

// Label of every pixel: index of nearest palette colour, -1 for transparent pixels
std::vector<int> label_pixels(const TraceImage &image, const Palette &pal)
{
    std::vector<int> labels(size_t(image.width) * size_t(image.height), -1);
    for (size_t i = 0; i < labels.size(); ++i) {
        const uint8_t *p = &image.rgba[i * 4];
        if (p[3] < ALPHA_SOLID)
            continue;
        labels[i] = nearest(pal.centers, {double(p[0]), double(p[1]), double(p[2])});
    }
    return labels;
}

// ---- contours ---------------------------------------------------------------------------------

// Marching squares with linear interpolation of the vertex on the cell edge (sub-pixel precision).
// The field is padded by a frame of zeros, so all contours are closed. MarchingSquares.hpp is not
// used because it snaps the vertices to pixels (binary search for the first active pixel).
// Returns rings in pixel coordinates (pixel centers at +0.5), or nothing when there are more
// than max_rings rings.
Polygons march(const cv::Mat &field, float iso, double mm_per_px, size_t max_rings, bool &too_many)
{
    too_many    = false;
    const int W = field.cols, H = field.rows;
    const int GW = W + 2, GH = H + 2;
    auto value = [&field, W, H](int gx, int gy) -> float {
        if (gx < 1 || gy < 1 || gx > W || gy > H)
            return 0.f;
        return field.ptr<float>(gy - 1)[gx - 1];
    };
    auto hid = [GW](int gx, int gy) { return int32_t((gy * GW + gx) * 2); };
    auto vid = [GW](int gx, int gy) { return int32_t((gy * GW + gx) * 2 + 1); };

    std::vector<int32_t> next(size_t(GW) * size_t(GH) * 2, -1);
    for (int cy = 0; cy + 1 < GH; ++cy)
        for (int cx = 0; cx + 1 < GW; ++cx) {
            // corners clockwise (Y down): a top left, b top right, c bottom right, d bottom left
            float va = value(cx, cy), vb = value(cx + 1, cy), vc = value(cx + 1, cy + 1), vd = value(cx, cy + 1);
            bool  ia = va >= iso, ib = vb >= iso, ic = vc >= iso, id = vd >= iso;
            int   mask = int(ia) | (int(ib) << 1) | (int(ic) << 2) | (int(id) << 3);
            if (mask == 0 || mask == 15)
                continue;
            // edges in clockwise order: top a->b, right b->c, bottom c->d, left d->a
            const int32_t e[4]     = {hid(cx, cy), vid(cx + 1, cy), hid(cx, cy + 1), vid(cx, cy)};
            const bool    start[4] = {ia, ib, ic, id};
            const bool    end[4]   = {ib, ic, id, ia};
            // X: edge leaves the inside, Y: edge enters the inside. Every segment goes from X to Y,
            // so the shared edge is X in one cell and Y in the neighbour: one successor per edge.
            auto is_x = [&](int k) { return start[k] && !end[k]; };
            auto is_y = [&](int k) { return !start[k] && end[k]; };
            if (mask == 5 || mask == 10) {
                // saddle: decide by the value in the middle of the cell
                bool connected = (va + vb + vc + vd) * 0.25f >= iso;
                for (int k = 0; k < 4; ++k) {
                    if (!is_x(k))
                        continue;
                    for (int s = 1; s < 4; ++s) {
                        int j = connected ? (k + s) % 4 : (k + 4 - s) % 4;
                        if (is_y(j)) {
                            next[size_t(e[k])] = e[j];
                            break;
                        }
                    }
                }
            } else {
                int x = -1, y = -1;
                for (int k = 0; k < 4; ++k) {
                    if (is_x(k))
                        x = k;
                    else if (is_y(k))
                        y = k;
                }
                if (x >= 0 && y >= 0)
                    next[size_t(e[x])] = e[y];
            }
        }

    auto position = [&](int32_t edge) -> Vec2d {
        int   cell = edge / 2;
        int   gx = cell % GW, gy = cell / GW;
        bool  horizontal = (edge % 2) == 0;
        float v0 = value(gx, gy);
        float v1 = horizontal ? value(gx + 1, gy) : value(gx, gy + 1);
        double t = (v1 != v0) ? std::clamp(double(iso - v0) / double(v1 - v0), 0., 1.) : 0.5;
        return horizontal ? Vec2d(gx - 0.5 + t, gy - 0.5) : Vec2d(gx - 0.5, gy - 0.5 + t);
    };

    const double to_scaled = mm_per_px / SCALING_FACTOR;
    Polygons     rings;
    for (size_t start = 0; start < next.size(); ++start) {
        if (next[start] < 0)
            continue;
        if (rings.size() >= max_rings) {
            too_many = true;
            return {};
        }
        Slic3r::Polygon ring;
        int32_t         cur = int32_t(start);
        while (cur >= 0) {
            Vec2d p = position(cur);
            Point pt(coord_t(std::llround(p.x() * to_scaled)), coord_t(std::llround(p.y() * to_scaled)));
            if (ring.points.empty() || ring.points.back() != pt)
                ring.points.push_back(pt);
            int32_t n         = next[size_t(cur)];
            next[size_t(cur)] = -1;
            cur               = n;
            if (cur == int32_t(start))
                break;
        }
        if (ring.points.size() > 1 && ring.points.front() == ring.points.back())
            ring.points.pop_back();
        if (ring.points.size() >= 3)
            rings.push_back(std::move(ring));
    }
    return rings;
}

void remove_specks(ExPolygons &shape, double min_area)
{
    if (min_area <= 0.)
        return;
    shape.erase(std::remove_if(shape.begin(), shape.end(), [min_area](const ExPolygon &e) { return std::abs(e.contour.area()) < min_area; }),
                shape.end());
    for (ExPolygon &e : shape)
        e.holes.erase(std::remove_if(e.holes.begin(), e.holes.end(), [min_area](const Slic3r::Polygon &h) { return std::abs(h.area()) < min_area; }),
                      e.holes.end());
}

// Align points to whole micrometers, as they are written into SVG
void snap_to_um(ExPolygons &shape)
{
    auto snap = [](Slic3r::Polygon &polygon) {
        for (Point &p : polygon.points) {
            p.x() = coord_t(coord_to_um(p.x()) * 1000);
            p.y() = coord_t(coord_to_um(p.y()) * 1000);
        }
        polygon.points.erase(std::unique(polygon.points.begin(), polygon.points.end()), polygon.points.end());
        if (polygon.points.size() > 1 && polygon.points.front() == polygon.points.back())
            polygon.points.pop_back();
    };
    for (ExPolygon &e : shape) {
        snap(e.contour);
        for (Slic3r::Polygon &h : e.holes)
            snap(h);
        e.holes.erase(std::remove_if(e.holes.begin(), e.holes.end(), [](const Slic3r::Polygon &h) { return h.points.size() < 3; }),
                      e.holes.end());
    }
    shape.erase(std::remove_if(shape.begin(), shape.end(), [](const ExPolygon &e) { return e.contour.points.size() < 3; }), shape.end());
}

std::string to_svg(const ExPolygons &shape, double width_mm, double height_mm, const std::string &fill, const std::string &meta)
{
    long long w = std::llround(width_mm * 1000.);
    long long h = std::llround(height_mm * 1000.);
    std::stringstream ss;
    ss << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\n";
    ss << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << um_to_mm_string(w) << "mm\" height=\"" << um_to_mm_string(h)
       << "mm\" viewBox=\"0 0 " << w << " " << h << "\">\n";
    ss << META_BEGIN << meta_escape(meta) << META_END << "\n";
    ss << "<path fill=\"" << fill << "\" fill-rule=\"evenodd\" d=\"";
    auto write_polygon = [&ss](const Slic3r::Polygon &polygon) {
        if (polygon.points.size() < 3)
            return;
        for (size_t i = 0; i < polygon.points.size(); ++i) {
            const Point &p = polygon.points[i];
            ss << (i == 0 ? "M" : (i == 1 ? "L" : " ")) << coord_to_um(p.x()) << " " << coord_to_um(p.y());
        }
        ss << "Z";
    };
    for (const ExPolygon &expoly : shape) {
        write_polygon(expoly.contour);
        for (const Slic3r::Polygon &hole : expoly.holes)
            write_polygon(hole);
    }
    ss << "\"/>\n</svg>\n";
    return ss.str();
}

const char *to_string(TraceShapeFrom s)
{
    switch (s) {
    case TraceShapeFrom::Alpha: return "alpha";
    case TraceShapeFrom::Brightness: return "brightness";
    default: return "auto";
    }
}

const char *to_string(TraceLayout l) { return l == TraceLayout::Stacked ? "stacked" : "flat"; }

// Composite over white, luminance 0..255 with the alpha in 0..1
void luminance_alpha(const TraceImage &image, cv::Mat &lum, cv::Mat &alpha)
{
    lum.create(image.height, image.width, CV_32F);
    alpha.create(image.height, image.width, CV_32F);
    for (int y = 0; y < image.height; ++y) {
        float *l = lum.ptr<float>(y);
        float *a = alpha.ptr<float>(y);
        for (int x = 0; x < image.width; ++x) {
            const uint8_t *p = &image.rgba[(size_t(y) * size_t(image.width) + size_t(x)) * 4];
            a[x]             = p[3] / 255.f;
            l[x]             = float(luminance(p));
        }
    }
}

int otsu_threshold(const TraceImage &image)
{
    cv::Mat gray(image.height, image.width, CV_8U);
    for (int y = 0; y < image.height; ++y) {
        uint8_t *g = gray.ptr<uint8_t>(y);
        for (int x = 0; x < image.width; ++x) {
            const uint8_t *p = &image.rgba[(size_t(y) * size_t(image.width) + size_t(x)) * 4];
            double         a = p[3] / 255.;
            g[x]             = uint8_t(std::clamp(std::lround(luminance(p) * a + 255. * (1. - a)), 0l, 255l));
        }
    }
    cv::Mat dst;
    double  t = cv::threshold(gray, dst, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
    return std::clamp(int(std::lround(t)), 0, 255);
}

// ---- image codecs -----------------------------------------------------------------------------
// libpng and libjpeg(-turbo) of the application are used directly: OpenCV imgcodecs would link
// its own copy of libjpeg, which clashes with the one the application already links.

// PNG of any type (palette, gray, 16 bit, transparency) into 8 bit RGBA, libpng simplified API
bool decode_png_data(const std::string &data, TraceImage &out)
{
    png_image image;
    std::memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, data.data(), data.size()))
        return false;
    image.format = PNG_FORMAT_RGBA;
    out.width    = int(image.width);
    out.height   = int(image.height);
    out.rgba.assign(PNG_IMAGE_SIZE(image), 0);
    if (!png_image_finish_read(&image, nullptr, out.rgba.data(), 0, nullptr)) {
        png_image_free(&image);
        out = TraceImage{};
        return false;
    }
    return true;
}

// channels: 1 gray, 3 RGB, 4 RGBA
std::string encode_png_data(const TraceImage &img, int channels)
{
    std::vector<uint8_t> pixels(size_t(img.width) * size_t(img.height) * size_t(channels));
    for (size_t i = 0, n = size_t(img.width) * size_t(img.height); i < n; ++i)
        for (int c = 0; c < channels; ++c)
            pixels[i * size_t(channels) + size_t(c)] = img.rgba[i * 4 + size_t(c)];
    png_image image;
    std::memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    image.width   = png_uint_32(img.width);
    image.height  = png_uint_32(img.height);
    image.format  = channels == 4 ? PNG_FORMAT_RGBA : (channels == 3 ? PNG_FORMAT_RGB : PNG_FORMAT_GRAY);
    png_alloc_size_t size = 0;
    if (!png_image_write_to_memory(&image, nullptr, &size, 0, pixels.data(), 0, nullptr) || size == 0)
        return {};
    std::string out(size, '\0');
    if (!png_image_write_to_memory(&image, out.data(), &size, 0, pixels.data(), 0, nullptr))
        return {};
    out.resize(size);
    return out;
}

// libjpeg reports fatal errors by longjmp-ing out; everything owning memory is outside of the
// setjmp scope so nothing is skipped over (same as Format/GLTF.cpp).
struct JpegError
{
    jpeg_error_mgr pub;
    std::jmp_buf   jump;
};
void jpeg_error_longjmp(j_common_ptr info) { std::longjmp(reinterpret_cast<JpegError *>(info->err)->jump, 1); }
void jpeg_no_message(j_common_ptr) {}

bool decode_jpeg_data(const std::string &data, TraceImage &out)
{
    std::vector<uint8_t>   rgb;
    jpeg_decompress_struct cinfo;
    JpegError              err;
    volatile bool          ok = false;
    int                    w = 0, h = 0;
    cinfo.err                  = jpeg_std_error(&err.pub);
    err.pub.error_exit         = &jpeg_error_longjmp;
    err.pub.output_message     = &jpeg_no_message;
    jpeg_create_decompress(&cinfo);
    if (setjmp(err.jump) == 0) {
        jpeg_mem_src(&cinfo, reinterpret_cast<const unsigned char *>(data.data()), (unsigned long) data.size());
        if (jpeg_read_header(&cinfo, TRUE) == JPEG_HEADER_OK && cinfo.jpeg_color_space != JCS_CMYK && cinfo.jpeg_color_space != JCS_YCCK) {
            cinfo.out_color_space = JCS_RGB;
            jpeg_start_decompress(&cinfo);
            if (cinfo.output_components == 3) {
                w = int(cinfo.output_width);
                h = int(cinfo.output_height);
                rgb.resize(size_t(w) * size_t(h) * 3);
                bool complete = true;
                while (cinfo.output_scanline < cinfo.output_height) {
                    JSAMPROW row = rgb.data() + size_t(cinfo.output_scanline) * size_t(w) * 3;
                    if (jpeg_read_scanlines(&cinfo, &row, 1) != 1) {
                        complete = false;
                        break;
                    }
                }
                if (complete)
                    jpeg_finish_decompress(&cinfo);
                ok = complete;
            }
        }
    }
    jpeg_destroy_decompress(&cinfo);
    if (!ok)
        return false;
    out.width  = w;
    out.height = h;
    out.rgba.resize(size_t(w) * size_t(h) * 4);
    for (size_t i = 0, n = size_t(w) * size_t(h); i < n; ++i) {
        out.rgba[i * 4]     = rgb[i * 3];
        out.rgba[i * 4 + 1] = rgb[i * 3 + 1];
        out.rgba[i * 4 + 2] = rgb[i * 3 + 2];
        out.rgba[i * 4 + 3] = 255;
    }
    return true;
}

} // namespace image_trace_detail

using namespace image_trace_detail;

bool TraceImage::has_alpha() const
{
    if (empty())
        return false;
    for (size_t i = 3; i < rgba.size(); i += 4)
        if (rgba[i] < 250)
            return true;
    return false;
}

bool ImageTraceParams::operator==(const ImageTraceParams &o) const
{
    return colors == o.colors && shape_from == o.shape_from && threshold == o.threshold && auto_threshold == o.auto_threshold &&
           invert == o.invert && remove_background == o.remove_background && blur == o.blur && despeckle == o.despeckle &&
           detail == o.detail && max_side == o.max_side && width == o.width && depth == o.depth && layout == o.layout &&
           depth_step == o.depth_step && keep_source == o.keep_source && group_id == o.group_id;
}

bool trace_image_dimensions(const std::string &d, int &width, int &height)
{
    width = height = 0;
    const size_t n = d.size();
    // PNG: signature + IHDR
    static const unsigned char png_sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (n >= 24 && std::equal(png_sig, png_sig + 8, reinterpret_cast<const unsigned char *>(d.data())) && d.compare(12, 4, "IHDR") == 0) {
        uint32_t w = be32(d, 16), h = be32(d, 20);
        if (w == 0 || h == 0 || w > uint32_t(std::numeric_limits<int>::max()) || h > uint32_t(std::numeric_limits<int>::max()))
            return false;
        width  = int(w);
        height = int(h);
        return true;
    }
    // JPEG: find start of frame
    if (n >= 4 && uint8_t(d[0]) == 0xFF && uint8_t(d[1]) == 0xD8) {
        size_t pos = 2;
        while (pos + 4 <= n) {
            if (uint8_t(d[pos]) != 0xFF)
                return false;
            uint8_t marker = uint8_t(d[pos + 1]);
            if (marker == 0xFF) { // fill byte
                ++pos;
                continue;
            }
            if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
                pos += 2;
                continue;
            }
            if (marker == 0xD9 || marker == 0xDA) // end of image / start of scan before frame
                return false;
            uint32_t len = be16(d, pos + 2);
            if (len < 2)
                return false;
            bool is_sof = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
            if (is_sof) {
                if (pos + 9 > n)
                    return false;
                height = int(be16(d, pos + 5));
                width  = int(be16(d, pos + 7));
                return width > 0 && height > 0;
            }
            pos += 2 + len;
        }
        return false;
    }
    return false;
}

bool decode_trace_image(const std::string &data, TraceImage &out, std::string *error)
{
    out = TraceImage{};
    auto fail = [error, &out](const std::string &message) {
        out = TraceImage{};
        if (error != nullptr)
            *error = message;
        return false;
    };
    if (data.empty())
        return fail("The image file is empty.");
    if (data.size() > IMAGE_TRACE_MAX_FILE_SIZE)
        return fail("The image file is too large.");
    int w = 0, h = 0;
    if (!trace_image_dimensions(data, w, h))
        return fail("Unsupported image format. Use a PNG or JPG image.");
    if (w > IMAGE_TRACE_MAX_SIDE || h > IMAGE_TRACE_MAX_SIDE || uint64_t(w) * uint64_t(h) > IMAGE_TRACE_MAX_PIXELS)
        return fail("The image is too large (" + std::to_string(w) + " x " + std::to_string(h) + " px).");
    bool is_jpeg = uint8_t(data[0]) == 0xFF && uint8_t(data[1]) == 0xD8;
    bool ok      = is_jpeg ? decode_jpeg_data(data, out) : decode_png_data(data, out);
    if (!ok || out.empty() || out.width != w || out.height != h)
        return fail("The image could not be decoded.");
    return true;
}

TraceImage downscale_trace_image(const TraceImage &image, int max_side)
{
    if (image.empty())
        return {};
    max_side = std::max(max_side, 1);
    int longest = std::max(image.width, image.height);
    if (longest <= max_side)
        return image;
    double scale = double(max_side) / longest;
    int    w     = std::max(1, int(std::lround(image.width * scale)));
    int    h     = std::max(1, int(std::lround(image.height * scale)));
    cv::Mat src = to_mat(image);
    cv::Mat dst;
    if (!image.has_alpha()) {
        cv::resize(src, dst, cv::Size(w, h), 0, 0, cv::INTER_AREA);
        return from_mat(dst);
    }
    // premultiplied alpha, so colors of transparent pixels do not bleed into the edges
    cv::Mat f;
    src.convertTo(f, CV_32FC4, 1. / 255.);
    for (int y = 0; y < f.rows; ++y) {
        cv::Vec4f *row = f.ptr<cv::Vec4f>(y);
        for (int x = 0; x < f.cols; ++x)
            for (int c = 0; c < 3; ++c)
                row[x][c] *= row[x][3];
    }
    cv::Mat small;
    cv::resize(f, small, cv::Size(w, h), 0, 0, cv::INTER_AREA);
    dst.create(h, w, CV_8UC4);
    for (int y = 0; y < h; ++y) {
        const cv::Vec4f *row = small.ptr<cv::Vec4f>(y);
        cv::Vec4b       *out = dst.ptr<cv::Vec4b>(y);
        for (int x = 0; x < w; ++x) {
            float a = row[x][3];
            for (int c = 0; c < 3; ++c)
                out[x][c] = cv::saturate_cast<uint8_t>(a > 1e-6f ? row[x][c] / a * 255.f : 0.f);
            out[x][3] = cv::saturate_cast<uint8_t>(a * 255.f);
        }
    }
    return from_mat(dst);
}

std::string encode_trace_source(const TraceImage &image, int max_side, size_t max_bytes)
{
    if (image.empty())
        return {};
    bool alpha = image.has_alpha();
    bool gray  = !alpha;
    for (size_t i = 0; gray && i + 3 < image.rgba.size(); i += 4)
        gray = image.rgba[i] == image.rgba[i + 1] && image.rgba[i] == image.rgba[i + 2];
    const int channels = alpha ? 4 : (gray ? 1 : 3);
    for (int side = std::max(max_side, 16);; side = side * 3 / 4) {
        TraceImage small = downscale_trace_image(image, side);
        std::string png  = encode_png_data(small, channels);
        if (png.empty())
            return {};
        if (png.size() <= max_bytes)
            return png;
        if (side <= 64)
            return {};
    }
}

ImageTraceResult trace_image(const TraceImage &image, const ImageTraceParams &params_in, const std::string &source_png, bool write_svg)
{
    ImageTraceResult result;
    if (image.empty()) {
        result.error = "The image is empty.";
        return result;
    }
    const ImageTraceParams p = sanitize(params_in);

    // Decisions shared by the preview and the final trace are made on the same small image
    const TraceImage analysis = downscale_trace_image(image, ANALYSIS_SIDE);
    const TraceImage work     = downscale_trace_image(image, p.max_side);
    const int        W = work.width, H = work.height;
    const double     mm_per_px = p.width / W;
    result.width               = p.width;
    result.height              = H * mm_per_px;
    result.work_width          = W;
    result.work_height         = H;
    const bool has_alpha       = image.has_alpha();

    struct Level
    {
        cv::Mat                field; // 0..1, shape where >= 0.5
        std::array<uint8_t, 3> color{0, 0, 0};
    };
    std::vector<Level> levels;
    float              iso = 0.5f;

    if (p.colors == 1) {
        cv::Mat lum, alpha;
        luminance_alpha(work, lum, alpha);
        bool use_alpha    = p.shape_from == TraceShapeFrom::Alpha || (p.shape_from == TraceShapeFrom::Auto && has_alpha);
        result.from_alpha = use_alpha;
        Level level;
        level.field.create(H, W, CV_32F);
        if (use_alpha) {
            for (int y = 0; y < H; ++y) {
                const float *a = alpha.ptr<float>(y);
                float       *f = level.field.ptr<float>(y);
                for (int x = 0; x < W; ++x)
                    f[x] = p.invert ? 1.f - a[x] : a[x];
            }
            iso = 0.5f;
        } else {
            int t            = p.auto_threshold ? otsu_threshold(analysis) : p.threshold;
            result.threshold = t;
            // Shape: brightness <= t (or > t when inverted); transparent pixels are never shape
            for (int y = 0; y < H; ++y) {
                const float *l = lum.ptr<float>(y);
                const float *a = alpha.ptr<float>(y);
                float       *f = level.field.ptr<float>(y);
                for (int x = 0; x < W; ++x)
                    f[x] = (p.invert ? l[x] / 255.f : (255.f - l[x]) / 255.f) * a[x];
            }
            iso = p.invert ? (t + 1.f) / 255.f : (255.f - t) / 255.f;
            iso = std::clamp(iso, 0.5f / 255.f, 1.f);
        }
        // average color of the shape
        double sum[3] = {0., 0., 0.}, count = 0.;
        for (int y = 0; y < H; ++y) {
            const float *f = level.field.ptr<float>(y);
            for (int x = 0; x < W; ++x)
                if (f[x] >= iso) {
                    const uint8_t *px = &work.rgba[(size_t(y) * size_t(W) + size_t(x)) * 4];
                    for (int c = 0; c < 3; ++c)
                        sum[c] += px[c];
                    count += 1.;
                }
        }
        for (int c = 0; c < 3; ++c)
            level.color[c] = count > 0. ? uint8_t(std::lround(sum[c] / count)) : 0;
        levels.push_back(std::move(level));
    } else {
        const bool remove_bg = p.remove_background && !has_alpha;
        Palette    pal       = compute_palette(analysis, p.colors + (remove_bg ? 1 : 0));
        if (pal.centers.empty()) {
            result.error = "The image has no visible pixels.";
            return result;
        }
        int background = -1;
        if (remove_bg && pal.centers.size() > 1) {
            // the most common color at the border of the image
            std::vector<int>    labels = label_pixels(analysis, pal);
            std::vector<size_t> counts(pal.centers.size(), 0);
            auto count = [&](int x, int y) {
                int l = labels[size_t(y) * size_t(analysis.width) + size_t(x)];
                if (l >= 0)
                    ++counts[size_t(l)];
            };
            for (int x = 0; x < analysis.width; ++x) {
                count(x, 0);
                count(x, analysis.height - 1);
            }
            for (int y = 0; y < analysis.height; ++y) {
                count(0, y);
                count(analysis.width - 1, y);
            }
            background = int(std::max_element(counts.begin(), counts.end()) - counts.begin());
        }
        // remaining colours from dark to light
        std::vector<int> order;
        for (int i = 0; i < int(pal.centers.size()); ++i)
            if (i != background)
                order.push_back(i);
        std::stable_sort(order.begin(), order.end(), [&pal](int a, int b) { return luminance(pal.centers[a]) < luminance(pal.centers[b]); });
        if (int(order.size()) > p.colors)
            order.resize(size_t(p.colors)); // can not happen, safety
        result.colors_found = int(order.size());

        std::vector<int> labels = label_pixels(work, pal);
        // Stacked: from the bottom to the top level, lightest on the top unless inverted
        if (p.layout == TraceLayout::Stacked && p.invert)
            std::reverse(order.begin(), order.end());
        std::vector<int> rank(pal.centers.size(), -1);
        for (size_t r = 0; r < order.size(); ++r)
            rank[size_t(order[r])] = int(r);

        for (size_t r = 0; r < order.size(); ++r) {
            Level level;
            level.field = cv::Mat::zeros(H, W, CV_32F);
            for (int y = 0; y < H; ++y) {
                float *f = level.field.ptr<float>(y);
                for (int x = 0; x < W; ++x) {
                    int l = labels[size_t(y) * size_t(W) + size_t(x)];
                    if (l < 0 || rank[size_t(l)] < 0)
                        continue;
                    bool in = p.layout == TraceLayout::Stacked ? rank[size_t(l)] >= int(r) : rank[size_t(l)] == int(r);
                    if (in)
                        f[x] = 1.f;
                }
            }
            const std::array<double, 3> &c = pal.centers[size_t(order[r])];
            for (int k = 0; k < 3; ++k)
                level.color[k] = uint8_t(std::clamp(std::lround(c[k]), 0l, 255l));
            levels.push_back(std::move(level));
        }
    }

    // ---- contours of levels -------------------------------------------------------------------
    const double scaled_per_mm = 1. / SCALING_FACTOR;
    const double min_area      = p.despeckle * scaled_per_mm * scaled_per_mm;
    const double tolerance     = std::max(p.detail * mm_per_px, 0.002) * scaled_per_mm;
    // Neighbouring levels of a flat multi color trace overlap a little, so independent
    // simplification leaves no gap between them (the later part wins the overlap).
    const bool   overlap_levels = p.layout == TraceLayout::Flat && levels.size() > 1;
    const double overlap        = std::max(p.detail, 0.25) * mm_per_px * scaled_per_mm;

    struct Traced
    {
        ExPolygons             shape;
        std::array<uint8_t, 3> color;
    };
    std::vector<Traced> traced;
    for (Level &level : levels) {
        if (p.blur > 0.01)
            cv::GaussianBlur(level.field, level.field, cv::Size(0, 0), p.blur, p.blur, cv::BORDER_REPLICATE);
        bool     too_many = false;
        Polygons rings    = march(level.field, iso, mm_per_px, MAX_RINGS, too_many);
        if (too_many) {
            result.too_detailed = true;
            result.error        = "The image is too detailed (noise or a photo?). Raise the smoothing, remove specks or use fewer colours.";
            return result;
        }
        ExPolygons shape = union_ex(rings, ClipperLib::pftEvenOdd);
        remove_specks(shape, min_area);
        shape = expolygons_simplify(shape, tolerance);
        remove_specks(shape, min_area);
        if (overlap_levels && !shape.empty())
            shape = offset_ex(shape, float(overlap));
        snap_to_um(shape);
        if (shape.empty())
            continue;
        traced.push_back({std::move(shape), level.color});
    }
    if (traced.empty()) {
        result.error = "Nothing to trace with these settings. Change the threshold or the number of colours.";
        return result;
    }

    // ---- result -------------------------------------------------------------------------------
    std::string group_id = p.group_id.empty() && write_svg ? create_code_group_id() : p.group_id;
    const int   count    = int(traced.size());
    const Point image_center(coord_t(std::llround(p.width * 1000.)) * 1000 / 2, coord_t(std::llround(result.height * 1000.)) * 1000 / 2);
    for (int i = 0; i < count; ++i) {
        ImageTraceLayer layer;
        layer.shape = std::move(traced[size_t(i)].shape);
        layer.color = traced[size_t(i)].color;
        layer.depth = p.layout == TraceLayout::Stacked ? p.depth + i * p.depth_step : p.depth;
        for (const ExPolygon &e : layer.shape) {
            layer.contours += 1 + e.holes.size();
            layer.points += e.contour.points.size();
            for (const Slic3r::Polygon &h : e.holes)
                layer.points += h.points.size();
        }
        BoundingBox bb = get_extents(layer.shape);
        Point       c  = bb.center();
        // SVG is Y down, the embossed shape is Y up
        layer.offset = Vec2d(unscale<double>(c.x() - image_center.x()), -unscale<double>(c.y() - image_center.y()));
        result.contours += layer.contours;
        result.points += layer.points;
        // NanoSVG makes a cubic Bezier of every line: 3 control points per point + closing segment
        if (layer.contours > untrusted::SVG_MAX_PATHS || 3 * layer.points + 4 * layer.contours > untrusted::SVG_MAX_POINTS)
            result.too_detailed = true;
        result.layers.push_back(std::move(layer));
    }
    if (p.colors > 1 && result.colors_found > count)
        result.warnings.push_back("Some colours had no shape left after removing specks.");
    if (result.too_detailed) {
        result.error = "The trace is too detailed for an SVG part. Raise the smoothing, the simplification or remove specks, "
                       "or lower the resolution.";
        return result;
    }
    if (!write_svg)
        return result;

    for (int i = 0; i < count; ++i) {
        ImageTraceLayer &layer = result.layers[size_t(i)];
        ImageTraceMeta   meta;
        meta.params          = p;
        meta.params.group_id = group_id;
        meta.layer           = i;
        meta.layers          = count;
        meta.color           = layer.color;
        meta.offset          = layer.offset;
        meta.width           = result.width;
        meta.height          = result.height;
        if (i == 0 && p.keep_source)
            meta.source_png = source_png;
        layer.svg = to_svg(layer.shape, result.width, result.height, image_trace_color_to_hex(layer.color), write_image_trace_meta(meta));

        // The same limits as for SVG from 3MF, so the project can be loaded again
        SvgRefusal    refusal = SvgRefusal::None;
        std::string   why;
        NSVGimage_ptr check = nsvgParse_checked(layer.svg, refusal, &why);
        if (check == nullptr) {
            result.too_detailed = true;
            result.error        = "The trace is too detailed for an SVG part (" + why +
                           "). Raise the smoothing, the simplification or remove specks, or lower the resolution.";
            return result;
        }
    }
    return result;
}

std::string write_image_trace_meta(const ImageTraceMeta &meta)
{
    const ImageTraceParams &p = meta.params;
    nlohmann::json          j;
    j["format"]            = META_FORMAT;
    j["group"]             = p.group_id;
    j["layer"]             = meta.layer;
    j["layers"]            = meta.layers;
    j["color"]             = image_trace_color_to_hex(meta.color);
    j["offset"]            = {meta.offset.x(), meta.offset.y()};
    j["size"]              = {meta.width, meta.height};
    j["colors"]            = p.colors;
    j["shape_from"]        = to_string(p.shape_from);
    j["threshold"]         = p.threshold;
    j["auto_threshold"]    = p.auto_threshold;
    j["invert"]            = p.invert;
    j["remove_background"] = p.remove_background;
    j["blur"]              = p.blur;
    j["despeckle"]         = p.despeckle;
    j["detail"]            = p.detail;
    j["max_side"]          = p.max_side;
    j["width"]             = p.width;
    j["depth"]             = p.depth;
    j["layout"]            = to_string(p.layout);
    j["depth_step"]        = p.depth_step;
    j["keep_source"]       = p.keep_source;
    if (!meta.source_png.empty())
        j["source"] = base64_encode(meta.source_png);
    // ASCII only output, so it is safe inside of any SVG encoding
    return j.dump(-1, ' ', true);
}

std::optional<ImageTraceMeta> read_image_trace_meta(const std::string &svg_data, bool with_source)
{
    size_t begin = svg_data.find(META_BEGIN);
    if (begin == std::string::npos)
        return {};
    begin += std::char_traits<char>::length(META_BEGIN);
    size_t end = svg_data.find(META_END, begin);
    if (end == std::string::npos)
        return {};

    nlohmann::json j = nlohmann::json::parse(meta_unescape(svg_data.substr(begin, end - begin)), nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return {};

    ImageTraceMeta    meta;
    ImageTraceParams &p = meta.params;
    try {
        p.group_id = j.value("group", std::string());
        if (p.group_id.empty())
            return {};
        meta.layers = std::clamp(j.value("layers", 1), 1, IMAGE_TRACE_MAX_COLORS);
        meta.layer  = std::clamp(j.value("layer", 0), 0, meta.layers - 1);
        std::string color = j.value("color", std::string("#000000"));
        if (color.size() == 7 && color[0] == '#') {
            for (int c = 0; c < 3; ++c) {
                unsigned v = 0;
                if (std::sscanf(color.c_str() + 1 + 2 * c, "%2x", &v) == 1)
                    meta.color[size_t(c)] = uint8_t(v);
            }
        }
        auto read_pair = [&j](const char *key, double &a, double &b) {
            auto it = j.find(key);
            if (it != j.end() && it->is_array() && it->size() == 2 && (*it)[0].is_number() && (*it)[1].is_number()) {
                a = (*it)[0].get<double>();
                b = (*it)[1].get<double>();
            }
        };
        double ox = 0., oy = 0.;
        read_pair("offset", ox, oy);
        meta.offset = Vec2d(std::isfinite(ox) ? std::clamp(ox, -1e4, 1e4) : 0., std::isfinite(oy) ? std::clamp(oy, -1e4, 1e4) : 0.);
        read_pair("size", meta.width, meta.height);
        p.colors            = j.value("colors", p.colors);
        std::string from    = j.value("shape_from", std::string("auto"));
        p.shape_from        = from == "alpha" ? TraceShapeFrom::Alpha : (from == "brightness" ? TraceShapeFrom::Brightness : TraceShapeFrom::Auto);
        p.threshold         = j.value("threshold", p.threshold);
        p.auto_threshold    = j.value("auto_threshold", p.auto_threshold);
        p.invert            = j.value("invert", p.invert);
        p.remove_background = j.value("remove_background", p.remove_background);
        p.blur              = j.value("blur", p.blur);
        p.despeckle         = j.value("despeckle", p.despeckle);
        p.detail            = j.value("detail", p.detail);
        p.max_side          = j.value("max_side", p.max_side);
        p.width             = j.value("width", p.width);
        p.depth             = j.value("depth", p.depth);
        p.layout            = j.value("layout", std::string("flat")) == "stacked" ? TraceLayout::Stacked : TraceLayout::Flat;
        p.depth_step        = j.value("depth_step", p.depth_step);
        p.keep_source       = j.value("keep_source", p.keep_source);
        std::string group   = p.group_id;
        p                   = sanitize(p);
        p.group_id          = group;
        if (!std::isfinite(meta.width) || !std::isfinite(meta.height) || meta.width < 0. || meta.height < 0.)
            meta.width = meta.height = 0.;
        if (with_source) {
            auto it = j.find("source");
            if (it != j.end() && it->is_string())
                meta.source_png = base64_decode(it->get<std::string>());
        }
    } catch (const nlohmann::json::exception &) {
        return {};
    }
    return meta;
}

std::optional<ImageTraceMeta> read_image_trace_meta(const ModelVolume &volume, bool with_source)
{
    if (!volume.emboss_shape.has_value())
        return {};
    const std::optional<EmbossShape::SvgFile> &svg = volume.emboss_shape->svg_file;
    if (!svg.has_value() || svg->file_data == nullptr)
        return {};
    return read_image_trace_meta(*svg->file_data, with_source);
}

std::vector<ModelVolume *> get_image_trace_volumes(const ModelObject &object, const std::string &group_id)
{
    std::vector<ModelVolume *> result;
    if (group_id.empty())
        return result;
    for (ModelVolume *volume : object.volumes) {
        std::optional<ImageTraceMeta> meta = read_image_trace_meta(*volume);
        if (meta.has_value() && meta->params.group_id == group_id)
            result.push_back(volume);
    }
    return result;
}

std::string image_trace_path_in_3mf(const std::string &group_id, int layer)
{
    // unique per generated data: copies of a trace share the path until one of them is edited
    return "3D/imagetrace_" + group_id + "_" + std::to_string(layer + 1) + "_" + create_code_group_id() + ".svg";
}

std::string image_trace_part_name(const std::string &base, int layer, int layers)
{
    std::string name = base.empty() ? std::string("Image") : base;
    if (layers > 1)
        name += " - " + std::to_string(layer + 1);
    return name;
}

std::string image_trace_color_to_hex(const std::array<uint8_t, 3> &color)
{
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X", color[0], color[1], color[2]);
    return buffer;
}

} // namespace Slic3r
