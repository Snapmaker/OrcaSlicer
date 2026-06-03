# Applied as PATCH_COMMAND for OpenEXR on Windows ARM64.
# Prepends a hard FORCE-off of all SSE/SIMD cache variables to
# OpenEXR/IlmImf/CMakeLists.txt so they are set *before*
# check_cxx_source_compiles ever runs — cmake -D flags and
# CMAKE_CACHE_ARGS can't win because the detection test compiles
# for the x64 configure-phase host, not the ARM64 target.

foreach(_f
    "OpenEXR/IlmImf/CMakeLists.txt"
    "IlmBase/Half/CMakeLists.txt"
)
    if(EXISTS "${_f}")
        file(READ "${_f}" _content)
        set(_guard "# [ARM64-SSE-PATCH] applied\n")
        if(NOT _content MATCHES "\\[ARM64-SSE-PATCH\\]")
            set(_prepend
                "${_guard}"
                "if(MSVC)\n"
                "  set(OPENEXR_IMF_HAVE_SSE2    OFF CACHE BOOL \"\" FORCE)\n"
                "  set(OPENEXR_IMF_HAVE_SSSE3   OFF CACHE BOOL \"\" FORCE)\n"
                "  set(OPENEXR_IMF_HAVE_SSE4_1  OFF CACHE BOOL \"\" FORCE)\n"
                "  set(ILMBASE_HAVE_SSE         OFF CACHE BOOL \"\" FORCE)\n"
                "  set(HALF_ENABLE_FP16_SUPPORT OFF CACHE BOOL \"\" FORCE)\n"
                "endif()\n"
            )
            string(CONCAT _patched ${_prepend} "${_content}")
            file(WRITE "${_f}" "${_patched}")
            message(STATUS "[ARM64 patch] Disabled SSE in ${_f}")
        else()
            message(STATUS "[ARM64 patch] Already patched: ${_f}")
        endif()
    endif()
endforeach()
