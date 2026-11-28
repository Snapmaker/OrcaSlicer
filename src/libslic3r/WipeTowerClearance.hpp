#ifndef slic3r_WipeTowerClearance_hpp_
#define slic3r_WipeTowerClearance_hpp_

#include "ClipperUtils.hpp"
#include "Polygon.hpp"

namespace Slic3r {

// With "No sparse layers" the nozzle goes back down to the prime tower only on tool-change layers,
// so the print head can pass over parts that are already taller than the tower (#1211).
// Returns the index of the first object outline closer to the tower outline than clearance_radius, or -1.
inline int no_sparse_layers_tower_conflict(const Polygon &tower, const Polygons &objects, coord_t clearance_radius)
{
    if (tower.empty() || clearance_radius <= 0)
        return -1;
    const Polygons zone = offset(tower, float(clearance_radius));
    for (size_t i = 0; i < objects.size(); ++i)
        if (!intersection(zone, Polygons{objects[i]}).empty())
            return int(i);
    return -1;
}

} // namespace Slic3r

#endif // slic3r_WipeTowerClearance_hpp_
