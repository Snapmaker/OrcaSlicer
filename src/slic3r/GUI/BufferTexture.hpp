#ifndef slic3r_BufferTexture_hpp_
#define slic3r_BufferTexture_hpp_

#include <cstddef>
#include <vector>

namespace Slic3r {
namespace GUI {

// Thin wrapper around an OpenGL texture buffer object (TBO): a linear float
// array uploaded once and sampled by shaders with texelFetch() through a
// samplerBuffer. Used by the GPU path pipeline to feed raw gcode path data
// (positions, widths, per-move attributes, segment records) to the shaders.
class BufferTexture
{
public:
    enum class EFormat : unsigned char
    {
        RGBA32F, // 4 floats per texel
        RG32F    // 2 floats per texel
    };

    BufferTexture() = default;
    ~BufferTexture();

    BufferTexture(const BufferTexture&) = delete;
    BufferTexture& operator=(const BufferTexture&) = delete;
    BufferTexture(BufferTexture&&) = delete;
    BufferTexture& operator=(BufferTexture&&) = delete;

    // (Re)uploads the given floats to the buffer; must be called with an
    // active GL context. floatCount must be a multiple of the texel size.
    void Upload(const float* data, size_t floatCount, EFormat format);
    void Upload(const std::vector<float>& data, EFormat format);

    // Binds the texture to the given texture unit, as samplerBuffer target.
    // The caller is responsible for restoring GL_TEXTURE0 afterwards.
    void Bind(unsigned int stage) const;

    // Releases the GL objects; safe to call even if nothing was uploaded.
    void Reset();

    bool IsValid() const { return _textureId != 0; }
    size_t TexelCount() const { return _texelCount; }

private:
    unsigned int _textureId{ 0 };
    unsigned int _bufferId{ 0 };
    size_t _texelCount{ 0 };
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_BufferTexture_hpp_
