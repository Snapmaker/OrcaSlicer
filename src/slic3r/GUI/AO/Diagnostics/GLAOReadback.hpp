#ifndef slic3r_GUI_AO_Diagnostics_GLAOReadback_hpp_
#define slic3r_GUI_AO_Diagnostics_GLAOReadback_hpp_

#include "../AOTypes.hpp"

#include <array>
#include <string>
#include <vector>

namespace Slic3r {
class GLShaderProgram;
namespace GUI { namespace AO {

// Synchronous, on-demand GPU readback for diagnostics. Every entry point protects and restores
// the GL state it touches (framebuffer, texture, pack/PBO bindings) and never submits work on
// its own; production execution resumes unchanged after each call. Results are owned CPU
// buffers; writers only ever see this owned data.
namespace AOReadback {

// Resolved color of a (possibly multisampled) target at the viewport region; RGB bytes.
bool read_color(std::vector<unsigned char>& pixels, unsigned int target, const std::array<int, 4>& viewport);
// Single-channel 2D texture (optionally one RGBA channel); 8-bit samples.
bool read_texture(std::vector<unsigned char>& pixels, unsigned int texture, int w, int h, int channel = -1);
// Depth attachment of an FBO; window-space floats.
bool read_depth(std::vector<float>& pixels, unsigned int fbo, int w, int h);
// Octahedral normal texture decoded to 3-component view normals using the depth mask.
bool read_normals(std::vector<float>& normals, unsigned int texture, int w, int h, const std::vector<float>& depth);
// R/R8UI-encoded compute texture; 8-bit samples (integer textures are read as raw values).
bool read_compute_texture(std::vector<unsigned char>& pixels, unsigned int texture, int w, int h, bool integer);
// One MIP level of the compute depth chain (R32F).
bool read_compute_mip(std::vector<float>& pixels, unsigned int texture, int level, int w, int h);
// Single channel of the currently bound diagnostic composite FBO; 8-bit samples.
bool read_composite_bytes(std::vector<unsigned char>& pixels, unsigned int fbo, int w, int h);
// RGBA32F diagnostic composite; 4-component floats.
bool read_composite_floats(std::vector<float>& pixels, unsigned int fbo, int w, int h);
// Drains GL errors; diagnostics failures must not pollute later core checks.
bool consume_errors();
bool context_lost();
void reset_context_loss();

// Per-sample color reader for a multisampled target. Owns temporary GL objects and its own
// sample-index shader for the lifetime of the loop; restores all state on destruction.
class SampleColorReader
{
public:
    SampleColorReader(unsigned int target, const std::array<int, 4>& viewport, int samples, unsigned int vao);
    ~SampleColorReader();
    SampleColorReader(const SampleColorReader&)            = delete;
    SampleColorReader& operator=(const SampleColorReader&) = delete;
    bool               valid() const { return m_valid; }
    // Reads one sample into RGB floats (w*h*3). Valid only while the target is unchanged.
    bool read(int sample, std::vector<float>& pixels);
    bool srgb_framebuffer() const { return m_srgb; }
    bool linear_source() const { return m_linear_source; }

private:
    unsigned int     m_fbo[2]{}, m_texture[2]{};
    unsigned int     m_vao{0}, m_viewport_w{0}, m_viewport_h{0};
    int              m_samples{0};
    bool             m_valid{false}, m_srgb{false}, m_linear_source{false};
    GLShaderProgram* m_shader{nullptr};
};

} // namespace AOReadback

}} // namespace GUI::AO
} // namespace Slic3r
#endif
