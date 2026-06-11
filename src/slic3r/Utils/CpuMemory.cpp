#include "CpuMemory.hpp"

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__) || defined(__linux__)
#include <unistd.h>
#include <sys/sysinfo.h>
#endif

namespace Slic3r {

bool CpuMemory::CurFreeMemoryLessThanSpecifySizeGb(int sizeGb)
{
#ifdef _WIN32
    MEMORYSTATUSEX memStatus;
    memStatus.dwLength = sizeof(MEMORYSTATUSEX);
    if (GlobalMemoryStatusEx(&memStatus)) {
        unsigned long long freeMemGb = memStatus.ullAvailPhys / (1024ULL * 1024ULL * 1024ULL);
        return freeMemGb < static_cast<unsigned long long>(sizeGb);
    }
    return false; // Can't determine, default to not blocking
#elif defined(__APPLE__) || defined(__linux__)
    // Linux and macOS
    long pages = sysconf(_SC_AVPHYS_PAGES);
    long pageSize = sysconf(_SC_PAGESIZE);
    if (pages > 0 && pageSize > 0) {
        unsigned long long freeMemGb = (unsigned long long)pages * (unsigned long long)pageSize / (1024ULL * 1024ULL * 1024ULL);
        return freeMemGb < static_cast<unsigned long long>(sizeGb);
    }
    return false; // Can't determine, default to not blocking
#else
    return false;
#endif
}

} // namespace Slic3r
