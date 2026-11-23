#include "GLAOProfiler.hpp"

#include <GL/glew.h>
#include <algorithm>
#include <boost/log/trivial.hpp>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>

namespace Slic3r { namespace GUI { namespace AO {

bool GLAOProfiler::request(const std::string& root)
{
    if (m_active)
        return false;
    const auto id = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    m_directory   = (std::filesystem::path(root) / ("run-" + std::to_string(id))).string();
    std::error_code error;
    if (!std::filesystem::create_directories(m_directory, error) || error)
        return false;
    std::ofstream status(std::filesystem::path(m_directory) / "status.txt");
    status << "Running: 30 warmup + 180 measured frames. Keep camera, settings and scene fixed.\n";
    status.close();
    if (!status)
        return false;
    m_completed = false;
    m_rows.clear();
    m_rows.reserve(210);
    m_signature.clear();
    m_pipeline.clear();
    m_submitted = m_skipped = 0;
    m_slot                  = -1;
    m_started               = std::chrono::steady_clock::now();
    m_active                = true;
    BOOST_LOG_TRIVIAL(info) << "AO benchmark started: " << m_directory;
    return true;
}

void GLAOProfiler::benchmark_begin(const std::string& signature)
{
    if (!m_active)
        return;
    if (!(GLEW_VERSION_3_3 || GLEW_ARB_timer_query)) {
        finish("ABORTED: GPU timestamp queries unavailable", true);
        return;
    }
    if (std::chrono::steady_clock::now() - m_started > std::chrono::seconds(120)) {
        finish("ABORTED: 120 second timeout", true);
        return;
    }
    if (m_signature.empty()) {
        m_signature = signature;
        std::ofstream metadata(std::filesystem::path(m_directory) / "metadata.txt");
        metadata << "benchmark_schema=10\n"
                 << signature << "\nGPU: " << reinterpret_cast<const char*>(glGetString(GL_RENDERER))
                 << "\nVendor: " << reinterpret_cast<const char*>(glGetString(GL_VENDOR))
                 << "\nGL: " << reinterpret_cast<const char*>(glGetString(GL_VERSION))
                 << "\nScope: GPU frame after camera setup, before SwapBuffers; CPU same submission region.\n"
                 << "No image capture. Scene cache refresh forced. No VSync changes.\n"
                 << "CPU stage stamps precede existing GPU query submissions; wall time includes driver calls and scheduling.\n"
                 << "CPU and GPU intervals overlap: do not add them. elapsed_ms is CPU frame-start time since benchmark request.\n";
    } else if (signature != m_signature) {
        finish("ABORTED: camera, viewport or AO settings changed", true);
        return;
    }
    for (auto& slot : m_slots) {
        if (!slot.pending)
            continue;
        GLint ready = GL_FALSE;
        glGetQueryObjectiv(slot.queries[FRAME_END], GL_QUERY_RESULT_AVAILABLE, &ready);
        if (!ready)
            continue;
        Row      row{slot.frame, slot.mask, {}, slot.cpu_ms, slot.elapsed_ms, slot.cpu_stamps};
        GLuint64 origin = 0;
        glGetQueryObjectui64v(slot.queries[0], GL_QUERY_RESULT, &origin);
        for (int i = 0; i < QUERY_COUNT; ++i) {
            if ((slot.mask & (1u << i)) == 0)
                continue;
            GLuint64 stamp = 0;
            glGetQueryObjectui64v(slot.queries[i], GL_QUERY_RESULT, &stamp);
            row.stamps[i] = double(stamp - origin) / 1e6;
        }
        m_rows.push_back(row);
        slot.pending = false;
    }
    if (m_rows.size() == 210) {
        finish("COMPLETE", true);
        return;
    }
    m_slot = -1;
    if (m_submitted >= 210)
        return; // Continue rendering only to drain pending queries without waiting.
    for (int i = 0; i < int(m_slots.size()); ++i) {
        auto& slot = m_slots[i];
        if (slot.pending)
            continue;
        if (!slot.queries[0])
            glGenQueries(QUERY_COUNT, slot.queries.data());
        slot.mask       = 0;
        slot.frame      = m_submitted++;
        m_slot          = i;
        m_cpu_start     = std::chrono::steady_clock::now();
        slot.elapsed_ms = std::chrono::duration<double, std::milli>(m_cpu_start - m_started).count();
        benchmark_mark(0);
        return;
    }
    ++m_skipped;
}

void GLAOProfiler::benchmark_pipeline(const std::string& description)
{
    if (!m_active)
        return;
    if (m_pipeline.empty()) {
        m_pipeline = description;
        std::ofstream out(std::filesystem::path(m_directory) / "metadata.txt", std::ios::app);
        out << description << '\n';
    } else if (m_pipeline != description)
        finish("ABORTED: actual AO pipeline changed", true);
}

void GLAOProfiler::benchmark_mark(int index)
{
    if (!m_active || m_slot < 0)
        return;
    auto& slot             = m_slots[m_slot];
    slot.cpu_stamps[index] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_cpu_start).count();
    glQueryCounter(slot.queries[index], GL_TIMESTAMP);
    slot.mask |= 1u << index;
}

void GLAOProfiler::benchmark_end()
{
    if (!m_active || m_slot < 0)
        return;
    benchmark_mark(FRAME_END);
    auto& slot   = m_slots[m_slot];
    slot.cpu_ms  = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_cpu_start).count();
    slot.pending = true;
    m_slot       = -1;
}

void GLAOProfiler::benchmark_finish(const std::string& reason, bool context_current) { finish(reason, context_current); }

void GLAOProfiler::finish(const std::string& reason, bool context_current)
{
    if (!m_active)
        return;
    m_active = false;
    m_slot   = -1;
    std::sort(m_rows.begin(), m_rows.end(), [](const Row& a, const Row& b) { return a.frame < b.frame; });
    std::ofstream csv(std::filesystem::path(m_directory) / "frames.csv");
    csv.imbue(std::locale::classic());
    csv << "frame,warmup,cpu_submit_ms,gpu_frame_ms,main_scene_ms,ao_total_ms,depth_copy_ms,receivers_ms,normals_ms,gtao_ms,filter_ms,"
           "upsample_ms,composite_ms,color_present_ms,depth_return_ms,filter_first_ms,filter_refine_ms,color_resolve_ms,window_present_ms,"
           "before_main_ms,after_main_ms"
        << ",elapsed_ms,cpu_ao_submit_ms,cpu_gtao_submit_ms,cpu_filter_first_submit_ms,cpu_filter_refine_submit_ms,cpu_depth_copy_submit_"
           "ms,cpu_normals_submit_ms"
        << ",cs_depth_prefilter_ms,cs_evaluate_ms,cs_denoise_first_ms,cs_denoise_second_ms,cs_denoise_third_ms,depth_resolve_ms,sample_"
           "depth_copy_ms,fs_denoise_second_ms,fs_denoise_third_ms,composite_classify_ms,composite_fast_ms,composite_edges_ms,receiver_"
           "first_ms,receiver_sample_ms\n";
    for (const auto& row : m_rows) {
        csv << row.frame << ',' << (row.frame < 30 ? 1 : 0) << ',' << std::setprecision(9) << row.cpu_ms;
        auto interval = [&](int a, int b) {
            csv << ',';
            if ((row.mask & (1u << a)) && (row.mask & (1u << b)))
                csv << row.stamps[b] - row.stamps[a];
        };
        interval(0, 15);
        interval(1, 14);
        interval(2, 10);
        for (int i = 2; i < 9; ++i)
            if ((row.mask & (1u << 19)) && (i == 5 || i == 6))
                csv << ',';
            else
                interval(i, i + 1);
        interval(11, 12);
        interval(12, 13);
        interval(6, 16);
        interval(16, 17); // Empty for half-resolution AO, which does not run refinement.
        interval(11, 18);
        interval(18, 12);
        interval(0, 1);
        interval(14, FRAME_END);
        csv << ',' << row.elapsed_ms;
        auto cpu_interval = [&](int a, int b) {
            csv << ',';
            if ((row.mask & (1u << a)) && (row.mask & (1u << b)))
                csv << row.cpu_stamps[b] - row.cpu_stamps[a];
        };
        cpu_interval(2, 10);
        if (row.mask & (1u << 19))
            csv << ',';
        else
            cpu_interval(5, 6);
        cpu_interval(6, 16);
        cpu_interval(16, 17);
        cpu_interval(2, 3);
        cpu_interval(4, 5);
        interval(19, 20);
        interval(20, 21);
        interval(21, 22);
        interval(22, 23);
        interval(23, 24);
        interval(2, 25);
        interval(25, 29); // Empty when no sample-depth copy was executed.
        interval(16, (row.mask & (1u << 26)) ? 26 : 17);
        interval(26, 17);
        interval(8, 27);
        interval(27, 28);
        interval(28, 9);
        interval(3, 30);
        if (row.mask & (1u << 29))
            interval(30, 4);
        else
            csv << ',';
        csv << '\n';
    }
    csv.close();
    std::ofstream status(std::filesystem::path(m_directory) / "status.txt");
    status << (csv ? reason : "ABORTED: CSV write failed") << "\nrows=" << m_rows.size() << "\nquery_ring_skipped_frames=" << m_skipped
           << '\n';
    BOOST_LOG_TRIVIAL(info) << "AO benchmark " << reason << ": " << m_directory;
    status.close();
    m_completed = reason == "COMPLETE" && bool(csv) && bool(status);
    if (context_current)
        release();
    else
        forget();
}

void GLAOProfiler::release()
{
    for (auto& slot : m_slots)
        if (slot.queries[0])
            glDeleteQueries(QUERY_COUNT, slot.queries.data());
    if (m_frame_queries_created) {
        for (auto& queries : m_frame_queries)
            glDeleteQueries(9, queries.data());
        m_frame_queries_created = false;
    }
    forget();
}

void GLAOProfiler::forget()
{
    m_slots                 = {};
    m_active                = false;
    m_slot                  = -1;
    m_frame_pending         = {};
    m_frame_queries         = {};
    m_frame_queries_created = false;
    m_recording             = false;
    m_has_gpu_sample        = false;
}

void GLAOProfiler::begin_frame_timing()
{
    m_recording = false;
    if (!timing_supported())
        return;
    // Poll all completed slots; never wait for the GPU or force extra frames.
    for (unsigned int slot = 0; slot < m_frame_queries.size(); ++slot)
        if (m_frame_pending[slot]) {
            GLint ready = GL_FALSE;
            glGetQueryObjectiv(m_frame_queries[slot][8], GL_QUERY_RESULT_AVAILABLE, &ready);
            if (ready) {
                std::array<GLuint64, 9> times{};
                for (int i = 0; i < 9; ++i)
                    glGetQueryObjectui64v(m_frame_queries[slot][i], GL_QUERY_RESULT, &times[i]);
                for (int i = 0; i < 7; ++i)
                    m_gpu_ms[i] = double(times[i + 1] - times[i]) / 1e6;
                m_gpu_ms[7]           = double(times[8] - times[0]) / 1e6;
                m_frame_pending[slot] = false;
                m_has_gpu_sample      = true;
            }
        }
    if (!m_frame_queries_created) {
        for (auto& queries : m_frame_queries)
            glGenQueries(9, queries.data());
        m_frame_queries_created = true;
    }
    for (unsigned int i = 0; i < m_frame_queries.size(); ++i) {
        m_query_slot = (m_query_slot + 1) % m_frame_queries.size();
        if (!m_frame_pending[m_query_slot]) {
            m_recording = true;
            mark(AOStage::AOBegin);
            break;
        }
    }
}

void GLAOProfiler::mark(AOStage stage)
{
    if (!stage_has_timestamp(stage))
        return;
    const int index = stage_index(stage);
    benchmark_mark(index);
    if (m_recording && index >= 2 && index <= 10)
        glQueryCounter(m_frame_queries[m_query_slot][index - 2], GL_TIMESTAMP);
}

void GLAOProfiler::end_frame_timing()
{
    mark(AOStage::AOEnd);
    if (m_recording)
        m_frame_pending[m_query_slot] = true;
    m_recording = false;
}

}}} // namespace Slic3r::GUI::AO
