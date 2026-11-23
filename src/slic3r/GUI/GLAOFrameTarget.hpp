#ifndef slic3r_GLAOFrameTarget_hpp_
#define slic3r_GLAOFrameTarget_hpp_

#include <array>
#include <functional>
#include <string>

namespace Slic3r { namespace GUI {

// Canvas-owned full-frame target. AO workspace rebuilds never delete these names.
class GLAOFrameTarget
{
public:
    struct Description
    {
        const void*  context{nullptr};
        int          width{0}, height{0}, samples{0};
        unsigned int color_format{0}, depth_format{0};
        bool         stencil{false};
    };
    class Session
    {
    public:
        explicit Session(GLAOFrameTarget* target);
        ~Session();
        Session(const Session&)            = delete;
        Session& operator=(const Session&) = delete;
        bool     present(const std::function<void(int)>& mark = {});
        void     restore();
        void     forget(); // Lost context: no GL calls in the destructor.
    private:
        GLAOFrameTarget*   m_target;
        int                m_read{0}, m_draw{0}, m_renderbuffer{0};
        std::array<int, 4> m_viewport{};
        bool               m_active{false};
    };
    GLAOFrameTarget()                                    = default;
    GLAOFrameTarget(const GLAOFrameTarget&)              = delete;
    GLAOFrameTarget&   operator=(const GLAOFrameTarget&) = delete;
    bool               prepare(const Description& description);
    void               release();
    void               forget_context();
    unsigned int       framebuffer() const { return m_fbo[0]; }
    const Description& description() const { return m_description; }
    const std::string& failure_reason() const { return m_failure; }

private:
    Description                 m_description;
    std::array<unsigned int, 2> m_fbo{};
    std::array<unsigned int, 3> m_rbo{};
    std::string                 m_failure;
};

}} // namespace Slic3r::GUI
#endif
