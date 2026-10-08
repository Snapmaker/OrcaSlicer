#include "../ClipperUtils.hpp"
#include "../ShortestPath.hpp"
#include "../Surface.hpp"
#include <cmath>
#include <algorithm>
#include <iostream>
#include <limits>
#include "FillBase.hpp"
#include "FillGyroid.hpp"

namespace Slic3r {

static inline double f(double x, double z_sin, double z_cos, bool vertical, bool flip)
{
    if (vertical) {
        double phase_offset = (z_cos < 0 ? M_PI : 0) + M_PI;
        double a   = sin(x + phase_offset);
        double b   = - z_cos;
        double res = z_sin * cos(x + phase_offset + (flip ? M_PI : 0.));
        double r   = sqrt(sqr(a) + sqr(b));
        return asin(a/r) + asin(res/r) + M_PI;
    }
    else {
        double phase_offset = z_sin < 0 ? M_PI : 0.;
        double a   = cos(x + phase_offset);
        double b   = - z_sin;
        double res = z_cos * sin(x + phase_offset + (flip ? 0 : M_PI));
        double r   = sqrt(sqr(a) + sqr(b));
        return (asin(a/r) + asin(res/r) + 0.5 * M_PI);
    }
}

// Repeats one period of a wave from the last sample at or before x_min to the first one at or after x_max.
static inline Polyline make_wave(
    const std::vector<Vec2d>& one_period, double x_min, double x_max, double offset, double scaleFactor, bool vertical)
{
    const double period = one_period.back().x();
    // The last sample of a period is the first one of the next.
    const size_t n  = one_period.size() - 1;
    double       x0 = std::floor(x_min / period) * period;
    size_t       i  = 0;
    while (i + 1 < n && x0 + one_period[i + 1].x() <= x_min)
        ++i;

    Polyline polyline;
    polyline.points.reserve(size_t((x_max - x0) / period + 1.) * n + 1);
    for (;;) {
        Vec2d      point(x0 + one_period[i].x(), one_period[i].y() + offset);
        const bool last = point.x() >= x_max;
        if (vertical)
            std::swap(point(0), point(1));
        polyline.points.emplace_back((point * scaleFactor).cast<coord_t>());
        if (last)
            break;
        if (++i == n) {
            i = 0;
            x0 += period;
        }
    }
    return polyline;
}

static std::vector<Vec2d> make_one_period(double scaleFactor, double z_cos, double z_sin, bool vertical, bool flip, double tolerance)
{
    std::vector<Vec2d> points;
    double dx = M_PI_2; // exact coordinates on main inflexion lobes
    double limit = 2*M_PI;
    points.reserve(coord_t(ceil(limit / tolerance / 3)));

    for (double x = 0.; x < limit - EPSILON; x += dx) {
        points.emplace_back(Vec2d(x, f(x, z_sin, z_cos, vertical, flip)));
    }
    points.emplace_back(Vec2d(limit, f(limit, z_sin, z_cos, vertical, flip)));

    // piecewise increase in resolution up to requested tolerance
    for(;;)
    {
        size_t size = points.size();
        for (unsigned int i = 1;i < size; ++i) {
            auto& lp = points[i-1]; // left point
            auto& rp = points[i];   // right point
            double x = lp(0) + (rp(0) - lp(0)) / 2;
            double y = f(x, z_sin, z_cos, vertical, flip);
            Vec2d ip = {x, y};
            if (std::abs(cross2(Vec2d(ip - lp), Vec2d(ip - rp))) > sqr(tolerance)) {
                points.emplace_back(std::move(ip));
            }
        }

        if (size == points.size())
            break;
        else
        {
            // insert new points in order
            std::sort(points.begin(), points.end(),
                      [](const Vec2d &lhs, const Vec2d &rhs) { return lhs(0) < rhs(0); });
        }
    }

    return points;
}

// Waves covering bbox, with the pattern anchored at origin.
static Polylines make_gyroid_waves(double gridZ, double density_adjusted, double line_spacing, const BoundingBox &bbox, const Point &origin)
{
    const double scaleFactor = scale_(line_spacing) / density_adjusted;

    // tolerance in scaled units. clamp the maximum tolerance as there's
    // no processing-speed benefit to do so beyond a certain point
    const double tolerance = std::min(line_spacing / 2, FillGyroid::PatternTolerance) / unscale<double>(scaleFactor);

    //scale factor for 5% : 8 712 388
    // 1z = 10^-6 mm ?
    const double z     = gridZ / scaleFactor;
    const double z_sin = sin(z);
    const double z_cos = cos(z);

    bool vertical = (std::abs(z_sin) <= std::abs(z_cos));
    // Range to cover in pattern units, with the waves running along x.
    Vec2d lo = (bbox.min - origin).cast<double>() / scaleFactor;
    Vec2d hi = (bbox.max - origin).cast<double>() / scaleFactor;
    double lower_bound = 0.;
    bool flip = true;
    if (vertical) {
        flip = false;
        lower_bound = -M_PI;
        std::swap(lo(0), lo(1));
        std::swap(hi(0), hi(1));
    }

    std::vector<Vec2d> one_period_odd = make_one_period(scaleFactor, z_cos, z_sin, vertical, flip, tolerance); // creates one period of the waves, so it doesn't have to be recalculated all the time
    flip = !flip;                                                                   // even polylines are a bit shifted
    std::vector<Vec2d> one_period_even = make_one_period(scaleFactor, z_cos, z_sin, vertical, flip, tolerance);

    // Every wave spans [offset + f_min, offset + f_max] across.
    double f_min = std::numeric_limits<double>::max();
    double f_max = std::numeric_limits<double>::lowest();
    for (const std::vector<Vec2d> *one_period : { &one_period_odd, &one_period_even })
        for (const Vec2d &point : *one_period) {
            f_min = std::min(f_min, point.y());
            f_max = std::max(f_max, point.y());
        }

    Polylines result;
    for (int i = int(std::ceil((lo.y() - f_max - lower_bound) / M_PI)); lower_bound + i * M_PI + f_min <= hi.y(); ++i) {
        Polyline &wave = result.emplace_back(make_wave(i % 2 == 0 ? one_period_odd : one_period_even, lo.x(), hi.x(),
                                                       lower_bound + i * M_PI, scaleFactor, vertical));
        wave.translate(origin);
    }

    return result;
}

// FIXME: needed to fix build on Mac on buildserver
constexpr double FillGyroid::PatternTolerance;

void FillGyroid::_fill_surface_single(
    const FillParams                &params,
    unsigned int                     thickness_layers,
    const std::pair<float, Point>   &direction,
    ExPolygon                        expolygon,
    Polylines                       &polylines_out)
{
    auto infill_angle = float(this->angle + (CorrectionAngle * 2*M_PI) / 360.);
    if(std::abs(infill_angle) >= EPSILON)
        expolygon.rotate(-infill_angle);

    BoundingBox bb = expolygon.contour.bounding_box();
    // Density adjusted to have a good %of weight.
    double      density_adjusted = std::max(0., params.density * DensityAdjust / params.multiline);
    // Distance between the gyroid waves in scaled coordinates.
    coord_t     distance = coord_t(scale_(this->spacing) / density_adjusted);

    // Anchor the pattern to our grid module; the 10-line shift keeps its established phase.
    const coord_t shift  = coord_t(10 * scale_(this->spacing));
    const Point   origin = align_to_grid(bb.min, Point(2*M_PI*distance, 2*M_PI*distance)) - Point(shift, shift);

    // Keep the pattern ends and the multiline copies outside the contour.
    bb.offset(scale_(this->spacing * params.multiline));

    // generate pattern
    Polylines polylines = make_gyroid_waves(scale_(this->z), density_adjusted, this->spacing, bb, origin);

    // Apply multiline offset if needed
    multiline_fill(polylines, params, spacing);

	polylines = intersection_pl(std::move(polylines), expolygon);

    if (! polylines.empty()) {
		// Remove very small bits, but be careful to not remove infill lines connecting thin walls!
        // The infill perimeter lines should be separated by around a single infill line width.
        const double minlength = scale_(0.8 * this->spacing);
		polylines.erase(
			std::remove_if(polylines.begin(), polylines.end(), [minlength](const Polyline &pl) { return pl.length() < minlength; }),
			polylines.end());
    }

	if (! polylines.empty()) {
		// connect lines
		size_t polylines_out_first_idx = polylines_out.size();
        chain_or_connect_infill(std::move(polylines), expolygon, polylines_out, this->spacing, params);

	    // new paths must be rotated back
        if (std::abs(infill_angle) >= EPSILON) {
	        for (auto it = polylines_out.begin() + polylines_out_first_idx; it != polylines_out.end(); ++ it)
	        	it->rotate(infill_angle);
	    }
    }
}

} // namespace Slic3r
