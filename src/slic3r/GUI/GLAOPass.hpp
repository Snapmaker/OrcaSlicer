#ifndef slic3r_GLAOPass_hpp_
#define slic3r_GLAOPass_hpp_

#include "libslic3r/Point.hpp"
#include "AOBenchmark.hpp"
#include "GLAOCompute.hpp"
#include <array>
#include <functional>
#include <string>

namespace Slic3r {
class GLShaderProgram;
namespace GUI {

// Owns only postprocess resources. All methods touching GL require the owning context current.
class GLAOPass
{
public:
    GLAOPass()                           = default;
    GLAOPass(const GLAOPass&)            = delete;
    GLAOPass& operator=(const GLAOPass&) = delete;
    enum class Quality { Off, Low, Medium, High };
    static constexpr float DEFAULT_STRENGTH = 0.5f;
    struct Frame
    {
        Matrix4d           projection;
        Matrix4d           inverse_projection;
        std::array<int, 4> viewport{};
        unsigned int       source{0}, target{0};
        bool               perspective{true};
        const char* scene_view{"external"}; // Diagnostic tag; supplied as a static literal by the canvas.
        float explosion_ratio{1.0f};
        bool composite_reference{false};
        bool cs_low_reference{false};
        bool precomputed_confidence{false};
        bool split_composite{false};
        bool pixel_composite{true}; // Default AO factor is shared by the covered MSAA samples.
        bool require_msaa_comparison{false};
        GLShaderProgram* split_classify{nullptr};
        GLShaderProgram* split_fast{nullptr};
        GLShaderProgram* split_edges{nullptr};
        GLShaderProgram* confidence_normal{nullptr};
        GLShaderProgram* confidence_composite{nullptr};
        GLShaderProgram* confidence_sample_composite{nullptr};
        // Optional OpenGL 4 shader; ordinary AO remains available on OpenGL 3.1.
        GLShaderProgram* sample_composite{nullptr};
        GLShaderProgram* edge_evaluate{nullptr};
        GLShaderProgram* edge_evaluate_xy{nullptr};
        bool fast_reconstruction{false};
        GLShaderProgram* edge_denoise{nullptr};
        std::array<GLShaderProgram*, 3> compute_shaders{};
    };
    struct Settings
    {
        Quality quality{Quality::Off};
        float   radius{2.5f}, thickness{1.0f}, intensity{1.0f};
        // Blend strength is independent of the AO exponent and sampling quality.
        float strength{DEFAULT_STRENGTH};
        // 0: normal composite, 1: depth, 2: normals, 3: AO, 4: composite confidence.
        int debug_view{0};
        int cs_slices_override{0}; // Unified comparison: 3 or 6, zero uses environment/default.
        int fs_slices_override{3}; // High default: 3x8; zero selects the original 4x8.
        int fs_edge_passes{3};
        bool fs_edge_filter{true}; // Full-resolution High; fall back to geometry if unavailable.
        bool force_cs{false};
        bool force_fs{false}; // Existing FS variant benchmarks retain their original meaning.
    };
    // normal, gtao, filter, upsample, composite. Pointers remain owned by GLShadersManager.
    using Shaders = std::array<GLShaderProgram*, 5>;
    static Quality resolve_quality(const std::string& value, const std::string& renderer);
    static float   resolve_strength(const std::string& value);
    bool           prepare(const Frame& frame, const Settings& settings, const Shaders& shaders);
    // Render window MSAA scenes directly in a consistent offscreen sample layout.
    // The callback draws opaque color/depth and evaluates AO; presentation resolves color once.
    bool render_scene(const Frame& frame, const std::function<bool(const Frame&)>& draw_scene);
    // receiver_draw runs with the AO depth FBO bound, depth writes on, and all color writes off.
    bool render(const Frame& frame, const Settings& settings, const std::function<void()>& receiver_draw);
    // The next successful AO render exports one diagnostic frame, then clears the request.
    void request_capture(const std::string& directory) { m_capture_directory = directory; }
    AOBenchmark benchmark;
    void release();
    // After the old context is released/destroyed, discard its names and allow a fresh capability probe.
    void                         forget_context();
    bool                         has_resources() const { return m_vao != 0; }
    const std::string&           failure_reason() const { return m_failure; }
    const std::array<double, 8>& gpu_ms() const { return m_gpu_ms; }
    bool                         has_gpu_timing() const { return m_timing; }
    bool                         has_gpu_sample() const { return m_has_gpu_sample; }
    bool fast_reconstruction_active() const { return m_use_edge_filter && m_fast_reconstruction; }
    bool pixel_composite_active() const { return m_pixel_active; }
    bool split_composite_active() const { return m_split_active; }
    bool precomputed_confidence_active() const { return m_use_confidence; }
    bool cs_low_reference_active() const { return m_use_compute && m_quality == Quality::Low && m_cs_low_reference; }
    Quality actual_quality() const { return m_quality; }
    int actual_slices() const { return m_effective_samples[0]; }
    bool fs_edge_filter_active() const { return m_use_edge_filter; }
    int fs_edge_passes() const { return m_use_edge_filter ? m_fs_edge_passes : 0; }
    const char*                  backend() const { return m_use_compute ? "cs" : "fs"; }
    const std::string&           backend_reason() const { return m_backend_reason; }

private:
    bool fail(const std::string& reason);
    bool validate_fs_shaders(bool needs_upsample);
    GLAOCompute        m_compute;
    bool               m_use_compute{false};
    bool m_use_edge_filter{false}, m_edge_filter_failed{false};
    Quality m_quality{Quality::Off};
    bool m_composite_reference{false};
    bool m_cs_low_reference{false};
    bool m_pixel_active{false};
    bool m_fast_reconstruction{false};
    bool m_use_confidence{false}, m_confidence_failed{false};
    unsigned int m_confidence_texture{0};
    unsigned int m_split_texture{0}, m_split_fbo{0};
    bool m_split_ready{false}, m_split_active{false}, m_split_failed{false};
    int m_split_position_samples{0};
    unsigned int m_split_position_target{0};
    std::array<std::array<float, 2>, 8> m_split_positions{};
    unsigned int m_edge_texture{0};
    int                m_denoise_passes{2};
    int m_fs_edge_passes{2};
    std::array<int, 2> m_effective_samples{};
    std::string        m_backend_reason;
    bool capture_depth(const Frame& frame);
    bool allocate_depth(unsigned int format);
    bool capture_sample_depth(const Frame& frame);
    GLShaderProgram* sample_composite_shader(const Frame& frame) const;
    void draw(int shader, unsigned int target, int w, int h, const Frame& frame, const Settings& settings);
    void begin_timing();
    void mark_time(int index);
    void end_timing();

    unsigned int                m_vao{0}, m_depth_fbo{0}, m_depth{0};
    std::array<unsigned int, 2> m_scene_fbo{};
    std::array<unsigned int, 3> m_scene_rbo{};
    int                         m_scene_samples{0};
    unsigned int                m_sample_fbo{0}, m_sample_depth{0}, m_depth_format{0};
    int                         m_sample_count{0};
    bool                        m_sample_active{false}, m_sample_failed{false};
    // normal, raw AO, filtered AO, full-size AO
    std::array<unsigned int, 4> m_fbo{}, m_texture{};
    int                         m_width{0}, m_height{0}, m_ao_width{0}, m_ao_height{0};
    unsigned int                m_source{0};
    int                         m_samples{-1};
    bool                        m_depth_validated{false};
    Shaders                     m_shaders{};
    std::string                 m_failure;
    std::string                 m_capture_directory;
    // Four frames, nine timestamps: eight intervals, with the last interval unused by half/full dispatch as needed.
    std::array<std::array<unsigned int, 9>, 4> m_queries{};
    std::array<bool, 4>                        m_pending{};
    std::array<double, 8>                      m_gpu_ms{};
    unsigned int                               m_query_slot{0};
    bool                                       m_timing{false}, m_recording{false}, m_has_gpu_sample{false};
};

} // namespace GUI
} // namespace Slic3r
#endif
