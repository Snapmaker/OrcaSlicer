#include "CadBody.hpp"

#include "libslic3r/TriangleMesh.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Slic3r { namespace BRep {

MeshFingerprint mesh_fingerprint(const indexed_triangle_set &its)
{
    MeshFingerprint fp;
    fp.facets   = uint32_t(its.indices.size());
    fp.vertices = uint32_t(its.vertices.size());
    if (its.vertices.empty())
        return fp;
    fp.volume = double(its_volume(its));
    Vec3d lo = its.vertices.front().cast<double>(), hi = lo, sum = Vec3d::Zero();
    for (const stl_vertex &v : its.vertices) {
        const Vec3d p = v.cast<double>();
        lo  = lo.cwiseMin(p);
        hi  = hi.cwiseMax(p);
        sum += p;
    }
    fp.bbox_min = lo;
    fp.bbox_max = hi;
    fp.centroid = sum / double(its.vertices.size());
    Vec3d m = Vec3d::Zero(), x = Vec3d::Zero();
    for (const stl_vertex &v : its.vertices) {
        const Vec3d d = v.cast<double>() - fp.centroid;
        m += d.cwiseProduct(d);
        x += Vec3d(d.x() * d.y(), d.y() * d.z(), d.z() * d.x());
    }
    fp.moments  = m / double(its.vertices.size());
    fp.products = x / double(its.vertices.size());
    return fp;
}

bool MeshFingerprint::matches(const MeshFingerprint &now, Vec3d &shift) const
{
    if (facets == 0 || facets != now.facets || vertices != now.vertices)
        return false;
    const Vec3d  size  = bbox_max - bbox_min;
    const double scale = std::max(size.maxCoeff(), 1e-3);
    // Float coordinates carry ~7 digits; a translated or re-read mesh differs from the original
    // by a few ULPs of its coordinates, which for a part sitting away from the origin is relative
    // to its position, not its size.
    const double reach = std::max({scale, bbox_min.cwiseAbs().maxCoeff(), bbox_max.cwiseAbs().maxCoeff(),
                                   now.bbox_min.cwiseAbs().maxCoeff(), now.bbox_max.cwiseAbs().maxCoeff()});
    const double tol   = 2e-6 * reach + 1e-6;
    const Vec3d  delta = now.centroid - centroid;
    if ((now.bbox_min - (bbox_min + delta)).cwiseAbs().maxCoeff() > tol ||
        (now.bbox_max - (bbox_max + delta)).cwiseAbs().maxCoeff() > tol)
        return false;
    // Volume: relative, plus what a `tol` shift of the whole surface would change.
    const double area_bound = 2. * (size.x() * size.y() + size.y() * size.z() + size.z() * size.x());
    if (std::abs(now.volume - volume) > 1e-6 * std::abs(volume) + tol * area_bound + 1e-9)
        return false;
    // Second moments: invariant to translation, sensitive to any vertex that moved.
    const double mtol = 2. * tol * scale + 1e-9;
    if ((now.moments - moments).cwiseAbs().maxCoeff() > mtol || (now.products - products).cwiseAbs().maxCoeff() > mtol)
        return false;
    shift = delta;
    return true;
}

std::shared_ptr<const CadBody> CadBody::translated(const Vec3d &delta, const MeshFingerprint &now) const
{
    auto out   = std::make_shared<CadBody>(*this);
    out->shift = shift + delta;
    out->mesh  = now;
    return out;
}

// ---- blob -------------------------------------------------------------------------------------

namespace {

constexpr char     BlobMagic[8] = {'E', 'S', 'C', 'A', 'D', 'B', 'D', 'Y'};
constexpr uint32_t BlobVersion  = 1;

template<class T> void put(std::string &out, const T &v)
{
    const char *p = reinterpret_cast<const char *>(&v);
    out.append(p, p + sizeof(T));
}
void put(std::string &out, const Vec3d &v)
{
    put(out, v.x());
    put(out, v.y());
    put(out, v.z());
}

struct Reader
{
    const std::string &in;
    size_t             pos = 0;
    bool               ok  = true;
    template<class T> T get()
    {
        T v{};
        if (!ok || pos + sizeof(T) > in.size()) {
            ok = false;
            return v;
        }
        std::memcpy(&v, in.data() + pos, sizeof(T));
        pos += sizeof(T);
        return v;
    }
    Vec3d vec()
    {
        const double x = get<double>(), y = get<double>(), z = get<double>();
        return Vec3d(x, y, z);
    }
};

} // namespace

std::string CadBody::to_blob() const
{
    std::string out;
    out.reserve(brep.size() + 256);
    out.append(BlobMagic, BlobMagic + sizeof(BlobMagic));
    put(out, BlobVersion);
    put(out, uint32_t(origin));
    put(out, int32_t(operations));
    put(out, shift);
    put(out, mesh.facets);
    put(out, mesh.vertices);
    put(out, mesh.volume);
    put(out, mesh.bbox_min);
    put(out, mesh.bbox_max);
    put(out, mesh.centroid);
    put(out, mesh.moments);
    put(out, mesh.products);
    put(out, uint64_t(brep.size()));
    out += brep;
    return out;
}

std::shared_ptr<const CadBody> CadBody::from_blob(const std::string &blob)
{
    if (blob.size() < sizeof(BlobMagic) || std::memcmp(blob.data(), BlobMagic, sizeof(BlobMagic)) != 0)
        return nullptr;
    Reader r{blob, sizeof(BlobMagic)};
    if (r.get<uint32_t>() != BlobVersion)
        return nullptr;
    auto body        = std::make_shared<CadBody>();
    const uint32_t o = r.get<uint32_t>();
    if (o > uint32_t(CadBodyOrigin::ConvertedMesh))
        return nullptr;
    body->origin         = CadBodyOrigin(o);
    body->operations     = r.get<int32_t>();
    body->shift          = r.vec();
    body->mesh.facets    = r.get<uint32_t>();
    body->mesh.vertices  = r.get<uint32_t>();
    body->mesh.volume    = r.get<double>();
    body->mesh.bbox_min  = r.vec();
    body->mesh.bbox_max  = r.vec();
    body->mesh.centroid  = r.vec();
    body->mesh.moments   = r.vec();
    body->mesh.products  = r.vec();
    const uint64_t n     = r.get<uint64_t>();
    if (!r.ok || n == 0 || r.pos + n != blob.size())
        return nullptr;
    body->brep.assign(blob.data() + r.pos, size_t(n));
    return body;
}

}} // namespace Slic3r::BRep
