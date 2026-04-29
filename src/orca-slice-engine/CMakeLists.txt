cmake_minimum_required(VERSION 3.13)
project(orca-slice-engine)

# Cloud slicing engine - minimal headless slicer
# Build with: cmake -DSLIC3R_GUI=OFF ..

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Source files
add_executable(orca-slice-engine
    main.cpp
    SliceEngine.cpp
    JsonReport.cpp
    Utils.cpp
    nanosvg.cpp  # nanosvg implementation for headless build
)

# Include directories
target_include_directories(orca-slice-engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_BINARY_DIR}
)

# Find cereal serialization library (header-only)
# When building as part of the main project, this is handled by the root CMakeLists.txt.
# When building standalone, we need to configure it ourselves.
if (NOT TARGET cereal::cereal)
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/modules/")
    find_package(cereal QUIET)
    if (TARGET cereal AND NOT TARGET cereal::cereal)
        # Fix the include path set by Findcereal.cmake (its relative "include" resolves
        # to this subdirectory, not the repo root where headers actually live)
        target_include_directories(cereal INTERFACE
            ${CMAKE_CURRENT_SOURCE_DIR}/../../deps/build/OrcaSlicer_dep/usr/local/include
        )
        add_library(cereal::cereal ALIAS cereal)
    elseif (NOT TARGET cereal)
        add_library(cereal INTERFACE)
        target_include_directories(cereal INTERFACE
            ${CMAKE_CURRENT_SOURCE_DIR}/../../deps/build/OrcaSlicer_dep/usr/local/include
        )
        add_library(cereal::cereal ALIAS cereal)
    endif()
endif()

# Compile definitions
target_compile_definitions(orca-slice-engine PRIVATE
    -DUSE_TBB
    -DTBB_USE_CAPTURED_EXCEPTION=0
    -DBOOST_ALL_NO_LIB
)

# For non-static builds, use dynamic Boost linking
if(NOT SLIC3R_STATIC)
    target_compile_definitions(orca-slice-engine PRIVATE BOOST_LOG_DYN_LINK)
endif()

# Link libraries - only core libslic3r and its dependencies
# libslic3r already pulls in all necessary dependencies transitively
target_link_libraries(orca-slice-engine
    libslic3r          # Core slicing library (pulls all necessary deps)
    cereal::cereal     # Serialization
    boost_libs         # Boost filesystem, log, etc.
    TBB::tbb           # Parallel processing
    TBB::tbbmalloc     # TBB memory allocator
    nanosvg            # SVG parsing (for thumbnails)
    OpenSSL::SSL       # HTTPS support
    OpenSSL::Crypto    # Crypto functions
)

# Platform-specific configurations
if (WIN32)
    target_link_libraries(orca-slice-engine
        Psapi.lib
        bcrypt.lib
    )
elseif (APPLE)
    target_link_libraries(orca-slice-engine
        "-liconv"
        "-framework IOKit"
        "-framework CoreFoundation"
    )
else ()
    # Linux
    target_link_libraries(orca-slice-engine
        ${CMAKE_DL_LIBS}
        Threads::Threads
    )
endif()

# Set output directory to build root
set_target_properties(orca-slice-engine PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/src/orca-slice-engine"
)

# Install target
install(TARGETS orca-slice-engine RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

message(STATUS "orca-slice-engine configured (headless mode)")
