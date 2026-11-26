#include "LineSplit.hpp"

#include <algorithm>

namespace Slic3r {
namespace Algorithm {

SplittedLine do_split_line(const ClipperZUtils::ZPath& path, const ExPolygons& clip, bool closed, ClipperUtils::OpenClipBoundaryRule rule)
{
    assert(path.size() > 1);
    if (path.size() < 2 || clip.empty())
        return {};

    ClipperUtils::ClipEdgesIndex index(clip);
    if (index.empty())
        return {};

    const Points                                    xy = ClipperZUtils::from_zpath(path);
    std::vector<ClipperUtils::OpenClipPathInterval> intervals;
    index.clip_path(xy, closed, rule, intervals);
    auto         next = intervals.cbegin();
    SplittedLine result;
    bool         any_clipped = false;

    for (size_t i = 0; i + 1 < path.size(); ++i) {
        const Point&  a     = xy[i];
        const Point&  b     = xy[i + 1];
        const int64_t src_z = int64_t(path[i].z());
        if (a == b) {
            bool pt_clipped = ! result.empty() && result.back().clipped;
            if (! pt_clipped) {
                for (const ExPolygon &ex : clip)
                    if (ex.contains(a, rule == ClipperUtils::OpenClipBoundaryRule::Inclusive)) {
                        pt_clipped = true;
                        break;
                    }
            }
            if (pt_clipped)
                any_clipped = true;
            result.emplace_back(a, pt_clipped, src_z);
            continue;
        }
        auto first = next;
        while (next != intervals.cend() && next->seg_idx == i)
            ++next;
        auto iv     = std::find_if(first, next, [](const auto& part) { return part.p0 != part.p1; });
        bool inside = iv == next ? (first != next && first->inside) : iv->inside;
        any_clipped |= inside;

        if (! result.empty() && result.back().p == a)
            result.back().clipped = inside;
        else
            result.emplace_back(a, inside, src_z);

        for (; iv != next; ++iv) {
            if (iv->p0 == iv->p1 || iv->inside == inside)
                continue;
            inside = iv->inside;
            any_clipped |= inside;
            if (iv->p0 == result.back().p)
                result.back().clipped = inside;
            else if (iv->p0 != b)
                result.emplace_back(iv->p0, inside, -(src_z + 1));
        }
    }

    if (! any_clipped)
        return {};

    result.emplace_back(xy.back(), false, int64_t(path.back().z()));
    if (closed)
        result.pop_back();
    return result;
}

} // Algorithm
} // Slic3r
