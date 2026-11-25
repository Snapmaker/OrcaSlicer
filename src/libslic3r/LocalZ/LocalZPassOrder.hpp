#ifndef slic3r_LocalZPassOrder_hpp_
#define slic3r_LocalZPassOrder_hpp_

#include "LocalZPlan.hpp"
#include <tuple>

namespace Slic3r {

// Pairwise height tolerances can create cycles when three passes are close together.
inline bool independent_local_z_pass_less(const SubLayerPlan& lhs, const SubLayerPlan& rhs)
{
    return std::tie(lhs.print_z, lhs.dependency_group, lhs.dependency_order) <
           std::tie(rhs.print_z, rhs.dependency_group, rhs.dependency_order);
}

inline bool isolated_local_z_pass_less(const SubLayerPlan& lhs, const SubLayerPlan& rhs)
{
    return std::tie(lhs.print_z, lhs.z_lo, lhs.pass_index) < std::tie(rhs.print_z, rhs.z_lo, rhs.pass_index);
}

} // namespace Slic3r

#endif
