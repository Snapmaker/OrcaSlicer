
include(ProcessorCount)
ProcessorCount(NPROC)

# OpenSSL 3.5 LTS. Notes for whoever touches this file next:
#  - The libs must land in <prefix>/lib (see --libdir below); CPython and Sentry look there.
#  - Nothing here installs a CMake package. Consumers use CMake's FindOpenSSL module through
#    CMAKE_PREFIX_PATH. OpenSSL itself installs lib/cmake/OpenSSL/OpenSSLConfig.cmake since 3.3;
#    nothing in this tree asks for it (no find_package(OpenSSL CONFIG), no
#    CMAKE_FIND_PACKAGE_PREFER_CONFIG).
#  - install_sw depends on build_programs, and up to 3.5 OpenSSL counts its ~200 test and fuzz
#    programs as programs (build_inst_programs arrived with 3.6). no-apps drops apps/openssl and,
#    per INSTALL.md, disables the tests with it; no-tests states that explicitly at no cost.
#    no-apps does not exist before 3.0 and Configure rejects unknown options: going back to a
#    1.1.1 URL means removing it again.
#  - deps/CMakeLists.txt probes the Flatpak runtime with find_package(OpenSSL 1.1...<3.2). Keep
#    that range: it must not match the GNOME runtime's OpenSSL (3.x, newer than 3.2), so that the
#    Flatpak builds this copy into /app, which is where python3.cmake's --with-openssl points.
#  - dep_CURL, dep_Sentry (macOS: links libssl/libcrypto into libsentry.dylib) and dep_python3
#    (_ssl, _hashlib) embed or compile against this library. ExternalProject stamps do not
#    cascade, so an existing deps tree needs those three rebuilt by hand after a version change.

if(DEFINED OPENSSL_ARCH)
    set(_cross_arch ${OPENSSL_ARCH})
else()
    if(WIN32)
        if("${DEPS_ARCH}" STREQUAL "arm64")
            set(_cross_arch "VC-WIN64-ARM")
        else()
            set(_cross_arch "VC-WIN64A")
        endif()
    elseif(APPLE)
        set(_cross_arch "darwin64-${CMAKE_OSX_ARCHITECTURES}-cc")
	endif()
endif()

if(WIN32)
    set(_openssl_msvc_env CC=cl CXX=cl RC=rc CL=/FS)
    # OpenSSL's perl Configure honors the CC environment variable, but the
    # VC-WIN64A makefile only works with cl (an unquoted clang-cl path with
    # spaces, e.g. exported by CLion, silently produces no .obj files and the
    # lib step fails with LNK1181). Pin the upstream toolchain.
    # Keep rc.exe resolved from the MSVC developer environment as well. The
    # absolute Windows SDK path contains spaces and OpenSSL's Configure writes it to
    # the generated nmake file without quoting, which skips .res generation.
    # /FS serializes access to OpenSSL's shared generated PDB when cl is
    # driven through nmake from a Ninja configure step.
    set(_conf_cmd ${CMAKE_COMMAND} -E env ${_openssl_msvc_env} perl Configure )
    set(_cross_comp_prefix_line "")
    set(_make_cmd ${CMAKE_COMMAND} -E env ${_openssl_msvc_env} nmake)
    set(_install_cmd ${CMAKE_COMMAND} -E env ${_openssl_msvc_env} nmake install_sw )
else()
    if(APPLE)
        set(_conf_cmd export MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET} && ./Configure -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET})
    else()
        set(_conf_cmd env "CC=${CMAKE_C_COMPILER}" "LDFLAGS=${CMAKE_EXE_LINKER_FLAGS}" "./config")
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
    DOWNLOAD_DIR ${DEP_DOWNLOAD_DIR}/OpenSSL
	CONFIGURE_COMMAND ${_conf_cmd} ${_cross_arch}
        "--openssldir=${DESTDIR}"
        "--prefix=${DESTDIR}"
        # OpenSSL's linux-x86_64 target sets multilib=64, so it installs to
        # <prefix>/lib64 while every other dep uses <prefix>/lib. CPython's
        # --with-openssl only ever emits -L<dir>/lib, so it misses the bundled
        # static libs and silently links the system OpenSSL instead. Pin libdir
        # so the prefix stays single-layout.
        "--libdir=lib"
        ${_cross_comp_prefix_line}
        no-shared
        no-asm
        no-apps
        no-tests
        no-ssl3-method
        no-dynamic-engine
    BUILD_IN_SOURCE ON
    BUILD_COMMAND ${_make_cmd} build_libs
    INSTALL_COMMAND ${_install_cmd}
)

if (CMAKE_GENERATOR MATCHES "Visual Studio")
    # OpenSSL builds with cl, but MSBuild runs nmake in this project's toolset
    # environment, and ClangCL's puts clang's headers first. Use the default.
    set_target_properties(dep_OpenSSL PROPERTIES VS_PLATFORM_TOOLSET "$(DefaultPlatformToolset)")
endif ()
