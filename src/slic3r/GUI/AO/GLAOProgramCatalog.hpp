#ifndef slic3r_GUI_AO_GLAOProgramCatalog_hpp_
#define slic3r_GUI_AO_GLAOProgramCatalog_hpp_

#include "AOTypes.hpp"

#include <array>

namespace Slic3r {
class GLShaderProgram;
namespace GUI { namespace AO {

// Typed, borrowed view of the AO programs. Ownership stays with GLShadersManager; the catalog
// only tracks identity (which programs exist right now) and readiness for plan generation.
class GLAOProgramCatalog
{
public:
    // Bumped whenever the program set, shader sources or define combinations change semantics.
    static constexpr uint32_t PROFILE_VERSION = 1;

    // Production-required, optional-production and experiment slots. All borrowed.
    struct Refs
    {
        // Common production stages.
        GLShaderProgram* normal{nullptr};          // Required.
        GLShaderProgram* evaluate{nullptr};        // FS geometry evaluation.
        GLShaderProgram* filter_geometry{nullptr}; // FS geometry filter.
        GLShaderProgram* upsample{nullptr};        // FS bilateral upsample (half-res presets).
        GLShaderProgram* composite_pixel{nullptr}; // Required.
        GLShaderProgram* receiver_depth{nullptr};  // Bed receiver depth (prepare view).

        // Optional production: FS High edge-guided filter chain.
        GLShaderProgram* edge_evaluate{nullptr};
        GLShaderProgram* edge_evaluate_xy{nullptr};
        GLShaderProgram* edge_denoise{nullptr};

        // Optional production: CS backend (prefilter, evaluate, denoise).
        std::array<GLShaderProgram*, 3> compute{{}};
        GLShaderProgram*                compute_evaluate_legacy_low{nullptr}; // Experiment-only variant.

        // Experiment-only composite variants.
        GLShaderProgram* composite_sample{nullptr}; // OpenGL 4 per-sample composite.
        GLShaderProgram* composite_sample_confidence{nullptr};
        GLShaderProgram* composite_sample_reference{nullptr};
        GLShaderProgram* composite_pixel_reference{nullptr};
        GLShaderProgram* composite_pixel_confidence{nullptr};
        GLShaderProgram* normal_confidence{nullptr};
        GLShaderProgram* split_classify{nullptr};
        GLShaderProgram* split_fast{nullptr};
        GLShaderProgram* split_edges{nullptr};
    };

    // Refreshes the borrowed pointers. Call with the canvas context identity each frame;
    // identity changes (context rebuild, program reload, different availability) bump the
    // generation so caches and health states can be re-evaluated.
    void update(const Refs& refs, const void* context);
    void forget_context();

    const Refs&              refs() const { return m_refs; }
    const AOProgramIdentity& identity() const { return m_identity; }

    // Production readiness (a missing optional program only degrades the related path).
    bool common_ready() const; // normal + pixel composite (+ receiver on prepare).
    bool fs_geometry_ready(bool needs_upsample) const;
    bool fs_edge_ready() const;                // edge_evaluate + edge_denoise.
    bool fs_fast_reconstruction_ready() const; // edge_evaluate_xy.
    bool cs_ready(bool legacy_low) const;
    bool composite_sample_ready(CompositeVariant variant, bool with_sample_confidence) const;
    bool split_ready() const;                      // classify + fast + edges.
    bool confidence_ready(bool with_sample) const; // normal_confidence + pixel composite (+ sample variant).

private:
    uint64_t          fingerprint() const;
    Refs              m_refs;
    const void*       m_context{nullptr};
    uint64_t          m_fingerprint{0};
    AOProgramIdentity m_identity{};
};

}} // namespace GUI::AO
} // namespace Slic3r
#endif
