#ifndef slic3r_GLAOCompute_hpp_
#define slic3r_GLAOCompute_hpp_

#include <array>
#include <string>
#include <functional>

namespace Slic3r { namespace GUI {

// Capability results belong to the current GL context, never to a renderer name.
// Auto prefers this backend when the active GL context and programs support it.
class GLAOCompute
{
public:
    GLAOCompute()                              = default;
    GLAOCompute(const GLAOCompute&)            = delete;
    GLAOCompute& operator=(const GLAOCompute&) = delete;
    struct Parameters
    {
        std::array<float, 16> inverse{};
        int                   width{}, height{}, slices{3}, steps{3}, denoise_passes{2};
        float                 radius{2.5f};
        bool                  perspective{true};
    };
    // Programs remain owned by the shader manager: prefilter, evaluation, denoise.
    bool prepare(int width, int height, const std::array<unsigned int, 3>& programs, int denoise_passes = 2);
    bool render(unsigned int depth, unsigned int normals, const Parameters& params, const std::function<void(int)>& stage_done = {});
    bool capture(const std::string& directory, int denoise_passes) const;
    void release();
    void forget_context();
    void disable(const std::string& reason);
    const std::string&           failure_reason() const { return m_failure; }
    unsigned int evaluation_program() const { return m_programs[1]; }
    unsigned int                 output() const { return m_textures[4]; }
    unsigned int                 depth_mips() const { return m_textures[0]; }
    int                          padded_width() const { return m_pw; }
    int                          padded_height() const { return m_ph; }
    static constexpr const char* upstream = "a5b1686c7ea37788eeb3576b5be47f7c03db532c";
    enum class Request { Auto, FS, CS };
    struct Capabilities
    {
        bool               supported{false};
        int                max_texture_size{0};
        std::array<int, 3> max_groups{};
        std::string        reason;
    };
    struct Selection
    {
        bool        use_cs{false};
        bool        benchmark_allowed{true};
        std::string reason;
    };

    static Request      parse_request(const char* value);
    static Capabilities query_capabilities();
    const Capabilities& capabilities();
    // Capability/program readiness determines routing; GPU timings never select a backend.
    static Selection select(Request request, const Capabilities& capabilities, bool backend_ready, bool benchmark);
    static bool valid_dispatch(const Capabilities& capabilities, int width, int height);

    // Explicitly mirrors only the bindings modified by CS passes. Construct after a successful GL 4.3 probe.
    // Scope must end before destroying resources that were bound at entry.
    class State
    {
    public:
        State();
        ~State();
        void forget_textures(const std::array<unsigned int, 6>& textures);
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

}} // namespace Slic3r::GUI
#endif
