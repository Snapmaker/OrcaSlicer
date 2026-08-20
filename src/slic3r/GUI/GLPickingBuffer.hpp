#pragma once

#include "OpenGLManager.hpp"

#include <GL/glew.h>

#include <array>
#include <cstdint>
#include <vector>

namespace Slic3r {
namespace GUI {

/**
 * @brief Owns the OpenGL resources used by volume picking.
 *
 * Resource creation, rendering, reading and destruction require the owning
 * OpenGL context to be current.
 */
class GLPickingBuffer
{
public:
    struct ColorPixel
    {
        std::array<GLubyte, 4> rgba{};

        bool IsBackground() const;
        bool IsValid() const;
        uint32_t DecodeId() const;
    };

    struct PointSample
    {
        ColorPixel color;
        GLfloat depth{1.0f};
    };

public:
    GLPickingBuffer() = default;
    ~GLPickingBuffer();

    GLPickingBuffer(const GLPickingBuffer&) = delete;
    GLPickingBuffer& operator=(const GLPickingBuffer&) = delete;

    /**
     * @brief Creates or resizes the framebuffer attachments.
     * @return true when the buffer is complete and ready for rendering.
     */
    bool EnsureSize(int width, int height);

    /**
     * @brief Releases all resources. The owning context must be current.
     */
    void Reset();

    bool IsReady() const;
    int GetWidth() const;
    int GetHeight() const;

    /**
     * @brief Binds the FBO and saves the existing draw and read bindings.
     */
    bool BeginRender();

    /**
     * @brief Restores the framebuffer bindings saved by BeginRender().
     */
    void EndRender();

    bool ReadPoint(int x, int y, PointSample& sample) const;
    bool ReadColorRect(int x, int y, int width, int height,
                       std::vector<ColorPixel>& pixels) const;

private:
    bool CreateResources(int width, int height);
    void DestroyResources();
    void BindFramebuffer(GLenum target, GLuint framebuffer) const;
    void GetFramebufferBindings(GLint& drawFramebuffer,
                                GLint& readFramebuffer) const;
    bool CheckFramebufferComplete() const;
    bool ValidateRect(int x, int y, int width, int height) const;

private:
    GLuint _framebuffer{0};
    GLuint _colorTexture{0};
    GLuint _depthRenderbuffer{0};

    int _width{0};
    int _height{0};

    GLint _previousDrawFramebuffer{0};
    GLint _previousReadFramebuffer{0};
    bool _renderBound{false};

    int _failedWidth{0};
    int _failedHeight{0};

    OpenGLManager::EFramebufferType _framebufferType{
        OpenGLManager::EFramebufferType::Unknown
    };
};

} // namespace GUI
} // namespace Slic3r
