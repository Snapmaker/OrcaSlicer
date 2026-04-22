cmake_minimum_required(VERSION 3.13)
project(orca-slice-engine)

# Cloud slicing engine - minimal headless slicer
# Build with: cmake -DSLIC3R_GUI=OFF ..

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Source files
add_executable(orca-slice-engine
    main.cpp
    nanosvg.cpp  # nanosvg implementation for headless build
)

# Include directories
target_include_directories(orca-slice-engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_BINARY_DIR}
)

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

# Link libraries - only core libslic3r, no GUI
target_link_libraries(orca-slice-engine
    libslic3r
    cereal::cereal
    boost_libs
    TBB::tbb
    TBB::tbbmalloc
    nanosvg
    OpenSSL::SSL
    OpenSSL::Crypto
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

# Install target
install(TARGETS orca-slice-engine RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

message(STATUS "orca-slice-engine configured (headless mode)")
