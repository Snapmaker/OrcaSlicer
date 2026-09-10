
include(ProcessorCount)
ProcessorCount(NPROC)

if(DEFINED OPENSSL_ARCH)
    set(_cross_arch ${OPENSSL_ARCH})
else()
    if(WIN32)
        set(_cross_arch "VC-WIN64A")
    elseif(APPLE)
        set(_cross_arch "darwin64-arm64-cc")
	endif()
endif()

if(WIN32)
    set(_conf_cmd perl Configure )
    set(_cross_comp_prefix_line "")
    configure_file(
        ${CMAKE_CURRENT_LIST_DIR}/build_windows.bat.in
        ${CMAKE_CURRENT_BINARY_DIR}/build_windows.bat
        @ONLY
    )
    configure_file(
        ${CMAKE_CURRENT_LIST_DIR}/build_windows_clean.bat.in
        ${CMAKE_CURRENT_BINARY_DIR}/build_windows_clean.bat
        @ONLY
    )
    set(_make_cmd "${CMAKE_CURRENT_BINARY_DIR}/build_windows.bat")
    set(_install_cmd ${_make_cmd} install_sw)
else()
    if(APPLE)
        set(_conf_cmd export MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET} && ./Configure -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET})
    else()
        set(_conf_cmd "./config")
    endif()
    set(_cross_comp_prefix_line "")
    set(_make_cmd make -j${NPROC})
    set(_install_cmd make -j${NPROC} install_sw)
    if (CMAKE_CROSSCOMPILING)
        set(_cross_comp_prefix_line "--cross-compile-prefix=${TOOLCHAIN_PREFIX}-")

        if (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "aarch64" OR ${CMAKE_SYSTEM_PROCESSOR} STREQUAL "arm64")
            set(_cross_arch "linux-aarch64")
        elseif (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "armhf") # For raspbian
            # TODO: verify
            set(_cross_arch "linux-armv4")
        endif ()
    endif ()
endif()

ExternalProject_Add(dep_OpenSSL
    #EXCLUDE_FROM_ALL ON
    URL "https://github.com/openssl/openssl/releases/download/openssl-3.5.7/openssl-3.5.7.tar.gz"
    URL_HASH SHA256=A8C0D28A529CA480F9F36CF5792E2CD21984552A3C8E4AA11A24AA31AEAC98E8
    # URL "https://github.com/openssl/openssl/archive/refs/tags/openssl-3.1.2.tar.gz"
    # URL_HASH SHA256=8c776993154652d0bb393f506d850b811517c8bd8d24b1008aef57fbe55d3f31
    DOWNLOAD_DIR ${DEP_DOWNLOAD_DIR}/OpenSSL
	CONFIGURE_COMMAND ${_conf_cmd} ${_cross_arch}
        "--openssldir=${DESTDIR}"
        "--prefix=${DESTDIR}"
        ${_cross_comp_prefix_line}
        no-shared
        no-asm
        no-apps
        no-ssl3-method
        no-dynamic-engine
    BUILD_IN_SOURCE ON
    BUILD_COMMAND ${_make_cmd} build_libs
    INSTALL_COMMAND ${_install_cmd}
)
