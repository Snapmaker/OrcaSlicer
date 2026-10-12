#ifndef slic3r_GUI_AO_GLAOComputeBackend_hpp_
#define slic3r_GUI_AO_GLAOComputeBackend_hpp_

#include <array>
#include <functional>
#include <string>

namespace Slic3r { namespace GUI { namespace AO {

// CS backend: XeGTAO depth MIP prefilter, visibility evaluation and edge-guided denoise.
// Capability results belong to the current GL context, never to a renderer name.
class GLAOComputeBackend
{
public:
    GLAOComputeBackend()                                     = default;
    GLAOComputeBackend(const GLAOComputeBackend&)            = delete;
    GLAOComputeBackend& operator=(const GLAOComputeBackend&) = delete;

    static constexpr const char* upstream = "a5b1686c7ea37788eeb3576b5be47f7c03db532c";

    struct Parameters
    {
        std::array<float, 16> inverse{};
        int                   width{}, height{}, slices{3}, steps{3}, denoise_passes{2};
        float                 radius{2.5f};
        bool                  perspective{true};
    };

    struct Capabilities
    {
        bool               supported{false};
        int                max_texture_size{0};
        std::array<int, 3> max_groups{};
        std::string        reason;
    };

    // Programs remain owned by the shader manager: prefilter, evaluation, denoise.
    // stage_done receives 0 (prefiltered), 1 (evaluated) and 2.. (each denoise pass).
    bool prepare(int width, int height, const std::array<unsigned int, 3>& programs, int denoise_passes = 2);
    bool render(unsigned int                    depth,
                unsigned int                    normals,
                const Parameters&               params,
                const std::function<void(int)>& stage_done   = {},
                const std::function<bool()>&    context_lost = {});
    void release();
    void forget_context();
    void disable(const std::string& reason);
    void reset_health() { m_failure.clear(); }
    void retry_resource_failure()
    {
        if (m_failure == "CS viewport exceeds resource limits" || m_failure == "CS texture allocation failed")
            m_failure.clear();
    }

    const std::string& failure_reason() const { return m_failure; }
    unsigned int       evaluation_program() const { return m_programs[1]; }
    unsigned int       output() const { return m_textures[4]; }
    unsigned int       texture(int index) const { return m_textures[index]; }
    static int         texture_count(int denoise_passes) { return denoise_passes == 3 ? 6 : 5; }
    int                padded_width() const { return m_pw; }
    int                padded_height() const { return m_ph; }
    int                width() const { return m_width; }
    int                height() const { return m_height; }
    int                denoise_passes_prepared() const { return m_textures[5] ? 3 : 2; }

    static Capabilities query_capabilities();
    // Context-local cached probe.
    const Capabilities& capabilities();
    static bool         valid_dispatch(const Capabilities& capabilities, int width, int height);

    // Mirrors only the bindings modified by CS passes. Construct after a successful GL 4.3 probe.
    // Scope must end before destroying resources that were bound at entry.
    class State
    {
    public:
        State();
        ~State();
        void forget_textures(const std::array<unsigned int, 6>& textures);
        void forget_context() { m_restore_state = false; }
        State(const State&)            = delete;
        State& operator=(const State&) = delete;

    private:
        struct ImageBinding
        {
            int name{}, level{}, layered{}, layer{}, access{}, format{};
        };
        int                         m_program{}, m_active_texture{};
        std::array<int, 5>          m_textures{}, m_samplers{};
        std::array<ImageBinding, 5> m_images{};
        bool                        m_restore_state{true};
    };

private:
    // The second intermediate is allocated only for three-pass presets (including static High).
    std::array<unsigned int, 6> m_textures{}; // depth MIPs, raw, edges, first intermediate, normalized final, second intermediate
    std::array<unsigned int, 3> m_programs{};
    int                         m_width{}, m_height{}, m_pw{}, m_ph{};
    std::string                 m_failure;
    Capabilities                m_capabilities;
    bool                        m_probed{false};
};

}}} // namespace Slic3r::GUI::AO
#endif
