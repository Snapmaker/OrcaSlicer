
set(_context_abi_line "")
set(_context_arch_line "")
if (APPLE AND CMAKE_OSX_ARCHITECTURES)
    if (CMAKE_OSX_ARCHITECTURES MATCHES "x86")
        set(_context_abi_line "-DBOOST_CONTEXT_ABI:STRING=sysv")
    elseif (CMAKE_OSX_ARCHITECTURES MATCHES "arm")
        set (_context_abi_line "-DBOOST_CONTEXT_ABI:STRING=aapcs")
    endif ()
    set(_context_arch_line "-DBOOST_CONTEXT_ARCHITECTURE:STRING=${CMAKE_OSX_ARCHITECTURES}")
endif ()

# Windows ARM64: Boost.Context's fcontext implementation requires assembling
# .asm files via armasm64, which trips a CMake ASM_ARMASM linker-module bug
# under the VS generator. The slicer uses neither boost::context nor
# boost::coroutine (0 references in src/), so exclude them on ARM64.
set(_boost_exclude "contract|fiber|numpy|stacktrace|wave|test")
if (MSVC AND "${DEPS_ARCH}" STREQUAL "arm64")
    set(_boost_exclude "${_boost_exclude}|context|coroutine|coroutine2")
endif()

Snapmaker_Orca_add_cmake_project(Boost
    URL "https://github.com/boostorg/boost/releases/download/boost-1.84.0/boost-1.84.0.tar.gz"
    URL_HASH SHA256=4d27e9efed0f6f152dc28db6430b9d3dfb40c0345da7342eaa5a987dde57bd95
    LIST_SEPARATOR |
    CMAKE_ARGS
        -DBOOST_EXCLUDE_LIBRARIES:STRING=${_boost_exclude}
        -DBOOST_LOCALE_ENABLE_ICU:BOOL=OFF # do not link to libicu, breaks compatibility between distros
        -DBUILD_TESTING:BOOL=OFF
        "${_context_abi_line}"
        "${_context_arch_line}"
)

set(DEP_Boost_DEPENDS ZLIB)