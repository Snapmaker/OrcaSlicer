#include "FilamentSyncAlgorithm.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

// ---- sRGB transfer function (IEC 61966-2-1) ----
constexpr float kSrgbThreshold  = 0.04045f;
constexpr float kSrgbLinearSlope = 1.0f / 12.92f;
constexpr float kSrgbGammaA     = 0.055f;
constexpr float kSrgbGammaB     = 1.055f;
constexpr float kSrgbGammaExp   = 2.4f;

// ---- CIE Lab (D65) constants ----
constexpr float kCieDelta    = 6.0f / 29.0f;       // δ
constexpr float kCieDelta2   = kCieDelta * kCieDelta;
constexpr float kCieDelta3   = kCieDelta * kCieDelta * kCieDelta;
constexpr float kCieFour29th = 4.0f / 29.0f;       // constant term in Lab f(t) linear branch
constexpr float kCieLabLScale = 116.0f;
constexpr float kCieLabLShift = 16.0f;
constexpr float kCieLabAScale = 500.0f;
constexpr float kCieLabBScale = 200.0f;

// ---- D65 reference white ----
constexpr float kD65Xn = 0.95047f;
constexpr float kD65Yn = 1.00000f;
constexpr float kD65Zn = 1.08883f;

// ---- Color channel range ----
constexpr float kColorMax = 255.0f;

struct Lab { float L, a, b; };

// ---- sRGB -> linear -> XYZ -> Lab helpers ----

float srgb_to_linear(float c)
{
    c /= kColorMax;
    return (c <= kSrgbThreshold)
        ? (c * kSrgbLinearSlope)
        : std::pow((c + kSrgbGammaA) / kSrgbGammaB, kSrgbGammaExp);
}

float lab_f(float t)
{
    return (t > kCieDelta3)
        ? std::cbrt(t)
        : (t / (3.0f * kCieDelta2) + kCieFour29th);
}

void rgb_to_lab(uint8_t r, uint8_t g, uint8_t b,
                float& L, float& a, float& b_val)
{
    // sRGB -> linear
    float lr = srgb_to_linear(r);
    float lg = srgb_to_linear(g);
    float lb = srgb_to_linear(b);

    // Linear RGB -> XYZ (D65, sRGB primaries)
    float x = 0.4124564f * lr + 0.3575761f * lg + 0.1804375f * lb;
    float y = 0.2126729f * lr + 0.7151522f * lg + 0.0721750f * lb;
    float z = 0.0193339f * lr + 0.1191920f * lg + 0.9503041f * lb;

    // XYZ -> Lab
    float fx = lab_f(x / kD65Xn);
    float fy = lab_f(y / kD65Yn);
    float fz = lab_f(z / kD65Zn);

    L     = kCieLabLScale * fy - kCieLabLShift;
    a     = kCieLabAScale * (fx - fy);
    b_val = kCieLabBScale * (fy - fz);
}

} // anonymous namespace

namespace Slic3r {

// ---- public API ------------------------------------------------------------

float delta_e_cie76(uint8_t r1, uint8_t g1, uint8_t b1,
                    uint8_t r2, uint8_t g2, uint8_t b2)
{
    float L1, a1, b1v, L2, a2, b2v;
    rgb_to_lab(r1, g1, b1, L1, a1, b1v);
    rgb_to_lab(r2, g2, b2, L2, a2, b2v);

    float dL = L1 - L2;
    float da = a1 - a2;
    float db = b1v - b2v;
    return std::sqrt(dL * dL + da * da + db * db);
}

std::vector<int> compute_color_match(
    const std::vector<uint8_t>& design_r,
    const std::vector<uint8_t>& design_g,
    const std::vector<uint8_t>& design_b,
    const std::vector<uint8_t>& machine_r,
    const std::vector<uint8_t>& machine_g,
    const std::vector<uint8_t>& machine_b)
{
    const size_t designCount  = design_r.size();
    const size_t machineCount = machine_r.size();

    std::vector<int> result(designCount, -1);
    if (designCount == 0 || machineCount == 0)
        return result;

    // Precompute Lab values for all machine filaments
    std::vector<Lab> machineLab(machineCount);
    for (size_t j = 0; j < machineCount; ++j)
        rgb_to_lab(machine_r[j], machine_g[j], machine_b[j],
                   machineLab[j].L, machineLab[j].a, machineLab[j].b);

    // For each design filament, find the perceptually closest machine filament
    for (size_t i = 0; i < designCount; ++i) {
        float designL, designA, designB;
        rgb_to_lab(design_r[i], design_g[i], design_b[i],
                   designL, designA, designB);

        float bestDist = std::numeric_limits<float>::max();
        int   bestIdx  = -1;
        for (size_t j = 0; j < machineCount; ++j) {
            float dL   = designL - machineLab[j].L;
            float dA   = designA - machineLab[j].a;
            float dB   = designB - machineLab[j].b;
            float dist = dL * dL + dA * dA + dB * dB; // squared Euclidean (avoids sqrt)
            if (dist < bestDist) {
                bestDist = dist;
                bestIdx  = static_cast<int>(j);
            }
        }
        result[i] = bestIdx;
    }

    return result;
}

std::vector<int> compute_direct_override(
    size_t design_count,
    size_t machine_count)
{
    std::vector<int> result(design_count, -1);
    const size_t n = std::min(design_count, machine_count);
    for (size_t i = 0; i < n; ++i)
        result[i] = static_cast<int>(i);
    return result;
}

} // namespace Slic3r
