#include <catch_main.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Utils.hpp"

namespace {
struct TestResources {
    TestResources()
    {
        Slic3r::set_resources_dir(TEST_RESOURCES_DIR);
    }
};

TestResources test_resources;
}

// The nanoSVG C implementation (NANOSVG_IMPLEMENTATION) used to be defined
// inline here. It now lives in the shared tests/nanosvg_impl.cpp, compiled into
// the test_nanosvg_impl static library that every test executable links, so
// that all headless test binaries (not just this one) can resolve libslic3r's
// nanoSVG references without duplicate definitions.
