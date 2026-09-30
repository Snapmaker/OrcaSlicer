// Provides the nanoSVG rasterizer implementation for the headless test executables.
//
// The nanoSVG *parser* implementation (nsvgParseFromFile, nsvgParse, nsvgDelete, ...)
// is already compiled into libslic3r via src/libslic3r/Format/svg.cpp
// (#define NANOSVG_IMPLEMENTATION), so it must NOT be redefined here — doing so
// produces duplicate symbol errors at link time. Only the rasterizer
// (nanosvgrast) is provided at the test-link level.

#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvgrast.h"
