if (IN_GIT_REPO)
    set(CGAL_DIRECTORY_FLAG --directory ${BINARY_DIR_REL}/dep_CGAL-prefix/src/dep_CGAL)
endif ()

Snapmaker_Orca_add_cmake_project(
    CGAL
    # GIT_REPOSITORY https://github.com/CGAL/cgal.git
    # GIT_TAG        3654f780ae0c64675cabaef0e5ddaf904c48b4b7 # releases/CGAL-5.6.3
    # Bumped CGAL 5.4 -> 5.6.3 (matching mainline OrcaSlicer) to fix the Emboss
    # "On Surface" cut_surface crash: a heap over-read inside CGAL's corefinement
    # exact-arithmetic (Epeck) constrained triangulation on macOS/arm64. 5.6.3 is
    # API-compatible with this Emboss/CutSurface code (mainline ships it). The
    # 5.4-only 0001-clang19.patch is dropped (does not apply to 5.6.3).
    URL      https://github.com/CGAL/cgal/releases/download/v5.6.3/CGAL-5.6.3.zip
    URL_HASH SHA256=5d577acb4a9918ccb960491482da7a3838f8d363aff47e14d703f19fd84733d4
    DEPENDS dep_Boost dep_GMP dep_MPFR
)

include(GNUInstallDirs)
