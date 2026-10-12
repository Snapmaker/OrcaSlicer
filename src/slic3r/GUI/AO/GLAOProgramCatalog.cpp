#include "GLAOProgramCatalog.hpp"
#include "../GLShader.hpp"

#include <cstdint>

namespace Slic3r { namespace GUI { namespace AO {

void GLAOProgramCatalog::update(const Refs& refs, const void* context)
{
    const bool context_changed = context != m_context;
    m_refs                     = refs;
    m_context                  = context;
    const uint64_t print       = fingerprint();
    if (context_changed || print != m_fingerprint) {
        m_fingerprint              = print;
        m_identity.profile_version = PROFILE_VERSION;
        m_identity.generation      = m_identity.generation + 1;
    }
}

void GLAOProgramCatalog::forget_context()
{
    m_refs                     = {};
    m_context                  = nullptr;
    m_fingerprint              = fingerprint();
    m_identity.profile_version = PROFILE_VERSION;
    m_identity.generation      = m_identity.generation + 1;
}

uint64_t GLAOProgramCatalog::fingerprint() const
{
    // Runtime revisions detect replacement even when availability and GL names are unchanged.
    const GLShaderProgram* const all[] = {m_refs.normal,
                                          m_refs.evaluate,
                                          m_refs.filter_geometry,
                                          m_refs.upsample,
                                          m_refs.composite_pixel,
                                          m_refs.receiver_depth,
                                          m_refs.edge_evaluate,
                                          m_refs.edge_evaluate_xy,
                                          m_refs.edge_denoise,
                                          m_refs.compute[0],
                                          m_refs.compute[1],
                                          m_refs.compute[2],
                                          m_refs.compute_evaluate_legacy_low,
                                          m_refs.composite_sample,
                                          m_refs.composite_sample_confidence,
                                          m_refs.composite_sample_reference,
                                          m_refs.composite_pixel_reference,
                                          m_refs.composite_pixel_confidence,
                                          m_refs.normal_confidence,
                                          m_refs.split_classify,
                                          m_refs.split_fast,
                                          m_refs.split_edges};
    uint64_t                     hash  = 1469598103934665603ull;
    for (const GLShaderProgram* program : all)
        hash = (hash ^ (program ? program->get_revision() : 0u)) * 1099511628211ull;
    return hash;
}

bool GLAOProgramCatalog::common_ready() const { return m_refs.normal && m_refs.composite_pixel; }

bool GLAOProgramCatalog::fs_geometry_ready(bool needs_upsample) const
{
    if (!m_refs.evaluate || !m_refs.filter_geometry)
        return false;
    return !needs_upsample || m_refs.upsample;
}

bool GLAOProgramCatalog::fs_edge_ready() const { return m_refs.edge_evaluate && m_refs.edge_denoise; }

bool GLAOProgramCatalog::fs_fast_reconstruction_ready() const { return m_refs.edge_evaluate_xy != nullptr; }

bool GLAOProgramCatalog::cs_ready(bool legacy_low) const
{
    if (!m_refs.compute[0] || !m_refs.compute[2])
        return false;
    return legacy_low ? m_refs.compute_evaluate_legacy_low != nullptr : m_refs.compute[1] != nullptr;
}

bool GLAOProgramCatalog::composite_sample_ready(CompositeVariant variant, bool with_sample_confidence) const
{
    switch (variant) {
    case CompositeVariant::Pixel: return true;
    case CompositeVariant::Sample: return m_refs.composite_sample != nullptr;
    case CompositeVariant::SplitMsaa: return m_refs.split_classify && m_refs.split_fast && m_refs.split_edges;
    case CompositeVariant::Confidence: return confidence_ready(with_sample_confidence);
    case CompositeVariant::Reference: return m_refs.composite_sample_reference && m_refs.composite_pixel_reference;
    }
    return false;
}

bool GLAOProgramCatalog::split_ready() const { return m_refs.split_classify && m_refs.split_fast && m_refs.split_edges; }

bool GLAOProgramCatalog::confidence_ready(bool with_sample) const
{
    if (!m_refs.normal_confidence || !m_refs.composite_pixel_confidence)
        return false;
    return !with_sample || m_refs.composite_sample_confidence;
}

}}} // namespace Slic3r::GUI::AO
