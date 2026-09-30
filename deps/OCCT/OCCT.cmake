if(WIN32)
    set(library_build_type "Shared")
else()
    set(library_build_type "Static")
endif()

# 0001-OCCT-fix.patch is a GIT-FORMAT diff, so --directory is load-bearing here: without
# it git apply resolves the paths against the repo root, skips every hunk and exits 0.
if (IN_GIT_REPO)
    set(OCCT_DIRECTORY_FLAG -DDIRECTORY=${BINARY_DIR_REL}/dep_OCCT-prefix/src/dep_OCCT)
endif ()

# ModelingAlgorithms is ON for the B-rep tools (STEP export, mesh -> B-rep, and the fillet /
# offset / feature operations the CAD work builds on). DataExchange already pulled in most of
# the module (TKBO TKBool TKGeomAlgo TKHLR TKMesh TKPrim TKShHealing TKTopAlgo); turning the
# module on adds exactly four toolkits: TKFillet, TKOffset, TKFeat and TKXMesh. Static on
# macOS/Linux (only referenced objects are linked); shared on Windows, where the app ships
# TKFillet/TKOffset/TKFeat plus TKBool (their dependency). Toolkit walk: OCCT adm/MODULES and
# src/<TK>/EXTERNLIB; the same analysis as Orca-Cad's docs/CAD/cad_dependency_weight.md.
Snapmaker_Orca_add_cmake_project(OCCT
    URL https://github.com/Open-Cascade-SAS/OCCT/archive/refs/tags/V7_6_0.zip
    URL_HASH SHA256=28334f0e98f1b1629799783e9b4d21e05349d89e695809d7e6dfa45ea43e1dbc
    PATCH_COMMAND ${CMAKE_COMMAND} -DGIT=${GIT_EXECUTABLE} ${OCCT_DIRECTORY_FLAG} -DLOOSE_WS=ON
                  -DP1=${CMAKE_CURRENT_LIST_DIR}/0001-OCCT-fix.patch
                  -P ${CMAKE_CURRENT_LIST_DIR}/../apply_patch.cmake
    #DEPENDS dep_Boost
    DEPENDS ${FREETYPE_PKG}
    CMAKE_ARGS
        -DBUILD_LIBRARY_TYPE=${library_build_type}
        -DUSE_TK=OFF
        -DUSE_TBB=OFF
	#-DUSE_FREETYPE=OFF
        -DUSE_FFMPEG=OFF
        -DUSE_VTK=OFF
        -DBUILD_DOC_Overview=OFF
        -DBUILD_MODULE_ApplicationFramework=OFF
        #-DBUILD_MODULE_DataExchange=OFF
        -DBUILD_MODULE_Draw=OFF
        -DBUILD_MODULE_FoundationClasses=OFF
        -DBUILD_MODULE_ModelingAlgorithms=ON
        -DBUILD_MODULE_ModelingData=OFF
        -DBUILD_MODULE_Visualization=OFF
)

# add_dependencies(dep_OCCT ${FREETYPE_PKG})
