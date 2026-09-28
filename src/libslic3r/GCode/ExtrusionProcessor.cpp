#include "ExtrusionProcessor.hpp"

namespace Slic3r {

std::vector<ProcessedPoint> ExtrusionQualityEstimator::estimate_extrusion_quality(const ExtrusionPath                &path,
                                                                                  const ConfigOptionPercents         &overlaps,
                                                                                  const ConfigOptionFloatsOrPercents &speeds,
                                                                                  float                               ext_perimeter_speed,
                                                                                  float                               original_speed,
                                                                                  bool                                slowdown_for_curled_edges)
{
    size_t                               speed_sections_count = std::min(overlaps.values.size(), speeds.values.size());
    std::vector<std::pair<float, float>> speed_sections;
    
    
    
    for (size_t i = 0; i < speed_sections_count; i++) {
        float distance = path.width * (1.0 - (overlaps.get_at(i) / 100.0));
        float speed    = speeds.get_at(i).percent ? (ext_perimeter_speed * speeds.get_at(i).value / 100.0) : speeds.get_at(i).value;
        speed_sections.push_back({distance, speed});
    }
    std::sort(speed_sections.begin(), speed_sections.end(),
              [](const std::pair<float, float> &a, const std::pair<float, float> &b) { 
                if (a.first == b.first) {
                    return a.second > b.second;
                }
                return a.first < b.first; });

    std::pair<float, float> last_section{INFINITY, 0};
    for (auto &section : speed_sections) {
        if (section.first == last_section.first) {
            section.second = last_section.second;
        } else {
            last_section = section;
        }
    }
    
    // Orca: Find the smallest overhang distance where speed adjustments begin
    float smallest_distance_with_lower_speed = std::numeric_limits<float>::infinity(); // Initialize to a large value
    bool found = false;
    for (const auto& section : speed_sections) {
        if (section.second <= original_speed) {
            if (section.first < smallest_distance_with_lower_speed) {
                smallest_distance_with_lower_speed = section.first;
                found = true;
            }
        }
    }

    // If a meaningful (i.e. needing slowdown) overhang distance was not found, then we shouldn't split the lines
    if (!found)
        smallest_distance_with_lower_speed=-1.f;

    // Orca: Pass to the point properties estimator the smallest ovehang distance that triggers a slowdown (smallest_distance_with_lower_speed)
    std::vector<ExtendedPoint> extended_points = estimate_points_properties<true, true, true, true>
                                                            (path.polyline.points,
                                                             prev_layer_boundaries[current_object],
                                                             path.width,
                                                             found ? 2.0f : -1.0f,
                                                             smallest_distance_with_lower_speed);
    const auto width_inv = 1.0f / path.width;
    std::vector<ProcessedPoint> processed_points;
    processed_points.reserve(extended_points.size());
    for (size_t i = 0; i < extended_points.size(); i++) {
        const ExtendedPoint &curr = extended_points[i];
        const ExtendedPoint &next = extended_points[i + 1 < extended_points.size() ? i + 1 : i];
        
        float artificial_distance_to_curled_lines = 0.0;
        if(slowdown_for_curled_edges) {
        	// The following code artifically increases the distance to provide slowdown for extrusions that are over curled lines
        	const double dist_limit = 10.0 * path.width;
				{
				Vec2d middle = 0.5 * (curr.position + next.position);
				auto line_indices = prev_curled_extrusions[current_object].all_lines_in_radius(Point::new_scale(middle), scale_(dist_limit));
					if (!line_indices.empty()) {
						double len   = (next.position - curr.position).norm();
						// For long lines, there is a problem with the additional slowdown. If by accident, there is small curled line near the middle of this long line
                	//  The whole segment gets slower unnecesarily. For these long lines, we do additional check whether it is worth slowing down.
                	// NOTE that this is still quite rough approximation, e.g. we are still checking lines only near the middle point
                	// TODO maybe split the lines into smaller segments before running this alg? but can be demanding, and GCode will be huge
                	if (len > 2) {
                    	Vec2d dir   = Vec2d(next.position - curr.position) / len;
                    	Vec2d right = Vec2d(-dir.y(), dir.x());

                    	Polygon box_of_influence = {
                        	scaled(Vec2d(curr.position + right * dist_limit)),
                        	scaled(Vec2d(next.position + right * dist_limit)),
                        	scaled(Vec2d(next.position - right * dist_limit)),
                        	scaled(Vec2d(curr.position - right * dist_limit)),
                    	};

                    	double projected_lengths_sum = 0;
                    	for (size_t idx : line_indices) {
                        	const CurledLine &line   = prev_curled_extrusions[current_object].get_line(idx);
                        	Lines             inside = intersection_ln({{line.a, line.b}}, {box_of_influence});
                        	if (inside.empty())
                            	continue;
                        	double projected_length = abs(dir.dot(unscaled(Vec2d((inside.back().b - inside.back().a).cast<double>()))));
                        	projected_lengths_sum += projected_length;
                    	}
                    	if (projected_lengths_sum < 0.4 * len) {
                        	line_indices.clear();
                    	}
                	}
                
                	for (size_t idx : line_indices) {
                    	const CurledLine &line                 = prev_curled_extrusions[current_object].get_line(idx);
                    	float             distance_from_curled = unscaled(line_alg::distance_to(line, Point::new_scale(middle)));
                    	float             dist                 = path.width * (1.0 - (distance_from_curled / dist_limit)) *
                                 (1.0 - (distance_from_curled / dist_limit)) *
                                 (line.curled_height / (path.height * 10.0f)); // max_curled_height_factor from SupportSpotGenerator
                    	artificial_distance_to_curled_lines = std::max(artificial_distance_to_curled_lines, dist);
                	}
					}
				}
			}	

        auto calculate_speed = [&speed_sections, &original_speed](float distance) {
            float final_speed;
            if (distance <= speed_sections.front().first) {
                final_speed = original_speed;
            } else if (distance >= speed_sections.back().first) {
                final_speed = speed_sections.back().second;
            } else {
                size_t section_idx = 0;
                while (distance > speed_sections[section_idx + 1].first) {
                    section_idx++;
                }
                float t = (distance - speed_sections[section_idx].first) /
                          (speed_sections[section_idx + 1].first - speed_sections[section_idx].first);
                t           = std::clamp(t, 0.0f, 1.0f);
                final_speed = (1.0f - t) * speed_sections[section_idx].second + t * speed_sections[section_idx + 1].second;
            }
            return round(final_speed);
        };
        
        float extrusion_speed = std::min(calculate_speed(curr.distance), calculate_speed(next.distance));
        /*float extrusion_speed = 0.f;
        if ((next.position - curr.position).norm() > 2.0 * path.width)
            extrusion_speed = calculate_speed(curr.distance);
        else
            extrusion_speed = std::min(calculate_speed(curr.distance), calculate_speed(next.distance));*/
        // ORCA: Clamp resulting speed to lowest of calculated speed based on the overhang values and the current speed
        // Fixes bug where resulting overhang speed is higher than the current speed due to (for example) volumetric flow limits.
        extrusion_speed = std::min(extrusion_speed, original_speed);
        
        if(slowdown_for_curled_edges) {
            float curled_speed = calculate_speed(artificial_distance_to_curled_lines);
        	extrusion_speed       = std::min(curled_speed, extrusion_speed); // adjust extrusion speed based on what is smallest - the calculated overhang speed or the artificial curled speed
        }
        
        float overlap = std::min(1 - (curr.distance+artificial_distance_to_curled_lines) * width_inv, 1 - (next.distance+artificial_distance_to_curled_lines) * width_inv);
			
        processed_points.push_back({ scaled(curr.position), extrusion_speed, overlap });
    }
    return processed_points;
}

} // namespace Slic3r
