# Applies the CGAL patches idempotently.
#
# ExternalProject re-runs PATCH_COMMAND whenever the command changes. A bare
# `git apply` would then fail on a source tree that a previous build already
# patched ("patch does not apply"), e.g. after editing the patch files or on
# a reused/cached build directory. Instead, first check the forward direction
# and only apply when needed; if the forward check fails but the reverse check
# succeeds, the tree is already patched and the step succeeds without touching
# anything.
#
# --git-dir points at a marker that never exists. This prevents git from
# detecting the enclosing OrcaSlicer work tree when the deps build lives
# inside it, so git apply behaves like a plain patch tool with paths resolved
# relative to CGAL_SRC_DIR. That keeps the step format-agnostic: 0001 is a
# plain unified diff while 0002 is a git-style diff, and with the outer
# repository detected, the former would mangle paths under --directory and
# the latter would be silently skipped by the subdirectory prefix check.
#
# Variables expected via -D on the cmake -P command line:
#   CGAL_SRC_DIR  absolute path of the extracted CGAL source tree
#   CGAL_PATCH1   absolute path of 0001-clang19.patch
#   CGAL_PATCH2   absolute path of 0002-robust-intersection-functors.patch

set(_git git
    "--git-dir=${CGAL_SRC_DIR}/.git-disabled-marker"
    -C "${CGAL_SRC_DIR}")
set(_args --ignore-space-change --whitespace=fix)

foreach(_patch IN ITEMS "${CGAL_PATCH1}" "${CGAL_PATCH2}")
    execute_process(
        COMMAND ${_git} apply --check ${_args} "${_patch}"
        RESULT_VARIABLE _fwd
        OUTPUT_VARIABLE _fwd_out
        ERROR_VARIABLE _fwd_err)

    if("${_fwd}" STREQUAL "0")
        execute_process(
            COMMAND ${_git} apply --verbose ${_args} "${_patch}"
            RESULT_VARIABLE _res
            OUTPUT_VARIABLE _out
            ERROR_VARIABLE _err)

        if(NOT "${_res}" STREQUAL "0")
            message(FATAL_ERROR
                "Failed to apply ${_patch}:\n${_out}\n${_err}")
        endif()
    else()
        execute_process(
            COMMAND ${_git} apply --check --reverse ${_args} "${_patch}"
            RESULT_VARIABLE _rev
            OUTPUT_VARIABLE _rev_out
            ERROR_VARIABLE _rev_err)

        if("${_rev}" STREQUAL "0")
            message(STATUS "Already applied, skipping: ${_patch}")
        else()
            message(FATAL_ERROR
                "Patch does not match source: ${_patch}\n"
                "Forward check:\n${_fwd_out}\n${_fwd_err}\n"
                "Reverse check:\n${_rev_out}\n${_rev_err}")
        endif()
    endif()
endforeach()