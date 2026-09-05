if(${CMAKE_VERSION} VERSION_GREATER_EQUAL "4.0")
    set(CMAKE_POLICY_VERSION_MINIMUM 3.13 CACHE STRING "" FORCE)
endif()

cmake_minimum_required(VERSION 3.13)


# The following line used to be in tests/CMakeLists.txt
# Having it there causes rebuilds of all targets on any CMakeLists.txt change under tests/
# It has no effect on how code is compiled or linked.
# It just lets you later do `set_property(TARGET foo PROPERTY FOLDER "bar")`
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if (WIN32)
    # Detect known CI environments
    set(IS_CI FALSE)
    if(DEFINED ENV{CI})
        set(IS_CI TRUE)
    elseif(DEFINED ENV{GITHUB_ACTIONS})
        set(IS_CI TRUE)
    elseif(DEFINED ENV{GITLAB_CI})
        set(IS_CI TRUE)
    elseif(DEFINED ENV{TF_BUILD})
        set(IS_CI TRUE)
    elseif(DEFINED ENV{BUILD_NUMBER}) # Jenkins
        set(IS_CI TRUE)
    endif()

    # Detect common misconfiguration (Strawberry Perl in PATH before CMake)
    # We use ENV to check the PATH order
    string(REPLACE "\\" "/" ENV_PATH "$ENV{PATH}")
    string(FIND "${ENV_PATH}" "Strawberry/c/bin" STRAWBERRY_POS)
    string(FIND "${ENV_PATH}" "Program Files/CMake/bin" CMAKE_POS)

    if (STRAWBERRY_POS GREATER -1 AND CMAKE_POS GREATER -1 AND STRAWBERRY_POS LESS CMAKE_POS)
        set(_warning_text "
#############################################################
Detected Strawberry Perl's 'c/bin' appearing before CMake in PATH.
This is known to cause CMake to misbehave (e.g., missing modules).
Please adjust your PATH so that:
C:\\Program Files\\CMake\\bin
appears before:
C:\\Strawberry\\c\\bin
You can do this in Environment Variables settings.
#############################################################
        ")

        if(NOT IS_CI)
            message(FATAL_ERROR "${_warning_text}")
        endif()
    endif()
endif ()

if (APPLE)
    # Require macOS 12.0+ (Monterey) as minimum version
    # This ensures proper "requires macOS 12.0" system dialog instead of crash on older systems
    if (NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum OS X deployment version" FORCE)
    endif ()
    message(STATUS "CMAKE_OSX_DEPLOYMENT_TARGET: ${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif ()

# Keep MSVC's default /W3 out of CMAKE_<LANG>_FLAGS so it can be applied to our own
# targets only. Silencing a bundled target would otherwise override a warning level,
# which cl reports as D9025 for every file it compiles.
if (POLICY CMP0092)
    cmake_policy(SET CMP0092 NEW)
endif ()

project(Snapmaker_Orca)

# Backward compatibility for old CMake versions
if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_VERSION VERSION_LESS "3.25")
    set(LINUX ON CACHE BOOL "" FORCE)
endif ()

include("version.inc")
include(GNUInstallDirs)
include(CMakeDependentOption)

# Set SLIC3R_BUILD_ID from Snapmaker_VERSION if not already defined
if(NOT DEFINED SLIC3R_BUILD_ID OR SLIC3R_BUILD_ID STREQUAL "")
    set(SLIC3R_BUILD_ID "${Snapmaker_VERSION}")
endif()

# Create Windows RC compatible version format (comma-separated: MAJOR,MINOR,PATCH,0)
set(SLIC3R_RC_VERSION "${ORCA_VERSION_MAJOR},${ORCA_VERSION_MINOR},${ORCA_VERSION_PATCH},0")

set(SLIC3R_RESOURCES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources")
file(TO_NATIVE_PATH "${SLIC3R_RESOURCES_DIR}" SLIC3R_RESOURCES_DIR_WIN)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "No build type selected, default to Release")
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type (default Release)" FORCE)
endif()

if (DEFINED BBL_RELEASE_TO_PUBLIC)
    add_compile_definitions("BBL_RELEASE_TO_PUBLIC=${BBL_RELEASE_TO_PUBLIC}")
    if (BBL_RELEASE_TO_PUBLIC)
        add_compile_definitions(WXINSPECTOR_DISABLE)
    endif ()
else ()
    add_compile_definitions("BBL_RELEASE_TO_PUBLIC=$<CONFIG:Release>")
    add_compile_definitions("$<$<CONFIG:Release>:WXINSPECTOR_DISABLE>")
endif ()

find_package(Git)
if(DEFINED ENV{git_commit_hash} AND NOT "$ENV{git_commit_hash}" STREQUAL "")
    message(STATUS "Specified git commit hash: $ENV{git_commit_hash}")
    if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
        # Convert the given hash to short hash
        execute_process(
            COMMAND ${GIT_EXECUTABLE} rev-parse --short "$ENV{git_commit_hash}"
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE GIT_COMMIT_HASH
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
    else()
        # No .git directory (e.g., Flatpak sandbox) — truncate directly
        string(SUBSTRING "$ENV{git_commit_hash}" 0 7 GIT_COMMIT_HASH)
    endif()
    add_definitions("-DGIT_COMMIT_HASH=\"${GIT_COMMIT_HASH}\"")
elseif(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    # Check current Git commit hash
    execute_process(
        COMMAND ${GIT_EXECUTABLE} log -1 --format=%h
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_COMMIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    add_definitions("-DGIT_COMMIT_HASH=\"${GIT_COMMIT_HASH}\"")
endif()

if(DEFINED ENV{SLIC3R_STATIC})
    set(SLIC3R_STATIC_INITIAL $ENV{SLIC3R_STATIC})
else()
    set(SLIC3R_STATIC_INITIAL 1)
endif()

option(SLIC3R_STATIC            "Compile Snapmaker_Orca with static libraries (Boost, TBB)" ${SLIC3R_STATIC_INITIAL})
option(SLIC3R_GUI               "Compile Snapmaker_Orca with GUI components (OpenGL, wxWidgets)" 1)
option(SLIC3R_FHS               "Assume Snapmaker_Orca is to be installed in a FHS directory structure" 0)
option(SLIC3R_PROFILE           "Compile Snapmaker_Orca with an invasive Shiny profiler" 0)
option(SLIC3R_PCH               "Use precompiled headers" 1)
option(SLIC3R_WARNINGS          "Emit compiler warnings for OrcaSlicer sources" 1)
option(SLIC3R_BUNDLED_WARNINGS  "Emit compiler warnings for bundled third-party sources" 0)
option(SLIC3R_MSVC_COMPILE_PARALLEL "Compile on Visual Studio in parallel" 1)
option(SLIC3R_MSVC_PDB          "Generate PDB files on MSVC in Release mode" 1)
option(SLIC3R_ASAN              "Enable ASan on Clang and GCC" 0)

# Sentry crash reporting - on by default everywhere except the Flatpak build
# (Flathub distribution stays telemetry-free).
if (WIN32 OR APPLE OR (UNIX AND NOT FLATPAK))
    set(SLIC3R_SENTRY_DEFAULT ON)
else()
    set(SLIC3R_SENTRY_DEFAULT OFF)
endif()
option(SLIC3R_SENTRY            "Enable Sentry crash reporting SDK" ${SLIC3R_SENTRY_DEFAULT})

# Python stubgen module
option(ORCA_BUILD_PYTHON_STUBGEN_MODULE "Build importable Python module for pybind11-stubgen" ON)
set(ORCA_BUNDLED_UV_EXECUTABLE "" CACHE FILEPATH "Path to a uv executable to bundle for Python package installation. Leave empty to auto-download.")

# Auto-download uv if not provided by the user.
# uv is pinned: the executable ships inside the signed app, so the build must
# be reproducible and the download verified. To upgrade, bump ORCA_UV_VERSION
# and refresh every hash below from the release's .sha256 assets, e.g.:
#   curl -sL https://github.com/astral-sh/uv/releases/download/<ver>/uv-<triple>.tar.gz.sha256
set(ORCA_UV_VERSION "0.11.21")
set(ORCA_UV_SHA256_aarch64-apple-darwin      "1f921d491ba5ffeea774eb04d6681ecee379101341cbb1500394993b541bf3f4")
set(ORCA_UV_SHA256_x86_64-apple-darwin       "f3c8e5708a84b920c18b691214d54d2b0da6b984789caae95d47c95120cb7765")
set(ORCA_UV_SHA256_aarch64-unknown-linux-gnu "88e800834007cc5efd4675f166eb2a51e7e3ad19876d85fa8805a6fb5c922397")
set(ORCA_UV_SHA256_x86_64-unknown-linux-gnu  "8c88519b0ef0af9801fcdee419bbb12116bd9e6b18e162ae093c932d8b264050")
set(ORCA_UV_SHA256_x86_64-pc-windows-msvc    "ace861f360c6de2babedc1607d0f454b6b09a820dbc8182dc15af927e4df9589")

# Version-scoped cache dir so a version bump invalidates the cached binary.
set(ORCA_UV_DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/.uv/${ORCA_UV_VERSION}")
set(ORCA_UV_BINARY "${ORCA_UV_DOWNLOAD_DIR}/uv")
if(WIN32)
    set(ORCA_UV_BINARY "${ORCA_UV_DOWNLOAD_DIR}/uv.exe")
endif()

# Older configures FORCE-cached the auto-downloaded path; any cache entry
# under .uv/ is that legacy value -- clear it so auto-download re-resolves
# (user-provided paths never live under .uv/).
if(ORCA_BUNDLED_UV_EXECUTABLE MATCHES "/\\.uv/")
    set(ORCA_BUNDLED_UV_EXECUTABLE "" CACHE FILEPATH "Path to the auto-downloaded uv executable" FORCE)
endif()

if(NOT ORCA_BUNDLED_UV_EXECUTABLE)
    if(NOT EXISTS "${ORCA_UV_BINARY}")
        # Select uv by TARGET arch. On macOS universal builds each leg sets a single
        # CMAKE_OSX_ARCHITECTURES (and the x86_64 leg cross-builds on an arm64 runner),
        # so prefer it over the host CMAKE_SYSTEM_PROCESSOR -- otherwise both legs fetch
        # the same arch and the later universal lipo merge of tools/uv/uv fails.
        set(_orca_uv_proc "${CMAKE_SYSTEM_PROCESSOR}")
        if(APPLE AND CMAKE_OSX_ARCHITECTURES)
            set(_orca_uv_proc "${CMAKE_OSX_ARCHITECTURES}")
        endif()
        # All release archives are tar.gz except the Windows zip.
        set(ORCA_UV_EXT "tar.gz")
        if(_orca_uv_proc MATCHES "x86_64|AMD64|amd64")
            if(WIN32)
                set(ORCA_UV_ARCH "x86_64-pc-windows-msvc")
                set(ORCA_UV_EXT "zip")
            elseif(APPLE)
                set(ORCA_UV_ARCH "x86_64-apple-darwin")
            else()
                set(ORCA_UV_ARCH "x86_64-unknown-linux-gnu")
            endif()
        elseif(_orca_uv_proc MATCHES "aarch64|arm64|ARM64")
            if(APPLE)
                set(ORCA_UV_ARCH "aarch64-apple-darwin")
            else()
                set(ORCA_UV_ARCH "aarch64-unknown-linux-gnu")
            endif()
        else()
            message(WARNING "Unsupported architecture for auto-downloading uv: ${_orca_uv_proc}")
        endif()

        if(ORCA_UV_ARCH)
            set(ORCA_UV_URL "https://github.com/astral-sh/uv/releases/download/${ORCA_UV_VERSION}/uv-${ORCA_UV_ARCH}.${ORCA_UV_EXT}")
            set(ORCA_UV_ARCHIVE "${ORCA_UV_DOWNLOAD_DIR}/uv-archive.${ORCA_UV_EXT}")
            file(MAKE_DIRECTORY "${ORCA_UV_DOWNLOAD_DIR}")
            message(STATUS "Downloading uv ${ORCA_UV_VERSION} from ${ORCA_UV_URL} ...")
            file(DOWNLOAD "${ORCA_UV_URL}" "${ORCA_UV_ARCHIVE}" STATUS ORCA_UV_DL_STATUS TLS_VERIFY ON)
            list(GET ORCA_UV_DL_STATUS 0 ORCA_UV_DL_CODE)
            list(GET ORCA_UV_DL_STATUS 1 ORCA_UV_DL_MSG)
            if(NOT ORCA_UV_DL_CODE EQUAL 0)
                # Network failure keeps the historical graceful degradation (uv is
                # optional at build time) -- but a hash mismatch below is fatal.
                message(WARNING "Failed to download uv: ${ORCA_UV_DL_MSG}")
                file(REMOVE "${ORCA_UV_ARCHIVE}")
            else()
                file(SHA256 "${ORCA_UV_ARCHIVE}" _orca_uv_actual_sha256)
                if(NOT _orca_uv_actual_sha256 STREQUAL "${ORCA_UV_SHA256_${ORCA_UV_ARCH}}")
                    file(REMOVE "${ORCA_UV_ARCHIVE}")
                    message(FATAL_ERROR
                        "uv archive checksum mismatch for ${ORCA_UV_ARCH} ${ORCA_UV_VERSION}:\n"
                        "  expected ${ORCA_UV_SHA256_${ORCA_UV_ARCH}}\n"
                        "  actual   ${_orca_uv_actual_sha256}\n"
                        "Possible tampering or a stale pin; refusing to bundle.")
                endif()
                message(STATUS "Extracting uv ...")
                file(ARCHIVE_EXTRACT INPUT "${ORCA_UV_ARCHIVE}" DESTINATION "${ORCA_UV_DOWNLOAD_DIR}")
                file(REMOVE "${ORCA_UV_ARCHIVE}")
                # Pinned archives have a fixed layout: the tarballs hold
                # uv-<triple>/uv, the Windows zip holds uv.exe at the root
                # (already at ORCA_UV_BINARY). A layout change implies a new
                # archive, hence a hash bump -- and RENAME fails loudly.
                if(NOT WIN32)
                    file(RENAME "${ORCA_UV_DOWNLOAD_DIR}/uv-${ORCA_UV_ARCH}/uv" "${ORCA_UV_BINARY}")
                    file(REMOVE_RECURSE "${ORCA_UV_DOWNLOAD_DIR}/uv-${ORCA_UV_ARCH}")
                endif()
            endif()
        endif()
    endif()

    if(EXISTS "${ORCA_UV_BINARY}")
        # Plain set (shadows the empty cache entry for this configure run):
        # the version-scoped path re-derives every configure, so a version
        # bump takes effect in warm build dirs without cache surgery.
        set(ORCA_BUNDLED_UV_EXECUTABLE "${ORCA_UV_BINARY}")
    endif()
endif()

# Resolve ORCA_BUNDLED_UV_EXECUTABLE -> ORCA_BUNDLED_UV_EXECUTABLE_CONFIG / ORCA_BUNDLED_UV_FILENAME
# Must run before add_subdirectory(src) so target_compile_definitions sees it.
if(ORCA_BUNDLED_UV_EXECUTABLE)
    if(EXISTS "${ORCA_BUNDLED_UV_EXECUTABLE}")
        file(TO_CMAKE_PATH "${ORCA_BUNDLED_UV_EXECUTABLE}" ORCA_BUNDLED_UV_EXECUTABLE_CONFIG)
        get_filename_component(ORCA_BUNDLED_UV_FILENAME "${ORCA_BUNDLED_UV_EXECUTABLE}" NAME)
    else()
        message(WARNING "ORCA_BUNDLED_UV_EXECUTABLE does not exist: ${ORCA_BUNDLED_UV_EXECUTABLE}")
        set(ORCA_BUNDLED_UV_EXECUTABLE "")
        set(ORCA_BUNDLED_UV_EXECUTABLE_CONFIG "")
        set(ORCA_BUNDLED_UV_FILENAME "")
    endif()
else()
    set(ORCA_BUNDLED_UV_EXECUTABLE_CONFIG "")
    set(ORCA_BUNDLED_UV_FILENAME "")
endif()

# If SLIC3R_FHS is 1 -> SLIC3R_DESKTOP_INTEGRATION is always 0, othrewise variable.
CMAKE_DEPENDENT_OPTION(SLIC3R_DESKTOP_INTEGRATION "Allow performing desktop integration during runtime" 1 "NOT SLIC3R_FHS" 0)

set(OPENVDB_FIND_MODULE_PATH "" CACHE PATH "Path to OpenVDB installation's find modules.")

set(SLIC3R_GTK "3" CACHE STRING "GTK version to use with wxWidgets on Linux")

set(IS_CROSS_COMPILE FALSE)

option (COLORED_OUTPUT "Always produce ANSI-colored output (GNU/Clang only)." OFF)
if (${COLORED_OUTPUT})
    if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
        add_compile_options (-fdiagnostics-color=always)
    elseif ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
        add_compile_options (-fcolor-diagnostics)
    endif ()
endif ()

if (APPLE)
    list(LENGTH CMAKE_OSX_ARCHITECTURES _arch_len)
    if (_arch_len GREATER 1)
        message(FATAL_ERROR "OrcaSlicer only supports building for one architecture at a time. Please make sure only one architecture is specified in CMAKE_OSX_ARCHITECTURES")
    endif ()
    set(CMAKE_FIND_FRAMEWORK LAST)
    set(CMAKE_FIND_APPBUNDLE LAST)
    list(FIND CMAKE_OSX_ARCHITECTURES ${CMAKE_SYSTEM_PROCESSOR} _arch_idx)
    if (CMAKE_OSX_ARCHITECTURES AND _arch_idx LESS 0)
        set(IS_CROSS_COMPILE TRUE)
    endif ()
    if (CMAKE_MACOSX_BUNDLE)
        set(CMAKE_INSTALL_RPATH @executable_path/../Frameworks)
    endif()
    SET(CMAKE_XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.snapmaker.snapmaker-orca")

    message(STATUS "Orca: IS_CROSS_COMPILE: ${IS_CROSS_COMPILE}")
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CMAKE_INSTALL_RPATH "$ORIGIN")
endif ()

# Proposal for C++ unit tests and sandboxes
option(SLIC3R_BUILD_SANDBOXES   "Build development sandboxes" OFF)
option(BUILD_TESTS       "Build unit tests" OFF)
option(ORCA_TOOLS       "Build Orca tools" OFF)

if (FLATPAK)
    set(SLIC3R_FHS ON CACHE BOOL "" FORCE)
    set(SLIC3R_DESKTOP_INTEGRATION OFF CACHE BOOL "" FORCE)
endif ()

if (IS_CROSS_COMPILE)
    message("Detected cross compilation setup. Tests and encoding checks will be forcedly disabled!")
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
endif ()

# Print out the SLIC3R_* cache options
get_cmake_property(_cache_vars CACHE_VARIABLES)
list (SORT _cache_vars)
foreach (_cache_var ${_cache_vars})
    if("${_cache_var}" MATCHES "^SLIC3R_")
        message(STATUS "${_cache_var}: ${${_cache_var}}")
    endif ()
endforeach()

if (SLIC3R_GUI)
    add_definitions(-DSLIC3R_GUI)
endif ()

if(SLIC3R_DESKTOP_INTEGRATION)
    add_definitions(-DSLIC3R_DESKTOP_INTEGRATION)
endif ()

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    message(STATUS "Automatically setting CMAKE_INSTALL_PREFIX")
    set_property(CACHE CMAKE_INSTALL_PREFIX PROPERTY VALUE "${CMAKE_BINARY_DIR}/Snapmaker_Orca")
endif()

message(STATUS "CMAKE_INSTALL_PREFIX: ${CMAKE_INSTALL_PREFIX}")

if (MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL Clang)
    set(IS_CLANG_CL TRUE)

    # clang-cl can interpret SYSTEM header paths if -imsvc is used
    set(CMAKE_INCLUDE_SYSTEM_FLAG_CXX "-imsvc")
else ()
    set(IS_CLANG_CL FALSE)
endif ()

if (MSVC)
    # CMP0092 only applies when the cache is created; an existing tree keeps its /W3,
    # which a silenced bundled target would then override (D9025, once per file).
    string(REGEX REPLACE "/W[0-4]" "" CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}")
    string(REGEX REPLACE "/W[0-4]" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

    # /MP only matters for the VS generators, where CMake turns it into the
    # MultiProcessorCompilation property. Ninja parallelises on its own, and
    # clang-cl warns "argument unused" if the flag reaches it.
    if (SLIC3R_MSVC_COMPILE_PARALLEL AND CMAKE_GENERATOR MATCHES "Visual Studio")
           add_compile_options(/MP)
    endif ()
    # /bigobj (Increase Number of Sections in .Obj file)
    # error C3859: virtual memory range for PCH exceeded; please recompile with a command line option of '-Zm90' or greater
    # Generate symbols at every build target, even for the release.
	# -Zm520 fixes error C3859 but forces the compiler to pre-allocate that memory for every translation unit regardless
	# combining /Zi with /FS frees up a significant amount of memory pressure across all parallel compile jobs and makes /MP faster overall.
    add_compile_options(-bigobj /Zi /FS)
    # Disable STL4007: Many result_type typedefs and all argument_type, first_argument_type, and second_argument_type typedefs are deprecated in C++17.
    #FIXME Remove this line after eigen library adapts to the new C++17 adaptor rules.
    add_compile_options(-D_SILENCE_CXX17_ADAPTOR_TYPEDEFS_DEPRECATION_WARNING)
    # Disable warnings on conversion from unsigned to signed (possible loss of data)
    # C4244: 'conversion' conversion from 'type1' to 'type2', possible loss of data. An integer type is converted to a smaller integer type.
    # C4267: The compiler detected a conversion from size_t to a smaller type.
    add_compile_options(/wd4244 /wd4267)
    # Disable warnings on comparison of unsigned and signed
    # C4018: signed/unsigned mismatch
    add_compile_options(/wd4018)
    # Prevent linker restart when TBB (or other deps) built with /GL are linked
	# Make your full (clean) build slower because it enables link-time code generation across all translation units.
	# helps incremental builds by preventing the double-link restart from TBB
    set(CMAKE_EXE_LINKER_FLAGS    "${CMAKE_EXE_LINKER_FLAGS} /LTCG")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /LTCG")
endif ()

if (${CMAKE_CXX_COMPILER_ID} STREQUAL "AppleClang" AND ${CMAKE_CXX_COMPILER_VERSION} VERSION_GREATER 15)
    add_compile_definitions(BOOST_NO_CXX98_FUNCTION_BASE _HAS_AUTO_PTR_ETC=0)
endif()

if (MINGW)
    add_compile_options(-Wa,-mbig-obj)
endif ()

if (NOT MSVC)
    # ARMs (Raspberry PI) use an unsigned char by default. Let's make it consistent for Snapmaker_Orca on all platforms.
    add_compile_options(-fsigned-char)
endif ()

if ("${DEP_BUILD_DIR}" STREQUAL "")
    get_filename_component(BIN_DIR_NAME ${CMAKE_BINARY_DIR} NAME)
    if (APPLE AND BIN_DIR_NAME STREQUAL "${CMAKE_OSX_ARCHITECTURES}")
        file(RELATIVE_PATH BIN_DIR_NAME ${CMAKE_BINARY_DIR}/../.. ${CMAKE_BINARY_DIR})
    endif ()
    set(DEP_BUILD_DIR "${CMAKE_SOURCE_DIR}/deps/${BIN_DIR_NAME}" CACHE PATH "Path to dependencies build directory" FORCE)
    message(STATUS "DEP_BUILD_DIR: ${DEP_BUILD_DIR} (generated automatically and saved to cache)")
    set(AUTOGENERATED_DEP_BUILD_DIR ${DEP_BUILD_DIR} CACHE PATH "Provides the last autogenerated DEP_BUILD_DIR" FORCE)
else ()
    message(STATUS "DEP_BUILD_DIR: ${DEP_BUILD_DIR} (from cache or command line)")
endif ()

if ("${CMAKE_PREFIX_PATH}" STREQUAL "" OR "${CMAKE_PREFIX_PATH}" STREQUAL "${AUTOGENERATED_PREFIX_PATH}")
    if (DEFINED AUTOGENERATED_DEP_BUILD_DIR AND NOT "${DEP_BUILD_DIR}" STREQUAL "${AUTOGENERATED_DEP_BUILD_DIR}")
        message(STATUS "CMAKE_PREFIX_PATH is being re-generated due to DEP_BUILD_DIR being manually updated")
        set(REGEN_DESTDIR TRUE)
        unset(AUTOGENERATED_DEP_BUILD_DIR CACHE)
    endif ()
else ()
    unset(AUTOGENERATED_PREFIX_PATH CACHE)
endif ()

# Display and check CMAKE_PREFIX_PATH
if ("${CMAKE_PREFIX_PATH}" STREQUAL "" OR REGEN_DESTDIR)
    set(CMAKE_PREFIX_PATH "${DEP_BUILD_DIR}/OrcaSlicer_dep/usr/local" CACHE PATH "Path to dependencies install directory" FORCE)
    message(STATUS "CMAKE_PREFIX_PATH: ${CMAKE_PREFIX_PATH} (generated automatically and saved to cache)")
    set(AUTOGENERATED_PREFIX_PATH ${CMAKE_PREFIX_PATH} CACHE STRING "Provides the last autogenerated CMAKE_PREFIX_PATH" FORCE)
    unset(REGEN_DESTDIR CACHE)
else ()
    message(STATUS "CMAKE_PREFIX_PATH: ${CMAKE_PREFIX_PATH} (from cache or command line)")
endif ()
set(PREFIX_PATH_CHECK ${CMAKE_PREFIX_PATH})

# the CMAKE_PREFIX_PATH environment variable is separate from the CMAKE_PREFIX_PATH cache variable and provides additional paths to search for libraries.
if (NOT "$ENV{CMAKE_PREFIX_PATH}" STREQUAL "")
    message(STATUS "CMAKE_PREFIX_PATH: $ENV{CMAKE_PREFIX_PATH} (from environment)")
    list(APPEND PREFIX_PATH_CHECK $ENV{CMAKE_PREFIX_PATH})
endif ()

# Check all directories in CMAKE_PREFIX_PATH variables
foreach (DIR ${PREFIX_PATH_CHECK})
    if (NOT EXISTS "${DIR}")
        message(WARNING "CMAKE_PREFIX_PATH element doesn't exist: ${DIR}")
    endif ()
endforeach ()

if (APPLE)
    set(CMAKE_MACOSX_RPATH ON CACHE BOOL "")
    set(CMAKE_MACOSX_BUNDLE ON CACHE BOOL "")
endif ()

if (APPLE AND CMAKE_MACOSX_RPATH AND "${CMAKE_INSTALL_RPATH}" STREQUAL "")
    set(CMAKE_INSTALL_RPATH ${CMAKE_PREFIX_PATH})
endif ()

# Add our own cmake module path.
list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake/modules/)
message(STATUS "PROJECT_SOURCE_DIR: ${PROJECT_SOURCE_DIR}")
message(STATUS "CMAKE_MODULE_PATH: ${CMAKE_MODULE_PATH}")

enable_testing ()

# Enable C++17 language standard.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    # Add DEBUG flags to debug builds.
    add_compile_options("$<$<CONFIG:DEBUG>:-DDEBUG>")
endif()

# To be able to link libslic3r with the Perl XS module.
# Once we get rid of Perl and libslic3r is linked statically, we can get rid of -fPIC
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# WIN10SDK_PATH is used to point CMake to the WIN10 SDK installation directory.
# We pick it from environment if it is not defined in another way
# ORCA: Removed Netfabb STL fixing service support in favor of CGAL.
if(WIN32)
    find_package(PkgConfig REQUIRED)
#     if(NOT DEFINED WIN10SDK_PATH)
#         if(DEFINED ENV{WIN10SDK_PATH})
#                 set(WIN10SDK_PATH "$ENV{WIN10SDK_PATH}")
#         endif()
#     endif()
#     if(DEFINED WIN10SDK_PATH)
#         #BBS: modify win10sdk_path
#         if (EXISTS "${WIN10SDK_PATH}/winrt/windows.graphics.printing3d.h")
#             set(WIN10SDK_INCLUDE_PATH "${WIN10SDK_PATH}")
#         else()
#             message("WIN10SDK_PATH is invalid: ${WIN10SDK_PATH}")
#             message("${WIN10SDK_PATH}/winrt/windows.graphics.printing3d.h was not found")
#             message("STL fixing by the Netfabb service will not be compiled")
#             unset(WIN10SDK_PATH)
#         endif()
#     else()
#         # Try to use the default Windows 10 SDK path.
#         if (DEFINED ENV{WindowsSdkDir} AND DEFINED ENV{WindowsSDKVersion})
#             set(WIN10SDK_INCLUDE_PATH "$ENV{WindowsSdkDir}/Include/$ENV{WindowsSDKVersion}")
#         else ()
#             set(WIN10SDK_INCLUDE_PATH "C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0")
#         endif ()
#         if (NOT EXISTS "${WIN10SDK_INCLUDE_PATH}/winrt/windows.graphics.printing3d.h")
#             message("${WIN10SDK_INCLUDE_PATH}/winrt/windows.graphics.printing3d.h was not found")
#             message("STL fixing by the Netfabb service will not be compiled")
#             unset(WIN10SDK_INCLUDE_PATH)
#         endif()
#     endif()
#     if(WIN10SDK_INCLUDE_PATH)
#         message("Building with Win10 Netfabb STL fixing service support")
#         add_definitions(-DHAS_WIN10SDK)
#         include_directories(SYSTEM "${WIN10SDK_INCLUDE_PATH}")
#     else()
#         message("Building without Win10 Netfabb STL fixing service support")
#     endif()
endif()

if (APPLE)
    message("OS X SDK Path: ${CMAKE_OSX_SYSROOT}")
    if (CMAKE_OSX_DEPLOYMENT_TARGET)
        message("OS X Deployment Target: ${CMAKE_OSX_DEPLOYMENT_TARGET}")
    else ()
        message("OS X Deployment Target: (default)")
    endif ()
endif ()

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig REQUIRED)

    # Boost on Raspberry-Pi does not link to pthreads.
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)

    find_package(DBus REQUIRED)
    include_directories(SYSTEM ${DBUS_INCLUDE_DIRS})
endif()

if (CMAKE_COMPILER_IS_GNUCC OR CMAKE_COMPILER_IS_GNUXX)
    # Adding -fext-numeric-literals to enable GCC extensions on definitions of quad float literals, which are required by Boost.
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fext-numeric-literals" )
endif()

if ((NOT MSVC OR IS_CLANG_CL) AND ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU" OR "${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang"))
    if (IS_CLANG_CL)
        # clang-cl reads -Wall as MSVC /Wall, which clang maps to -Weverything. /W4 is
        # its -Wall -Wextra and, unlike /clang:-Wall, is ordered with the -Wno-* below
        # instead of after them. The -Wextra-only warnings are dropped again so the set
        # matches what -Wall gives the GNU/Clang builds.
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W4" )
        add_compile_options(-Wno-unused-parameter -Wno-ignored-qualifiers -Wno-missing-field-initializers)
    elseif (NOT MINGW)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall" )
    endif ()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-reorder" )

    # On GCC and Clang, no return from a non-void function is a warning only. Here, we make it an error.
    add_compile_options(-Werror=return-type)

    # Since some portions of code are just commented out or put under conditional compilation, there are
    # a bunch of warning related to unused functions and variables. Suppress those warnings to not pollute
    # compilers diagnostics output with warnings we not going to look at
    add_compile_options(-Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-label -Wno-unused-local-typedefs)

    # Ignore signed/unsigned comparison warnings
    add_compile_options(-Wno-sign-compare)

    # The mismatch of tabs and spaces throughout the project can sometimes
    # cause this warning to appear even though the indentation is fine.
    # Some includes also cause the warning
    add_compile_options(-Wno-misleading-indentation)

    # Disable warning if enum value does not have a corresponding case in switch statement
    add_compile_options(-Wno-switch)

    # removes LOTS of extraneous Eigen warnings (GCC only supports it since 6.1)
    # https://eigen.tuxfamily.org/bz/show_bug.cgi?id=1221
    if("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang" OR CMAKE_CXX_COMPILER_VERSION VERSION_GREATER 6.0)
        add_compile_options(-Wno-ignored-attributes) # Tamas: Eigen include dirs are marked as SYSTEM
    endif()

    # Clang reports legacy OpenGL calls as deprecated. Turn off the warning for now
    # to reduce the clutter, we know about this one. It should be reenabled after
    # we finally get rid of the deprecated code.
    if("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
        add_compile_options(-Wno-deprecated-declarations)
    endif()

    if((${CMAKE_CXX_COMPILER_ID} STREQUAL "Clang" OR ${CMAKE_CXX_COMPILER_ID} STREQUAL "AppleClang") AND ${CMAKE_CXX_COMPILER_VERSION} VERSION_GREATER 15)
        include(CheckCXXCompilerFlag)
        check_cxx_compiler_flag(-Wno-error=enum-constexpr-conversion HAS_WNO_ERROR_ENUM_CONSTEXPR_CONV)
        if(HAS_WNO_ERROR_ENUM_CONSTEXPR_CONV)
            add_compile_options(-Wno-error=enum-constexpr-conversion)
        endif()
    endif()

    #GCC generates loads of -Wunknown-pragmas when compiling igl. The fix is not easy due to a bug in gcc, see
    # https://gcc.gnu.org/bugzilla/show_bug.cgi?id=66943 or
    # https://gcc.gnu.org/bugzilla/show_bug.cgi?id=53431
    # We will turn the warning of for GCC for now:
    if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
        # GCC generates loads of -Wunknown-pragmas when compiling igl. The fix is not easy due to a bug in gcc, see
        # https://gcc.gnu.org/bugzilla/show_bug.cgi?id=66943 or
        # https://gcc.gnu.org/bugzilla/show_bug.cgi?id=53431
        # We will turn the warning of for GCC for now:
        add_compile_options(-Wno-unknown-pragmas)
    endif()

    # Compress the debug info with zstd to save space in Flatpak CI builds
    if(FLATPAK)
        if(("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER 13.0) OR
           ("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 15.0))
            add_compile_options(-gz=zstd)
        endif()
    endif()

    if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER 14)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-error=template-id-cdtor" )
    endif()

endif()

if (SLIC3R_ASAN)
    # ASAN should be available on MSVC starting with Visual Studio 2019 16.9
    # https://devblogs.microsoft.com/cppblog/address-sanitizer-for-msvc-now-generally-available/
    add_compile_options(-fsanitize=address)

    if (NOT MSVC)
        add_compile_options(-fno-omit-frame-pointer)
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address")
        set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=address")
        set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -fsanitize=address")
    else()
        add_compile_definitions(_DISABLE_STRING_ANNOTATION=1 _DISABLE_VECTOR_ANNOTATION=1)
    endif ()

    if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -lasan")
    endif ()
endif ()

if (APPLE)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Werror=partial-availability -Werror=unguarded-availability -Werror=unguarded-availability-new")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Werror=partial-availability -Werror=unguarded-availability -Werror=unguarded-availability-new")
    
    # Generate debug symbols for Release builds on macOS (required for dSYM generation and Sentry crash reporting)
    # This adds -g flag to produce DWARF debug info that dsymutil can extract
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -g")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -g")
    set(CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO} -g")
    set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} -g")
endif ()

if(MSVC)
# Ignore truncating casts in initializers & constructors
# https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/compiler-warning-level-1-c4305
add_compile_options(/wd4305)
endif()

# Where all the bundled libraries reside?
set(LIBDIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(LIBDIR_BIN ${CMAKE_CURRENT_BINARY_DIR}/src)
message(STATUS "LIBDIR: ${LIBDIR}")
message(STATUS "LIBDIR_BIN: ${LIBDIR_BIN}")

# For the bundled boost libraries (boost::nowide)
include_directories(SYSTEM ${LIBDIR})
# For generated header files
include_directories(SYSTEM ${LIBDIR_BIN}/dev-utils/platform)
# For ligigl
include_directories(SYSTEM ${LIBDIR}/libigl)

if(WIN32)
    add_definitions(-D_USE_MATH_DEFINES -D_WIN32 -D_CRT_SECURE_NO_WARNINGS -D_SCL_SECURE_NO_WARNINGS)
    if(MSVC)
        # BOOST_ALL_NO_LIB: Avoid the automatic linking of Boost libraries on Windows. Rather rely on explicit linking.
        add_definitions(-DBOOST_ALL_NO_LIB -DBOOST_USE_WINAPI_VERSION=0x602 -DBOOST_SYSTEM_USE_UTF8 )
        # Force the source code encoding to UTF-8. See Snapmaker_Orca GH pull request #5583
        add_compile_options("$<$<C_COMPILER_ID:MSVC>:/utf-8>")
        add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
    endif(MSVC)
endif(WIN32)

add_definitions(-DwxUSE_UNICODE -D_UNICODE -DUNICODE -DWXINTL_NO_GETTEXT_MACRO)

# Disable unsafe implicit wxString to const char* / std::string and vice versa. This implicit conversion breaks the UTF-8 encoding quite often.
add_definitions(-DwxNO_UNSAFE_WXSTRING_CONV)

if (SLIC3R_PROFILE)
    message("Snapmaker_Orca will be built with a Shiny invasive profiler")
    add_definitions(-DSLIC3R_PROFILE)
endif ()

# Disable optimization for RelWithDebInfo
if(CMAKE_C_FLAGS_RELWITHDEBINFO MATCHES "/O2")
    string(REGEX REPLACE "/O2" "/Od" CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO}")
endif()

if(CMAKE_CXX_FLAGS_RELWITHDEBINFO MATCHES "/O2")
    string(REGEX REPLACE "/O2" "/Od" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
endif()

if("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
    if(CMAKE_C_FLAGS_RELWITHDEBINFO MATCHES "-O2")
        string(REGEX REPLACE "-O2" "-O0" CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO}")
    endif()

    if(CMAKE_CXX_FLAGS_RELWITHDEBINFO MATCHES "-O2")
        string(REGEX REPLACE "-O2" "-O0" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
    endif()
endif()

if(MSVC)
    if(CMAKE_C_FLAGS_RELWITHDEBINFO MATCHES "/Ob1")
        string(REGEX REPLACE "/Ob1" "/Ob0" CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO}")
    endif()
    if(CMAKE_CXX_FLAGS_RELWITHDEBINFO MATCHES "/Ob1")
        string(REGEX REPLACE "/Ob1" "/Ob0" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
    endif()

    set(CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO} /RTC1")
    set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} /RTC1")
endif()

list(REMOVE_DUPLICATES CMAKE_C_FLAGS_RELWITHDEBINFO)
list(REMOVE_DUPLICATES CMAKE_CXX_FLAGS_RELWITHDEBINFO)

# Find and configure boost
if(SLIC3R_STATIC)
    # Use static boost libraries.
    set(Boost_USE_STATIC_LIBS ON)
    # Use boost libraries linked statically to the C++ runtime.
    # set(Boost_USE_STATIC_RUNTIME ON)
else()
    add_definitions(-DBOOST_LOG_DYN_LINK)
endif()
#set(Boost_DEBUG ON)
# set(Boost_COMPILER "-mgw81")
# boost::process was introduced first in version 1.64.0,
# boost::beast::detail::base64 was introduced first in version 1.66.0
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()
set(Boost_NO_SYSTEM_PATHS TRUE)
find_package(Boost 1.83.0 REQUIRED COMPONENTS system filesystem thread log log_setup locale regex chrono atomic date_time iostreams program_options nowide)

# Find and configure Eigen3
find_package(Eigen3 5.0.1 REQUIRED)

add_library(boost_libs INTERFACE)
add_library(boost_headeronly INTERFACE)

if (APPLE)
    # BOOST_ASIO_DISABLE_KQUEUE : prevents a Boost ASIO bug on OS X: https://svn.boost.org/trac/boost/ticket/5339
    target_compile_definitions(boost_headeronly INTERFACE BOOST_ASIO_DISABLE_KQUEUE)
endif()

if(NOT SLIC3R_STATIC)
    target_compile_definitions(boost_headeronly INTERFACE BOOST_LOG_DYN_LINK)
endif()

function(slic3r_remap_configs targets from_Cfg to_Cfg)
    if(MSVC)
        string(TOUPPER ${from_Cfg} from_CFG)

        foreach(tgt ${targets})
            if(TARGET ${tgt})
                set_target_properties(${tgt} PROPERTIES MAP_IMPORTED_CONFIG_${from_CFG} ${to_Cfg})
            endif()
        endforeach()
    endif()
endfunction()

target_include_directories(boost_headeronly SYSTEM INTERFACE ${Boost_INCLUDE_DIRS})
target_link_libraries(boost_libs INTERFACE boost_headeronly ${Boost_LIBRARIES})

# Find and configure intel-tbb
if(SLIC3R_STATIC)
    set(TBB_STATIC 1)
endif()
set(TBB_DEBUG 1)
set(CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO RelWithDebInfo Release "")
find_package(TBB REQUIRED)
# include_directories(SYSTEM ${TBB_INCLUDE_DIRS})
# add_definitions(${TBB_DEFINITIONS})
# if(MSVC)
#     # Suppress implicit linking of the TBB libraries by the Visual Studio compiler.
#     add_definitions(-D__TBB_NO_IMPLICIT_LINKAGE)
# endif()
# The Intel TBB library will use the std::exception_ptr feature of C++11.
# add_definitions(-DTBB_USE_CAPTURED_EXCEPTION=0)

find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
if (SLIC3R_SENTRY)
    find_package(Sentry QUIET)
    if (NOT Sentry_FOUND)
        message(WARNING "Sentry SDK not found. SLIC3R_SENTRY will be disabled. "
                        "Build deps with Sentry support or use -DSLIC3R_SENTRY=OFF")
        set(SLIC3R_SENTRY OFF CACHE BOOL "Enable Sentry crash reporting SDK" FORCE)
    endif()
endif()
find_package(Freetype REQUIRED)


add_library(libcurl INTERFACE)
target_link_libraries(libcurl INTERFACE CURL::libcurl)

find_package(ZLIB REQUIRED)
target_link_libraries(libcurl INTERFACE ZLIB::ZLIB)

if (SLIC3R_SENTRY)
    add_library(sentry INTERFACE)
    target_link_libraries(sentry INTERFACE sentry::sentry)

    # Add sentry library directory and link sentry.lib explicitly for Windows
    if (WIN32 AND CMAKE_PREFIX_PATH)
        # Try to find sentry.lib in the install prefix
        file(GLOB _sentry_lib "${CMAKE_PREFIX_PATH}/lib/sentry.lib")
        if (_sentry_lib)
            target_link_directories(sentry INTERFACE "${CMAKE_PREFIX_PATH}/lib")
            target_link_libraries(sentry INTERFACE "sentry.lib")
            message(STATUS "Found sentry.lib at: ${CMAKE_PREFIX_PATH}/lib")
        else()
            message(WARNING "sentry.lib not found in ${CMAKE_PREFIX_PATH}/lib. Sentry SDK may not have been built yet.")
        endif()
    	elseif (APPLE AND CMAKE_PREFIX_PATH)
		# Try to find libsentry.dylib in the install prefix for macOS
		file(GLOB _sentry_lib "${CMAKE_PREFIX_PATH}/lib/libsentry.dylib")
		if (_sentry_lib)
			target_link_directories(sentry INTERFACE "${CMAKE_PREFIX_PATH}/lib")
			target_link_libraries(sentry INTERFACE "sentry")
			message(STATUS "Found libsentry.dylib at: ${CMAKE_PREFIX_PATH}/lib")
		else()
			message(WARNING "libsentry.dylib not found in ${CMAKE_PREFIX_PATH}/lib. Sentry SDK may not have been built yet.")
		endif()
	endif()
endif()

# Fixing curl's cmake config script bugs
if (NOT WIN32)
    # Required by libcurl
    #find_package(ZLIB REQUIRED)
    #target_link_libraries(libcurl INTERFACE ZLIB::ZLIB)
    #find_package(Libssh2 REQUIRED)
    #target_link_libraries(libcurl INTERFACE Libssh2::libssh2)
else()
    target_link_libraries(libcurl INTERFACE crypt32)
endif()

if (SLIC3R_STATIC AND NOT SLIC3R_STATIC_EXCLUDE_CURL)
    if (NOT APPLE)
        # libcurl is always linked dynamically to the system libcurl on OSX.
        # On other systems, libcurl is linked statically if SLIC3R_STATIC is set.
        target_compile_definitions(libcurl INTERFACE CURL_STATICLIB)
    endif()
    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # As of now, our build system produces a statically linked libcurl,
        # which links the OpenSSL library dynamically.
        find_package(OpenSSL REQUIRED)
        message("OpenSSL include dir: ${OPENSSL_INCLUDE_DIR}")
        message("OpenSSL libraries: ${OPENSSL_LIBRARIES}")
        target_include_directories(libcurl SYSTEM INTERFACE ${OPENSSL_INCLUDE_DIR})
        target_link_libraries(libcurl INTERFACE ${OPENSSL_LIBRARIES})
    endif()
endif()

## OPTIONAL packages

# Find expat or use bundled version
# Always use the system libexpat on Linux.

find_package(EXPAT)

if (NOT EXPAT_FOUND)
    # deps_src/expat provides the expat target with XML_LARGE_SIZE enabled.
    # Just set the variables needed by consumers.
    set(EXPAT_FOUND 1)
    set(EXPAT_INCLUDE_DIRS ${LIBDIR}/expat/)
    set(EXPAT_LIBRARIES expat)
endif ()

find_package(PNG REQUIRED)

set(OpenGL_GL_PREFERENCE "LEGACY")
find_package(OpenGL REQUIRED)

if(APPLE AND CMAKE_VERSION VERSION_GREATER_EQUAL "4.0")
    set(OPENGL_LIBRARIES "-framework OpenGL" CACHE STRING "OpenGL framework" FORCE)
endif()

find_package(glfw3 REQUIRED)

# Find the Cereal serialization library
find_package(cereal REQUIRED)
if (NOT TARGET cereal::cereal)
    set_target_properties(cereal PROPERTIES IMPORTED_GLOBAL TRUE)
    add_library(cereal::cereal ALIAS cereal)
else ()
    set_target_properties(cereal::cereal PROPERTIES IMPORTED_GLOBAL TRUE)
endif ()

# l10n
set(L10N_DIR "${SLIC3R_RESOURCES_DIR}/i18n")
set(BBL_L18N_DIR "${CMAKE_CURRENT_SOURCE_DIR}/localization/i18n")
add_custom_target(gettext_make_pot
    COMMAND xgettext --keyword=L --keyword=_L --keyword=_u8L --keyword=L_CONTEXT:1,2c --keyword=_L_CONTEXT:1,2c --keyword=_u8L_CONTEXT:1,2c --keyword=_L_PLURAL:1,2 --add-comments=TRN --from-code=UTF-8 --no-location --debug --boost
        -f "${BBL_L18N_DIR}/list.txt"
        -o "${BBL_L18N_DIR}/Snapmaker_Orca.pot"
    COMMAND hintsToPot ${SLIC3R_RESOURCES_DIR} ${BBL_L18N_DIR}
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Generate pot file from strings in the source tree"
)
add_custom_target(gettext_merge_po_with_pot
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Merge localization po with new generated pot file"
)
file(GLOB BBL_L10N_PO_FILES "${BBL_L18N_DIR}/*/Snapmaker_Orca*.po")
foreach(po_file ${BBL_L10N_PO_FILES})
    GET_FILENAME_COMPONENT(po_dir "${po_file}" DIRECTORY)
    SET(po_new_file "${po_dir}/Snapmaker_Orca_.po")
    add_custom_command(
        TARGET gettext_merge_po_with_pot PRE_BUILD
        COMMAND msgmerge -N -o ${po_file} ${po_file} "${BBL_L18N_DIR}/Snapmaker_Orca.pot"
    )
endforeach()
add_custom_target(gettext_po_to_mo
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Generate localization po files (binary) from mo files (texts)"
)
file(GLOB L10N_PO_FILES "${BBL_L18N_DIR}/*/Snapmaker_Orca*.po")
foreach(po_file ${BBL_L10N_PO_FILES})
    GET_FILENAME_COMPONENT(SECOND_FOLDER_ABSOLUTE ${po_file} DIRECTORY)
    string(REGEX REPLACE ".*/(.*)" "\\1" po_dir "${SECOND_FOLDER_ABSOLUTE}" )
    SET(mo_file "${L10N_DIR}/${po_dir}/Snapmaker_Orca.mo")
    add_custom_command(
        TARGET gettext_po_to_mo PRE_BUILD
        COMMAND msgfmt ARGS --check-format -o ${mo_file} ${po_file}
        #COMMAND msgfmt ARGS --check-compatibility -o ${mo_file} ${po_file}
    )
endforeach()

find_package(NLopt 1.4 REQUIRED)

# Use bundled Python from deps instead of system Python
set(_bundled_python_version "3.12.13")
set(_bundled_python_abi "312")
string(REGEX REPLACE "^([0-9]+\\.[0-9]+)\\..*$" "\\1" _bundled_python_version_short "${_bundled_python_version}")
set(_bundled_python_root "${CMAKE_PREFIX_PATH}/libpython")
set(Python3_ROOT_DIR "${_bundled_python_root}" CACHE PATH "Root directory for bundled Python" FORCE)
set(Python3_USE_STATIC_LIBS OFF)
set(Python3_FIND_STRATEGY LOCATION)
set(Python3_FIND_IMPLEMENTATIONS CPython)

if(WIN32)
    set(_bundled_python_executable "${_bundled_python_root}/python.exe")
    set(_bundled_python_library "${_bundled_python_root}/libs/python${_bundled_python_abi}.lib")
    if(EXISTS "${_bundled_python_root}/python_d.exe")
        set(_bundled_python_executable "${_bundled_python_root}/python_d.exe")
    endif()
    if(EXISTS "${_bundled_python_root}/libs/python${_bundled_python_abi}_d.lib")
        set(_bundled_python_library "${_bundled_python_root}/libs/python${_bundled_python_abi}_d.lib")
    endif()

    set(Python3_FIND_REGISTRY NEVER)
    set(Python3_EXECUTABLE "${_bundled_python_executable}" CACHE FILEPATH "Bundled Python executable" FORCE)
    set(Python3_INCLUDE_DIR "${_bundled_python_root}/include" CACHE PATH "Bundled Python include directory" FORCE)
    set(Python3_LIBRARY "${_bundled_python_library}" CACHE FILEPATH "Bundled Python embed import library" FORCE)
elseif(APPLE)
    set(Python3_FIND_FRAMEWORK NEVER)

    find_program(_bundled_python_executable
        NAMES python${_bundled_python_version_short} python3
        PATHS "${_bundled_python_root}/bin"
        NO_DEFAULT_PATH
    )
    find_path(_bundled_python_include_dir
        NAMES Python.h
        PATHS
            "${_bundled_python_root}/include/python${_bundled_python_version_short}"
            "${_bundled_python_root}/include"
        NO_DEFAULT_PATH
    )
    find_library(_bundled_python_library
        NAMES python${_bundled_python_version_short} libpython${_bundled_python_version_short}
        PATHS "${_bundled_python_root}/lib"
        NO_DEFAULT_PATH
    )

    if(NOT _bundled_python_executable)
        message(FATAL_ERROR "Bundled Python executable not found under ${_bundled_python_root}/bin")
    endif()
    if(NOT _bundled_python_include_dir)
        message(FATAL_ERROR "Bundled Python headers not found under ${_bundled_python_root}/include")
    endif()
    if(NOT _bundled_python_library)
        message(FATAL_ERROR "Bundled Python library not found under ${_bundled_python_root}/lib")
    endif()

    set(Python3_EXECUTABLE "${_bundled_python_executable}" CACHE FILEPATH "Bundled Python executable" FORCE)
    set(Python3_INCLUDE_DIR "${_bundled_python_include_dir}" CACHE PATH "Bundled Python include directory" FORCE)
    set(Python3_LIBRARY "${_bundled_python_library}" CACHE FILEPATH "Bundled Python embed library" FORCE)
endif()

find_package(Python3 ${_bundled_python_version} EXACT REQUIRED
    COMPONENTS Interpreter Development.Embed
)

# Provide a minimal pybind11::embed target sourced from deps_src/pybind11 headers.
set(PYBIND11_SOURCE_DIR "${CMAKE_SOURCE_DIR}/deps_src/pybind11")
if(NOT EXISTS "${PYBIND11_SOURCE_DIR}/include/pybind11/pybind11.h")
    message(FATAL_ERROR "pybind11 headers not found in ${PYBIND11_SOURCE_DIR}. Did you initialize submodules?")
endif()

add_library(pybind11_headers INTERFACE)
target_include_directories(pybind11_headers INTERFACE "${PYBIND11_SOURCE_DIR}/include")
add_library(pybind11::headers ALIAS pybind11_headers)
add_library(pybind11::pybind11 ALIAS pybind11_headers)

add_library(pybind11_embed INTERFACE)
target_link_libraries(pybind11_embed INTERFACE pybind11_headers Python3::Python)
target_compile_definitions(pybind11_embed INTERFACE PYBIND11_SIMPLE_GIL_MANAGEMENT)
add_library(pybind11::embed ALIAS pybind11_embed)


if(SLIC3R_STATIC)
    set(OPENVDB_USE_STATIC_LIBS ON)
    set(USE_BLOSC TRUE)
endif ()

find_package(OpenVDB 5.0 COMPONENTS openvdb)
if(OpenVDB_FOUND)
    slic3r_remap_configs(IlmBase::Half RelWithDebInfo Release)
    slic3r_remap_configs(Blosc::blosc RelWithDebInfo Release)
else ()
    message(FATAL_ERROR "OpenVDB could not be found with the bundled find module. "
                   "You can try to specify the find module location of your "
                   "OpenVDB installation with the OPENVDB_FIND_MODULE_PATH cache variable.")
endif ()

find_path(SPNAV_INCLUDE_DIR spnav.h)
if (SPNAV_INCLUDE_DIR)
    find_library(SPNAV_LIB NAMES libspnav.a) # Force linking libspnav statically
    if (SPNAV_LIB)
        add_definitions(-DHAVE_SPNAV)
        message(STATUS "SPNAV library found")
    else()
        message(STATUS "SPNAV library NOT found, Spacenavd not supported")
    endif()
else()
    message(STATUS "SPNAV library NOT found, Spacenavd not supported")
endif()

set(TOP_LEVEL_PROJECT_DIR ${PROJECT_SOURCE_DIR})
function(Snapmaker_Orca_copy_dlls target config postfix output_dlls)
    if ("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "AMD64")
        set(_arch "x64")
    elseif ("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "X86")
        set(_arch "x86")
    elseif ("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "ARM64")
        set(_arch "arm64")
    else ()
        message(FATAL_ERROR "Unable to detect architecture: ${CMAKE_SYSTEM_PROCESSOR}")
    endif ()

    get_property(_is_multi GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    get_target_property(_alt_out_dir ${target} RUNTIME_OUTPUT_DIRECTORY)

    if (_alt_out_dir)
        set (_out_dir "${_alt_out_dir}")
        message ("set out_dir to _alt_out_dir: ${_out_dir}")
    elseif (_is_multi)
        set(_out_dir "${CMAKE_CURRENT_BINARY_DIR}/${config}")
        message ("set out_dir to CMAKE_CURRENT_BINARY_DIR/config: ${_out_dir}")
    else ()
        set(_out_dir "${CMAKE_CURRENT_BINARY_DIR}")
        message ("set out_dir to CMAKE_CURRENT_BINARY_DIR: ${_out_dir}")
    endif ()

    file(COPY ${TOP_LEVEL_PROJECT_DIR}/deps/GMP/gmp/lib/win-${_arch}/libgmp-10.dll
            ${TOP_LEVEL_PROJECT_DIR}/deps/MPFR/mpfr/lib/win-${_arch}/libmpfr-4.dll
            ${TOP_LEVEL_PROJECT_DIR}/deps/WebView2/lib/win-${_arch}/WebView2Loader.dll
        DESTINATION ${_out_dir})

    # Copy Sentry and Crashpad artifacts if they were built by deps
    if (SLIC3R_SENTRY AND EXISTS "${CMAKE_PREFIX_PATH}/bin/sentry.dll")
        file(COPY "${CMAKE_PREFIX_PATH}/bin/sentry.dll"
                  "${CMAKE_PREFIX_PATH}/bin/crashpad_handler.exe"
            DESTINATION ${_out_dir}
        )
    endif()

    file(COPY ${CMAKE_PREFIX_PATH}/bin/occt/TKBO.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKBRep.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKCAF.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKCDF.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKernel.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKG2d.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKG3d.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKGeomAlgo.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKGeomBase.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKHLR.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKLCAF.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKMath.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKMesh.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKPrim.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKService.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKShHealing.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKSTEP.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKSTEP209.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKSTEPAttr.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKSTEPBase.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKTopAlgo.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKV3d.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKVCAF.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKXCAF.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKXDESTEP.dll
            ${CMAKE_PREFIX_PATH}/bin/occt/TKXSBase.dll
            ${CMAKE_PREFIX_PATH}/bin/freetype.dll
            ${CMAKE_PREFIX_PATH}/bin/avcodec-61.dll
            ${CMAKE_PREFIX_PATH}/bin/swresample-5.dll
            ${CMAKE_PREFIX_PATH}/bin/swscale-8.dll
            ${CMAKE_PREFIX_PATH}/bin/avutil-59.dll
         DESTINATION ${_out_dir})

    # Build the DLL list in a local variable first
    set(_dll_list
        ${_out_dir}/libgmp-10.dll
        ${_out_dir}/libmpfr-4.dll
        ${_out_dir}/WebView2Loader.dll

        ${_out_dir}/TKBO.dll
        ${_out_dir}/TKBRep.dll
        ${_out_dir}/TKCAF.dll
        ${_out_dir}/TKCDF.dll
        ${_out_dir}/TKernel.dll
        ${_out_dir}/TKG2d.dll
        ${_out_dir}/TKG3d.dll
        ${_out_dir}/TKGeomAlgo.dll
        ${_out_dir}/TKGeomBase.dll
        ${_out_dir}/TKHLR.dll
        ${_out_dir}/TKLCAF.dll
        ${_out_dir}/TKMath.dll
        ${_out_dir}/TKMesh.dll
        ${_out_dir}/TKPrim.dll
        ${_out_dir}/TKService.dll
        ${_out_dir}/TKShHealing.dll
        ${_out_dir}/TKSTEP.dll
        ${_out_dir}/TKSTEP209.dll
        ${_out_dir}/TKSTEPAttr.dll
        ${_out_dir}/TKSTEPBase.dll
        ${_out_dir}/TKTopAlgo.dll
        ${_out_dir}/TKV3d.dll
        ${_out_dir}/TKVCAF.dll
        ${_out_dir}/TKXCAF.dll
        ${_out_dir}/TKXDESTEP.dll
        ${_out_dir}/TKXSBase.dll

        ${_out_dir}/freetype.dll
        ${_out_dir}/avcodec-61.dll
        ${_out_dir}/swresample-5.dll
        ${_out_dir}/swscale-8.dll
        ${_out_dir}/avutil-59.dll
    )

    # Add Sentry DLLs to the list if Sentry is enabled
    if (SLIC3R_SENTRY)
        list(APPEND _dll_list
            ${_out_dir}/sentry.dll
            ${_out_dir}/crashpad_handler.exe
        )
    endif()

    # Set the output variable in parent scope (only once, with complete list)
    set(${output_dlls} ${_dll_list} PARENT_SCOPE)

endfunction()

function(orcaslicer_copy_sos target config postfix output_sos)

    get_property(_is_multi GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    get_target_property(_alt_out_dir ${target} RUNTIME_OUTPUT_DIRECTORY)

    if (_alt_out_dir)
        set(_out_dir "${_alt_out_dir}")
    elseif (_is_multi)
        set(_out_dir "${CMAKE_CURRENT_BINARY_DIR}/${config}")
    else ()
        set(_out_dir "${CMAKE_CURRENT_BINARY_DIR}")
    endif ()

    file(COPY ${CMAKE_PREFIX_PATH}/lib/libavcodec.so
            ${CMAKE_PREFIX_PATH}/lib/libavcodec.so.61
            ${CMAKE_PREFIX_PATH}/lib/libavcodec.so.61.3.100
            ${CMAKE_PREFIX_PATH}/lib/libavutil.so
            ${CMAKE_PREFIX_PATH}/lib/libavutil.so.59
            ${CMAKE_PREFIX_PATH}/lib/libavutil.so.59.8.100
            ${CMAKE_PREFIX_PATH}/lib/libswscale.so
            ${CMAKE_PREFIX_PATH}/lib/libswscale.so.8
            ${CMAKE_PREFIX_PATH}/lib/libswscale.so.8.1.100
            ${CMAKE_PREFIX_PATH}/lib/libswresample.so
            ${CMAKE_PREFIX_PATH}/lib/libswresample.so.5
            ${CMAKE_PREFIX_PATH}/lib/libswresample.so.5.1.100
         DESTINATION ${_out_dir})

    set(${output_sos}
        ${_out_dir}/libavcodec.so
        ${_out_dir}/libavcodec.so.61
        ${_out_dir}/libavcodec.so.61.3.100
        ${_out_dir}/libavutil.so
        ${_out_dir}/libavutil.so.59
        ${_out_dir}/libavutil.so.59.8.100
        ${_out_dir}/libswscale.so
        ${_out_dir}/libswscale.so.8
        ${_out_dir}/libswscale.so.8.1.100
        ${_out_dir}/libswresample.so
        ${_out_dir}/libswresample.so.5
        ${_out_dir}/libswresample.so.5.1.100
        PARENT_SCOPE
    )
endfunction()

# Bundled sources set their own warning flags, and a plain -Wall there means /Wall
# (= -Weverything) under clang-cl. Target options are applied after the ones a target
# set on itself, so these win. Targets are discovered rather than listed so a newly
# bundled library needs no maintenance here.
function(orcaslicer_silence_third_party_warnings _dir)
    get_property(_subdirs DIRECTORY "${_dir}" PROPERTY SUBDIRECTORIES)
    foreach (_subdir IN LISTS _subdirs)
        orcaslicer_silence_third_party_warnings("${_subdir}")
    endforeach ()
    get_property(_targets DIRECTORY "${_dir}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach (_target IN LISTS _targets)
        get_target_property(_type ${_target} TYPE)
        if (NOT _type STREQUAL "INTERFACE_LIBRARY" AND NOT _type STREQUAL "UTILITY")
            if (MSVC AND NOT IS_CLANG_CL)
                # Drop any level the target set for itself, or -w overrides it and cl
                # reports D9025 once per file.
                get_target_property(_opts ${_target} COMPILE_OPTIONS)
                if (_opts)
                    string(REGEX REPLACE "/W[0-4]|/Wall" "" _opts "${_opts}")
                    string(REGEX REPLACE ";;+" ";" _opts "${_opts}")
                    set_target_properties(${_target} PROPERTIES COMPILE_OPTIONS "${_opts}")
                endif ()
                # CMake maps a level into the VS generator's WarningLevel element, while a
                # bare -w stays on the command line and trips D9025 there, once per file.
                target_compile_options(${_target} PRIVATE /W0)
            else ()
                target_compile_options(${_target} PRIVATE -w)
            endif ()
        endif ()
    endforeach ()
endfunction()


# libslic3r, Snapmaker_Orca GUI and the Snapmaker_Orca executable.
add_subdirectory(deps_src)

if (NOT SLIC3R_BUNDLED_WARNINGS)
    orcaslicer_silence_third_party_warnings("${CMAKE_CURRENT_SOURCE_DIR}/deps_src")
endif ()

# Warning level for the targets added below: our sources, plus glad and libvgcode,
# which are vendored but live under src/. The deps_src libraries were configured just
# above. CMP0092 left MSVC without a default level, so it is set here.
if (NOT SLIC3R_WARNINGS)
    add_compile_options(-w)
elseif (MSVC AND NOT IS_CLANG_CL)
    # /we4715 is C4715, no return from a non-void function, matching the
    # -Werror=return-type the GNU/Clang builds apply.
    add_compile_options(/W3 /we4715)
endif ()

add_subdirectory(src)
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT Snapmaker_Orca_app_gui)

add_dependencies(gettext_make_pot hintsToPot)

if(SLIC3R_BUILD_SANDBOXES)
    add_subdirectory(sandboxes)
endif()

if(BUILD_TESTS)
    add_subdirectory(tests)
    if (NOT SLIC3R_BUNDLED_WARNINGS)
        # Catch2 is vendored under tests/ and sets its own warning flags too.
        orcaslicer_silence_third_party_warnings("${CMAKE_CURRENT_SOURCE_DIR}/tests/catch2")
    endif ()
endif()

if (NOT WIN32 AND NOT APPLE)
    set(SLIC3R_APP_CMD "snapmaker-orca")
    configure_file(${LIBDIR}/dev-utils/platform/unix/build_appimage.sh.in ${CMAKE_CURRENT_BINARY_DIR}/build_appimage.sh USE_SOURCE_PERMISSIONS @ONLY)
endif()

# Resources install target, configure fhs.hpp on UNIX
if (WIN32)
    install(DIRECTORY "${SLIC3R_RESOURCES_DIR}/" DESTINATION "./resources")
    if(ORCA_BUNDLED_UV_EXECUTABLE)
        install(PROGRAMS "${ORCA_BUNDLED_UV_EXECUTABLE}" DESTINATION "./resources/tools/uv" RENAME "${ORCA_BUNDLED_UV_FILENAME}")
    endif()
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
    include(InstallRequiredSystemLibraries)
    install (PROGRAMS ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} DESTINATION ".")
elseif (SLIC3R_FHS)
    # CMAKE_INSTALL_FULL_DATAROOTDIR: read-only architecture-independent data root (share)
    set(SLIC3R_FHS_RESOURCES "${CMAKE_INSTALL_FULL_DATAROOTDIR}/Snapmaker_Orca")
    install(DIRECTORY ${SLIC3R_RESOURCES_DIR}/ DESTINATION ${SLIC3R_FHS_RESOURCES}
        PATTERN "*/udev" EXCLUDE
    )
    if(ORCA_BUNDLED_UV_EXECUTABLE)
        install(PROGRAMS "${ORCA_BUNDLED_UV_EXECUTABLE}" DESTINATION "${SLIC3R_FHS_RESOURCES}/tools/uv" RENAME "${ORCA_BUNDLED_UV_FILENAME}")
    endif()
    install(FILES src/dev-utils/platform/unix/Snapmaker_Orca.desktop DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
    foreach(SIZE 32 128 192)
        install(FILES ${SLIC3R_RESOURCES_DIR}/images/Snapmaker_Orca_${SIZE}px.png
            DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/${SIZE}x${SIZE}/apps RENAME Snapmaker_Orca.png
        )
    endforeach()
elseif (CMAKE_MACOSX_BUNDLE)
    # install(DIRECTORY "${SLIC3R_RESOURCES_DIR}/" DESTINATION "${CMAKE_INSTALL_PREFIX}/Snapmaker Orca.app/Contents/resources")
    if(ORCA_BUNDLED_UV_EXECUTABLE)
        install(PROGRAMS "${ORCA_BUNDLED_UV_EXECUTABLE}" DESTINATION "${CMAKE_INSTALL_PREFIX}/Snapmaker Orca.app/Contents/Resources/tools/uv" RENAME "${ORCA_BUNDLED_UV_FILENAME}")
    endif()
else ()
    install(FILES src/dev-utils/platform/unix/Snapmaker_Orca.desktop DESTINATION ${CMAKE_INSTALL_PREFIX}/resources/applications)
    install(DIRECTORY "${SLIC3R_RESOURCES_DIR}/" DESTINATION "${CMAKE_INSTALL_PREFIX}/resources")
    if(ORCA_BUNDLED_UV_EXECUTABLE)
        install(PROGRAMS "${ORCA_BUNDLED_UV_EXECUTABLE}" DESTINATION "${CMAKE_INSTALL_PREFIX}/resources/tools/uv" RENAME "${ORCA_BUNDLED_UV_FILENAME}")
    endif()
endif ()

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(LIBRARY_FILES
    ${LIBDIR_BIN}/libavcodec.so.61
    ${LIBDIR_BIN}/libavcodec.so.61.3.100
    ${LIBDIR_BIN}/libavutil.so.59
    ${LIBDIR_BIN}/libavutil.so.59.8.100
    ${LIBDIR_BIN}/libswresample.so.5
    ${LIBDIR_BIN}/libswresample.so.5.1.100
    ${LIBDIR_BIN}/libswscale.so.8
    ${LIBDIR_BIN}/libswscale.so.8.1.100
    )
    install(FILES ${LIBRARY_FILES} DESTINATION "${CMAKE_INSTALL_PREFIX}/bin")
endif ()

install(FILES ${CMAKE_SOURCE_DIR}/LICENSE.txt DESTINATION ".")
configure_file(${LIBDIR}/dev-utils/platform/unix/fhs.hpp.in ${LIBDIR_BIN}/dev-utils/platform/unix/fhs.hpp)

set (CPACK_PACKAGE_NAME "Snapmaker_Orca")
set (CPACK_PACKAGE_VENDOR "Snapmaker")
set (CPACK_PACKAGE_VERSION_MAJOR "${ORCA_VERSION_MAJOR}")
set (CPACK_PACKAGE_VERSION_MINOR "${ORCA_VERSION_MINOR}")
set (CPACK_PACKAGE_VERSION_PATCH "${ORCA_VERSION_PATCH}")
set (CPACK_PACKAGE_FILE_NAME "Snapmaker_Orca_Windows_Installer_V${Snapmaker_VERSION}")
# NOTE: Orca upstream appends an "_x64"/"_arm64" arch suffix here. Snapmaker's
# release workflow (.github/workflows/build_orca.yml) publishes a single x64
# Windows installer and refers to it by the unsuffixed name, so the suffix is
# intentionally not applied. Re-enable it here together with the workflow if a
# Windows ARM64 build is ever added.
set (CPACK_PACKAGE_DESCRIPTION_SUMMARY "Snapmaker Orca is an open source slicer for FDM printers")
set (CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/Snapmaker/OrcaSlicer")
set (CPACK_PACKAGE_INSTALL_DIRECTORY ${CPACK_PACKAGE_NAME})
set (CPACK_PACKAGE_ICON "${CMAKE_SOURCE_DIR}/resources/images\\\\Snapmaker_Orca.ico")
set (CPACK_NSIS_MUI_ICON "${CPACK_PACKAGE_ICON}")
set (CPACK_NSIS_MUI_UNIICON "${CPACK_PACKAGE_ICON}")
set (CPACK_NSIS_INSTALLED_ICON_NAME "$INSTDIR\\\\snapmaker-orca.exe")
set(CPACK_NSIS_UNINSTALL_NAME "Uninstall")
# Preinstall guard in one place:
# 1) Block while app is running.
# 2) If old version exists, uninstall it with clear failure reason.
# We keep this in EXTRA_PREINSTALL because CPack's default .onInit uninstall prompt runs before section logic.
set(CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS [=[
SnapmakerOrcaPre_check_loop:
  ExecWait 'cmd.exe /c tasklist /FI "IMAGENAME eq snapmaker-orca.exe" 2>nul | find /i "snapmaker-orca.exe" >nul' $0
  IntCmp $0 0 SnapmakerOrcaPre_in_use SnapmakerOrcaPre_try_legacy SnapmakerOrcaPre_try_legacy
SnapmakerOrcaPre_try_legacy:
  ExecWait 'cmd.exe /c tasklist /FI "IMAGENAME eq Snapmaker_Orca.exe" 2>nul | find /i "Snapmaker_Orca.exe" >nul' $0
  IntCmp $0 0 SnapmakerOrcaPre_in_use SnapmakerOrcaPre_idle SnapmakerOrcaPre_idle
SnapmakerOrcaPre_idle:
  Goto SnapmakerOrcaPre_done
SnapmakerOrcaPre_in_use:
  IfSilent SnapmakerOrcaPre_silent SnapmakerOrcaPre_prompt
SnapmakerOrcaPre_silent:
  SetErrorLevel 7
  Quit
SnapmakerOrcaPre_prompt:
  MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "Snapmaker Orca is still running.$\nClose the program, then click Retry to continue, or Cancel to exit the installer." IDRETRY SnapmakerOrcaPre_check_loop
  Abort
SnapmakerOrcaPre_done:
  ReadRegStr $0 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Snapmaker_Orca" "UninstallString"
  StrCmp $0 "" SnapmakerOrcaPre_read_hkcu_uninst SnapmakerOrcaPre_has_uninst
SnapmakerOrcaPre_read_hkcu_uninst:
  ReadRegStr $0 HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Snapmaker_Orca" "UninstallString"
SnapmakerOrcaPre_has_uninst:
  StrCmp $0 "" SnapmakerOrcaPre_finish

  MessageBox MB_YESNOCANCEL|MB_ICONEXCLAMATION "Snapmaker_Orca is already installed.$\n$\nDo you want to uninstall the old version before installing the new one?" /SD IDYES IDYES SnapmakerOrcaPre_uninst IDNO SnapmakerOrcaPre_finish
  Abort

SnapmakerOrcaPre_uninst:
  ClearErrors
  StrCpy $2 $0 1 0
  StrCmp $2 "$\"" SnapmakerOrcaPre_trim_first_quote SnapmakerOrcaPre_check_last_quote
SnapmakerOrcaPre_trim_first_quote:
  StrCpy $0 $0 "" 1
SnapmakerOrcaPre_check_last_quote:
  StrCpy $2 $0 1 -1
  StrCmp $2 "$\"" SnapmakerOrcaPre_trim_last_quote SnapmakerOrcaPre_have_uninst_path
SnapmakerOrcaPre_trim_last_quote:
  StrCpy $0 $0 -1
SnapmakerOrcaPre_have_uninst_path:

  StrLen $2 "Uninstall.exe"
  StrCpy $3 $0 -$2
  ; $3 ends with "\" (e.g. ...\Snapmaker_Orca\). _?="...\" breaks Windows cmdline parsing
  ; (\" escapes the closing quote), truncating _?= at the first space — strip trailing backslashes.
SnapmakerOrcaPre_trim_inst_slash:
  StrCpy $2 $3 1 -1
  StrCmp $2 "\" SnapmakerOrcaPre_do_trim_slash SnapmakerOrcaPre_trim_slash_done
SnapmakerOrcaPre_do_trim_slash:
  StrCpy $3 $3 -1
  Goto SnapmakerOrcaPre_trim_inst_slash
SnapmakerOrcaPre_trim_slash_done:
  IfFileExists "$0" 0 SnapmakerOrcaPre_uninst_launch_failed
  ; Same as CMake CPack NSIS.template.in and NSIS maintainer guidance: use _?=$DIR with NO
  ; extra quotes around the directory. _?="C:\Program Files\..." breaks parsing (log shows
  ; _?="C:\Program Fil...) and causes uninstall failure. See NSIS forums t-212468.
  ExecWait '"$0" /S _?=$3' $1
  IfErrors SnapmakerOrcaPre_uninst_launch_failed 0
  IntCmp $1 0 SnapmakerOrcaPre_finish SnapmakerOrcaPre_uninst_failed SnapmakerOrcaPre_uninst_failed

SnapmakerOrcaPre_uninst_failed:
  MessageBox MB_OK|MB_ICONSTOP "Uninstall failed.$\nUninstaller path: $0$\nWorking dir: $3$\nExit code: $1"
  Abort

SnapmakerOrcaPre_uninst_launch_failed:
  MessageBox MB_OK|MB_ICONSTOP "Failed to start uninstaller process.$\nUninstaller path: $0$\nWorking dir: $3$\n$\nSystem error: the uninstall command could not be executed by Windows (IfErrors set).$\nPossible causes: missing/corrupted uninstaller, access denied, or invalid install directory."
  Abort

SnapmakerOrcaPre_finish:
]=])
file(READ "${CMAKE_SOURCE_DIR}/cmake/nsis/SnapmakerURLProtocols_install.nsh" CPACK_NSIS_EXTRA_INSTALL_COMMANDS)
file(READ "${CMAKE_SOURCE_DIR}/cmake/nsis/SnapmakerURLProtocols_uninstall.nsh" CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS)
# Desktop shortcut inside MUI_STARTMENU_WRITE_BEGIN: respects "do not create shortcuts" on the start menu page.
set(CPACK_NSIS_CREATE_ICONS_EXTRA [=[
  SetShellVarContext current
  CreateShortCut "$DESKTOP\Snapmaker Orca.lnk" "$INSTDIR\snapmaker-orca.exe" "" "$INSTDIR\snapmaker-orca.exe" 0
  SetShellVarContext all
]=])
set(CPACK_NSIS_DELETE_ICONS_EXTRA [=[
  SetShellVarContext current
  Delete "$DESKTOP\Snapmaker Orca.lnk"
  SetShellVarContext all
]=])
set (CPACK_PACKAGE_CHECKSUM SHA256)
set (CPACK_PACKAGE_INSTALL_REGISTRY_KEY "Snapmaker_Orca")
set (CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL OFF)
set (CPACK_NSIS_EXECUTABLES_DIRECTORY ".")
set(CPACK_PACKAGE_EXECUTABLES "snapmaker-orca;Snapmaker Orca")
set (CPACK_RESOURCE_FILE_LICENSE ${CMAKE_SOURCE_DIR}/LICENSE.txt) # must also include in install command

set(CPACK_WIX_UPGRADE_GUID "058245e8-20e0-4a95-9ab7-1acfe17ad511")
set(CPACK_GENERATOR NSIS)
set(CPACK_VERBATIM_VARIABLES ON)
# Strip CPack "Install Options" page; shortcuts follow the start-menu checkbox only.
if (WIN32)
    file(READ "${CMAKE_ROOT}/Modules/Internal/CPack/NSIS.template.in" _snapmaker_nsis_tpl)
    string(REPLACE "\r\n" "\n" _snapmaker_nsis_tpl "${_snapmaker_nsis_tpl}")
    string(REPLACE "\r" "\n" _snapmaker_nsis_tpl "${_snapmaker_nsis_tpl}")
    set(_snapmaker_nsis_old_registry [=[
  ;Read a value from an InstallOptions INI file
  !insertmacro MUI_INSTALLOPTIONS_READ $DO_NOT_ADD_TO_PATH "NSIS.InstallOptions.ini" "Field 2" "State"
  !insertmacro MUI_INSTALLOPTIONS_READ $ADD_TO_PATH_ALL_USERS "NSIS.InstallOptions.ini" "Field 3" "State"
  !insertmacro MUI_INSTALLOPTIONS_READ $ADD_TO_PATH_CURRENT_USER "NSIS.InstallOptions.ini" "Field 4" "State"

  ; Write special uninstall registry entries
  Push "StartMenu"
  Push "$STARTMENU_FOLDER"
  Call ConditionalAddToRegistry
  Push "DoNotAddToPath"
  Push "$DO_NOT_ADD_TO_PATH"
  Call ConditionalAddToRegistry
  Push "AddToPathAllUsers"
  Push "$ADD_TO_PATH_ALL_USERS"
  Call ConditionalAddToRegistry
  Push "AddToPathCurrentUser"
  Push "$ADD_TO_PATH_CURRENT_USER"
  Call ConditionalAddToRegistry
  Push "InstallToDesktop"
  Push "$INSTALL_DESKTOP"
  Call ConditionalAddToRegistry

]=])
    set(_snapmaker_nsis_new_registry [=[
  ; Write special uninstall registry entries
  Push "StartMenu"
  Push "$STARTMENU_FOLDER"
  Call ConditionalAddToRegistry

]=])
    string(REPLACE "${_snapmaker_nsis_old_registry}" "${_snapmaker_nsis_new_registry}" _snapmaker_nsis_tpl "${_snapmaker_nsis_tpl}")
    foreach(_snapmaker_nsis_erase IN ITEMS
        [=[  Var DO_NOT_ADD_TO_PATH
  Var ADD_TO_PATH_ALL_USERS
  Var ADD_TO_PATH_CURRENT_USER
  Var INSTALL_DESKTOP
]=]
        [=[  ReserveFile "NSIS.InstallOptions.ini"
  !insertmacro MUI_RESERVEFILE_INSTALLOPTIONS

]=]
        [=[  Page custom InstallOptionsPage
]=]
        [=[  !insertmacro MUI_INSTALLOPTIONS_READ $INSTALL_DESKTOP "NSIS.InstallOptions.ini" "Field 5" "State"
]=]
        [=[Section "-Add to path"
  Push $INSTDIR\bin
  StrCmp "@CPACK_NSIS_MODIFY_PATH@" "ON" 0 doNotAddToPath
  StrCmp $DO_NOT_ADD_TO_PATH "1" doNotAddToPath 0
    Call AddToPath
  doNotAddToPath:
SectionEnd

]=]
        [=[;--------------------------------
; Create custom pages
Function InstallOptionsPage
  !insertmacro MUI_HEADER_TEXT "Install Options" "Choose options for installing @CPACK_NSIS_PACKAGE_NAME@"
  !insertmacro MUI_INSTALLOPTIONS_DISPLAY "NSIS.InstallOptions.ini"

FunctionEnd

]=]
        [=[  ReadRegStr $DO_NOT_ADD_TO_PATH SHCTX \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\@CPACK_PACKAGE_INSTALL_REGISTRY_KEY@" "DoNotAddToPath"
  ReadRegStr $ADD_TO_PATH_ALL_USERS SHCTX \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\@CPACK_PACKAGE_INSTALL_REGISTRY_KEY@" "AddToPathAllUsers"
  ReadRegStr $ADD_TO_PATH_CURRENT_USER SHCTX \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\@CPACK_PACKAGE_INSTALL_REGISTRY_KEY@" "AddToPathCurrentUser"
  ;MessageBox MB_OK "Add to path: $DO_NOT_ADD_TO_PATH all users: $ADD_TO_PATH_ALL_USERS"
  ReadRegStr $INSTALL_DESKTOP SHCTX \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\@CPACK_PACKAGE_INSTALL_REGISTRY_KEY@" "InstallToDesktop"
  ;MessageBox MB_OK "Install to desktop: $INSTALL_DESKTOP "

]=]
        [=[  Push $INSTDIR\bin
  StrCmp $DO_NOT_ADD_TO_PATH_ "1" doNotRemoveFromPath 0
    Call un.RemoveFromPath
  doNotRemoveFromPath:
]=]
        [=[  StrCmp "@CPACK_NSIS_MODIFY_PATH@" "ON" 0 noOptionsPage
    !insertmacro MUI_INSTALLOPTIONS_EXTRACT "NSIS.InstallOptions.ini"

  noOptionsPage:
]=]
    )
        string(REPLACE "${_snapmaker_nsis_erase}" "" _snapmaker_nsis_tpl "${_snapmaker_nsis_tpl}")
    endforeach()
    set(_snapmaker_nsis_tpl_dir "${CMAKE_BINARY_DIR}/cmake/cpack")
    file(MAKE_DIRECTORY "${_snapmaker_nsis_tpl_dir}")
    file(WRITE "${_snapmaker_nsis_tpl_dir}/NSIS.template.in" "${_snapmaker_nsis_tpl}")
    list(PREPEND CMAKE_MODULE_PATH "${_snapmaker_nsis_tpl_dir}")
endif ()
include(CPack)
