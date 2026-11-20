#include "GLAOComputeBackend.hpp"

#include <GL/glew.h>
#include <algorithm>
#include <cmath>

namespace Slic3r { namespace GUI { namespace AO {

namespace {
constexpr int depth_mip_levels = 5;
constexpr int depth_tile_size  = 1 << (depth_mip_levels - 1);

int padded_extent(int extent) { return (extent + depth_tile_size - 1) / depth_tile_size * depth_tile_size; }

bool fits_viewport(const GLAOComputeBackend::Capabilities& caps, int width, int height)
{
    const int max_extent = caps.max_texture_size - (depth_tile_size - 1);
    return GLAOComputeBackend::valid_dispatch(caps, width, height) && width <= max_extent && height <= max_extent;
}

double texture_storage_bytes(int width, int height, int denoise_passes)
{
    // Each work texture is one byte per pixel. Linear depth uses four bytes per texel
    // and has five mip levels at dimensions padded to a full reduction tile.
    double    bytes = double(width) * height * (GLAOComputeBackend::texture_count(denoise_passes) - 1);
    const int pw = padded_extent(width), ph = padded_extent(height);
    for (int level = 0; level < depth_mip_levels; ++level)
        bytes += 4.0 * (pw >> level) * (ph >> level);
    return bytes;
}
} // namespace

void GLAOComputeBackend::release()
{
    if (!m_textures[0])
        return;
    State state;
    state.forget_textures(m_textures);
    glDeleteTextures(int(m_textures.size()), m_textures.data());
    m_textures = {};
    m_width = m_height = m_pw = m_ph = 0;
}

void GLAOComputeBackend::forget_context()
{
    m_textures = {};
    m_programs = {};
    m_width = m_height = m_pw = m_ph = 0;
    m_failure.clear();
    m_failed_storage_bytes = 0;
    m_probed       = false;
    m_capabilities = {};
}

void GLAOComputeBackend::disable(const std::string& reason)
{
    release();
    m_failed_storage_bytes = 0;
    m_failure = reason;
}

void GLAOComputeBackend::reset_health()
{
    if (m_failure == "OpenGL context lost")
        return;
    m_failure.clear();
    m_failed_storage_bytes = 0;
}

bool GLAOComputeBackend::can_retry_resource_failure(int width, int height, int denoise_passes) const
{
    if (denoise_passes < 1 || denoise_passes > 3 || !fits_viewport(m_capabilities, width, height))
        return false;
    return m_failure == "CS viewport exceeds resource limits" ||
           (m_failed_storage_bytes > 0 && texture_storage_bytes(width, height, denoise_passes) < m_failed_storage_bytes);
}

std::string GLAOComputeBackend::failure_reason_for(int width, int height, int denoise_passes) const
{
    return can_retry_resource_failure(width, height, denoise_passes) ? std::string{} : m_failure;
}

bool GLAOComputeBackend::prepare(int width, int height, const std::array<unsigned int, 3>& programs, int denoise_passes)
{
    if (denoise_passes < 1 || denoise_passes > 3)
        return false;
    if (!m_failure.empty()) {
        if (!can_retry_resource_failure(width, height, denoise_passes))
            return false;
        reset_health();
    }
    const auto& caps = capabilities();
    if (!caps.supported) {
        m_failure = caps.reason;
        return false;
    }
    if (!fits_viewport(caps, width, height)) {
        m_failure = "CS viewport exceeds resource limits";
        return false;
    }
    for (auto program : programs)
        if (!program) {
            m_failure = "optional XeGTAO compute program unavailable";
            return false;
        }
    m_programs = programs;
    if (width == m_width && height == m_height && (denoise_passes == 3) == (m_textures[5] != 0))
        return true;
    State state;
    state.forget_textures(m_textures);
    release();
    m_width  = width;
    m_height = height;
    // Pad by edge replication to 16: all five MIPs have complete reduction tiles, including odd viewports.
    m_pw            = padded_extent(width);
    m_ph            = padded_extent(height);
    const int count = texture_count(denoise_passes);
    glGenTextures(count, m_textures.data());
    const GLenum formats[] = {GL_R32F, GL_R8UI, GL_R8UI, GL_R8UI, GL_R8, GL_R8UI};
    const char*  labels[]  = {"XeGTAO.LinearDepthMips",     "XeGTAO.RawEncoded", "XeGTAO.Edges",
                              "XeGTAO.DenoiseIntermediate", "XeGTAO.Visibility", "XeGTAO.DenoiseSecondIntermediate"};
    glActiveTexture(GL_TEXTURE0);
    for (int i = 0; i < count; ++i) {
        glBindTexture(GL_TEXTURE_2D, m_textures[i]);
        glTexStorage2D(GL_TEXTURE_2D, i == 0 ? depth_mip_levels : 1, formats[i], i == 0 ? m_pw : width, i == 0 ? m_ph : height);
        if (glObjectLabel)
            glObjectLabel(GL_TEXTURE, m_textures[i], -1, labels[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, i == 0 ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    bool allocation_failed = false, out_of_memory = false, context_lost = false;
    for (GLenum error = glGetError(); error != GL_NO_ERROR; error = glGetError()) {
        allocation_failed = true;
        out_of_memory |= error == GL_OUT_OF_MEMORY;
        context_lost |= error == GL_CONTEXT_LOST;
    }
    if (context_lost) {
        state.forget_context();
        forget_context();
        m_failure = "OpenGL context lost";
        return false;
    }
    if (allocation_failed) {
        disable("CS texture allocation failed");
        if (out_of_memory)
            m_failed_storage_bytes = texture_storage_bytes(width, height, denoise_passes);
        return false;
    }
    return true;
}

bool GLAOComputeBackend::render(unsigned int                    depth,
                                unsigned int                    normals,
                                const Parameters&               p,
                                const std::function<void(int)>& stage_done,
                                const std::function<bool()>&    context_lost)
{
    if (!m_failure.empty() || !m_textures[0] || p.width != m_width || p.height != m_height || !std::isfinite(p.radius) || p.radius <= 0 ||
        p.slices < 1 || p.steps < 1 || p.slices > 16 || p.steps > 32 || p.denoise_passes < 1 || p.denoise_passes > 3 ||
        (p.denoise_passes == 3 && !m_textures[5]))
        return false;
    State      state;
    const auto cancelled = [&]() {
        if (!context_lost || !context_lost())
            return false;
        state.forget_context();
        m_failure = "OpenGL context lost";
        return true;
    };
    auto texture = [](int unit, GLuint id) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, id);
        glBindSampler(unit, 0);
    };
    auto use = [&](GLuint program) {
        glUseProgram(program);
        glUniform2i(glGetUniformLocation(program, "full_size"), m_width, m_height);
    };
    // These images are reused across frames. Wait for earlier sampling (including
    // the final composite) before any dispatch overwrites their storage.
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    use(m_programs[0]);
    glUniformMatrix4fv(glGetUniformLocation(m_programs[0], "inverse_projection"), 1, GL_FALSE, p.inverse.data());
    glUniform1f(glGetUniformLocation(m_programs[0], "radius"), p.radius);
    texture(0, depth);
    for (int level = 0; level < 5; ++level)
        glBindImageTexture(level, m_textures[0], level, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
    glDispatchCompute(m_pw / 16, m_ph / 16, 1);
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
    if (stage_done)
        stage_done(0);
    if (cancelled())
        return false;
    use(m_programs[1]);
    glUniformMatrix4fv(glGetUniformLocation(m_programs[1], "inverse_projection"), 1, GL_FALSE, p.inverse.data());
    glUniform1f(glGetUniformLocation(m_programs[1], "radius"), p.radius);
    glUniform1i(glGetUniformLocation(m_programs[1], "perspective"), p.perspective);
    glUniform1i(glGetUniformLocation(m_programs[1], "slices"), p.slices);
    glUniform1i(glGetUniformLocation(m_programs[1], "steps"), p.steps);
    glUniform1i(glGetUniformLocation(m_programs[1], "max_lod"), std::min(4, int(std::log2(std::max(m_width, m_height)))));
    texture(0, m_textures[0]);
    texture(1, normals);
    glBindImageTexture(0, m_textures[1], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8UI);
    glBindImageTexture(1, m_textures[2], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8UI);
    glDispatchCompute((m_width + 7) / 8, (m_height + 7) / 8, 1);
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
    if (stage_done)
        stage_done(1);
    if (cancelled())
        return false;
    use(m_programs[2]);
    texture(1, m_textures[2]);
    texture(2, m_textures[0]);
    glBindImageTexture(0, m_textures[3], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8UI);
    glBindImageTexture(1, m_textures[4], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8);
    for (int pass = 0; pass < p.denoise_passes; ++pass) {
        // Preserve raw and both intermediate stages for capture; never read and
        // write the same image during a dispatch.
        const bool   final  = pass == p.denoise_passes - 1;
        const GLuint input  = pass == 0 ? m_textures[1] : pass == 1 ? m_textures[3] : m_textures[5];
        const GLuint output = final ? 0 : pass == 0 ? m_textures[3] : m_textures[5];
        glBindImageTexture(0, output, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8UI);
        texture(0, input);
        glUniform1i(glGetUniformLocation(m_programs[2], "final_apply"), pass == p.denoise_passes - 1);
        glDispatchCompute((m_width + 15) / 16, (m_height + 7) / 8, 1);
        glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
        if (stage_done)
            stage_done(pass + 2);
        if (cancelled())
            return false;
    }
    const GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        if (error == GL_CONTEXT_LOST)
            state.forget_context();
        m_failure = error == GL_CONTEXT_LOST ? "OpenGL context lost" : "XeGTAO dispatch failed";
        return false;
    }
    return true;
}

const GLAOComputeBackend::Capabilities& GLAOComputeBackend::capabilities()
{
    if (!m_probed) {
        m_capabilities = query_capabilities();
        m_probed       = true;
    }
    return m_capabilities;
}

GLAOComputeBackend::Capabilities GLAOComputeBackend::query_capabilities()
{
    Capabilities result;
    if (!glGetString(GL_VERSION)) {
        result.reason = "no current OpenGL context";
        return result;
    }
    if (!GLEW_VERSION_3_0) {
        result.reason = "OpenGL 4.3 required";
        return result;
    }
    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    if (major < 4 || (major == 4 && minor < 3) || !GLEW_VERSION_4_3) {
        result.reason = "OpenGL 4.3 required";
        return result;
    }
    if (!glDispatchCompute || !glBindImageTexture || !glMemoryBarrier || !glTexStorage2D || !glGetIntegeri_v) {
        result.reason = "required compute entry point unavailable";
        return result;
    }
    GLint invocations = 0, shared_bytes = 0, images = 0, compute_images = 0, textures = 0;
    glGetIntegerv(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, &invocations);
    glGetIntegerv(GL_MAX_COMPUTE_SHARED_MEMORY_SIZE, &shared_bytes);
    glGetIntegerv(GL_MAX_IMAGE_UNITS, &images);
    glGetIntegerv(GL_MAX_COMPUTE_IMAGE_UNIFORMS, &compute_images);
    glGetIntegerv(GL_MAX_COMPUTE_TEXTURE_IMAGE_UNITS, &textures);
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &result.max_texture_size);
    for (GLuint axis = 0; axis < 3; ++axis) {
        GLint size = 0;
        glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, axis, &size);
        glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, axis, &result.max_groups[axis]);
        if (size < (axis == 2 ? 1 : 8) || result.max_groups[axis] < 1) {
            result.reason = "8x8 compute work group unavailable";
            return result;
        }
    }
    if (invocations < 64 || shared_bytes < 1024 || images < 5 || compute_images < 5 || textures < 5 || result.max_texture_size < 1) {
        result.reason = "compute resource limits insufficient";
        return result;
    }
    result.supported = true;
    return result;
}

bool GLAOComputeBackend::valid_dispatch(const Capabilities& caps, int width, int height)
{
    return caps.supported && width > 0 && height > 0 && width <= caps.max_texture_size && height <= caps.max_texture_size &&
           (width - 1) / 8 + 1 <= caps.max_groups[0] && (height - 1) / 8 + 1 <= caps.max_groups[1];
}

GLAOComputeBackend::State::State()
{
    glGetIntegerv(GL_CURRENT_PROGRAM, &m_program);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &m_active_texture);
    for (GLuint i = 0; i < m_images.size(); ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &m_textures[i]);
        glGetIntegerv(GL_SAMPLER_BINDING, &m_samplers[i]);
        auto& image = m_images[i];
        glGetIntegeri_v(GL_IMAGE_BINDING_NAME, i, &image.name);
        glGetIntegeri_v(GL_IMAGE_BINDING_LEVEL, i, &image.level);
        glGetIntegeri_v(GL_IMAGE_BINDING_LAYERED, i, &image.layered);
        glGetIntegeri_v(GL_IMAGE_BINDING_LAYER, i, &image.layer);
        glGetIntegeri_v(GL_IMAGE_BINDING_ACCESS, i, &image.access);
        glGetIntegeri_v(GL_IMAGE_BINDING_FORMAT, i, &image.format);
    }
    glActiveTexture(m_active_texture);
}

void GLAOComputeBackend::State::forget_textures(const std::array<unsigned int, 6>& textures)
{
    for (size_t i = 0; i < m_textures.size(); ++i) {
        for (auto texture : textures) {
            if (!texture)
                continue;
            if (m_textures[i] == int(texture))
                m_textures[i] = 0;
            if (m_images[i].name == int(texture))
                m_images[i].name = 0;
        }
    }
}

GLAOComputeBackend::State::~State()
{
    if (!m_restore_state)
        return;
    for (GLuint i = 0; i < m_images.size(); ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, m_textures[i]);
        glBindSampler(i, m_samplers[i]);
        const auto& image = m_images[i];
        glBindImageTexture(i, image.name, image.level, image.layered != 0, image.layer, image.access, image.format);
    }
    glActiveTexture(m_active_texture);
    glUseProgram(m_program);
}

}}} // namespace Slic3r::GUI::AO
