// Stub implementation of the STEP/CAD import API for platforms where
// OpenCASCADE (OCCT) is unavailable, e.g. Windows ARM64 (OCCT 7.6 has no
// ARM64 support). Compiled in place of STEP.cpp when SLIC3R_ENABLE_STEP is
// not defined. STEP import simply reports failure; the rest of the slicer
// (STL/3MF/OBJ import, slicing, GUI) is unaffected.

#ifndef SLIC3R_ENABLE_STEP

#include "STEP.hpp"

namespace Slic3r {

bool load_step(const char * /*path*/, Model * /*model*/,
               bool& is_cancel,
               double /*linear_defletion*/,
               double /*angle_defletion*/,
               bool /*isSplitCompound*/,
               ImportStepProgressFn /*proFn*/,
               StepIsUtf8Fn /*isUtf8Fn*/,
               long& /*mesh_face_num*/)
{
    is_cancel = false;
    return false; // STEP import not available in this build
}

Step::Step(fs::path path, ImportStepProgressFn stepFn, StepIsUtf8Fn isUtf8Fn)
    : m_stop_mesh(false), m_path(path.string()), m_stepFn(stepFn), m_utf8Fn(isUtf8Fn)
{}

Step::Step(std::string path, ImportStepProgressFn stepFn, StepIsUtf8Fn isUtf8Fn)
    : m_stop_mesh(false), m_path(std::move(path)), m_stepFn(stepFn), m_utf8Fn(isUtf8Fn)
{}

bool Step::load() { return false; }
unsigned int Step::get_triangle_num(double /*linear*/, double /*angle*/) { return 0; }
unsigned int Step::get_triangle_num_tbb(double /*linear*/, double /*angle*/) { return 0; }
void Step::clean_mesh_data() {}

bool StepPreProcessor::preprocess(const char* /*path*/, std::string& /*output_path*/) { return false; }
bool StepPreProcessor::isUtf8File(const char* /*path*/) { return true; }
bool StepPreProcessor::isUtf8(const std::string /*str*/) { return true; }
bool StepPreProcessor::isGBK(const std::string /*str*/) { return false; }
int  StepPreProcessor::preNum(const unsigned char /*byte*/) { return 0; }

} // namespace Slic3r

#endif // !SLIC3R_ENABLE_STEP
