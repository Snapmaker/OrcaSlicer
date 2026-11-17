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

set(_git git --git-dir=${CGAL_SRC_DIR}/.git-disabled-marker -C ${CGAL_SRC_DIR})
set(_args --ignore-space-change --whitespace=fix)
set(_patches ${CGAL_PATCH1} ${CGAL_PATCH2})

execute_process(
    COMMAND ${_git} apply --check ${_args} ${_patches}
    RESULT_VARIABLE _fwd
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)

if(_fwd EQUAL 0)
    execute_process(
        COMMAND ${_git} apply --verbose ${_args} ${_patches}
        RESULT_VARIABLE _res
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err)
    if(NOT _res EQUAL 0)
        message(FATAL_ERROR "git apply failed:\n${_out}\n${_err}")
    endif()
else()
    execute_process(
        COMMAND ${_git} apply --check --reverse ${_args} ${_patches}
        RESULT_VARIABLE _rev
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err)
    if(_rev EQUAL 0)
        message(STATUS "CGAL patches already applied - skipping")
    else()
        message(FATAL_ERROR "git apply --check failed (forward and reverse), the CGAL tree is neither clean nor fully patched:\n${_out}\n${_err}")
    endif()
endif()
