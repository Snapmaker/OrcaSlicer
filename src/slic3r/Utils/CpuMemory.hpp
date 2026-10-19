// Snapmaker Orca: how much physical memory the system can still hand out. Same path as upstream's
// file so its changes conflict visibly; its fixed 5 GB gate CurFreeMemoryLessThanSpecifySizeGb()
// is not ported, the render LOD decides per job.
#pragma once

#include <cstdint>

namespace Slic3r {

struct CpuMemory
{
    // Bytes of physical memory available to a new allocation without swapping, as the operating
    // system estimates it; 0 when unknown. Cheap enough to be called once per background job,
    // not meant for a per-frame path. Thread safe.
    static uint64_t available_bytes();
};

} // namespace Slic3r
