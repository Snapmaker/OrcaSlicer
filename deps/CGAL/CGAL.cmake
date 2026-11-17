Snapmaker_Orca_add_cmake_project(
    CGAL
    # GIT_REPOSITORY https://github.com/CGAL/cgal.git
    # GIT_TAG        bec70a6d52d8aacb0b3d82a7b4edc3caa899184b # releases/CGAL-5.0
    # For whatever reason, this keeps downloading forever (repeats downloads if finished)
    URL      https://github.com/CGAL/cgal/archive/refs/tags/v5.4.zip
    URL_HASH SHA256=d7605e0a5a5ca17da7547592f6f6e4a59430a0bc861948974254d0de43eab4c0
    # Both patches in one git apply call (as in OpenCV.cmake): ExternalProject
    # does not run PATCH_COMMAND through a shell, so "&&" chaining would not work.
    # The helper script makes the step idempotent: a tree that was already
    # patched by a previous build (or a cached build dir) is detected via a
    # reverse check instead of failing with "patch does not apply".
    PATCH_COMMAND ${CMAKE_COMMAND}
        -DCGAL_SRC_DIR=${CMAKE_BINARY_DIR}/dep_CGAL-prefix/src/dep_CGAL
        -DCGAL_PATCH1=${CMAKE_CURRENT_LIST_DIR}/0001-clang19.patch
        -DCGAL_PATCH2=${CMAKE_CURRENT_LIST_DIR}/0002-robust-intersection-functors.patch
        -P ${CMAKE_CURRENT_LIST_DIR}/apply_cgal_patches.cmake
    DEPENDS dep_Boost dep_GMP dep_MPFR
)

include(GNUInstallDirs)
