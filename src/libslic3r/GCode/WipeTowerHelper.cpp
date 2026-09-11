#include "WipeTowerHelper.hpp"
#include "libslic3r/Triangulation.hpp"

using namespace Slic3r;

const std::map<float, float> WipeTowerHelper::min_depth_per_height = {
    {5.f,5.f}, {100.f, 20.f}, {250.f, 40.f}, {350.f, 60.f}
};

Polygon WipeTowerHelper::rib_section(float width, float depth, float rib_length, float rib_width, bool fillet_wall)
{
    Polygon res;
    res.points.resize(16);
    float theta = std::atan(width / depth);
    float costheta = std::cos(theta);
    float sintheta = std::sin(theta);
    float w = rib_width / 2.f;
    float diag = std::sqrt(width * width + depth * depth);
    float l = (rib_length - diag) / 2;
    Vec2f diag_dir1 = Vec2f{ width, depth }.normalized();
    Vec2f diag_dir1_perp{ -diag_dir1[1], diag_dir1[0] };
    Vec2f diag_dir2 = Vec2f{ -width, depth }.normalized();
    Vec2f diag_dir2_perp{ -diag_dir2[1], diag_dir2[0] };
    std::vector<Vec2f> p{ {0, 0}, {width, 0}, {width, depth}, {0, depth} };
    Polyline p_render;
    for (auto& x : p) p_render.points.push_back(scaled(x));
    res.points[0] = scaled(Vec2f{ p[0].x(), p[0].y() + w / sintheta });
    res.points[1] = scaled(Vec2f{ p[0] - diag_dir1 * l + diag_dir1_perp * w });
    res.points[2] = scaled(Vec2f{ p[0] - diag_dir1 * l - diag_dir1_perp * w });
    res.points[3] = scaled(Vec2f{ p[0].x() + w / costheta, p[0].y() });

    res.points[4] = scaled(Vec2f{ p[1].x() - w / costheta, p[1].y() });
    res.points[5] = scaled(Vec2f{ p[1] - diag_dir2 * l + diag_dir2_perp * w });
    res.points[6] = scaled(Vec2f{ p[1] - diag_dir2 * l - diag_dir2_perp * w });
    res.points[7] = scaled(Vec2f{ p[1].x(), p[1].y() + w / sintheta });

    res.points[8] = scaled(Vec2f{ p[2].x(), p[2].y() - w / sintheta });
    res.points[9] = scaled(Vec2f{ p[2] + diag_dir1 * l - diag_dir1_perp * w });
    res.points[10] = scaled(Vec2f{ p[2] + diag_dir1 * l + diag_dir1_perp * w });
    res.points[11] = scaled(Vec2f{ p[2].x() - w / costheta, p[2].y() });

    res.points[12] = scaled(Vec2f{ p[3].x() + w / costheta, p[3].y() });
    res.points[13] = scaled(Vec2f{ p[3] + diag_dir2 * l - diag_dir2_perp * w });
    res.points[14] = scaled(Vec2f{ p[3] + diag_dir2 * l + diag_dir2_perp * w });
    res.points[15] = scaled(Vec2f{ p[3].x(), p[3].y() - w / sintheta });
    res.remove_duplicate_points();
    if (fillet_wall) { res = rounding_polygon(res); }
    res.points.shrink_to_fit();
    return res;
}

TriangleMesh WipeTowerHelper::its_make_rib_tower(float width, float depth, float height, float rib_length, float rib_width, bool fillet_wall)
{
    TriangleMesh res;
    Polygon bottom = rib_section(width, depth, rib_length, rib_width, fillet_wall);
    float diag = std::sqrt(width * width + depth * depth);
    Polygon top = rib_section(width, depth, diag, rib_width, fillet_wall);
    if (fillet_wall)
        assert(bottom.points.size() == top.points.size());
    int offset = bottom.points.size();
    res.its.vertices.reserve(offset * 2);
    if (bottom.area() < scaled(EPSILON) || top.area() < scaled(EPSILON) || bottom.points.size() != top.points.size())
        return res;
    auto faces_bottom = Triangulation::triangulate(bottom);
    auto faces_top = Triangulation::triangulate(top);
    res.its.indices.reserve(offset * 2 + faces_bottom.size() + faces_top.size());
    for (auto& t : faces_bottom)
        res.its.indices.push_back({ t[1], t[0], t[2] });
    for (auto& t : faces_top)
        res.its.indices.push_back({ t[0] + offset, t[1] + offset, t[2] + offset });

    for (int i = 0; i < bottom.size(); i++)
        res.its.vertices.push_back({ unscaled<float>(bottom[i][0]), unscaled<float>(bottom[i][1]), 0 });
    for (int i = 0; i < top.size(); i++)
        res.its.vertices.push_back({ unscaled<float>(top[i][0]), unscaled<float>(top[i][1]), height });

    for (int i = 0; i < offset; i++) {
        int a = i;
        int b = (i + 1) % offset;
        int c = i + offset;
        int d = b + offset;
        res.its.indices.push_back({ a, b, c });
        res.its.indices.push_back({ d, c, b });
    }
    return res;
}

TriangleMesh WipeTowerHelper::its_make_rib_brim(const Polygon& brim, float layer_height)
{
    TriangleMesh res;
    if (brim.area() < scaled(EPSILON))
        return res;
    int offset = brim.size();
    res.its.vertices.reserve(brim.size() * 2);
    auto faces = Triangulation::triangulate(brim);
    res.its.indices.reserve(brim.size() * 2 + 2 * faces.size());
    for (auto& t : faces)
        res.its.indices.push_back({ t[1], t[0], t[2] });
    for (auto& t : faces)
        res.its.indices.push_back({ t[0] + offset, t[1] + offset, t[2] + offset });

    for (int i = 0; i < brim.size(); i++)
        res.its.vertices.push_back({ unscaled<float>(brim[i][0]), unscaled<float>(brim[i][1]), 0 });
    for (int i = 0; i < brim.size(); i++)
        res.its.vertices.push_back({ unscaled<float>(brim[i][0]), unscaled<float>(brim[i][1]), layer_height });

    for (int i = 0; i < offset; i++) {
        int a = i;
        int b = (i + 1) % offset;
        int c = i + offset;
        int d = b + offset;
        res.its.indices.push_back({ a, b, c });
        res.its.indices.push_back({ d, c, b });
    }
    return res;
}

float WipeTowerHelper::get_limit_depth_by_height(float max_height)
{
    float min_wipe_tower_depth = 0.f;
    auto iter = min_depth_per_height.begin();
    while (iter != min_depth_per_height.end()) {
        auto curr_height_to_depth = *iter;

        // This is the case that wipe tower height is lower than the first min_depth_to_height member.
        if (curr_height_to_depth.first >= max_height) {
            min_wipe_tower_depth = curr_height_to_depth.second;
            break;
        }
        iter++;

        // If curr_height_to_depth is the last member, use its min_depth.
        if (iter == min_depth_per_height.end()) {
            min_wipe_tower_depth = curr_height_to_depth.second;
            break;
        }

        // If wipe tower height is between the current and next member, set the min_depth as linear interpolation between them
        auto next_height_to_depth = *iter;
        if (next_height_to_depth.first > max_height) {
            float height_base = curr_height_to_depth.first;
            float height_diff = next_height_to_depth.first - curr_height_to_depth.first;
            float min_depth_base = curr_height_to_depth.second;
            float depth_diff = next_height_to_depth.second - curr_height_to_depth.second;

            min_wipe_tower_depth = min_depth_base + (max_height - curr_height_to_depth.first) / height_diff * depth_diff;
            break;
        }
    }
    return min_wipe_tower_depth;
}

Vec2f WipeTowerHelper::move_box_inside_box(const BoundingBox& box1, const BoundingBox& box2, int scaled_offset)
{
    Vec2f res{ 0, 0 };
    if (box1.size()[0] >= box2.size()[0] - 2 * scaled_offset || box1.size()[1] >= box2.size()[1] - 2 * scaled_offset) {
        return res;
    }

    if (box1.max[0] > box2.max[0] - scaled_offset) {
        res[0] = unscaled<float>((box2.max[0] - scaled_offset) - box1.max[0]);
    }
    else if (box1.min[0] < box2.min[0] + scaled_offset) {
        res[0] = unscaled<float>((box2.min[0] + scaled_offset) - box1.min[0]);
    }

    if (box1.max[1] > box2.max[1] - scaled_offset) {
        res[1] = unscaled<float>((box2.max[1] - scaled_offset) - box1.max[1]);
    }
    else if (box1.min[1] < box2.min[1] + scaled_offset) {
        res[1] = unscaled<float>((box2.min[1] + scaled_offset) - box1.min[1]);
    }
    return res;
}
