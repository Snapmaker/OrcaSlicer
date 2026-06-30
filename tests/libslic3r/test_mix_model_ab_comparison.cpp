// In-process A/B comparison of the legacy Justin-Hayes polynomial mixer
// (Slic3r::filament_mixer_lerp) vs the calibrated prusa-fdm-mixer model.
//
// Compiled ONLY when ENABLE_PRUSA_FDM_MIXER_DEMO is on (Technologies.hpp).
// When the macro is off this translation unit is empty (no test cases), so the
// file can stay permanently registered in tests/libslic3r/CMakeLists.txt.
//
// Both predictions are converted to Lab and compared with the SAME yardstick
// (prusa's rgb_to_lab + delta_e_2000), so the printed table is an
// apples-to-apples characterisation of WHERE the two models disagree — NOT a
// ground-truth accuracy claim. Accuracy vs real prints requires measured
// swatches (see prusa-fdm-mixer methodology); this test surfaces the
// disagreement surface, which prusa reports is largest on saturated
// complementary mixes.
#include "libslic3r/Technologies.hpp"  // defines ENABLE_PRUSA_FDM_MIXER_DEMO (must precede the #if)

#if ENABLE_PRUSA_FDM_MIXER_DEMO

#include <catch2/catch.hpp>
#include "libslic3r/filament_mixer.h"
#include "libslic3r/prusa_fdm_mixer.hpp"

#include <string>

namespace {
struct Pair { const char* name; const char* a; const char* b; };
}

TEST_CASE("prusa vs legacy mixer disagreement map", "[mixed_filament][ab_demo]")
{
    // Saturated / complementary pairs where the two models are expected to
    // differ most; plus a neutral grey ramp for reference.
    const Pair pairs[] = {
        {"cyan + magenta", "#009bc3", "#c3141f"},
        {"blue + yellow",  "#1f3a93", "#f6b921"},
        {"red + green",    "#c3141f", "#2e8b57"},
        {"red + yellow",   "#c3141f", "#f6b921"},
        {"blue + red",     "#1f3a93", "#c3141f"},
        {"green + magenta","#2e8b57", "#c3141f"},
        {"black + white",  "#000000", "#ffffff"},
    };

    WARN("=== prusa-fdm-mixer vs legacy polynomial (pair @ t=0.5) ===");

    double sum_de = 0.0;
    double max_de = 0.0;
    int    count  = 0;

    for (const Pair& p : pairs) {
        const prusa_fdm_mixer::RGB a = prusa_fdm_mixer::hex_to_rgb(p.a);
        const prusa_fdm_mixer::RGB b = prusa_fdm_mixer::hex_to_rgb(p.b);

        // Legacy prediction (Justin-Hayes degree-4 polynomial).
        unsigned char lr = 0, lg = 0, lb = 0;
        Slic3r::filament_mixer_lerp(a.r, a.g, a.b, b.r, b.g, b.b, 0.5f, &lr, &lg, &lb);
        const prusa_fdm_mixer::RGB legacy{lr, lg, lb};

        // Prusa prediction (calibrated Yule-Nielsen).
        const prusa_fdm_mixer::RGB prusa = prusa_fdm_mixer::mix_rgb({ {p.a, 0.5}, {p.b, 0.5} });

        const prusa_fdm_mixer::LAB ll = prusa_fdm_mixer::rgb_to_lab(legacy);
        const prusa_fdm_mixer::LAB pl = prusa_fdm_mixer::rgb_to_lab(prusa);
        const double de = prusa_fdm_mixer::delta_e_2000(ll, pl);

        REQUIRE(de >= 0.0);             // finite, non-negative
        sum_de += de;
        if (de > max_de) max_de = de;
        ++count;

        WARN("| " << p.name
                  << " | legacy(" << (int)legacy.r << "," << (int)legacy.g << "," << (int)legacy.b << ")"
                  << " | prusa("  << (int)prusa.r  << "," << (int)prusa.g  << "," << (int)prusa.b  << ")"
                  << " | dE=" << de << " |");
    }

    WARN("summary: n=" << count << " avg_dE=" << (sum_de / count) << " max_dE=" << max_de);

    // Deterministic invariants ---------------------------------------------

    // 1. Legacy endpoints are exact (filament_mixer::lerp returns the input
    //    colour verbatim for t<=0 / t>=1; no polynomial math runs).
    for (const Pair& p : pairs) {
        const prusa_fdm_mixer::RGB a = prusa_fdm_mixer::hex_to_rgb(p.a);
        const prusa_fdm_mixer::RGB b = prusa_fdm_mixer::hex_to_rgb(p.b);

        unsigned char r = 0, g = 0, bb = 0;
        Slic3r::filament_mixer_lerp(a.r, a.g, a.b, b.r, b.g, b.b, 0.0f, &r, &g, &bb);
        REQUIRE(static_cast<int>(r) == a.r);
        REQUIRE(static_cast<int>(g) == a.g);
        REQUIRE(static_cast<int>(bb) == a.b);

        Slic3r::filament_mixer_lerp(a.r, a.g, a.b, b.r, b.g, b.b, 1.0f, &r, &g, &bb);
        REQUIRE(static_cast<int>(r) == b.r);
        REQUIRE(static_cast<int>(g) == b.g);
        REQUIRE(static_cast<int>(bb) == b.b);
    }

    // 2. Prusa endpoints are gradient-safe: ratio>=1 returns the source colour
    //    (within a small Lab round-trip tolerance, since mix_rgb goes
    //    Lab->RGB internally).
    for (const Pair& p : pairs) {
        const prusa_fdm_mixer::RGB a = prusa_fdm_mixer::hex_to_rgb(p.a);
        const prusa_fdm_mixer::RGB b = prusa_fdm_mixer::hex_to_rgb(p.b);

        const prusa_fdm_mixer::RGB pa = prusa_fdm_mixer::mix_rgb({ {p.a, 1.0}, {p.b, 0.0} });
        const prusa_fdm_mixer::RGB pb = prusa_fdm_mixer::mix_rgb({ {p.a, 0.0}, {p.b, 1.0} });

        const double de_a = prusa_fdm_mixer::delta_e_2000(prusa_fdm_mixer::rgb_to_lab(pa), prusa_fdm_mixer::rgb_to_lab(a));
        const double de_b = prusa_fdm_mixer::delta_e_2000(prusa_fdm_mixer::rgb_to_lab(pb), prusa_fdm_mixer::rgb_to_lab(b));
        REQUIRE(de_a < 2.0);
        REQUIRE(de_b < 2.0);
    }
}

#endif // ENABLE_PRUSA_FDM_MIXER_DEMO
