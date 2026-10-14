#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "libslic3r/FlushVolCalc.hpp"

// ---------------------------------------------------------------------------
// Test-side RGB2HSV
//
// libslic3r's FlushVolCalc.cpp calls the free function RGB2HSV(), which is
// defined in src/slic3r/Utils/ColorSpaceConvert.cpp and compiled into the
// libslic3r_gui library. libslic3r_tests only links libslic3r (not that
// library), so the symbol cannot be resolved at link time. Here we provide a
// pure-math implementation (identical to the production one) so the test links.
// If the production RGB2HSV changes, keep this copy in sync.
// ---------------------------------------------------------------------------
void RGB2HSV(float r, float g, float b, float* h, float* s, float* v)
{
    float Cmax  = std::max(std::max(r, g), b);
    float Cmin  = std::min(std::min(r, g), b);
    float delta = Cmax - Cmin;

    if (std::abs(delta) < 0.001) {
        *h = 0.f;
    }
    else if (Cmax == r) {
        *h = 60.f * std::fmod((g - b) / delta, 6.f);
    }
    else if (Cmax == g) {
        *h = 60.f * ((b - r) / delta + 2);
    }
    else {
        *h = 60.f * ((r - g) / delta + 4);
    }

    if (std::abs(Cmax) < 0.001) {
        *s = 0.f;
    }
    else {
        *s = delta / Cmax;
    }

    *v = Cmax;
}

using namespace Slic3r;

namespace {

struct Rgba {
    const char*    name;
    unsigned char a, r, g, b;
};

const Rgba kColors[] = {
    {"red",     255, 255, 0,   0},
    {"green",   255, 0,   255, 0},
    {"blue",    255, 0,   0,   255},
    {"yellow",  255, 255, 255, 0},
    {"cyan",    255, 0,   255, 255},
    {"magenta", 255, 255, 0,   255},
    {"white",   255, 255, 255, 255},
    {"black",   255, 0,   0,   0},
};

// Convenience wrapper: build a calculator with min=0, max=g_max_flush_volume and compute.
int calc(unsigned char sa, unsigned char sr, unsigned char sg, unsigned char sb,
         unsigned char da, unsigned char dr, unsigned char dg, unsigned char db)
{
    return FlushVolCalculator(0, g_max_flush_volume)
        .calc_flush_vol(sa, sr, sg, sb, da, dr, dg, db);
}

}

SCENARIO("FlushVolCalc: deterministic golden values", "[FlushVolCalc]") {
    GIVEN("a calculator with min=0, max=800") {
        THEN("same-color pairs equal the lower bound min + 60") {
            CHECK(calc(255, 0, 0, 0, 255, 0, 0, 0) == 60);            // black -> black
            CHECK(calc(255, 255, 0, 0, 255, 255, 0, 0) == 60);        // red -> red
        }

        THEN("switching to a brighter color needs more flushing (asymmetric)") {
            CHECK(calc(255, 0, 0, 0, 255, 255, 255, 255) == 560);     // black -> white
            CHECK(calc(255, 255, 255, 255, 255, 0, 0, 0) == 80);      // white -> black
        }

        THEN("a transparent color (alpha == 0) is treated as white") {
            CHECK(calc(0, 0, 0, 0, 255, 0, 0, 0) == 80);              // transparent -> black
            CHECK(calc(0, 0, 0, 0, 255, 255, 255, 255) == 60);        // transparent -> white
        }
    }
}

SCENARIO("FlushVolCalc: min offset and max clamp", "[FlushVolCalc]") {
    GIVEN("a support-material calculator with min=420") {
        FlushVolCalculator support(420, g_max_flush_volume);

        THEN("black->white 560 + 420 = 980 is clamped to max=800") {
            CHECK(support.calc_flush_vol(255, 0, 0, 0, 255, 255, 255, 255) == 800);
        }
    }

    GIVEN("a calculator with min=100") {
        FlushVolCalculator raised(100, g_max_flush_volume);

        THEN("the same-color result is raised to 60 + 100 = 160") {
            CHECK(raised.calc_flush_vol(255, 0, 0, 0, 255, 0, 0, 0) == 160);
        }
    }
}

SCENARIO("FlushVolCalc: color pair golden matrix", "[FlushVolCalc]") {
    GIVEN("the probe-captured 8x8 golden matrix") {
        FlushVolCalculator calc(0, g_max_flush_volume);
        const int           expected[8][8] = {
            { 60, 443, 237, 540, 494, 307, 586,  90},   // red
            {242,  60, 251, 408, 307, 237, 460, 107},   // green
            {393, 529,  60, 653, 540, 408, 661,  80},   // blue
            {256, 242, 266,  60, 237, 251, 307, 127},   // yellow
            {247, 234, 256, 393,  60, 242, 408, 114},   // cyan
            {234, 388, 242, 529, 443,  60, 540,  96},   // magenta
            {262, 248, 272, 234, 242, 256,  60,  80},   // white
            {408, 540, 307, 661, 586, 460, 560,  60},   // black
        };

        THEN("each source->target color pair matches the captured value") {
            for (int i = 0; i < 8; ++i)
                for (int j = 0; j < 8; ++j) {
                    DYNAMIC_SECTION(kColors[i].name << " -> " << kColors[j].name) {
                        CHECK(calc.calc_flush_vol(kColors[i].a, kColors[i].r, kColors[i].g, kColors[i].b,
                                                  kColors[j].a, kColors[j].r, kColors[j].g, kColors[j].b)
                              == expected[i][j]);
                    }
                }
        }
    }
}

SCENARIO("FlushVolCalc: alpha independence and bounds", "[FlushVolCalc]") {
    GIVEN("a calculator with min=0, max=800") {
        FlushVolCalculator calc(0, g_max_flush_volume);

        THEN("any alpha > 0 is equivalent (only alpha == 0 is special-cased)") {
            CHECK(calc.calc_flush_vol(255, 255, 0, 0, 255, 0, 0, 255) ==
                  calc.calc_flush_vol(128, 255, 0, 0, 255, 0, 0, 255));
        }

        THEN("all color pairs fall within [min + 60, max]") {
            for (const Rgba& s : kColors)
                for (const Rgba& d : kColors) {
                    int v = calc.calc_flush_vol(s.a, s.r, s.g, s.b, d.a, d.r, d.g, d.b);
                    CHECK(v >= 60);
                    CHECK(v <= g_max_flush_volume);
                }
        }
    }
}
