// Hackish wrapper around the ClipperLib library to compile the Clipper2 library with the Z support.

#ifndef clipper2_z_hpp
#define clipper2_z_hpp

// Orca: allow clipper2_z.hpp and clipper.h to be included in either order in the same TU.
#ifdef USINGZ
#include "clipper.h"
#else

// Save and clear active 2D Clipper2 header guards so clipper.h and all its sub-headers
// can be re-evaluated under USINGZ (namespace Clipper2Lib_Z).
// Note: CLIPPER_VERSION_H is intentionally left defined because CLIPPER2_VERSION is at global scope.
#ifdef CLIPPER_H
#define CLIPPER2_Z_HAD_CLIPPER_H
#undef CLIPPER_H
#endif
#ifdef CLIPPER_CORE_H
#define CLIPPER2_Z_HAD_CLIPPER_CORE_H
#undef CLIPPER_CORE_H
#endif
#ifdef CLIPPER_ENGINE_H
#define CLIPPER2_Z_HAD_CLIPPER_ENGINE_H
#undef CLIPPER_ENGINE_H
#endif
#ifdef CLIPPER_OFFSET_H_
#define CLIPPER2_Z_HAD_CLIPPER_OFFSET_H_
#undef CLIPPER_OFFSET_H_
#endif
#ifdef CLIPPER_RECTCLIP_H
#define CLIPPER2_Z_HAD_CLIPPER_RECTCLIP_H
#undef CLIPPER_RECTCLIP_H
#endif
#ifdef CLIPPER_MINKOWSKI_H
#define CLIPPER2_Z_HAD_CLIPPER_MINKOWSKI_H
#undef CLIPPER_MINKOWSKI_H
#endif
#ifdef CLIPPER_TRIANGULATION_H
#define CLIPPER2_Z_HAD_CLIPPER_TRIANGULATION_H
#undef CLIPPER_TRIANGULATION_H
#endif
#ifdef CLIPPER2_NODE_ALLOCATOR
#define CLIPPER2_Z_HAD_NODE_ALLOCATOR
#undef CLIPPER2_NODE_ALLOCATOR
#endif
#if defined(CLIPPER2_HI_PRECISION) && CLIPPER2_HI_PRECISION
#ifdef CC_MIN
#define CLIPPER2_Z_HAD_CC_MIN
#undef CC_MIN
#endif
#ifdef CC_MAX
#define CLIPPER2_Z_HAD_CC_MAX
#undef CC_MAX
#endif
#endif

// Enable the Z coordinate support.
#define USINGZ

#include "clipper.h"

#undef USINGZ

// Restore the prior 2D guard state: keep defined only headers already included before us,
// and clear the rest so a subsequent #include <clipper2/clipper.h> still populates Clipper2Lib.
#ifdef CLIPPER2_Z_HAD_CLIPPER_H
#undef CLIPPER2_Z_HAD_CLIPPER_H
#else
#undef CLIPPER_H
#endif
#ifdef CLIPPER2_Z_HAD_CLIPPER_CORE_H
#undef CLIPPER2_Z_HAD_CLIPPER_CORE_H
#else
#undef CLIPPER_CORE_H
#endif
#ifdef CLIPPER2_Z_HAD_CLIPPER_ENGINE_H
#undef CLIPPER2_Z_HAD_CLIPPER_ENGINE_H
#else
#undef CLIPPER_ENGINE_H
#endif
#ifdef CLIPPER2_Z_HAD_CLIPPER_OFFSET_H_
#undef CLIPPER2_Z_HAD_CLIPPER_OFFSET_H_
#else
#undef CLIPPER_OFFSET_H_
#endif
#ifdef CLIPPER2_Z_HAD_CLIPPER_RECTCLIP_H
#undef CLIPPER2_Z_HAD_CLIPPER_RECTCLIP_H
#else
#undef CLIPPER_RECTCLIP_H
#endif
#ifdef CLIPPER2_Z_HAD_CLIPPER_MINKOWSKI_H
#undef CLIPPER2_Z_HAD_CLIPPER_MINKOWSKI_H
#else
#undef CLIPPER_MINKOWSKI_H
#endif
#ifdef CLIPPER2_Z_HAD_CLIPPER_TRIANGULATION_H
#undef CLIPPER2_Z_HAD_CLIPPER_TRIANGULATION_H
#else
#undef CLIPPER_TRIANGULATION_H
#endif
#ifdef CLIPPER2_Z_HAD_NODE_ALLOCATOR
#undef CLIPPER2_Z_HAD_NODE_ALLOCATOR
#else
#undef CLIPPER2_NODE_ALLOCATOR
#endif
#if defined(CLIPPER2_HI_PRECISION) && CLIPPER2_HI_PRECISION
#ifdef CLIPPER2_Z_HAD_CC_MIN
#undef CLIPPER2_Z_HAD_CC_MIN
#else
#undef CC_MIN
#endif
#ifdef CLIPPER2_Z_HAD_CC_MAX
#undef CLIPPER2_Z_HAD_CC_MAX
#else
#undef CC_MAX
#endif
#endif

#endif // USINGZ
#endif // clipper2_z_hpp
