#include "LineSplit.hpp"

#include <algorithm>

namespace Slic3r {
namespace Algorithm {

static Point to_point(const ClipperZUtils::ZPoint& p) { return {p.x(), p.y()}; }

SplittedLine do_split_line(const ClipperZUtils::ZPath& path, const ExPolygons& clip, bool closed)
{
    assert(path.size() > 1);
    if (path.size() < 2 || clip.empty())
        return {};

    ClipperUtils::ClipEdgesIndex index(clip);
    if (index.empty())
        return {};

    SplittedLine                               result;
    std::vector<ClipperUtils::OpenClipInterval> intervals;
    bool                                       any_clipped = false;

    for (size_t i = 0; i + 1 < path.size(); ++i) {
        const Point   a     = to_point(path[i]);
        const Point   b     = to_point(path[i + 1]);
        const int64_t src_z = int64_t(path[i].z());
        if (a == b) {
            bool pt_clipped = ! result.empty() && result.back().clipped;
            if (! pt_clipped) {
                for (const ExPolygon &ex : clip)
                    if (ex.contains(a, true)) {
                        pt_clipped = true;
                        break;
                    }
            }
            if (pt_clipped)
                any_clipped = true;
            result.emplace_back(a, pt_clipped, src_z);
            continue;
        }
        index.clip_segment(a, b, ClipperUtils::OpenClipBoundaryRule::Inclusive, intervals);
        assert(! intervals.empty());

        const bool fallback_inside = ! intervals.empty() && intervals.front().inside;
        size_t     write           = 0;
        for (size_t k = 0; k < intervals.size(); ++k) {
            if (intervals[k].p0 == intervals[k].p1)
                continue;
            if (write > 0 && intervals[write - 1].inside == intervals[k].inside) {
                intervals[write - 1].t1 = intervals[k].t1;
                intervals[write - 1].p1 = intervals[k].p1;
            } else {
                intervals[write++] = intervals[k];
            }
        }
        if (write == 0) {
            intervals.resize(1);
            intervals[0] = { 0., 1., a, b, fallback_inside };
        } else {
            intervals.resize(write);
            intervals.front().p0 = a;
            intervals.back().p1  = b;
        }

        for (const ClipperUtils::OpenClipInterval &iv : intervals)
            if (iv.inside)
                any_clipped = true;

        if (! result.empty() && result.back().p == a)
            result.back().clipped = intervals.front().inside;
        else
            result.emplace_back(a, intervals.front().inside, src_z);

        for (size_t k = 1; k < intervals.size(); ++k) {
            if (intervals[k].p0 == result.back().p) {
                result.back().clipped = intervals[k].inside;
                continue;
            }
            if (intervals[k].p0 == b)
                continue;
            result.emplace_back(intervals[k].p0, intervals[k].inside, -(src_z + 1));
        }
    }

    if (! any_clipped)
        return {};

    result.emplace_back(to_point(path.back()), false, int64_t(path.back().z()));
    if (closed)
        result.pop_back();
    return result;
}

} // Algorithm
} // Slic3r
