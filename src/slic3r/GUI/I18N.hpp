#pragma once
// Forwarding shim: the translation macros moved to the widgets library, which
// is the lowest layer that may use wx. libslic3r keeps its own callback-seam
// version (libslic3r/I18N.hpp); this file only preserves the legacy include
// path for the ~190 GUI-side includers.
#include <widgets/I18N.hpp>
