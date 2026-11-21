#ifndef slic3r_GUI_AO_Diagnostics_GLAOProfiler_hpp_
#define slic3r_GUI_AO_Diagnostics_GLAOProfiler_hpp_

#include "../AOObserver.hpp"

#include <GL/glew.h>
#include <array>
#include <chrono>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace AO {

// Unified GPU timing: the bounded benchmark (30 warmup + 180 measured frames, schema 10 CSV)
// and the compact per-frame ring that feeds the render-statistics panel. No queries are
// allocated or submitted unless a consumer (benchmark or panel) is active.
class GLAOProfiler
{
public:
    // --- Bounded benchmark (opt-in). GL calls require the canvas context to be current. ---
    bool               request(const std::string& root);
    bool               benchmark_active() const { return m_active; }
    bool               benchmark_completed() const { return m_completed; }
    const std::string& benchmark_directory() const { return m_directory; }
    void               benchmark_begin(const std::string& signature);
    void               benchmark_pipeline(const std::string& description);
    void               benchmark_end();
    // When the owning context is unavailable, record the result and forget query names;
    // context destruction owns their eventual GPU cleanup.
    void benchmark_finish(const std::string& reason, bool context_current = true);
    void release();
    void forget();

    // --- Compact per-frame ring (render-statistics panel). ---
    bool timing_supported() const { return GLEW_VERSION_3_3 || GLEW_ARB_timer_query; }
    void begin_frame_timing();
    void stop_frame_timing() { m_recording = false; }
    void cancel_frame_timing()
    {
        m_recording                   = false;
        m_frame_pending[m_query_slot] = false;
    }
    // Routes a semantic stage to the benchmark ring and (when recording) the compact ring.
    void                         mark(AOStage stage);
    void                         end_frame_timing();
    bool                         has_gpu_sample() const { return m_has_gpu_sample; }
    const std::array<double, 8>& gpu_ms() const { return m_gpu_ms; }

    static constexpr int stage_index(AOStage stage) { return int(stage); }

private:
    // Benchmark ring. IDs 0..15 retain the original intervals; extra marks execute before the
    // frame-end mark (15). Schema 10 column contract depends on these values.
    static constexpr int QUERY_COUNT = 31;
    static constexpr int FRAME_END   = 15;
    struct Slot
    {
        std::array<unsigned int, QUERY_COUNT> queries{};
        unsigned int                          mask{0};
        int                                   frame{0};
        bool                                  pending{false};
        double                                cpu_ms{0};
        double                                elapsed_ms{0};
        std::array<double, QUERY_COUNT>       cpu_stamps{};
    };
    struct Row
    {
        int                             frame;
        unsigned int                    mask;
        std::array<double, QUERY_COUNT> stamps;
        double                          cpu_ms;
        double                          elapsed_ms;
        std::array<double, QUERY_COUNT> cpu_stamps;
    };
    void benchmark_mark(int index);
    void finish(const std::string& reason, bool context_current);

    std::array<Slot, 8>                   m_slots{};
    std::vector<Row>                      m_rows;
    std::string                           m_directory, m_signature;
    std::string                           m_pipeline;
    std::chrono::steady_clock::time_point m_started, m_cpu_start;
    int                                   m_slot{-1}, m_submitted{0}, m_skipped{0};
    bool                                  m_active{false};
    bool                                  m_completed{false};

    // Compact ring: four frames, nine timestamps, eight intervals.
    std::array<std::array<unsigned int, 9>, 4> m_frame_queries{};
    std::array<bool, 4>                        m_frame_pending{};
    std::array<double, 8>                      m_gpu_ms{};
    unsigned int                               m_query_slot{0};
    bool                                       m_recording{false};
    bool                                       m_has_gpu_sample{false};
    bool                                       m_frame_queries_created{false};
};

}}} // namespace Slic3r::GUI::AO
#endif
