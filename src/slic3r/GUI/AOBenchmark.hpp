#ifndef slic3r_AOBenchmark_hpp_
#define slic3r_AOBenchmark_hpp_

#include <array>
#include <chrono>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI {

// Opt-in, bounded benchmark. GL calls require the canvas context to be current.
class AOBenchmark
{
public:
    bool request(const std::string& root);
    bool active() const { return m_active; }
    bool completed() const { return m_completed; }
    const std::string& directory() const { return m_directory; }
    void begin(const std::string& signature);
    void mark(int index);
    void pipeline(const std::string& description);
    void end();
    void finish(const std::string& reason);
    void release();
    void forget();

private:
    // IDs 0..15 retain the original intervals. Extra marks execute before frame-end (15).
    static constexpr int QUERY_COUNT = 31;
    static constexpr int FRAME_END = 15;
    struct Slot {
        std::array<unsigned int, QUERY_COUNT> queries{};
        unsigned int mask{0};
        int frame{0};
        bool pending{false};
        double cpu_ms{0};
        double elapsed_ms{0};
        std::array<double, QUERY_COUNT> cpu_stamps{};
    };
    struct Row {
        int frame;
        unsigned int mask;
        std::array<double, QUERY_COUNT> stamps;
        double cpu_ms;
        double elapsed_ms;
        std::array<double, QUERY_COUNT> cpu_stamps;
    };
    std::array<Slot, 8> m_slots{};
    std::vector<Row> m_rows;
    std::string m_directory, m_signature;
    std::string                           m_pipeline;
    std::chrono::steady_clock::time_point m_started, m_cpu_start;
    int m_slot{-1}, m_submitted{0}, m_skipped{0};
    bool m_active{false};
    bool m_completed{false};
};

}} // namespace Slic3r::GUI
#endif
