// Based on implementation by @platsch

#ifndef slic3r_SlicingAdaptive_hpp_
#define slic3r_SlicingAdaptive_hpp_

#include "Slicing.hpp"
#include "admesh/stl.h"

namespace Slic3r
{

class ModelVolume;

class SlicingAdaptive
{
public:
    void  clear();
    void  set_slicing_parameters(SlicingParameters params) { m_slicing_params = params; }
    void  prepare(const ModelObject &object);
    // Return next layer height starting from the last print_z, using a quality measure
    // (quality in range from 0 to 1, 0 - highest quality at low layer heights, 1 - lowest print quality at high layer heights).
    // The layer height curve shall be centered roughly around the default profile's layer height for quality 0.5.
	float next_layer_height(const float print_z, float quality, size_t &current_facet);
    float horizontal_facet_distance(float z);

	struct FaceZ {
		std::pair<float, float> z_span;
		// Cosine of the normal vector towards the Z axis.
		float					n_cos;
		// Sine of the normal vector towards the Z axis.
		float					n_sin;
	};

	// A curved edge constraint: the bend of the surface across an edge shared by two facets,
	// expressed as the normal curvature of the surface in the vertical (slicing) direction.
	struct EdgeZ {
		std::pair<float, float> z_span;
		// Normal curvature of the surface in the vertical (slicing) direction at this edge,
		// an estimate of 1/R where R is the radius of curvature of the vertical section
		// (the curve of the surface intersected by the vertical plane spanned by Z and the
		// surface normal). Discrete estimate:
		// kappa_v = dihedral_angle * horizontal_component(edge_direction) / centroid_distance.
		float					kappa_v;
	};

protected:
	SlicingParameters 		m_slicing_params;

	std::vector<FaceZ>		m_faces;
	std::vector<EdgeZ>		m_edges;
	// Cursor into m_edges, advancing monotonically with the increasing print_z.
	size_t					m_current_edge { 0 };
};

}; // namespace Slic3r

#endif /* slic3r_SlicingAdaptive_hpp_ */
