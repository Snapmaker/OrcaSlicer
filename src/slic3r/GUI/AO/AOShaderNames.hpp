#ifndef slic3r_GUI_AO_AOShaderNames_hpp_
#define slic3r_GUI_AO_AOShaderNames_hpp_

namespace Slic3r { namespace GUI { namespace AO {

// Registered names shared by GLShadersManager registration and the AO catalog lookup.
// Programs remain created and owned by GLShadersManager.
namespace ShaderNames {
constexpr const char* normal                      = "ao_reconstruct_view_normals";
constexpr const char* normal_confidence           = "ao_reconstruct_view_normals_confidence";
constexpr const char* evaluate_reference_fs       = "ao_evaluate_visibility_reference_fs";
constexpr const char* filter_geometry_reference   = "ao_denoise_visibility_geometry_reference";
constexpr const char* upsample                    = "ao_upsample_visibility_bilateral";
constexpr const char* composite_pixel             = "ao_composite_color_pixel";
constexpr const char* composite_pixel_confidence  = "ao_composite_color_pixel_confidence";
constexpr const char* composite_pixel_reference   = "ao_composite_color_pixel_reference";
constexpr const char* evaluate_fs                 = "ao_evaluate_visibility_fs";
constexpr const char* filter_geometry             = "ao_denoise_visibility_geometry";
constexpr const char* evaluate_edges_fs           = "ao_evaluate_visibility_edges_fs";
constexpr const char* evaluate_edges_xy_fs        = "ao_evaluate_visibility_edges_xy_fs";
constexpr const char* filter_edge_guided_fs       = "ao_denoise_visibility_edge_guided_fs";
constexpr const char* receiver_depth              = "ao_render_receiver_depth";
constexpr const char* composite_sample            = "ao_composite_color_sample";
constexpr const char* composite_sample_confidence = "ao_composite_color_sample_confidence";
constexpr const char* split_classify              = "ao_classify_sample_surfaces";
constexpr const char* split_fast                  = "ao_composite_color_pixel_fast";
constexpr const char* split_edges                 = "ao_composite_color_sample_edges";
constexpr const char* composite_sample_reference  = "ao_composite_color_sample_reference";
constexpr const char* compute_prefilter           = "ao_prefilter_depth_mips";
constexpr const char* compute_evaluate            = "ao_evaluate_visibility_edges_cs";
constexpr const char* compute_evaluate_legacy_low = "ao_evaluate_visibility_edges_legacy_low_cs";
constexpr const char* compute_denoise             = "ao_denoise_visibility_edge_guided_cs";
} // namespace ShaderNames

}}} // namespace Slic3r::GUI::AO
#endif
