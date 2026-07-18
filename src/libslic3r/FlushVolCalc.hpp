#ifndef slic3r_FlushVolCalc_hpp_
#define slic3r_FlushVolCalc_hpp_

#include "libslic3r.h"
#include "Config.hpp"
#include "FlushVolPredictor.hpp"


namespace Slic3r {

extern const int g_min_flush_volume_from_support;
extern const int g_flush_volume_to_support;
extern const int g_max_flush_volume;

// Parameter set for the HSV color-distance flush formula.
// Supports independent calibration per flow type (StandardFlow / HighFlow).
struct FlushFormulaParams {
    float lumi_pow_exp;       // exponent: pow(delta_lumi, exp)
    float lumi_pow_scale;     // multiplier for the pow luminance term
    float lumi_linear_scale;  // multiplier for linear luminance delta (to < from)
    float inter_hsv_from_w;   // weight of from_hsv_v in inter_hsv_v blend
    float hs_scale;           // multiplier: hs_dist -> hs_flush
    float triangle_angle;     // angle (degrees) for calc_triangle_3rd_edge
    float hs_cap;             // cap for DeltaHS_BBS
};

class FlushVolCalculator
{
public:
    // flush_dataset: FlushDataset enum — 0=StandardFlow, 1=HighFlow
    FlushVolCalculator(int min, int max, float multiplier = 1.0f, int flush_dataset = static_cast<int>(FlushDataset::StandardFlow));
    ~FlushVolCalculator()
    {
    }

    int calc_flush_vol(unsigned char src_a, unsigned char src_r, unsigned char src_g, unsigned char src_b,
        unsigned char dst_a, unsigned char dst_r, unsigned char dst_g, unsigned char dst_b);

    // Path B: Full HSV color-distance formula with stain-risk compensation.
    // params selects the coefficient set for StandardFlow or HighFlow.
    int calc_flush_vol_rgb(unsigned char src_r, unsigned char src_g, unsigned char src_b,
        unsigned char dst_r, unsigned char dst_g, unsigned char dst_b,
        const FlushFormulaParams& params);

    bool get_flush_vol_from_data(unsigned char src_r, unsigned char src_g, unsigned char src_b,
        unsigned char dst_r, unsigned char dst_g, unsigned char dst_b, float& flush);

private:
    int m_min_flush_vol;
    int m_max_flush_vol;
    float m_multiplier;
    int m_flush_dataset;
    GenericFlushPredictor m_predictor;
};


}

#endif
