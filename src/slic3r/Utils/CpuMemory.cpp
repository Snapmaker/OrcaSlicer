#include "CpuMemory.hpp"

// Platform headers stay outside namespace Slic3r (upstream #737: inside, their declarations
// would land in that namespace).
#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_host.h>
#elif defined(__linux__)
#include <cstdio>
#include <sys/sysinfo.h>
#endif

namespace Slic3r {

uint64_t CpuMemory::available_bytes()
{
#if defined(_WIN32)
    MEMORYSTATUSEX status;
    status.dwLength = sizeof(status);
    return GlobalMemoryStatusEx(&status) ? uint64_t(status.ullAvailPhys) : 0;
#elif defined(__APPLE__)
    // macOS frees inactive and purgeable pages on demand, so they count as available.
    // free_count already includes the speculative pages.
    const mach_port_t      host  = mach_host_self();
    vm_size_t              page_size = 0;
    vm_statistics64_data_t stats;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    uint64_t               bytes = 0;
    if (host_page_size(host, &page_size) == KERN_SUCCESS &&
        host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&stats), &count) == KERN_SUCCESS)
        bytes = (uint64_t(stats.free_count) + uint64_t(stats.inactive_count) + uint64_t(stats.purgeable_count)) *
                uint64_t(page_size);
    // mach_host_self() hands out a send right on every call; upstream leaks it.
    mach_port_deallocate(mach_task_self(), host);
    return bytes;
#elif defined(__linux__)
    // MemAvailable is the kernel's own estimate and includes the reclaimable page cache;
    // sysinfo().freeram (upstream) reads close to zero on a machine that has been up for a while.
    if (FILE *meminfo = std::fopen("/proc/meminfo", "r")) {
        char               line[256];
        unsigned long long kib   = 0;
        bool               found = false;
        while (!found && std::fgets(line, sizeof(line), meminfo) != nullptr)
            found = std::sscanf(line, "MemAvailable: %llu kB", &kib) == 1;
        std::fclose(meminfo);
        if (found)
            return uint64_t(kib) * 1024ull;
    }
    struct sysinfo info;
    if (sysinfo(&info) == 0)
        return (uint64_t(info.freeram) + uint64_t(info.bufferram)) * uint64_t(info.mem_unit);
    return 0;
#else
    return 0;
#endif
}

} // namespace Slic3r
