#ifndef WipeTowerHelper_
#define WipeTowerHelper_

#include "libslic3r/Polygon.hpp"
#include "libslic3r/TriangleMesh.hpp"

namespace Slic3r
{

class WipeTowerHelper 
{
public:
    static const std::map<float, float> min_depth_per_height;

    static Polygon rib_section(float width, float depth, float rib_length, float rib_width, bool fillet_wall);

    static TriangleMesh its_make_rib_tower(float width, float depth, float height, float rib_length, float rib_width, bool fillet_wall);

    static TriangleMesh its_make_rib_brim(const Polygon& brim, float layer_height);

    static float get_limit_depth_by_height(float max_height);

    static Vec2f move_box_inside_box(const BoundingBox& box1, const BoundingBox& box2, int scaled_offset = 0);
};

}

#endif // !WipeTowerHelper_
