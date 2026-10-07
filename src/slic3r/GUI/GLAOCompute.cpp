#include "GLAOCompute.hpp"

#include <GL/glew.h>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <vector>

namespace Slic3r { namespace GUI {

void GLAOCompute::release()
{
    glDeleteTextures(int(m_textures.size()), m_textures.data());
    m_textures = {};
    m_width = m_height = m_pw = m_ph = 0;
}

void GLAOCompute::forget_context()
{
    m_textures = {};
    m_programs = {};
    m_width = m_height = m_pw = m_ph = 0;
    m_failure.clear();
    m_probed       = false;
    m_capabilities = {};
}

void GLAOCompute::disable(const std::string& reason)
{
    release();
    m_failure = reason;
}

bool GLAOCompute::prepare(int width, int height, const std::array<unsigned int, 3>& programs, int denoise_passes)
{
    if (denoise_passes < 1 || denoise_passes > 3)
        return false;
    if (!m_failure.empty())
        return false;
    const auto& caps = capabilities();
    if (!caps.supported) {
        m_failure = caps.reason;
        return false;
    }
    if (!valid_dispatch(caps, width, height) || width > caps.max_texture_size - 15 || height > caps.max_texture_size - 15) {
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
    m_pw                    = ((width + 15) / 16) * 16;
    m_ph                    = ((height + 15) / 16) * 16;
    const int texture_count = denoise_passes == 3 ? 6 : 5;
    glGenTextures(texture_count, m_textures.data());
    const GLenum formats[] = {GL_R32F, GL_R8UI, GL_R8UI, GL_R8UI, GL_R8, GL_R8UI};
    const char*  labels[]  = {"XeGTAO.LinearDepthMips",     "XeGTAO.RawEncoded", "XeGTAO.Edges",
                              "XeGTAO.DenoiseIntermediate", "XeGTAO.Visibility", "XeGTAO.DenoiseSecondIntermediate"};
    glActiveTexture(GL_TEXTURE0);
    for (int i = 0; i < texture_count; ++i) {
        glBindTexture(GL_TEXTURE_2D, m_textures[i]);
        glTexStorage2D(GL_TEXTURE_2D, i == 0 ? 5 : 1, formats[i], i == 0 ? m_pw : width, i == 0 ? m_ph : height);
        if (glObjectLabel)
            glObjectLabel(GL_TEXTURE, m_textures[i], -1, labels[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, i == 0 ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    if (glGetError() != GL_NO_ERROR) {
        disable("CS texture allocation failed");
        return false;
    }
    return true;
}

bool GLAOCompute::render(unsigned int depth, unsigned int normals, const Parameters& p, const std::function<void(int)>& stage_done)
{
    if (!m_failure.empty() || !m_textures[0] || p.width != m_width || p.height != m_height || !std::isfinite(p.radius) || p.radius <= 0 ||
        p.slices < 1 || p.steps < 1 || p.slices > 16 || p.steps > 32 || p.denoise_passes < 1 || p.denoise_passes > 3 ||
        (p.denoise_passes == 3 && !m_textures[5]))
        return false;
    State state;
    auto  texture = [](int unit, GLuint id) {
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
    }
    const GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        m_failure = error == GL_CONTEXT_LOST ? "OpenGL context lost" : "XeGTAO dispatch failed";
        return false;
    }
    return true;
}

bool GLAOCompute::capture(const std::string& directory, int denoise_passes) const
{
    if (!m_textures[0])
        return false;
    State        state;
    GLint        pack[5]{};
    const GLenum names[] = {GL_PACK_ALIGNMENT, GL_PACK_ROW_LENGTH, GL_PACK_SKIP_ROWS, GL_PACK_SKIP_PIXELS, GL_PIXEL_PACK_BUFFER_BINDING};
    for (int i = 0; i < 5; ++i)
        glGetIntegerv(names[i], &pack[i]);
    struct Restore
    {
        const GLenum* names;
        GLint*        pack;
        ~Restore()
        {
            for (int i = 0; i < 4; ++i)
                glPixelStorei(names[i], pack[i]);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, pack[4]);
        }
    } restore{names, pack};
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
    glActiveTexture(GL_TEXTURE0);
    const std::filesystem::path root(directory);
    bool                        ok = true;
    glBindTexture(GL_TEXTURE_2D, m_textures[0]);
    for (int level = 0; level < 5; ++level) {
        int                w = m_pw >> level, h = m_ph >> level;
        std::vector<float> data(size_t(w) * h);
        glGetTexImage(GL_TEXTURE_2D, level, GL_RED, GL_FLOAT, data.data());
        std::ofstream out(root / ("cs-depth-mip-" + std::to_string(level) + ".pfm"), std::ios::binary);
        out << "Pf\n" << w << ' ' << h << "\n-1.0\n";
        out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size() * sizeof(float)));
        ok &= bool(out);
    }
    const char*                files[] = {"cs-raw-encoded.pgm", "cs-edges-packed.pgm", "cs-denoise-first-encoded.pgm", "cs-final.pgm",
                                          "cs-denoise-second-encoded.pgm"};
    std::vector<unsigned char> data(size_t(m_width) * m_height);
    auto                       pgm = [&](const std::string& name, const std::vector<unsigned char>& pixels) {
        std::ofstream out(root / name, std::ios::binary);
        out << "P5\n" << m_width << ' ' << m_height << "\n255\n";
        for (int y = m_height - 1; y >= 0; --y)
            out.write(reinterpret_cast<const char*>(pixels.data() + size_t(y) * m_width), m_width);
        ok &= bool(out);
    };
    for (int i = 1; i < (denoise_passes == 3 ? 6 : 5); ++i) {
        if (i == 3 && denoise_passes == 1)
            continue;
        glBindTexture(GL_TEXTURE_2D, m_textures[i]);
        glGetTexImage(GL_TEXTURE_2D, 0, i == 4 ? GL_RED : GL_RED_INTEGER, GL_UNSIGNED_BYTE, data.data());
        pgm(files[i - 1], data);
        if (i == 2)
            for (int direction = 0; direction < 4; ++direction) {
                auto channel = data;
                for (auto& value : channel)
                    value = ((value >> (6 - direction * 2)) & 3) * 85;
                pgm("cs-edge-" + std::to_string(direction) + ".pgm", channel);
            }
    }
    std::ofstream meta(root / "cs-info.txt");
    meta << "backend=cs\nupstream=" << upstream << "\nprecision=fp32\npadded_size=" << m_pw << 'x' << m_ph
         << "\nedge_order=left,right,top(+Y),bottom(-Y)\nencoded_visibility_scale=1.5\ndenoise_passes=" << denoise_passes
         << "\nworking_texture_bytes=" << (size_t(m_pw) * m_ph * 4 * 341 / 256 + size_t(m_width) * m_height * (m_textures[5] ? 5 : 4))
         << '\n';
    return ok && bool(meta) && glGetError() == GL_NO_ERROR;
}

GLAOCompute::Request GLAOCompute::parse_request(const char* value)
{
    if (!value || std::strcmp(value, "auto") == 0)
        return Request::Auto;
    if (std::strcmp(value, "cs") == 0)
        return Request::CS;
    // Unknown diagnostic requests must never silently enable an experimental backend.
    return Request::FS;
}

const GLAOCompute::Capabilities& GLAOCompute::capabilities()
{
    if (!m_probed) {
        m_capabilities = query_capabilities();
        m_probed       = true;
    }
    return m_capabilities;
}

GLAOCompute::Capabilities GLAOCompute::query_capabilities()
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

bool GLAOCompute::valid_dispatch(const Capabilities& caps, int width, int height)
{
    return caps.supported && width > 0 && height > 0 && width <= caps.max_texture_size && height <= caps.max_texture_size &&
           (width - 1) / 8 + 1 <= caps.max_groups[0] && (height - 1) / 8 + 1 <= caps.max_groups[1];
}

GLAOCompute::Selection GLAOCompute::select(Request request, const Capabilities& caps, bool backend_ready, bool benchmark)
{
    Selection result;
    if (request == Request::FS)
        result.reason = "FS explicitly selected";
    else if (!caps.supported)
        result.reason = caps.reason;
    else if (!backend_ready)
        result.reason = "CS backend not ready";
    else
        result.use_cs = true;
    result.benchmark_allowed = !(benchmark && request == Request::CS && !result.use_cs);
    return result;
}

GLAOCompute::State::State()
{
    glGetIntegerv(GL_CURRENT_PROGRAM, &m_program);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &m_active_texture);
    for (GLuint i = 0; i < m_images.size(); ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &m_textures[i]);
        glGetIntegeri_v(GL_SAMPLER_BINDING, i, &m_samplers[i]);
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

void GLAOCompute::State::forget_textures(const std::array<unsigned int, 6>& textures)
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

GLAOCompute::State::~State()
{
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

}} // namespace Slic3r::GUI
