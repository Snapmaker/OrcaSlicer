#include "VariableWidth.hpp"

#include "BoundingBox.hpp"

namespace Slic3r {

static Points closed_loop_points(const ThickPolyline& thick_polyline)
{
    Points points = thick_polyline.points;
    if (points.size() > 1 && points.front() == points.back())
        points.pop_back();
    return points;
}

static bool points_match_cyclic(const Points& lhs, const Points& rhs, const coord_t point_tolerance, const bool reversed)
{
    if (lhs.size() != rhs.size() || lhs.empty())
        return false;

    const double tolerance_sq = double(point_tolerance) * double(point_tolerance);
    for (size_t start = 0; start < rhs.size(); ++start) {
        if ((lhs.front() - rhs[start]).cast<double>().squaredNorm() > tolerance_sq)
            continue;

        bool matches = true;
        for (size_t i = 0; i < lhs.size(); ++i) {
            const size_t rhs_idx = reversed ? (start + rhs.size() - i) % rhs.size() : (start + i) % rhs.size();
            if ((lhs[i] - rhs[rhs_idx]).cast<double>().squaredNorm() > tolerance_sq) {
                matches = false;
                break;
            }
        }

        if (matches)
            return true;
    }

    return false;
}

static bool are_near_duplicate_closed_gap_fill_loops(const ThickPolyline& lhs, const ThickPolyline& rhs)
{
    if (!lhs.is_closed() || !rhs.is_closed())
        return false;

    const Points lhs_points = closed_loop_points(lhs);
    const Points rhs_points = closed_loop_points(rhs);
    if (lhs_points.size() < 3 || lhs_points.size() != rhs_points.size())
        return false;

    const coord_t bbox_tolerance  = scale_(0.05);
    const coord_t point_tolerance = scale_(0.05);
    const double  length_tolerance = double(scale_(0.20));

    const BoundingBox lhs_bbox(lhs_points);
    const BoundingBox rhs_bbox(rhs_points);
    if (!lhs_bbox.defined || !rhs_bbox.defined)
        return false;

    if (std::abs(lhs_bbox.min.x() - rhs_bbox.min.x()) > bbox_tolerance ||
        std::abs(lhs_bbox.min.y() - rhs_bbox.min.y()) > bbox_tolerance ||
        std::abs(lhs_bbox.max.x() - rhs_bbox.max.x()) > bbox_tolerance ||
        std::abs(lhs_bbox.max.y() - rhs_bbox.max.y()) > bbox_tolerance)
        return false;

    if (std::abs(length(lhs_points) - length(rhs_points)) > length_tolerance)
        return false;

    const double area_tolerance = double(scale_(0.05)) * std::max(length(lhs_points), length(rhs_points));
    if (std::abs(std::abs(area(lhs_points)) - std::abs(area(rhs_points))) > area_tolerance)
        return false;

    return points_match_cyclic(lhs_points, rhs_points, point_tolerance, false) ||
           points_match_cyclic(lhs_points, rhs_points, point_tolerance, true);
}

static ExtrusionPaths closed_gap_fill_loop_to_extrusion_paths(const ThickPolyline& thick_polyline, ExtrusionRole role, const Flow& flow)
{
    ExtrusionPaths paths;
    if (!thick_polyline.is_closed())
        return paths;

    ThickLines lines = thick_polyline.thicklines();
    if (lines.empty())
        return paths;

    double total_length = 0.0;
    double weighted_width_sum = 0.0;
    for (const ThickLine& line : lines) {
        const coordf_t line_len = line.length();
        if (line_len < SCALED_EPSILON)
            continue;

        total_length += line_len;
        weighted_width_sum += line_len * 0.5 * (line.a_width + line.b_width);
    }

    if (total_length <= SCALED_EPSILON)
        return paths;

    ExtrusionPath path(role);
    path.polyline.points = thick_polyline.points;
    if (!path.polyline.is_valid())
        return paths;

    const double average_width = weighted_width_sum / total_length;
    const Flow   new_flow      = (role == erOverhangPerimeter && flow.bridge()) ?
        flow :
        flow.with_width(unscale<float>(average_width) + flow.height() * float(1. - 0.25 * PI));
    path.mm3_per_mm = new_flow.mm3_per_mm();
    path.width      = new_flow.width();
    path.height     = new_flow.height();
    paths.emplace_back(std::move(path));
    return paths;
}

ExtrusionMultiPath thick_polyline_to_multi_path(const ThickPolyline& thick_polyline, ExtrusionRole role, const Flow& flow, const float tolerance, const float merge_tolerance)
{
    ExtrusionMultiPath multi_path;
    ExtrusionPath      path(role);
    ThickLines         lines = thick_polyline.thicklines();

    for (int i = 0; i < (int)lines.size(); ++i) {
        const ThickLine& line = lines[i];
        assert(line.a_width >= SCALED_EPSILON && line.b_width >= SCALED_EPSILON);

        const coordf_t line_len = line.length();
        if (line_len < SCALED_EPSILON) {
            // The line is so tiny that we don't care about its width when we connect it to another line.
            if (!path.empty())
                path.polyline.points.back() = line.b; // If the variable path is non-empty, connect this tiny line to it.
            else if (i + 1 < (int)lines.size()) // If there is at least one following line, connect this tiny line to it.
                lines[i + 1].a = line.a;
            else if (!multi_path.paths.empty())
                multi_path.paths.back().polyline.points.back() = line.b; // Connect this tiny line to the last finished path.

            // If any of the above isn't satisfied, then remove this tiny line.
            continue;
        }

        double thickness_delta = fabs(line.a_width - line.b_width);
        if (thickness_delta > tolerance) {
            const auto segments = (unsigned int)ceil(thickness_delta / tolerance);
            const coordf_t seg_len = line_len / segments;
            Points pp;
            std::vector<coordf_t> width;
            {
                pp.push_back(line.a);
                width.push_back(line.a_width);
                for (size_t j = 1; j < segments; ++j) {
                    pp.push_back((line.a.cast<double>() + (line.b - line.a).cast<double>().normalized() * (j * seg_len)).cast<coord_t>());

                    coordf_t w = line.a_width + (j*seg_len) * (line.b_width-line.a_width) / line_len;
                    width.push_back(w);
                    width.push_back(w);
                }
                pp.push_back(line.b);
                width.push_back(line.b_width);

                assert(pp.size() == segments + 1u);
                assert(width.size() == segments*2);
            }

            // delete this line and insert new ones
            lines.erase(lines.begin() + i);
            for (size_t j = 0; j < segments; ++j) {
                ThickLine new_line(pp[j], pp[j+1]);
                new_line.a_width = width[2*j];
                new_line.b_width = width[2*j+1];
                lines.insert(lines.begin() + i + j, new_line);
            }

            -- i;
            continue;
        }

        const double w        = fmax(line.a_width, line.b_width);
        const Flow   new_flow = (role == erOverhangPerimeter && flow.bridge()) ? flow : flow.with_width(unscale<float>(w) + flow.height() * float(1. - 0.25 * PI));
        if (path.polyline.points.empty()) {
            path.polyline.append(line.a);
            path.polyline.append(line.b);
            // Convert from spacing to extrusion width based on the extrusion model
            // of a square extrusion ended with semi circles.
            #ifdef SLIC3R_DEBUG
            printf("  filling %f gap\n", flow.width);
            #endif
            path.mm3_per_mm  = new_flow.mm3_per_mm();
            path.width       = new_flow.width();
            path.height      = new_flow.height();
        } else {
            assert(path.width >= EPSILON);
            thickness_delta = scaled<double>(fabs(path.width - new_flow.width()));
            if (thickness_delta <= merge_tolerance) {
                // the width difference between this line and the current flow
                // (of the previous line) width is within the accepted tolerance
                path.polyline.append(line.b);
            } else {
                // we need to initialize a new line
                multi_path.paths.emplace_back(std::move(path));
                path = ExtrusionPath(role);
                -- i;
            }
        }
    }
    if (path.polyline.is_valid())
        multi_path.paths.emplace_back(std::move(path));
    return multi_path;
}

//BBS: new function to filter width to avoid too fragmented segments
static ExtrusionPaths thick_polyline_to_extrusion_paths_2(const ThickPolyline& thick_polyline, ExtrusionRole role, const Flow& flow, const float tolerance)
{
    ExtrusionPaths paths;
    ExtrusionPath path(role);
    ThickLines lines = thick_polyline.thicklines();

    size_t start_index = 0;
    double max_width, min_width;

    for (int i = 0; i < (int)lines.size(); ++i) {
        const ThickLine& line = lines[i];

        if (i == 0) {
            max_width = line.a_width;
            min_width = line.a_width;
        }

        const coordf_t line_len = line.length();
        if (line_len < SCALED_EPSILON) continue;

        double thickness_delta = std::max(fabs(max_width - line.b_width), fabs(min_width - line.b_width));
        //BBS: has large difference in width
        if (thickness_delta > tolerance) {
            //BBS: 1 generate path from start_index to i(not included)
            if (start_index != i){
                path = ExtrusionPath(role);
                double length = lines[start_index].length();
                double sum = lines[start_index].length() * 0.5 * (lines[start_index].a_width + lines[start_index].b_width);
                path.polyline.append(lines[start_index].a);
                for (int idx = start_index + 1; idx < i; idx++) {
                    length += lines[idx].length();
                    sum += lines[idx].length() * 0.5 * (lines[idx].a_width + lines[idx].b_width);
                    path.polyline.append(lines[idx].a);
                }
                path.polyline.append(lines[i].a);
                if (length > SCALED_EPSILON) {
                    double w = sum / length;
                    Flow new_flow = flow.with_width(unscale<float>(w) + flow.height() * float(1. - 0.25 * PI));
                    path.mm3_per_mm = new_flow.mm3_per_mm();
                    path.width = new_flow.width();
                    path.height = new_flow.height();
                    paths.emplace_back(std::move(path));
                }
            }

            start_index = i;
            max_width = line.a_width;
            min_width = line.a_width;

            //BBS: 2 handle the i-th segment
            thickness_delta = fabs(line.a_width - line.b_width);
            if (thickness_delta > tolerance){
                const unsigned int segments = (unsigned int)ceil(thickness_delta / tolerance);
                const coordf_t seg_len = line_len / segments;
                Points pp;
                std::vector<coordf_t> width;
                {
                    pp.push_back(line.a);
                    width.push_back(line.a_width);
                    for (size_t j = 1; j < segments; ++j) {
                        pp.push_back((line.a.cast<double>() + (line.b - line.a).cast<double>().normalized() * (j * seg_len)).cast<coord_t>());

                        coordf_t w = line.a_width + (j * seg_len) * (line.b_width - line.a_width) / line_len;
                        width.push_back(w);
                        width.push_back(w);
                    }
                    pp.push_back(line.b);
                    width.push_back(line.b_width);

                    assert(pp.size() == segments + 1u);
                    assert(width.size() == segments * 2);
                }

                // delete this line and insert new ones
                lines.erase(lines.begin() + i);
                for (size_t j = 0; j < segments; ++j) {
                    ThickLine new_line(pp[j], pp[j + 1]);
                    new_line.a_width = width[2 * j];
                    new_line.b_width = width[2 * j + 1];
                    lines.insert(lines.begin() + i + j, new_line);
                }
                --i;
                continue;
            }
        }
        //BBS: just update the max and min width and continue
        else {
            max_width = std::max(max_width, std::max(line.a_width, line.b_width));
            min_width = std::min(min_width, std::min(line.a_width, line.b_width));
        }
    }
    //BBS: handle the remaining segment
    size_t final_size = lines.size();
    if (start_index < final_size) {
        path = ExtrusionPath(role);
        double length = lines[start_index].length();
        double sum = lines[start_index].length() * lines[start_index].a_width;
        path.polyline.append(lines[start_index].a);
        for (int idx = start_index + 1; idx < final_size; idx++) {
            length += lines[idx].length();
            sum += lines[idx].length() * lines[idx].a_width;
            path.polyline.append(lines[idx].a);
        }
        path.polyline.append(lines[final_size - 1].b);
        if (length > SCALED_EPSILON) {
            double w = sum / length;
            Flow new_flow = flow.with_width(unscale<float>(w) + flow.height() * float(1. - 0.25 * PI));
            path.mm3_per_mm = new_flow.mm3_per_mm();
            path.width = new_flow.width();
            path.height = new_flow.height();
            paths.emplace_back(std::move(path));
        }
    }

    return paths;
}

void variable_width(const ThickPolylines& polylines, ExtrusionRole role, const Flow& flow, std::vector<ExtrusionEntity*>& out)
{
    // This value determines granularity of adaptive width, as G-code does not allow
    // variable extrusion within a single move; this value shall only affect the amount
    // of segments, and any pruning shall be performed before we apply this tolerance.
    const float tolerance = float(scale_(0.05));
    const ThickPolylines* source_polylines = &polylines;
    ThickPolylines        deduplicated_gap_fill_loops;

    if (role == erGapFill && polylines.size() > 1) {
        deduplicated_gap_fill_loops.reserve(polylines.size());
        for (const ThickPolyline& polyline : polylines) {
            bool is_duplicate = false;
            for (const ThickPolyline& kept : deduplicated_gap_fill_loops) {
                if (are_near_duplicate_closed_gap_fill_loops(polyline, kept)) {
                    is_duplicate = true;
                    break;
                }
            }

            if (!is_duplicate)
                deduplicated_gap_fill_loops.emplace_back(polyline);
        }

        source_polylines = &deduplicated_gap_fill_loops;
    }

    for (const ThickPolyline& p : *source_polylines) {
        ExtrusionPaths paths;
        if (role == erGapFill && p.is_closed())
            paths = closed_gap_fill_loop_to_extrusion_paths(p, role, flow);
        if (paths.empty())
            paths = thick_polyline_to_extrusion_paths_2(p, role, flow, tolerance);
        // Append paths to collection.
        if (!paths.empty()) {
            if (paths.front().first_point() == paths.back().last_point())
                out.emplace_back(new ExtrusionLoop(std::move(paths)));
            else {
                for (ExtrusionPath& path : paths)
                    out.emplace_back(new ExtrusionPath(std::move(path)));
            }
        }
    }
}

}
