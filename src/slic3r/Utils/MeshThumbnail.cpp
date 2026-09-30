#include "MeshThumbnail.hpp"

#include "libslic3r/miniz_extension.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace Slic3r {
namespace Library {

namespace fs = boost::filesystem;

static bool read_all(const std::string& path, size_t max_bytes, std::string& out)
{
    boost::system::error_code ec;
    const uintmax_t           size = fs::file_size(fs::path(path), ec);
    if (ec || size == 0 || size > max_bytes)
        return false;
    boost::nowide::ifstream f(path.c_str(), std::ios::binary);
    if (!f)
        return false;
    out.resize(size_t(size));
    f.read(&out[0], std::streamsize(size));
    out.resize(size_t(f.gcount()));
    return !out.empty();
}

// A number in the C form ("-1.5e3"), whatever the locale; advances p. false when there is none.
static bool parse_number(const char*& p, const char* end, float& out)
{
    while (p < end && (*p == ' ' || *p == '\t'))
        ++p;
    const char* s   = p;
    bool        neg = false;
    if (p < end && (*p == '-' || *p == '+'))
        neg = *p++ == '-';
    double v = 0;
    bool   digits = false;
    while (p < end && *p >= '0' && *p <= '9') {
        v = v * 10 + (*p++ - '0');
        digits = true;
    }
    if (p < end && *p == '.') {
        ++p;
        double f = 0.1;
        while (p < end && *p >= '0' && *p <= '9') {
            v += (*p++ - '0') * f;
            f *= 0.1;
            digits = true;
        }
    }
    if (!digits) {
        p = s;
        return false;
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
        const char* e    = p++;
        bool        eneg = false;
        if (p < end && (*p == '-' || *p == '+'))
            eneg = *p++ == '-';
        int  x = 0;
        bool ed = false;
        while (p < end && *p >= '0' && *p <= '9') {
            if (x < 400) x = x * 10 + (*p - '0');
            ++p;
            ed = true;
        }
        if (ed)
            v *= std::pow(10.0, eneg ? -x : x);
        else
            p = e;
    }
    out = float(neg ? -v : v);
    return std::isfinite(out);
}

static bool finite3(const float* v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

// ------------------------------------------------------------------------------ STL ----

bool parse_stl(const std::string& bytes, Triangles& out, const MeshLimits& limits)
{
    out.clear();
    const size_t size = bytes.size();
    uint32_t     n    = 0;
    if (size >= 84)
        std::memcpy(&n, bytes.data() + 80, 4); // little-endian, as every platform we build for
    const bool exact_binary = size >= 84 && uint64_t(84) + uint64_t(n) * 50 == size;
    size_t     s = 0;
    while (s < size && std::isspace((unsigned char) bytes[s]))
        ++s;
    const bool ascii = !exact_binary && bytes.compare(s, 5, "solid") == 0;

    if (!ascii) {
        if (size < 84 || uint64_t(84) + uint64_t(n) * 50 > size || n == 0 || n > limits.max_triangles)
            return false;
        out.reserve(size_t(n) * 9);
        const char* p = bytes.data() + 84;
        for (uint32_t i = 0; i < n; ++i, p += 50) {
            float v[9];
            std::memcpy(v, p + 12, sizeof(v));
            if (finite3(v) && finite3(v + 3) && finite3(v + 6))
                out.insert(out.end(), v, v + 9);
        }
        return !out.empty();
    }

    const char* p   = bytes.data() + s;
    const char* end = bytes.data() + size;
    float       tri[9];
    int         corner = 0;
    while (p < end) {
        const char* hit = static_cast<const char*>(std::memchr(p, 'v', size_t(end - p)));
        if (hit == nullptr)
            break;
        p = hit + 1;
        if (size_t(end - hit) < 6 || std::memcmp(hit, "vertex", 6) != 0)
            continue;
        p = hit + 6;
        float* v = tri + corner * 3;
        if (!parse_number(p, end, v[0]) || !parse_number(p, end, v[1]) || !parse_number(p, end, v[2]))
            continue;
        if (++corner == 3) {
            corner = 0;
            out.insert(out.end(), tri, tri + 9);
            if (out.size() / 9 >= limits.max_triangles)
                break;
        }
    }
    return !out.empty();
}

bool read_stl(const std::string& path, Triangles& out, const MeshLimits& limits)
{
    std::string bytes;
    return read_all(path, limits.max_bytes, bytes) && parse_stl(bytes, out, limits);
}

// ------------------------------------------------------------------------------ OBJ ----

bool parse_obj(const std::string& text, Triangles& out, const MeshLimits& limits)
{
    out.clear();
    std::vector<float> verts;
    std::vector<long>  face;
    const char*        p   = text.data();
    const char*        end = p + text.size();
    while (p < end) {
        const char* eol = static_cast<const char*>(std::memchr(p, '\n', size_t(end - p)));
        if (eol == nullptr)
            eol = end;
        const char* q = p;
        p             = eol + 1;
        while (q < eol && (*q == ' ' || *q == '\t'))
            ++q;
        if (eol - q < 2 || (q[1] != ' ' && q[1] != '\t'))
            continue;
        if (q[0] == 'v') {
            q += 2;
            float v[3];
            if (parse_number(q, eol, v[0]) && parse_number(q, eol, v[1]) && parse_number(q, eol, v[2]))
                verts.insert(verts.end(), v, v + 3);
            else
                verts.insert(verts.end(), 3, std::numeric_limits<float>::quiet_NaN()); // keeps the numbering
        } else if (q[0] == 'f') {
            q += 2;
            face.clear();
            const long count = long(verts.size() / 3);
            while (q < eol) {
                while (q < eol && (*q == ' ' || *q == '\t' || *q == '\r'))
                    ++q;
                if (q >= eol)
                    break;
                // "i", "i/t", "i//n" or "i/t/n": only the vertex index matters.
                bool neg = false;
                if (*q == '-') {
                    neg = true;
                    ++q;
                }
                long idx = 0;
                bool ok  = false;
                while (q < eol && *q >= '0' && *q <= '9') {
                    if (idx < 1000000000L) idx = idx * 10 + (*q - '0');
                    ++q;
                    ok = true;
                }
                while (q < eol && *q != ' ' && *q != '\t' && *q != '\r')
                    ++q;
                if (!ok)
                    continue;
                idx = neg ? count - idx : idx - 1;
                face.push_back(idx >= 0 && idx < count ? idx : -1);
            }
            for (size_t k = 2; k < face.size(); ++k) {
                const long c[3] = {face[0], face[k - 1], face[k]};
                if (c[0] < 0 || c[1] < 0 || c[2] < 0)
                    continue;
                const float* a = &verts[size_t(c[0]) * 3];
                const float* b = &verts[size_t(c[1]) * 3];
                const float* d = &verts[size_t(c[2]) * 3];
                if (!finite3(a) || !finite3(b) || !finite3(d))
                    continue;
                out.insert(out.end(), a, a + 3);
                out.insert(out.end(), b, b + 3);
                out.insert(out.end(), d, d + 3);
            }
            if (out.size() / 9 >= limits.max_triangles)
                break;
        }
    }
    return !out.empty();
}

bool read_obj(const std::string& path, Triangles& out, const MeshLimits& limits)
{
    std::string text;
    return read_all(path, limits.max_bytes, text) && parse_obj(text, out, limits);
}

// ------------------------------------------------------------------------------ AMF ----

bool parse_amf(const std::string& xml, Triangles& out, const MeshLimits& limits)
{
    out.clear();
    std::vector<float> verts;
    size_t             base = 0;  // first vertex of the current object
    float              xyz[3] = {0, 0, 0};
    long               v[3]   = {-1, -1, -1};
    const char*        p   = xml.data();
    const char*        end = p + xml.size();
    while (p < end) {
        const char* lt = static_cast<const char*>(std::memchr(p, '<', size_t(end - p)));
        if (lt == nullptr)
            break;
        p = lt + 1;
        const char* n = p;
        while (p < end && *p != '>' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != '/')
            ++p;
        const std::string tag(n, p);
        if (tag == "object") {
            base = verts.size() / 3;
            continue;
        }
        if (tag.size() != 1 && tag.size() != 2)
            continue;
        const char* gt = static_cast<const char*>(std::memchr(p, '>', size_t(end - p)));
        if (gt == nullptr)
            break;
        p = gt + 1;
        if (tag == "x" || tag == "y" || tag == "z") {
            float f;
            if (!parse_number(p, end, f))
                continue;
            xyz[tag[0] - 'x'] = f;
            if (tag == "z")
                verts.insert(verts.end(), xyz, xyz + 3);
        } else if (tag == "v1" || tag == "v2" || tag == "v3") {
            float f;
            if (!parse_number(p, end, f) || f < 0)
                continue;
            v[tag[1] - '1'] = long(base) + long(f);
            if (tag != "v3")
                continue;
            const long count = long(verts.size() / 3);
            if (v[0] >= 0 && v[1] >= 0 && v[2] >= 0 && v[0] < count && v[1] < count && v[2] < count) {
                for (long i : v)
                    out.insert(out.end(), &verts[size_t(i) * 3], &verts[size_t(i) * 3] + 3);
                if (out.size() / 9 >= limits.max_triangles)
                    break;
            }
            v[0] = v[1] = v[2] = -1;
        }
    }
    return !out.empty();
}

bool read_amf(const std::string& path, Triangles& out, const MeshLimits& limits)
{
    std::string bytes;
    if (!read_all(path, limits.max_bytes, bytes))
        return false;
    if (bytes.size() < 4 || bytes.compare(0, 2, "PK") != 0)
        return parse_amf(bytes, out, limits);
    // A zipped AMF holds the XML as its (first) file.
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (!mz_zip_reader_init_mem(&zip, bytes.data(), bytes.size(), 0))
        return false;
    bool ok = false;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip) && !ok; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st) || st.m_is_directory || st.m_uncomp_size > limits.max_bytes)
            continue;
        size_t len = 0;
        void*  data = mz_zip_reader_extract_to_heap(&zip, i, &len, 0);
        if (data == nullptr)
            continue;
        const std::string xml(static_cast<const char*>(data), len);
        mz_free(data);
        ok = parse_amf(xml, out, limits);
    }
    mz_zip_reader_end(&zip);
    return ok;
}

// ------------------------------------------------------------------------------ render ----

std::vector<unsigned char> render_rgba(const Triangles& tris, int size)
{
    const size_t n = tris.size() / 9;
    if (n == 0 || size <= 0)
        return {};

    // The view: turned 35 degrees about Z, looking down 30 degrees. Screen right, screen up, depth
    // (away from the eye) as rows of the rotation.
    const double deg = 3.14159265358979323846 / 180.0;
    const double th = -35.0 * deg, ph = 30.0 * deg;
    const double ct = std::cos(th), st = std::sin(th), cp = std::cos(ph), sp = std::sin(ph);
    const double R[3][3] = {{ct, -st, 0}, {st * sp, ct * sp, cp}, {st * cp, ct * cp, -sp}};

    // Projected bounds, to fit the picture.
    double lo[2] = {1e300, 1e300}, hi[2] = {-1e300, -1e300};
    for (size_t i = 0; i < n * 3; ++i) {
        const float* v = &tris[i * 3];
        for (int a = 0; a < 2; ++a) {
            const double s = R[a][0] * v[0] + R[a][1] * v[1] + R[a][2] * v[2];
            lo[a] = std::min(lo[a], s);
            hi[a] = std::max(hi[a], s);
        }
    }
    const int    ss    = 2; // drawn at twice the size, then averaged down: smooth edges
    const int    W     = size * ss;
    const double span  = std::max(hi[0] - lo[0], hi[1] - lo[1]);
    if (!(span > 0) || !std::isfinite(span))
        return {};
    const double scale = W * 0.82 / span;
    const double cx = (lo[0] + hi[0]) / 2, cy = (lo[1] + hi[1]) / 2;

    std::vector<float>    zbuf(size_t(W) * W, std::numeric_limits<float>::infinity());
    std::vector<uint8_t>  shade(size_t(W) * W, 0);

    // Toward the light: upper left, on the eye's side. A soft fill keeps any face from going black.
    double L[3] = {-0.45, 0.65, -0.62};
    const double ll = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
    for (double& c : L) c /= ll;

    for (size_t t = 0; t < n; ++t) {
        double P[3][3]; // screen x, screen y (down), depth
        for (int k = 0; k < 3; ++k) {
            const float* v = &tris[(t * 3 + k) * 3];
            const double sx = R[0][0] * v[0] + R[0][1] * v[1] + R[0][2] * v[2];
            const double sy = R[1][0] * v[0] + R[1][1] * v[1] + R[1][2] * v[2];
            const double sz = R[2][0] * v[0] + R[2][1] * v[1] + R[2][2] * v[2];
            P[k][0] = (sx - cx) * scale + W / 2.0;
            P[k][1] = W / 2.0 - (sy - cy) * scale;
            P[k][2] = sz;
        }
        // Face normal in view space (x right, y up, z away), turned toward the eye: files are not
        // always consistently oriented.
        const double ax = P[1][0] - P[0][0], ay = -(P[1][1] - P[0][1]), az = (P[1][2] - P[0][2]) * scale;
        const double bx = P[2][0] - P[0][0], by = -(P[2][1] - P[0][1]), bz = (P[2][2] - P[0][2]) * scale;
        double nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
        const double nl = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (!(nl > 0))
            continue;
        nx /= nl; ny /= nl; nz /= nl;
        if (nz > 0) { nx = -nx; ny = -ny; nz = -nz; }
        const double diffuse = std::max(0.0, nx * L[0] + ny * L[1] + nz * L[2]);
        const double facing  = -nz; // 1 = straight at the eye
        const double lum     = std::min(1.0, 0.18 + 0.72 * diffuse + 0.14 * facing);
        const uint8_t level  = uint8_t(std::lround(1 + lum * 254));

        const double area = (P[1][0] - P[0][0]) * (P[2][1] - P[0][1]) - (P[2][0] - P[0][0]) * (P[1][1] - P[0][1]);
        if (std::abs(area) < 1e-12)
            continue;
        const int x0 = std::max(0, int(std::floor(std::min({P[0][0], P[1][0], P[2][0]}))));
        const int x1 = std::min(W - 1, int(std::ceil(std::max({P[0][0], P[1][0], P[2][0]}))));
        const int y0 = std::max(0, int(std::floor(std::min({P[0][1], P[1][1], P[2][1]}))));
        const int y1 = std::min(W - 1, int(std::ceil(std::max({P[0][1], P[1][1], P[2][1]}))));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const double px = x + 0.5, py = y + 0.5;
                const double w0 = ((P[1][0] - px) * (P[2][1] - py) - (P[2][0] - px) * (P[1][1] - py)) / area;
                const double w1 = ((P[2][0] - px) * (P[0][1] - py) - (P[0][0] - px) * (P[2][1] - py)) / area;
                const double w2 = 1.0 - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0)
                    continue;
                const float z   = float(w0 * P[0][2] + w1 * P[1][2] + w2 * P[2][2]);
                const size_t at = size_t(y) * W + x;
                if (z < zbuf[at]) {
                    zbuf[at]  = z;
                    shade[at] = level;
                }
            }
    }

    // The model's colour, lit: a cool grey that reads on light and dark backgrounds.
    const double base[3] = {150, 172, 184};
    std::vector<unsigned char> rgba(size_t(size) * size * 4, 0);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            double acc[3] = {0, 0, 0};
            int    cover  = 0;
            for (int j = 0; j < ss; ++j)
                for (int i = 0; i < ss; ++i) {
                    const uint8_t s = shade[size_t(y * ss + j) * W + (x * ss + i)];
                    if (s == 0)
                        continue;
                    const double l = s / 255.0;
                    for (int c = 0; c < 3; ++c)
                        acc[c] += std::min(255.0, base[c] * (0.35 + 0.9 * l));
                    ++cover;
                }
            if (cover == 0)
                continue;
            unsigned char* px = &rgba[(size_t(y) * size + x) * 4];
            for (int c = 0; c < 3; ++c)
                px[c] = (unsigned char) std::lround(acc[c] / cover);
            px[3] = (unsigned char) (255 * cover / (ss * ss));
        }
    return rgba;
}

std::string encode_png(const std::vector<unsigned char>& rgba, int size)
{
    if (size <= 0 || rgba.size() != size_t(size) * size * 4)
        return std::string();
    size_t len = 0;
    void*  png = tdefl_write_image_to_png_file_in_memory(rgba.data(), size, size, 4, &len);
    if (png == nullptr)
        return std::string();
    std::string out(static_cast<const char*>(png), len);
    mz_free(png);
    return out;
}

std::string mesh_thumbnail_png(const std::string& path, const std::string& type, int size)
{
    Triangles tris;
    bool      ok = false;
    if (type == "stl")
        ok = read_stl(path, tris);
    else if (type == "obj")
        ok = read_obj(path, tris);
    else if (type == "amf")
        ok = read_amf(path, tris);
    if (!ok)
        return std::string();
    return encode_png(render_rgba(tris, size), size);
}

} // namespace Library
} // namespace Slic3r
