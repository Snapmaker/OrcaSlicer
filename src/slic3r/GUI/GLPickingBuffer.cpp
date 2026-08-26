#include "GLPickingBuffer.hpp"

#include "3DScene.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/Utils.hpp"

#include <cassert>

namespace Slic3r {
namespace GUI {

bool GLPickingBuffer::ColorPixel::IsBackground() const
{
    return rgba[0] == 0 && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 0;
}

bool GLPickingBuffer::ColorPixel::IsValid() const
{
    return picking_checksum_alpha_channel(rgba[0], rgba[1], rgba[2]) == rgba[3];
}

uint32_t GLPickingBuffer::ColorPixel::DecodeId() const
{
    return picking_encode(rgba[0], rgba[1], rgba[2]);
}

GLPickingBuffer::~GLPickingBuffer()
{
    assert(_framebuffer == 0);
    assert(_colorTexture == 0);
    assert(_depthRenderbuffer == 0);
}

bool GLPickingBuffer::EnsureSize(int width, int height)
{
    if (width <= 0 || height <= 0)
        return false;

    const int maxTextureSize = OpenGLManager::get_gl_info().get_max_tex_size();
    if (maxTextureSize <= 0 || width > maxTextureSize || height > maxTextureSize)
        return false;

    if (IsReady() && width == _width && height == _height)
        return true;

    if (!IsReady() && width == _failedWidth && height == _failedHeight)
        return false;

    Reset();
    if (!CreateResources(width, height)) {
        DestroyResources();
        _failedWidth = width;
        _failedHeight = height;
        return false;
    }

    _failedWidth = 0;
    _failedHeight = 0;
    return true;
}

void GLPickingBuffer::Reset()
{
    assert(!_renderBound);
    DestroyResources();
    _failedWidth = 0;
    _failedHeight = 0;
}

bool GLPickingBuffer::IsReady() const
{
    return _framebuffer != 0 && _colorTexture != 0 && _depthRenderbuffer != 0 && _width > 0 && _height > 0 &&
           _framebufferType != OpenGLManager::EFramebufferType::Unknown;
}

int GLPickingBuffer::GetWidth() const
{
    return _width;
}

int GLPickingBuffer::GetHeight() const
{
    return _height;
}

bool GLPickingBuffer::BeginRender()
{
    if (!IsReady() || _renderBound)
        return false;

    GetFramebufferBindings(_previousDrawFramebuffer, _previousReadFramebuffer);
    BindFramebuffer(GL_FRAMEBUFFER, _framebuffer);
    _renderBound = true;
    return true;
}

void GLPickingBuffer::EndRender()
{
    if (!_renderBound)
        return;

    BindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(_previousDrawFramebuffer));
    BindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(_previousReadFramebuffer));
    _renderBound = false;
}

bool GLPickingBuffer::ReadPoint(int x, int y, PointSample& sample) const
{
    if (!ValidateRect(x, y, 1, 1))
        return false;

    GLint previousDrawFramebuffer = 0;
    GLint previousReadFramebuffer = 0;
    GetFramebufferBindings(previousDrawFramebuffer, previousReadFramebuffer);
    (void)previousDrawFramebuffer;
    BindFramebuffer(GL_READ_FRAMEBUFFER, _framebuffer);
    Slic3r::ScopeGuard restoreFramebuffer([this, previousReadFramebuffer]() {
        BindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousReadFramebuffer));
    });

    glsafe(::glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, sample.color.rgba.data()));
    glsafe(::glReadPixels(x, y, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &sample.depth));
    return true;
}

bool GLPickingBuffer::ReadColorRect(int x, int y, int width, int height, std::vector<ColorPixel>& pixels) const
{
    pixels.clear();
    if (!ValidateRect(x, y, width, height))
        return false;

    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
    pixels.resize(pixelCount);

    GLint previousDrawFramebuffer = 0;
    GLint previousReadFramebuffer = 0;
    GetFramebufferBindings(previousDrawFramebuffer, previousReadFramebuffer);
    BindFramebuffer(GL_READ_FRAMEBUFFER, _framebuffer);
    Slic3r::ScopeGuard restoreFramebuffer([this, previousReadFramebuffer]() {
        BindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousReadFramebuffer));
    });

    glsafe(::glReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data()));
    return true;
}

bool GLPickingBuffer::CreateResources(int width, int height)
{
    _framebufferType = OpenGLManager::get_framebuffers_type();
    if (_framebufferType == OpenGLManager::EFramebufferType::Unknown)
        return false;

    GLint previousDrawFramebuffer = 0;
    GLint previousReadFramebuffer = 0;
    GLint previousTexture = 0;
    GLint previousRenderbuffer = 0;
    GetFramebufferBindings(previousDrawFramebuffer, previousReadFramebuffer);
    glsafe(::glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture));
    const GLenum renderbufferBinding =
        _framebufferType == OpenGLManager::EFramebufferType::Arb ? GL_RENDERBUFFER_BINDING : GL_RENDERBUFFER_BINDING_EXT;
    glsafe(::glGetIntegerv(renderbufferBinding, &previousRenderbuffer));

    Slic3r::ScopeGuard restoreBindings([this, previousDrawFramebuffer, previousReadFramebuffer, previousTexture, previousRenderbuffer]() {
        if (_framebufferType == OpenGLManager::EFramebufferType::Arb)
            glsafe(::glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(previousRenderbuffer)));
        else if (_framebufferType == OpenGLManager::EFramebufferType::Ext)
            glsafe(::glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, static_cast<GLuint>(previousRenderbuffer)));

        glsafe(::glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture)));
        BindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDrawFramebuffer));
        BindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousReadFramebuffer));
    });

    if (_framebufferType == OpenGLManager::EFramebufferType::Arb) {
        glsafe(::glGenFramebuffers(1, &_framebuffer));
    } else {
        glsafe(::glGenFramebuffersEXT(1, &_framebuffer));
    }
    if (_framebuffer == 0)
        return false;

    BindFramebuffer(GL_FRAMEBUFFER, _framebuffer);

    glsafe(::glGenTextures(1, &_colorTexture));
    if (_colorTexture == 0)
        return false;

    glsafe(::glBindTexture(GL_TEXTURE_2D, _colorTexture));
    glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));

    if (_framebufferType == OpenGLManager::EFramebufferType::Arb) {
        glsafe(::glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _colorTexture, 0));
        glsafe(::glGenRenderbuffers(1, &_depthRenderbuffer));
        if (_depthRenderbuffer == 0)
            return false;
        glsafe(::glBindRenderbuffer(GL_RENDERBUFFER, _depthRenderbuffer));
        glsafe(::glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT, width, height));
        glsafe(::glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, _depthRenderbuffer));
    } else {
        glsafe(::glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, _colorTexture, 0));
        glsafe(::glGenRenderbuffersEXT(1, &_depthRenderbuffer));
        if (_depthRenderbuffer == 0)
            return false;
        glsafe(::glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, _depthRenderbuffer));
        glsafe(::glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH_COMPONENT, width, height));
        glsafe(::glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT,
                                              GL_RENDERBUFFER_EXT, _depthRenderbuffer));
    }

    const GLenum drawBuffers[] = {GL_COLOR_ATTACHMENT0};
    glsafe(::glDrawBuffers(1, drawBuffers));
    if (!CheckFramebufferComplete())
        return false;

    _width = width;
    _height = height;
    return true;
}

void GLPickingBuffer::DestroyResources()
{
    if (_framebufferType == OpenGLManager::EFramebufferType::Arb) {
        if (_depthRenderbuffer != 0)
            glsafe(::glDeleteRenderbuffers(1, &_depthRenderbuffer));
        if (_framebuffer != 0)
            glsafe(::glDeleteFramebuffers(1, &_framebuffer));
    } else if (_framebufferType == OpenGLManager::EFramebufferType::Ext) {
        if (_depthRenderbuffer != 0)
            glsafe(::glDeleteRenderbuffersEXT(1, &_depthRenderbuffer));
        if (_framebuffer != 0)
            glsafe(::glDeleteFramebuffersEXT(1, &_framebuffer));
    }

    if (_colorTexture != 0)
        glsafe(::glDeleteTextures(1, &_colorTexture));

    _framebuffer = 0;
    _colorTexture = 0;
    _depthRenderbuffer = 0;
    _width = 0;
    _height = 0;
    _framebufferType = OpenGLManager::EFramebufferType::Unknown;
}

void GLPickingBuffer::BindFramebuffer(GLenum target, GLuint framebuffer) const
{
    if (_framebufferType == OpenGLManager::EFramebufferType::Arb)
        glsafe(::glBindFramebuffer(target, framebuffer));
    else if (_framebufferType == OpenGLManager::EFramebufferType::Ext) {
        // Base EXT_framebuffer_object has one binding point. Split read/draw
        // targets require EXT_framebuffer_blit.
        glsafe(::glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, framebuffer));
    }
}

void GLPickingBuffer::GetFramebufferBindings(GLint& drawFramebuffer, GLint& readFramebuffer) const
{
    if (_framebufferType == OpenGLManager::EFramebufferType::Ext) {
        glsafe(::glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &drawFramebuffer));
        readFramebuffer = drawFramebuffer;
    } else {
        glsafe(::glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFramebuffer));
        glsafe(::glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer));
    }
}

bool GLPickingBuffer::CheckFramebufferComplete() const
{
    if (_framebufferType == OpenGLManager::EFramebufferType::Arb)
        return ::glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    if (_framebufferType == OpenGLManager::EFramebufferType::Ext)
        return ::glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) == GL_FRAMEBUFFER_COMPLETE_EXT;

    return false;
}

bool GLPickingBuffer::ValidateRect(int x, int y, int width, int height) const
{
    if (!IsReady() || x < 0 || y < 0 || width <= 0 || height <= 0)
        return false;

    return x <= _width - width && y <= _height - height;
}

} // namespace GUI
} // namespace Slic3r
