#ifndef slic3r_GUI_FrameProfiler_hpp_
#define slic3r_GUI_FrameProfiler_hpp_

// Orca #15884 Stage B, adapted for Edge.
//
// Orca ships FrameProfiler as a .cpp/.hpp pair plus a Preferences toggle and a
// SceneBenchmark averaging API. Edge keeps the idea (scoped CPU timers, optional
// GL timestamp queries, exponential smoothing) but:
//   - header-only, so src/slic3r/CMakeLists.txt stays untouched (#238 also edits it);
//   - no AppConfig key / Preferences toggle: armed only while the existing BBS
//     "Render statistics" window is visible (off by default, zero cost when hidden);
//   - CPU-only fallback when GL timer queries are missing (older GL, or a macOS
//     core profile where GL_EXTENSIONS is invalid and the 3.3 entry points are not
//     loaded).
// SceneBenchmark / start_averaging are not ported.

#include <array>
#include <chrono>
#include <cstring>
#include <vector>

namespace Slic3r {
namespace GUI {

// One named pass. name must have static lifetime (string literal).
struct FrameTimingSection
{
    const char *name{nullptr};
    double      cpu_ms{0.0};
    double      gpu_ms{0.0};
};

inline double smooth_frame_timing_ms(double previous, double sample, double alpha = 0.1)
{
    return previous + alpha * (sample - previous);
}

// Exponential moving average. A new name is taken as-is (first sample). Names
// present last frame but missing from sample are dropped — that pass did not run.
inline std::vector<FrameTimingSection> smooth_frame_timings(const std::vector<FrameTimingSection> &previous,
                                                            const std::vector<FrameTimingSection> &sample,
                                                            double                                 alpha = 0.1)
{
    std::vector<FrameTimingSection> out;
    out.reserve(sample.size());
    for (FrameTimingSection section : sample) {
        for (const FrameTimingSection &prev : previous) {
            if (prev.name != nullptr && section.name != nullptr && std::strcmp(prev.name, section.name) == 0) {
                section.cpu_ms = smooth_frame_timing_ms(prev.cpu_ms, section.cpu_ms, alpha);
                section.gpu_ms = smooth_frame_timing_ms(prev.gpu_ms, section.gpu_ms, alpha);
                break;
            }
        }
        out.push_back(section);
    }
    return out;
}

} // namespace GUI
} // namespace Slic3r

#ifndef SLIC3R_FRAME_PROFILER_NO_GL

#include <glad/gl.h>

#include "3DScene.hpp"

namespace Slic3r {
namespace GUI {

class FrameProfiler
{
public:
    static constexpr double SMOOTHING = 0.1;

    class Scope
    {
    public:
        Scope(FrameProfiler &profiler, const char *name)
            : m_profiler(profiler.is_recording() ? &profiler : nullptr), m_name(name)
        {
            if (m_profiler != nullptr)
                m_profiler->begin_scope();
        }

        Scope(const Scope &)            = delete;
        Scope &operator=(const Scope &) = delete;

        Scope(Scope &&other) noexcept : m_profiler(other.m_profiler), m_name(other.m_name) { other.m_profiler = nullptr; }

        Scope &operator=(Scope &&other) noexcept
        {
            if (this != &other) {
                finish();
                m_profiler       = other.m_profiler;
                m_name           = other.m_name;
                other.m_profiler = nullptr;
            }
            return *this;
        }

        ~Scope() { finish(); }

    private:
        void finish()
        {
            if (m_profiler != nullptr) {
                m_profiler->end_scope(m_name);
                m_profiler = nullptr;
            }
        }

        FrameProfiler *m_profiler{nullptr};
        const char    *m_name{nullptr};
    };

    bool is_recording() const { return m_recording != nullptr || m_cpu_only; }
    bool gpu_queries_active() const { return m_gpu_support == GpuTimerSupport::Yes; }

    const std::vector<FrameTimingSection> &sections() const { return m_sections; }

    const char *gpu_timer_mode_label() const
    {
        switch (m_gpu_support) {
        case GpuTimerSupport::Yes: return "available (GL 3.3 / timer query)";
        case GpuTimerSupport::No:  return "CPU only (older GL / no timer query)";
        default:                   return "not probed yet";
        }
    }

    void begin_frame()
    {
        m_cpu_only   = false;
        m_recording  = nullptr;
        m_scope_open = false;
        m_cpu_count  = 0;

        if (m_gpu_support == GpuTimerSupport::Unknown)
            m_gpu_support = detect_gpu_timer_support() ? GpuTimerSupport::Yes : GpuTimerSupport::No;

        if (m_gpu_support == GpuTimerSupport::Yes) {
            collect(false);
            Frame &frame = m_frames[m_next];
            // Every in-flight slot still waits on the GPU: keep CPU this frame, skip queries.
            if (!frame.pending && acquire_queries(frame)) {
                frame.count     = 0;
                m_recording     = &frame;
                m_last_mark     = clock::now();
                return;
            }
        }

        m_cpu_only  = true;
        m_last_mark = clock::now();
    }

    void end_frame()
    {
        if (m_scope_open)
            end_scope("unscoped");

        std::vector<FrameTimingSection> sample;
        if (m_recording != nullptr) {
            sample.reserve(m_recording->count);
            for (size_t i = 0; i < m_recording->count; ++i) {
                double gpu_ms = 0.0;
                for (const FrameTimingSection &prev : m_sections) {
                    if (prev.name != nullptr && m_recording->names[i] != nullptr && std::strcmp(prev.name, m_recording->names[i]) == 0) {
                        gpu_ms = prev.gpu_ms;
                        break;
                    }
                }
                sample.push_back({m_recording->names[i], m_recording->cpu_ms[i], gpu_ms});
            }
            m_recording->pending = m_recording->count > 0;
            m_recording          = nullptr;
            m_next               = (m_next + 1) % FRAMES_IN_FLIGHT;
        } else if (m_cpu_only) {
            sample.reserve(m_cpu_count);
            for (size_t i = 0; i < m_cpu_count; ++i)
                sample.push_back({m_cpu_names[i], m_cpu_ms[i], 0.0});
        }

        if (!sample.empty())
            m_sections = smooth_frame_timings(m_sections, sample, SMOOTHING);

        m_cpu_only  = false;
        m_cpu_count = 0;
    }

    // Frees query objects. GL context must be current.
    void reset()
    {
        for (Frame &frame : m_frames) {
            if (frame.queries[0] != 0)
                glsafe(::glDeleteQueries(GLsizei(frame.queries.size()), frame.queries.data()));
            frame = Frame();
        }
        m_recording  = nullptr;
        m_cpu_only   = false;
        m_scope_open = false;
        m_cpu_count  = 0;
        m_sections.clear();
        m_next = 0;
    }

private:
    using clock = std::chrono::steady_clock;

    static constexpr size_t FRAMES_IN_FLIGHT = 4;
    static constexpr size_t MAX_SECTIONS     = 32;

    enum class GpuTimerSupport { Unknown, Yes, No };

    struct Frame
    {
        std::array<GLuint, 2 * MAX_SECTIONS>      queries{};
        std::array<const char *, MAX_SECTIONS>    names{};
        std::array<double, MAX_SECTIONS>          cpu_ms{};
        size_t                                    count{0};
        bool                                      pending{false};
    };

    static bool pointers_loaded()
    {
        return glad_glQueryCounter != nullptr && glad_glGetQueryObjectui64v != nullptr && glad_glGetQueryObjectiv != nullptr &&
               glad_glGenQueries != nullptr && glad_glDeleteQueries != nullptr;
    }

    // GL_TIMESTAMP / glQueryCounter are core in 3.3. Edge's GLAD loads those
    // pointers only with GLAD_GL_VERSION_3_3, so a 3.2 + ARB_timer_query context
    // falls back to CPU (safe, no GetProcAddress maze).
    //
    // On a core profile (macOS always; Linux/Windows when requested) GL_EXTENSIONS
    // is invalid — never probe the string there. Function pointers + the 3.3 flag
    // are the availability test.
    static bool detect_gpu_timer_support()
    {
        if (!pointers_loaded())
            return false;
        if (GLAD_GL_VERSION_3_3)
            return true;

#ifndef __APPLE__
        // Compatibility profile only: GL_EXTENSIONS is a space-separated string.
        ::glGetError();
        const char *ext = reinterpret_cast<const char *>(::glGetString(GL_EXTENSIONS));
        const GLenum err = ::glGetError();
        if (err != GL_NO_ERROR || ext == nullptr)
            return false;
        return std::strstr(ext, "GL_ARB_timer_query") != nullptr && pointers_loaded();
#else
        // macOS is a core profile. Without the 3.3 entry points, stay CPU-only.
        return false;
#endif
    }

    bool acquire_queries(Frame &frame)
    {
        if (frame.queries[0] != 0)
            return true;
        ::glGetError();
        glsafe(::glGenQueries(GLsizei(frame.queries.size()), frame.queries.data()));
        if (::glGetError() != GL_NO_ERROR) {
            frame.queries.fill(0);
            m_gpu_support = GpuTimerSupport::No;
            return false;
        }
        return true;
    }

    void begin_scope()
    {
        if (!is_recording() || m_scope_open)
            return;
        m_scope_open = true;
        m_last_mark  = clock::now();
        if (m_recording != nullptr && m_recording->count < MAX_SECTIONS)
            glsafe(::glQueryCounter(m_recording->queries[2 * m_recording->count], GL_TIMESTAMP));
    }

    void end_scope(const char *name)
    {
        if (!m_scope_open)
            return;
        m_scope_open = false;

        const double cpu_ms = std::chrono::duration<double, std::milli>(clock::now() - m_last_mark).count();

        if (m_recording != nullptr && m_recording->count < MAX_SECTIONS) {
            Frame &frame = *m_recording;
            glsafe(::glQueryCounter(frame.queries[2 * frame.count + 1], GL_TIMESTAMP));
            // Else the GPU time would include later CPU work until the driver flushes.
            glsafe(::glFlush());
            frame.names[frame.count]  = name;
            frame.cpu_ms[frame.count] = cpu_ms;
            ++frame.count;
        } else if (m_cpu_count < MAX_SECTIONS) {
            m_cpu_names[m_cpu_count] = name;
            m_cpu_ms[m_cpu_count]    = cpu_ms;
            ++m_cpu_count;
        }
    }

    void collect(bool wait)
    {
        for (size_t i = 0; i < FRAMES_IN_FLIGHT; ++i) {
            Frame &frame = m_frames[(m_next + i) % FRAMES_IN_FLIGHT];
            if (!frame.pending)
                continue;

            if (!wait) {
                GLint available = 0;
                glsafe(::glGetQueryObjectiv(frame.queries[2 * frame.count - 1], GL_QUERY_RESULT_AVAILABLE, &available));
                if (available == 0)
                    break;
            }

            std::array<GLuint64, 2 * MAX_SECTIONS> stamps{};
            for (size_t j = 0; j < 2 * frame.count; ++j)
                glsafe(::glGetQueryObjectui64v(frame.queries[j], GL_QUERY_RESULT, &stamps[j]));
            frame.pending = false;

            // CPU was already published at end_frame. Only fold in GPU ms.
            for (size_t j = 0; j < frame.count; ++j) {
                const double gpu_ms = stamps[2 * j + 1] > stamps[2 * j] ? double(stamps[2 * j + 1] - stamps[2 * j]) * 1e-6 : 0.0;
                bool         found  = false;
                for (FrameTimingSection &section : m_sections) {
                    if (section.name != nullptr && frame.names[j] != nullptr && std::strcmp(section.name, frame.names[j]) == 0) {
                        section.gpu_ms = smooth_frame_timing_ms(section.gpu_ms, gpu_ms, SMOOTHING);
                        found          = true;
                        break;
                    }
                }
                if (!found)
                    m_sections.push_back({frame.names[j], frame.cpu_ms[j], gpu_ms});
            }
        }
    }

    std::array<Frame, FRAMES_IN_FLIGHT> m_frames;
    Frame                              *m_recording{nullptr};
    size_t                              m_next{0};
    clock::time_point                   m_last_mark{};
    std::vector<FrameTimingSection>     m_sections;
    GpuTimerSupport                     m_gpu_support{GpuTimerSupport::Unknown};
    bool                                m_cpu_only{false};
    bool                                m_scope_open{false};
    std::array<const char *, MAX_SECTIONS> m_cpu_names{};
    std::array<double, MAX_SECTIONS>       m_cpu_ms{};
    size_t                                 m_cpu_count{0};
};

} // namespace GUI
} // namespace Slic3r

#endif // !SLIC3R_FRAME_PROFILER_NO_GL

#endif // slic3r_GUI_FrameProfiler_hpp_
