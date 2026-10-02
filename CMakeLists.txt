cmake_minimum_required(VERSION 3.13)

# Verify that your CMake version is exactly 3.31.x series or lower on windows
if ( ((MSVC) OR (WIN32)) AND (${CMAKE_VERSION} VERSION_GREATER_EQUAL "4.0") )
    message(FATAL_ERROR "Only cmake versions between 3.13.x and 3.31.x is supported on windows. Detected version: ${CMAKE_VERSION}")
endif()

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

project(Snapmaker_Orca)

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

find_package(Git)
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    # Check current Git commit hash
    execute_process(
        COMMAND ${GIT_EXECUTABLE} log -1 --format=%h
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_COMMIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    # Not passed as a global -DGIT_COMMIT_HASH any more: that put the commit into every compile
    # command, so each new commit + reconfigure recompiled everything (Ninja and MSBuild both track
    # command lines) and invalidated every compiler-cache entry. No source reads the macro
    # (common_func.hpp falls back to "0000000"); a file that needs it should get it through its own
    # COMPILE_DEFINITIONS source property or a configured header.
endif()

if(DEFINED ENV{SLIC3R_STATIC})
        set(SLIC3R_STATIC_INITIAL $ENV{SLIC3R_STATIC})
else()
        if (MSVC OR MINGW OR APPLE)
                set(SLIC3R_STATIC_INITIAL 1)
        else()
                set(SLIC3R_STATIC_INITIAL 0)
        endif()
endif()

option(SLIC3R_STATIC 			"Compile Snapmaker_Orca with static libraries (Boost, TBB, glew)" ${SLIC3R_STATIC_INITIAL})
option(SLIC3R_GUI    			"Compile Snapmaker_Orca with GUI components (OpenGL, wxWidgets)" 1)
option(SLIC3R_FHS               "Assume Snapmaker_Orca is to be installed in a FHS directory structure" 0)
option(SLIC3R_WX_STABLE         "Build against wxWidgets stable (3.0) as oppsed to dev (3.1) on Linux" 0)
option(SLIC3R_PROFILE 			"Compile Snapmaker_Orca with an invasive Shiny profiler" 0)
option(SLIC3R_PCH               "Use precompiled headers" 1)
option(SLIC3R_MSVC_COMPILE_PARALLEL "Compile on Visual Studio in parallel" 1)
option(SLIC3R_MSVC_PDB          "Generate PDB files on MSVC in Release mode" 1)
option(SLIC3R_PERL_XS           "Compile XS Perl module and enable Perl unit and integration tests" 0)
option(SLIC3R_ASAN              "Enable ASan on Clang and GCC" 0)

# Unity ("jumbo") build of the two big libraries: CMake compiles their sources in batches of
# SLIC3R_UNITY_BUILD_BATCH_SIZE files per translation unit, so the common headers are parsed once per
# batch instead of once per file. Off by default, so CI and the Visual Studio workflow are unchanged.
# Sources that cannot share a translation unit are listed next to each slic3r_unity_build() call.
option(SLIC3R_UNITY_BUILD       "Unity (jumbo) build of libslic3r and libslic3r_gui" 0)
set(SLIC3R_UNITY_BUILD_BATCH_SIZE 8 CACHE STRING "Source files per unity translation unit (SLIC3R_UNITY_BUILD; use 16 with /Z7, see src/slic3r)")

# slic3r_unity_build(<target> [SKIP <source>...]): unity-build <target> when SLIC3R_UNITY_BUILD is on;
# the SKIP sources keep a translation unit of their own. Call it from the directory that owns the sources.
function(slic3r_unity_build _target)
    if (NOT SLIC3R_UNITY_BUILD)
        return()
    endif ()
    cmake_parse_arguments(_UB "" "" "SKIP" ${ARGN})
    set_target_properties(${_target} PROPERTIES UNITY_BUILD ON UNITY_BUILD_BATCH_SIZE ${SLIC3R_UNITY_BUILD_BATCH_SIZE})
    if (_UB_SKIP)
        set_source_files_properties(${_UB_SKIP} PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
    endif ()
endfunction()

# Sentry crash handling - built in on Windows and macOS by default.
#
# This switch builds in the SDK, which starts crashpad, which writes the local minidumps we
# debug with (%LOCALAPPDATA%\EdgeSlicer\reports\*.dmp). Whether a crash is REPORTED anywhere
# is decided at run time: only with a destination (EDGESLICER_SENTRY_DSN below, or a tester's
# "ultra_sentry_dsn" in the app config) AND the user's opt-in (Preferences > General >
# "Send crash reports", off by default) - see src/sentry_wrapper/SentryWrapper.cpp and
# docs/privacy.md. Turn this OFF to drop local crash capture too.
if (WIN32 OR APPLE)
    set(SLIC3R_SENTRY_DEFAULT ON)
else()
    set(SLIC3R_SENTRY_DEFAULT OFF)
endif()
option(SLIC3R_SENTRY            "Build in the Sentry/crashpad crash handler (local minidumps; uploads need a DSN)" ${SLIC3R_SENTRY_DEFAULT})

# Crash-report destination, compiled in. Empty (every local build) means crash reports are
# never uploaded. CI release builds set it from the SENTRY_DSN repository secret, through the
# environment variable of the same name so the platform build scripts need no extra argument;
# -DEDGESLICER_SENTRY_DSN=... works too. Never commit a DSN. It goes into a generated header
# in the build tree, not onto compiler command lines, and is never printed here.
set(EDGESLICER_SENTRY_DSN "" CACHE STRING "Sentry DSN compiled in for opt-in crash reports (empty = no uploads)")
mark_as_advanced(EDGESLICER_SENTRY_DSN)
set(EDGESLICER_SENTRY_ENVIRONMENT "" CACHE STRING "Sentry environment reported with crashes (default: dev)")
set(_edge_sentry_dsn "${EDGESLICER_SENTRY_DSN}")
if (_edge_sentry_dsn STREQUAL "" AND DEFINED ENV{EDGESLICER_SENTRY_DSN})
    set(_edge_sentry_dsn "$ENV{EDGESLICER_SENTRY_DSN}")
endif ()
string(STRIP "${_edge_sentry_dsn}" _edge_sentry_dsn)
set(_edge_sentry_env "${EDGESLICER_SENTRY_ENVIRONMENT}")
if (_edge_sentry_env STREQUAL "" AND DEFINED ENV{EDGESLICER_SENTRY_ENVIRONMENT})
    set(_edge_sentry_env "$ENV{EDGESLICER_SENTRY_ENVIRONMENT}")
endif ()
if (_edge_sentry_env STREQUAL "")
    set(_edge_sentry_env "dev")
endif ()
if (NOT _edge_sentry_dsn STREQUAL "" AND NOT _edge_sentry_dsn MATCHES "^https?://[^\"\\ ]+$")
    message(FATAL_ERROR "EDGESLICER_SENTRY_DSN is not a Sentry DSN (expected https://<key>@<host>/<project>)")
endif ()
if (NOT _edge_sentry_env MATCHES "^[A-Za-z0-9._-]+$")
    message(FATAL_ERROR "EDGESLICER_SENTRY_ENVIRONMENT may only contain letters, digits, '.', '_' and '-'")
endif ()
if (_edge_sentry_dsn STREQUAL "")
    message(STATUS "Crash reports: no DSN compiled in - crashes stay on the user's machine (environment ${_edge_sentry_env})")
else ()
    message(STATUS "Crash reports: DSN compiled in, opt-in at run time (environment ${_edge_sentry_env})")
endif ()
set(EDGESLICER_SENTRY_DSN_VALUE "${_edge_sentry_dsn}")
set(EDGESLICER_SENTRY_ENVIRONMENT_VALUE "${_edge_sentry_env}")
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/sentry_wrapper/edgeslicer_sentry_config.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/generated/edgeslicer_sentry_config.h @ONLY)

# Build flavour suffix: empty for releases and local builds, "-nightly.<YYYYMMDD>+<shortsha>" for
# the nightly builds (.github/workflows/nightly.yml, docs/nightly.md). It is appended to
# Snapmaker_VERSION wherever the build names itself to a person or a service - About, the splash
# screen, the crash reporter's release, the update check and the installer's file name - and
# nowhere else: project files, presets and printer protocols keep the plain release number.
# Taken from -DEDGESLICER_VERSION_SUFFIX, else the environment variable of that name, else a
# one-line version_suffix.txt at the top of the source tree (CI writes it; the Flatpak build copies
# the tree and sees no environment). It goes into a generated header, not onto compiler command
# lines, so a new suffix recompiles only the few files that show the version.
set(EDGESLICER_VERSION_SUFFIX "" CACHE STRING "Version suffix of a nightly build, e.g. -nightly.20260926+abc1234 (empty = release)")
set(_edge_version_suffix "${EDGESLICER_VERSION_SUFFIX}")
if (_edge_version_suffix STREQUAL "" AND DEFINED ENV{EDGESLICER_VERSION_SUFFIX})
    set(_edge_version_suffix "$ENV{EDGESLICER_VERSION_SUFFIX}")
endif ()
if (_edge_version_suffix STREQUAL "" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/version_suffix.txt")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/version_suffix.txt")
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/version_suffix.txt" _edge_version_suffix LIMIT_COUNT 1)
endif ()
string(STRIP "${_edge_version_suffix}" _edge_version_suffix)
if (NOT _edge_version_suffix STREQUAL "" AND NOT _edge_version_suffix MATCHES "^[-+][A-Za-z0-9][A-Za-z0-9.+-]*$")
    message(FATAL_ERROR "EDGESLICER_VERSION_SUFFIX '${_edge_version_suffix}' must start with '-' or '+' and hold only letters, digits, '.', '-' and '+'")
endif ()
set(EDGESLICER_VERSION_SUFFIX_VALUE "${_edge_version_suffix}")
set(EDGESLICER_FULL_VERSION "${Snapmaker_VERSION}${_edge_version_suffix}")
# File names avoid '+': GitHub renames release assets whose names carry special characters.
string(REPLACE "+" "-" EDGESLICER_FILE_VERSION "${EDGESLICER_FULL_VERSION}")
if (NOT _edge_version_suffix STREQUAL "")
    message(STATUS "EdgeSlicer version: ${EDGESLICER_FULL_VERSION}")
endif ()
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/common_func/edgeslicer_version.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/generated/edgeslicer_version.h @ONLY)
include_directories(${CMAKE_CURRENT_BINARY_DIR}/generated)
# If SLIC3R_FHS is 1 -> SLIC3R_DESKTOP_INTEGRATION is always 0, othrewise variable.
CMAKE_DEPENDENT_OPTION(SLIC3R_DESKTOP_INTEGRATION "Allow perfoming desktop integration during runtime" 1 "NOT SLIC3R_FHS" 0)

set(OPENVDB_FIND_MODULE_PATH "" CACHE PATH "Path to OpenVDB installation's find modules.")

set(SLIC3R_GTK "2" CACHE STRING "GTK version to use with wxWidgets on Linux")

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
    set(CMAKE_FIND_FRAMEWORK LAST)
    set(CMAKE_FIND_APPBUNDLE LAST)
    list(FIND CMAKE_OSX_ARCHITECTURES ${CMAKE_SYSTEM_PROCESSOR} _arch_idx)
    if (CMAKE_OSX_ARCHITECTURES AND _arch_idx LESS 0)
        set(IS_CROSS_COMPILE TRUE)
    endif ()
    if (CMAKE_MACOSX_BUNDLE)
        set(CMAKE_INSTALL_RPATH @executable_path/../Frameworks)
    endif()
    # com.snapmaker.* claimed snapmaker.com, a domain this project does not own.
    # Changing a bundle id is one-way: macOS treats the result as a different application,
    # so keychain items, TCC/privacy grants and any notarisation history start again.
    SET(CMAKE_XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "io.github.acerage.EdgeSlicer")

    message(STATUS "Orca: IS_CROSS_COMPILE: ${IS_CROSS_COMPILE}")
endif ()

# Proposal for C++ unit tests and sandboxes
option(SLIC3R_BUILD_SANDBOXES   "Build development sandboxes" OFF)
# Ultra: the QuadriFlow-backed quad remesher ("Quad remesh...", Phase 2 of the remesh
# spec). Optional because QuadriFlow is a new native dependency and CI on macOS/Linux
# has not been through a deps build with it yet; when the package is missing the whole
# feature - libslic3r entry points, dialog and menu items - compiles out and the menu
# entry is not shown. See deps/QuadriFlow/QuadriFlow.cmake.
option(SLIC3R_QUAD_REMESH       "Build the QuadriFlow-backed quad remesher" ON)
option(BUILD_TESTS       "Build unit tests" OFF)
option(ORCA_TOOLS       "Build Orca tools" OFF)

if (IS_CROSS_COMPILE)
    message("Detected cross compilation setup. Tests and encoding checks will be forcedly disabled!")
    set(SLIC3R_PERL_XS OFF CACHE BOOL "" FORCE)
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

if (MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL Clang)
    set(IS_CLANG_CL TRUE)

    # clang-cl can interpret SYSTEM header paths if -imsvc is used
    set(CMAKE_INCLUDE_SYSTEM_FLAG_CXX "-imsvc")

    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall \
        -Wno-old-style-cast -Wno-reserved-id-macro -Wno-c++98-compat-pedantic")
else ()
    set(IS_CLANG_CL FALSE)
endif ()

# With a compiler cache as the launcher (-DCMAKE_CXX_COMPILER_LAUNCHER=sccache), pass SYSTEM include
# directories to MSVC as plain -I: sccache 0.17 hashes the paths of /external:I arguments (it does
# not hash -I), so otherwise every build directory and every checkout gets cache entries of its own.
# /external:anglebrackets keeps the <...> dependency headers quiet, as /external:I did.
if (MSVC AND NOT IS_CLANG_CL AND CMAKE_CXX_COMPILER_LAUNCHER)
    set(CMAKE_INCLUDE_SYSTEM_FLAG_C "-I")
    set(CMAKE_INCLUDE_SYSTEM_FLAG_CXX "-I")
    add_compile_options(/external:anglebrackets /external:W0)
endif ()

if (MSVC)
    if (SLIC3R_MSVC_COMPILE_PARALLEL AND NOT IS_CLANG_CL)
           add_compile_options(/MP)
    endif ()
    # /bigobj (Increase Number of Sections in .Obj file)
    # error C3859: virtual memory range for PCH exceeded; please recompile with a command line option of '-Zm90' or greater
    # Generate symbols at every build target, even for the release.
    # /Z7 embeds the debug info in each object instead of a shared per-target PDB (/Zi), which lets a
    # compiler cache (sccache) capture the outputs and removes the mspdbsrv contention between parallel builds.
    option(SLIC3R_MSVC_Z7 "Embed debug info in objects (/Z7) instead of a shared PDB (/Zi)" OFF)
    if (SLIC3R_MSVC_Z7)
        add_compile_options(-bigobj -Zm520 /Z7)
    else ()
        add_compile_options(-bigobj -Zm520 /Zi)
    endif ()
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

# Display and check CMAKE_PREFIX_PATH
message(STATUS "SLIC3R_STATIC: ${SLIC3R_STATIC}")
if (NOT "${CMAKE_PREFIX_PATH}" STREQUAL "")
    message(STATUS "CMAKE_PREFIX_PATH: ${CMAKE_PREFIX_PATH} (from cache or command line)")
    set(PREFIX_PATH_CHECK ${CMAKE_PREFIX_PATH})
elseif (NOT "$ENV{CMAKE_PREFIX_PATH}" STREQUAL "")
    message(STATUS "CMAKE_PREFIX_PATH: $ENV{CMAKE_PREFIX_PATH} (from environment)")
    set(PREFIX_PATH_CHECK $ENV{CMAKE_PREFIX_PATH})
else ()
    message(STATUS "CMAKE_PREFIX_PATH: (default)")
endif ()

foreach (DIR ${PREFIX_PATH_CHECK})
    if (NOT EXISTS "${DIR}")
        message(WARNING "CMAKE_PREFIX_PATH element doesn't exist: ${DIR}")
    endif ()
endforeach ()

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
if(WIN32)
    if(NOT DEFINED WIN10SDK_PATH)
        if(DEFINED ENV{WIN10SDK_PATH})
                set(WIN10SDK_PATH "$ENV{WIN10SDK_PATH}")
        endif()
    endif()
    if(DEFINED WIN10SDK_PATH)
        #BBS: modify win10sdk_path
        if (EXISTS "${WIN10SDK_PATH}/winrt/windows.graphics.printing3d.h")
            set(WIN10SDK_INCLUDE_PATH "${WIN10SDK_PATH}")
        else()
            message("WIN10SDK_PATH is invalid: ${WIN10SDK_PATH}")
            message("${WIN10SDK_PATH}/winrt/windows.graphics.printing3d.h was not found")
            message("STL fixing by the Netfabb service will not be compiled")
            unset(WIN10SDK_PATH)
        endif()
    else()
        # Try to use the default Windows 10 SDK path.
        set(WIN10SDK_INCLUDE_PATH "$ENV{WindowsSdkDir}/Include/$ENV{WindowsSDKVersion}")
        if (NOT EXISTS "${WIN10SDK_INCLUDE_PATH}/winrt/windows.graphics.printing3d.h")
            message("${WIN10SDK_INCLUDE_PATH}/winrt/windows.graphics.printing3d.h was not found")
            message("STL fixing by the Netfabb service will not be compiled")
            unset(WIN10SDK_INCLUDE_PATH)
        endif()
    endif()
    if(WIN10SDK_INCLUDE_PATH)
        message("Building with Win10 Netfabb STL fixing service support")
        add_definitions(-DHAS_WIN10SDK)
        include_directories(SYSTEM "${WIN10SDK_INCLUDE_PATH}")
    else()
        message("Building without Win10 Netfabb STL fixing service support")
    endif()
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

    if (CMAKE_VERSION VERSION_LESS "3.1")
        # Workaround for an old CMake, which does not understand CMAKE_CXX_STANDARD.
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
    endif()

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

if (NOT MSVC AND ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU" OR "${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang"))
    if (NOT MINGW)
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
        add_compile_options(-Wno-error=enum-constexpr-conversion)
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

    # Bit of a hack for flatpak building: compress the debug info with zstd to save space in CI
    if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER 13.0)
        add_compile_options(-gz=zstd)
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
# 添加编译选项，忽略警告 C4305 （格式转换截断）
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
find_package(Boost 1.83.0 REQUIRED COMPONENTS system filesystem thread log log_setup locale regex chrono atomic date_time iostreams program_options nowide)

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
# Adapted from Orca #14593 (case-insensitive): only map RelWithDebInfo imported configs to Release when
# that config is in use, or on MSVC. Non-MSVC multi-config generators with
# CMAKE_BUILD_TYPE unset now fall back to the deps' first available config,
# which is harmless with Edge's single-config deps.
string(TOUPPER "${CMAKE_BUILD_TYPE}" _bt)
if (_bt STREQUAL "RELWITHDEBINFO" OR MSVC)
    set(CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO RelWithDebInfo Release "")
endif()
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

# HTTP/2, which APNs requires (see AppPush.cpp and the phase 0 spike). A static libcurl carries
# no link interface of its own, so the transitive library has to be named here or every nghttp2
# symbol http2.c references comes out unresolved at link time. NGHTTP2::nghttp2 also carries the
# NGHTTP2_STATICLIB definition the header needs. Optional on purpose: a deps tree built before
# this change still links, it just has no HTTP/2 - and AppPush says so on the hub page rather
# than failing every push with a transport error.
find_package(NGHTTP2 QUIET)
if (NGHTTP2_FOUND)
    target_link_libraries(libcurl INTERFACE NGHTTP2::nghttp2)
    message(STATUS "Found nghttp2 at ${NGHTTP2_LIBRARY}: libcurl will have HTTP/2")
else ()
    message(WARNING "nghttp2 not found: this build of libcurl will have no HTTP/2, so APNs push "
                    "will be unavailable. Rebuild the deps to pick it up.")
endif ()

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

# Find eigen3 or use bundled version
if (NOT SLIC3R_STATIC)
    find_package(Eigen3 3.3)
endif ()
if (NOT EIGEN3_FOUND)
    set(EIGEN3_FOUND 1)
    set(EIGEN3_INCLUDE_DIR ${LIBDIR}/eigen/)
endif ()
include_directories(BEFORE SYSTEM ${EIGEN3_INCLUDE_DIR})

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

# GUI OpenGL loading is now GLAD (src/glad). GLEW remains for optional
# sandboxes/OpenCSG and is still discovered here so those targets can link it.
set(GLEW_ROOT "${CMAKE_PREFIX_PATH}")
message("GLEW_ROOT: ${GLEW_ROOT}")
# Find glew or use bundled version
if (SLIC3R_STATIC AND NOT SLIC3R_STATIC_EXCLUDE_GLEW)
    set(GLEW_USE_STATIC_LIBS ON)
    set(GLEW_VERBOSE ON)
endif()

find_package(GLEW REQUIRED)

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
    COMMAND xgettext --keyword=L --keyword=_L --keyword=_u8L --keyword=L_CONTEXT:1,2c --keyword=_L_PLURAL:1,2 --add-comments=TRN --from-code=UTF-8 --no-location --debug --boost
        -f "${BBL_L18N_DIR}/list.txt"
        -o "${BBL_L18N_DIR}/Snapmaker_Orca.pot"
	COMMAND hintsToPot ${SLIC3R_RESOURCES_DIR} ${BBL_L18N_DIR}
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Generate pot file from strings in the source tree"
)
add_custom_target(gettext_merge_po_with_pot
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Merge localization po with new generted pot file"
)
file(GLOB BBL_L10N_PO_FILES "${BBL_L18N_DIR}/*/Snapmaker_Orca*.po")
foreach(po_file ${BBL_L10N_PO_FILES})
    GET_FILENAME_COMPONENT(po_dir "${po_file}" DIRECTORY)
    SET(po_new_file "${po_dir}/Snapmaker_Orca_.po")
    add_custom_command(
        TARGET gettext_merge_po_with_pot PRE_BUILD
        COMMAND msgmerge -N -o ${po_file} ${po_file} "${BBL_L18N_DIR}/Snapmaker_Orca.pot"
        DEPENDS ${po_file}
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
    # The catalog base name must match GUI_App.cpp's AddCatalog(SLIC3R_APP_KEY); the .po
    # source files keep their historical names (nothing derives the .mo name from them).
    SET(mo_file "${L10N_DIR}/${po_dir}/EdgeSlicer.mo")
    add_custom_command(
        TARGET gettext_po_to_mo PRE_BUILD
        COMMAND msgfmt ARGS --check-format -o ${mo_file} ${po_file}
        #COMMAND msgfmt ARGS --check-compatibility -o ${mo_file} ${po_file}
        DEPENDS ${po_file}
    )
endforeach()

find_package(NLopt 1.4 REQUIRED)

# Ultra: Manifold - robust mesh booleans (primary backend of the Mesh Boolean gizmo)
find_package(manifold CONFIG REQUIRED)


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

# Ultra: QuadriFlow, for the quad remesher. Optional on purpose - unlike OpenVDB above,
# a missing package is a warning and a disabled feature, not a fatal error, so a tree
# whose deps predate deps/QuadriFlow still builds. SLIC3R_QUAD_REMESH stays off unless
# the package was actually found, and everything downstream keys off that one variable.
if (SLIC3R_QUAD_REMESH)
    find_package(QuadriFlow QUIET)
    if (NOT QuadriFlow_FOUND)
        set(SLIC3R_QUAD_REMESH OFF)
        message(STATUS "QuadriFlow not found - the quad remesher will be omitted. Build it with the deps project (deps/QuadriFlow) to enable it.")
    else ()
        message(STATUS "QuadriFlow found - building the quad remesher")
    endif ()
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

# OCCT (STEP import/export, B-rep tools) comes from the deps prefix. OCCT_PREFIX points at a
# separately built OCCT instead (same layout: bin/occt, lib/occt, lib/cmake/occt, include/occt),
# e.g. to try an OCCT with other modules without touching the shared deps prefix.
set(OCCT_PREFIX "${CMAKE_PREFIX_PATH}" CACHE PATH "Prefix holding OCCT (bin/occt, lib/cmake/occt)")

# OCCT toolkits the app loads at run time on Windows (OCCT is shared there). TKBool, TKFillet,
# TKOffset and TKFeat come from OCCT's ModelingAlgorithms module (B-rep booleans, fillets,
# offsets, features).
set(OCCT_RUNTIME_TOOLKITS
    TKBO TKBool TKBRep TKCAF TKCDF TKernel TKFeat TKFillet TKG2d TKG3d TKGeomAlgo TKGeomBase
    TKHLR TKLCAF TKMath TKMesh TKOffset TKPrim TKService TKShHealing TKSTEP TKSTEP209
    TKSTEPAttr TKSTEPBase TKTopAlgo TKV3d TKVCAF TKXCAF TKXDESTEP TKXSBase)

function(Snapmaker_Orca_copy_dlls target config postfix output_dlls)
    if ("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "AMD64")
        set(_arch "x64")
    elseif ("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "X86")
        set(_arch "x86")
    else ()
        message(FATAL_ERROR "Unable to detect architecture")
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

    set(_occt_dlls_src "")
    set(_occt_dlls_out "")
    foreach (_tk IN LISTS OCCT_RUNTIME_TOOLKITS)
        list(APPEND _occt_dlls_src ${OCCT_PREFIX}/bin/occt/${_tk}.dll)
        list(APPEND _occt_dlls_out ${_out_dir}/${_tk}.dll)
    endforeach ()
    file(COPY ${_occt_dlls_src}
            ${CMAKE_PREFIX_PATH}/bin/freetype.dll
         DESTINATION ${_out_dir})

    # Build the DLL list in a local variable first
    set(_dll_list
        ${_out_dir}/libgmp-10.dll
        ${_out_dir}/libmpfr-4.dll
        ${_out_dir}/WebView2Loader.dll

        ${_occt_dlls_out}

        ${_out_dir}/freetype.dll
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


# libslic3r, Snapmaker_Orca GUI and the Snapmaker_Orca executable.
add_subdirectory(deps_src)
add_subdirectory(src)
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT Snapmaker_Orca_app_gui)

add_dependencies(gettext_make_pot hintsToPot)

# Perl bindings, currently only used for the unit / integration tests of libslic3r.
# Also runs the unit / integration tests.
#FIXME Port the tests into C++ to finally get rid of the Perl!
if (SLIC3R_PERL_XS)
    add_subdirectory(xs)
endif ()

if(SLIC3R_BUILD_SANDBOXES)
    add_subdirectory(sandboxes)
endif()

if(BUILD_TESTS)
    add_subdirectory(tests)
endif()

if (NOT WIN32 AND NOT APPLE)
    set(SLIC3R_APP_CMD "EdgeSlicer")
    configure_file(${LIBDIR}/dev-utils/platform/unix/build_appimage.sh.in ${CMAKE_CURRENT_BINARY_DIR}/build_appimage.sh USE_SOURCE_PERMISSIONS @ONLY)
endif()


# Resources install target, configure fhs.hpp on UNIX
if (WIN32)
    install(DIRECTORY "${SLIC3R_RESOURCES_DIR}/" DESTINATION "./resources")
    # EdgeSlicer: the bundled network plugin (UltraNet, a private clean-room component built in
    # the same toolchain). The host copies <exe dir>/ultranet/*.dll into <data dir>/plugins on
    # first run (GUI_App). Point ULTRANET_BIN_DIR at a folder holding bambu_networking.dll and
    # BambuSource.dll to ship it; without it the packages carry no plugin and Bambu cloud
    # sign-in falls back to the Bambu CDN download.
    if (ULTRANET_BIN_DIR AND EXISTS "${ULTRANET_BIN_DIR}/bambu_networking.dll")
        install(DIRECTORY "${ULTRANET_BIN_DIR}/" DESTINATION "./ultranet" FILES_MATCHING PATTERN "*.dll")
        # The marker that tells our plugin apart from Bambu's (the library name is the same). The
        # first-run copier carries it into <data dir>/plugins, where every Bambu CDN download and
        # update prompt is gated on it - see src/slic3r/GUI/PluginGuard.hpp. Ship the sidecar's own
        # copy when it has one, otherwise generate it.
        if (EXISTS "${ULTRANET_BIN_DIR}/ultranet.txt")
            install(FILES "${ULTRANET_BIN_DIR}/ultranet.txt" DESTINATION "./ultranet")
        else()
            file(WRITE "${CMAKE_BINARY_DIR}/ultranet.txt"
                 "UltraNet: EdgeSlicer's own network plug-in. Do not replace with the Bambu CDN package.
")
            install(FILES "${CMAKE_BINARY_DIR}/ultranet.txt" DESTINATION "./ultranet")
        endif()
        message(STATUS "EdgeSlicer: shipping the network plugin from ${ULTRANET_BIN_DIR}")
    endif()

    # EdgeSlicer: FlashForge's FlashNetwork library. The FlashForge device tab drives Creator 5 and
    # friends entirely through this closed, pre-built DLL - without it the tab cannot initialise and
    # no LAN send is possible. FlashForge publishes it nowhere: it exists only inside their Flash
    # Studio Desktop installers, so there is no URL to download it from on first need and it has to
    # be shipped. Point FLASHNETWORK_BIN_DIR at a folder holding FlashNetwork.dll to include it:
    #
    #   -DFLASHNETWORK_BIN_DIR=C:/path/to/flashnetwork_out/ship
    #
    # The library is redistributed unmodified; see docs/superpowers/specs/2026-09-11-flashforge-lan-ship.md
    # for its provenance (Flash Studio Desktop 1.7.13) and the sha256 it is pinned to. It is x64 and
    # needs only the MSVC runtime we already ship, so no companion DLLs travel with it.
    #
    # It does need one data file: FLASHNETWORK9.DAT, FlashForge's server-settings file, which
    # fnet_initlize() reads and without which it returns FNET_ERROR - so a package with the DLL and
    # no DAT has a FlashForge tab that never starts (2.4.2.0). The file is published by FlashForge
    # in its AGPL-3.0 Orca-Flashforge repository (resources/data/FLASHNETWORK9.DAT); it is
    # generation-matched to the library (the older FLASHNETWORK7.DAT is rejected by the 3.4.2 DLL),
    # so it travels in the same ship folder as the DLL rather than in this tree, and installs where
    # FlashForge's own client reads it: resources/data.
    if (FLASHNETWORK_BIN_DIR AND EXISTS "${FLASHNETWORK_BIN_DIR}/FlashNetwork.dll")
        install(FILES "${FLASHNETWORK_BIN_DIR}/FlashNetwork.dll" DESTINATION ".")
        # Any companion DLLs a future build of the library may need.
        install(DIRECTORY "${FLASHNETWORK_BIN_DIR}/" DESTINATION "." FILES_MATCHING PATTERN "*.dll")
        message(STATUS "EdgeSlicer: shipping FlashForge's FlashNetwork library from ${FLASHNETWORK_BIN_DIR}")
        file(GLOB _flashnetwork_dats "${FLASHNETWORK_BIN_DIR}/FLASHNETWORK*.DAT")
        if (_flashnetwork_dats)
            install(FILES ${_flashnetwork_dats} DESTINATION "./resources/data")
            message(STATUS "EdgeSlicer: shipping FlashNetwork server settings ${_flashnetwork_dats}")
        else()
            message(WARNING "EdgeSlicer: ${FLASHNETWORK_BIN_DIR} has FlashNetwork.dll but no FLASHNETWORK9.DAT - "
                            "the library will refuse to initialise and the FlashForge tab will not work")
        endif()
    else()
        message(STATUS "EdgeSlicer: no FLASHNETWORK_BIN_DIR - FlashForge LAN printing will be unavailable in this package")
    endif()

    # EdgeSlicer: the bundled ffmpeg for the remote hub's camera Quality steps. go2rtc re-encodes
    # by shelling out to an ffmpeg binary and carries no codec of its own, so without one the
    # per-phone Quality setting is reduced to the Bambu MJPEG frame-rate knob. It installs beside
    # go2rtc.exe, which is where RemoteHub.cpp's ffmpeg_path() looks first and what start_go2rtc()
    # names in go2rtc's `ffmpeg: bin:` - so it is found by path, never by PATH.
    #
    #   -DFFMPEG_BIN_DIR=C:/path/to/ffmpeg_out/ship
    #
    # The build is **LGPL-3.0** (BtbN/FFmpeg-Builds, --disable-libx264 --disable-libx265), chosen
    # over a GPL one so the tool carries no copyleft stronger than the app's own AGPL-3.0. It is
    # redistributed unmodified as a separate executable that the hub invokes as a subprocess; its
    # LICENSE.txt ships beside it and it is listed in CopyrightsDialog::fill_entries(). See
    # docs/superpowers/specs/2026-09-12-bundled-ffmpeg.md for the pinned release tag, the sha256,
    # and why the H.264 encoder is libopenh264 rather than libx264.
    #
    # Both packages get it from this one rule: the portable zip is the install tree, and the NSIS
    # installer is built from the same tree.
    if (FFMPEG_BIN_DIR AND EXISTS "${FFMPEG_BIN_DIR}/ffmpeg.exe")
        install(PROGRAMS "${FFMPEG_BIN_DIR}/ffmpeg.exe" DESTINATION "./resources/tools/go2rtc")
        # The licence text has to travel with the binary - it is the LGPL's condition for shipping
        # it at all, and a user who finds an ffmpeg.exe in our tree should be able to see whose it
        # is without leaving the folder.
        if (EXISTS "${FFMPEG_BIN_DIR}/FFMPEG-LICENSE.txt")
            install(FILES "${FFMPEG_BIN_DIR}/FFMPEG-LICENSE.txt" DESTINATION "./resources/tools/go2rtc")
        endif()
        # Any companion DLLs a shared build would need. A static build (what we ship) has none, so
        # this is a no-op there and the package stays one extra file.
        install(DIRECTORY "${FFMPEG_BIN_DIR}/" DESTINATION "./resources/tools/go2rtc" FILES_MATCHING PATTERN "*.dll")
        message(STATUS "EdgeSlicer: shipping ffmpeg for camera quality variants from ${FFMPEG_BIN_DIR}")
    else()
        message(STATUS "EdgeSlicer: no FFMPEG_BIN_DIR - camera Quality falls back to the MJPEG frame-rate knob")
    endif()

    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
    include(InstallRequiredSystemLibraries)
    install (PROGRAMS ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} DESTINATION ".")
elseif (SLIC3R_FHS)
    # CMAKE_INSTALL_FULL_DATAROOTDIR: read-only architecture-independent data root (share)
    set(SLIC3R_FHS_RESOURCES "${CMAKE_INSTALL_FULL_DATAROOTDIR}/${SLIC3R_APP_KEY}")
    install(DIRECTORY ${SLIC3R_RESOURCES_DIR}/ DESTINATION ${SLIC3R_FHS_RESOURCES}
        PATTERN "*/udev" EXCLUDE
    )
    install(FILES src/dev-utils/platform/unix/Snapmaker_Orca.desktop DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
    foreach(SIZE 32 128 192)
        install(FILES ${SLIC3R_RESOURCES_DIR}/images/Snapmaker_Orca_${SIZE}px.png
            DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/${SIZE}x${SIZE}/apps RENAME Snapmaker_Orca.png
        )
    endforeach()
elseif (CMAKE_MACOSX_BUNDLE)
    # install(DIRECTORY "${SLIC3R_RESOURCES_DIR}/" DESTINATION "${CMAKE_INSTALL_PREFIX}/Snapmaker Orca.app/Contents/resources")
else ()
    install(FILES src/dev-utils/platform/unix/Snapmaker_Orca.desktop DESTINATION ${CMAKE_INSTALL_PREFIX}/resources/applications)
    install(DIRECTORY "${SLIC3R_RESOURCES_DIR}/" DESTINATION "${CMAKE_INSTALL_PREFIX}/resources")
endif ()

install(FILES ${CMAKE_SOURCE_DIR}/LICENSE.txt DESTINATION ".")
configure_file(${LIBDIR}/dev-utils/platform/unix/fhs.hpp.in ${LIBDIR_BIN}/dev-utils/platform/unix/fhs.hpp)

# Ultra: its own package identity so it installs SIDE BY SIDE with the official Snapmaker Orca
# (own Program Files folder, Start-menu folder and Add/Remove entry) instead of overwriting it.
set (CPACK_PACKAGE_NAME "EdgeSlicer")
set (CPACK_PACKAGE_VENDOR "EdgeSlicer")
set (CPACK_PACKAGE_VERSION_MAJOR "${ORCA_VERSION_MAJOR}")
set (CPACK_PACKAGE_VERSION_MINOR "${ORCA_VERSION_MINOR}")
set (CPACK_PACKAGE_VERSION_PATCH "${ORCA_VERSION_PATCH}")
# Ultra: keep the 4th version component (2.3.6.x) in Add/Remove Programs; the MAJOR/MINOR/PATCH
# triple above drops it, which made consecutive patch releases indistinguishable there.
set (CPACK_PACKAGE_VERSION "${Snapmaker_VERSION}")
# A nightly carries its suffix in the file name only; Add/Remove Programs keeps the release number.
set (CPACK_PACKAGE_FILE_NAME "EdgeSlicer_Windows_Installer_V${EDGESLICER_FILE_VERSION}")
set (CPACK_PACKAGE_DESCRIPTION_SUMMARY "EdgeSlicer is an open source slicer for FDM printers, for Snapmaker and Bambu Lab machines")
set (CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/aceRage/EdgeSlicer")
set (CPACK_PACKAGE_INSTALL_DIRECTORY ${CPACK_PACKAGE_NAME})
set (CPACK_PACKAGE_ICON "${CMAKE_SOURCE_DIR}/resources/images\\\\EdgeSlicer.ico")
set (CPACK_NSIS_MUI_ICON "${CPACK_PACKAGE_ICON}")
set (CPACK_NSIS_MUI_UNIICON "${CPACK_PACKAGE_ICON}")
set (CPACK_NSIS_INSTALLED_ICON_NAME "$INSTDIR\\\\EdgeSlicer.exe")
set(CPACK_NSIS_UNINSTALL_NAME "Uninstall")
# Preinstall guard in one place:
# 1) Block while app is running.
# 2) If old version exists, uninstall it with clear failure reason.
# We keep this in EXTRA_PREINSTALL because CPack's default .onInit uninstall prompt runs before section logic.
set(CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS [=[
SnapmakerOrcaPre_check_loop:
  ; Three arms: the name we ship now, and the two names this fork shipped under before the
  ; binary rename. An upgrade over either old install must still be blocked while it runs.
  ExecWait 'cmd.exe /c tasklist /FI "IMAGENAME eq EdgeSlicer.exe" 2>nul | find /i "EdgeSlicer.exe" >nul' $0
  IntCmp $0 0 SnapmakerOrcaPre_in_use SnapmakerOrcaPre_try_legacy1 SnapmakerOrcaPre_try_legacy1
SnapmakerOrcaPre_try_legacy1:
  ExecWait 'cmd.exe /c tasklist /FI "IMAGENAME eq snapmaker-orca.exe" 2>nul | find /i "snapmaker-orca.exe" >nul' $0
  IntCmp $0 0 SnapmakerOrcaPre_in_use SnapmakerOrcaPre_try_legacy2 SnapmakerOrcaPre_try_legacy2
SnapmakerOrcaPre_try_legacy2:
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
  MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "EdgeSlicer is still running.$\nClose the program, then click Retry to continue, or Cancel to exit the installer." IDRETRY SnapmakerOrcaPre_check_loop
  Abort
SnapmakerOrcaPre_done:
  ; Only look for a previous install of OURS - never touch the official Snapmaker Orca entry.
  ; Every name this fork has shipped under is checked, newest first, so the first EdgeSlicer
  ; installer still finds an UltraOne or a Snapmaker-Ultra install and offers to remove it.
  ReadRegStr $0 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\EdgeSlicer" "UninstallString"
  StrCmp $0 "" 0 +2
  ReadRegStr $0 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\UltraOne" "UninstallString"
  StrCmp $0 "" 0 +2
  ReadRegStr $0 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Snapmaker-Ultra" "UninstallString"
  StrCmp $0 "" SnapmakerOrcaPre_read_hkcu_uninst SnapmakerOrcaPre_has_uninst
SnapmakerOrcaPre_read_hkcu_uninst:
  ReadRegStr $0 HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\EdgeSlicer" "UninstallString"
  StrCmp $0 "" 0 +2
  ReadRegStr $0 HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\UltraOne" "UninstallString"
  StrCmp $0 "" 0 +2
  ReadRegStr $0 HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Snapmaker-Ultra" "UninstallString"
SnapmakerOrcaPre_has_uninst:
  StrCmp $0 "" SnapmakerOrcaPre_finish

  MessageBox MB_YESNOCANCEL|MB_ICONEXCLAMATION "A previous version (EdgeSlicer, UltraOne or Snapmaker-Ultra) is already installed.$\n$\nDo you want to uninstall the old version before installing the new one?" /SD IDYES IDYES SnapmakerOrcaPre_uninst IDNO SnapmakerOrcaPre_finish
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
  CreateShortCut "$DESKTOP\EdgeSlicer.lnk" "$INSTDIR\EdgeSlicer.exe" "" "$INSTDIR\EdgeSlicer.exe" 0
  SetShellVarContext all
]=])
set(CPACK_NSIS_DELETE_ICONS_EXTRA [=[
  SetShellVarContext current
  Delete "$DESKTOP\EdgeSlicer.lnk"
  ; and the shortcuts the pre-rename installers left, if this is an upgrade of one
  Delete "$DESKTOP\UltraOne.lnk"
  Delete "$DESKTOP\Snapmaker-Ultra.lnk"
  SetShellVarContext all
]=])
set (CPACK_PACKAGE_CHECKSUM SHA256)
# A new product name is a new Add/Remove entry. The preinstall guard below still reads the
# old "Snapmaker-Ultra" key so an existing install is found and offered for uninstall.
set (CPACK_PACKAGE_INSTALL_REGISTRY_KEY "EdgeSlicer")
set (CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL OFF)
set (CPACK_NSIS_EXECUTABLES_DIRECTORY ".")
set(CPACK_PACKAGE_EXECUTABLES "EdgeSlicer;EdgeSlicer")
set (CPACK_RESOURCE_FILE_LICENSE ${CMAKE_SOURCE_DIR}/LICENSE.txt) # must also include in install command

# New GUID for the new product: sharing UltraOne's would make the installer treat an
# EdgeSlicer install as an upgrade of UltraOne and remove it in place.
set(CPACK_WIX_UPGRADE_GUID "3ac9f21d-6b40-4e77-9d18-58c0e4b7a2f6")
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
