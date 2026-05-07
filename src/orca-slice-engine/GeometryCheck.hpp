#pragma once

#include <vector>

namespace Slic3r {
class Model;
class ModelObject;
class ModelVolume;
} // namespace Slic3r

struct Issue;

// Run all geometry quality checks on the model.
// Returns a vector of Issue objects describing each defect found.
// Performed before slicing so defects can be reported in JSON output.
std::vector<Issue> run_geometry_checks(const Slic3r::Model& model, int plate_id = -1);
