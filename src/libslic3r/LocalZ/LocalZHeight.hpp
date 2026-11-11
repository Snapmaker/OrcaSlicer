#ifndef slic3r_LocalZHeight_hpp_
#define slic3r_LocalZHeight_hpp_

#include <algorithm>
#include <cmath>

namespace Slic3r {

// Fit the next deposition to the remaining contiguous mixed domain. Reserve
// a printable final pass when possible; otherwise absorb a short tail into
// this component. This bounds the terminal ratio approximation to one minimum
// pass. Zero means the domain/height limits admit no such deposition.
inline double local_z_bounded_pass_height(double requested, double remaining, double minimum, double maximum)
{
    constexpr double epsilon = 1e-6;
    if (!std::isfinite(requested) || !std::isfinite(remaining) || !std::isfinite(minimum) || !std::isfinite(maximum) || minimum <= 0.0 ||
        maximum + epsilon < minimum || remaining + epsilon < minimum)
        return 0.0;

    double       height = std::min(remaining, std::clamp(requested, minimum, std::max(minimum, maximum)));
    const double tail   = remaining - height;
    if (tail > epsilon && tail < minimum - epsilon) {
        if (remaining - minimum >= minimum - epsilon)
            height = remaining - minimum;
        else if (remaining <= maximum + epsilon)
            height = remaining;
        else
            return 0.0;
    }
    return height;
}

} // namespace Slic3r

#endif
