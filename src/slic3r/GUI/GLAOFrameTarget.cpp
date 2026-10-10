#include "GLAOFrameTarget.hpp"
#include <GL/glew.h>

namespace Slic3r { namespace GUI {
namespace {
bool same_description(const GLAOFrameTarget::Description& a, const GLAOFrameTarget::Description& b)
{
    return a.context == b.context && a.width == b.width && a.height == b.height && a.samples == b.samples &&
           a.color_format == b.color_format && a.depth_format == b.depth_format && a.stencil == b.stencil;
}
bool errors(bool* context_lost = nullptr)
{
    bool   failed = false;
    GLenum error  = GL_NO_ERROR;
    while ((error = glGetError()) != GL_NO_ERROR) {
        failed = true;
        if (context_lost && error == GL_CONTEXT_LOST)
            *context_lost = true;
    }
    return failed;
}
} // namespace

bool GLAOFrameTarget::prepare(const Description& description)
{
    if (!m_failure.empty() && same_description(description, m_description))
        return false;
    if (description.width <= 0 || description.height <= 0 || description.samples <= 1 || !GLEW_VERSION_4_0)
        return false;
    if (m_description.context != description.context)
        forget_context(); // The canvas normally releases the old owning context first.
    // Preserve a pending reset signal before reusing or deleting any resource names.
    bool context_lost = false;
    errors(&context_lost);
    if (context_lost) {
        forget_context();
        m_description = description;
        m_failure     = "OpenGL context lost";
        return false;
    }
    if (m_fbo[0] && same_description(description, m_description))
        return true;
    GLint read = 0, draw = 0, renderbuffer = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
    errors(&context_lost);
    if (context_lost) {
        forget_context();
        m_description = description;
        m_failure     = "OpenGL context lost";
        return false;
    }
    // Never resize storage while a session is using it.
    if (m_fbo[0] && (GLuint(read) == m_fbo[0] || GLuint(draw) == m_fbo[0] || GLuint(read) == m_fbo[1] || GLuint(draw) == m_fbo[1] ||
                     (renderbuffer != 0 &&
                      (GLuint(renderbuffer) == m_rbo[0] || GLuint(renderbuffer) == m_rbo[1] || GLuint(renderbuffer) == m_rbo[2])))) {
        m_failure = "frame target resize attempted during drawing";
        return false;
    }
    release();
    m_description = description;
    glGenFramebuffers(2, m_fbo.data());
    glGenRenderbuffers(3, m_rbo.data());
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo[0]);
    glBindRenderbuffer(GL_RENDERBUFFER, m_rbo[0]);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, description.samples, description.color_format, description.width, description.height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_rbo[0]);
    glBindRenderbuffer(GL_RENDERBUFFER, m_rbo[1]);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, description.samples, description.depth_format, description.width, description.height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, description.stencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                              m_rbo[1]);
    bool  complete       = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    GLint actual_samples = 0;
    glGetIntegerv(GL_SAMPLES, &actual_samples);
    complete &= actual_samples == description.samples;
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo[1]);
    glBindRenderbuffer(GL_RENDERBUFFER, m_rbo[2]);
    glRenderbufferStorage(GL_RENDERBUFFER, description.color_format, description.width, description.height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_rbo[2]);
    complete &= glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    const bool failed = errors(&context_lost) || !complete;
    if (context_lost) {
        forget_context();
        m_description = description;
        m_failure     = "OpenGL context lost";
        return false;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
    glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
    if (failed) {
        release();
        m_description = description;
        m_failure     = "full-frame MSAA target allocation failed";
        return false;
    }
    m_failure.clear();
    return true;
}

void GLAOFrameTarget::release()
{
    // No names exist on legacy contexts where the core entry points may be absent.
    if (m_fbo[0] || m_fbo[1])
        glDeleteFramebuffers(2, m_fbo.data());
    if (m_rbo[0] || m_rbo[1] || m_rbo[2])
        glDeleteRenderbuffers(3, m_rbo.data());
    forget_context();
}

void GLAOFrameTarget::forget_context()
{
    m_fbo         = {};
    m_rbo         = {};
    m_description = {};
    m_failure.clear();
}

GLAOFrameTarget::Session::Session(GLAOFrameTarget* target) : m_target(target)
{
    if (!target)
        return;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &m_read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &m_draw);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &m_renderbuffer);
    glGetIntegerv(GL_VIEWPORT, m_viewport.data());
    glBindFramebuffer(GL_FRAMEBUFFER, target->m_fbo[0]);
    glViewport(0, 0, target->m_description.width, target->m_description.height);
    m_active = true;
}

GLAOFrameTarget::Session::~Session() { restore(); }

bool GLAOFrameTarget::Session::present(const std::function<void(int)>& mark)
{
    if (!m_active)
        return true;
    GLint current_draw = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &current_draw);
    bool       lost_before_present = false;
    const bool prior_error         = errors(&lost_before_present);
    if (lost_before_present) {
        forget();
        m_target->m_failure = "OpenGL context lost";
        return false;
    }
    if (prior_error) {
        m_target->m_failure = "OpenGL error before full-frame presentation";
        restore();
        return false;
    }
    if (GLuint(current_draw) != m_target->m_fbo[0]) {
        m_target->m_failure = "a nested pass changed the full-frame draw target";
        restore();
        return false;
    }
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    if (mark)
        mark(11);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_target->m_fbo[0]);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_target->m_fbo[1]);
    const auto& d = m_target->m_description;
    glBlitFramebuffer(0, 0, d.width, d.height, 0, 0, d.width, d.height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    if (mark)
        mark(18);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_target->m_fbo[1]);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_draw);
    glBlitFramebuffer(0, 0, d.width, d.height, m_viewport[0], m_viewport[1], m_viewport[0] + m_viewport[2], m_viewport[1] + m_viewport[3],
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    if (mark)
        mark(12);
    bool       context_lost = false;
    const bool failed       = errors(&context_lost);
    if (context_lost) {
        forget();
        m_target->m_failure = "OpenGL context lost";
        return false;
    }
    if (scissor)
        glEnable(GL_SCISSOR_TEST);
    restore();
    if (failed)
        m_target->m_failure = "full-frame MSAA color presentation failed";
    return !failed;
}

void GLAOFrameTarget::Session::restore()
{
    if (!m_active)
        return;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_read);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_draw);
    glBindRenderbuffer(GL_RENDERBUFFER, m_renderbuffer);
    glViewport(m_viewport[0], m_viewport[1], m_viewport[2], m_viewport[3]);
    m_active = false;
}

void GLAOFrameTarget::Session::forget() { m_active = false; }
}} // namespace Slic3r::GUI
