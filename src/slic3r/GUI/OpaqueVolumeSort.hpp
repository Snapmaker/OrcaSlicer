#ifndef slic3r_GUI_OpaqueVolumeSort_hpp_
#define slic3r_GUI_OpaqueVolumeSort_hpp_

namespace Slic3r {

// Orca #15884 Stage A: opaque volumes draw selected first, then nearest-first
// (descending eye-space z) so the depth test can skip shading hidden fragments.
// Transparent volumes keep their existing far-to-near sort in volumes_to_render.
//
// The comparator is pure so Catch2 can pin the order without a GL harness.
// Equal keys compare equivalent (neither is less). volumes_to_render uses
// std::sort, so equal-depth order is unspecified.

struct OpaqueVolumeSortKey
{
    bool   selected{false};
    double eye_z{0.0};
};

inline bool opaque_volume_front_to_back_less(const OpaqueVolumeSortKey &a, const OpaqueVolumeSortKey &b)
{
    if (a.selected != b.selected)
        return a.selected;
    return a.eye_z > b.eye_z;
}

} // namespace Slic3r

#endif // slic3r_GUI_OpaqueVolumeSort_hpp_
