set(_srcdir ${CMAKE_CURRENT_LIST_DIR}/mpfr)

if (MSVC)
    set(_output  ${DESTDIR}/include/mpfr.h
                 ${DESTDIR}/include/mpf2mpfr.h
                 ${DESTDIR}/lib/libmpfr-4.lib 
                 ${DESTDIR}/bin/libmpfr-4.dll)

    add_custom_command(
        OUTPUT  ${_output}
        COMMAND ${CMAKE_COMMAND} -E copy ${_srcdir}/include/mpfr.h ${DESTDIR}/include/
        COMMAND ${CMAKE_COMMAND} -E copy ${_srcdir}/include/mpf2mpfr.h ${DESTDIR}/include/
        COMMAND ${CMAKE_COMMAND} -E copy ${_srcdir}/lib/win-${DEPS_ARCH}/libmpfr-4.lib ${DESTDIR}/lib/
        COMMAND ${CMAKE_COMMAND} -E copy ${_srcdir}/lib/win-${DEPS_ARCH}/libmpfr-4.dll ${DESTDIR}/bin/
    )

    add_custom_target(dep_MPFR SOURCES ${_output})

else ()

    if (APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)")
        # New logic only for macOS arm64.
        set(_mpfr_ccflags "-O2 -DNDEBUG -fPIC -DPIC -Wall -Wmissing-prototypes -Wpointer-arith -pedantic -fomit-frame-pointer -fno-common")
        set(_mpfr_build_tgt "")
        set(_cross_compile_arg "")

        set(_mpfr_build_arch aarch64)
        if (IS_CROSS_COMPILE)
            if (${CMAKE_OSX_ARCHITECTURES} MATCHES "arm")
                set(_mpfr_host_arch aarch64)
                set(_mpfr_host_arch_flags "-arch arm64")
            elseif (${CMAKE_OSX_ARCHITECTURES} MATCHES "x86_64")
                set(_mpfr_host_arch x86_64)
                set(_mpfr_host_arch_flags "-arch x86_64")
            endif()
            set(_mpfr_ccflags "${_mpfr_ccflags} ${_mpfr_host_arch_flags} -mmacosx-version-min=${DEP_OSX_TARGET}")
            set(_mpfr_build_tgt --build=${_mpfr_build_arch}-apple-darwin --host=${_mpfr_host_arch}-apple-darwin)
        else ()
            set(_mpfr_ccflags "${_mpfr_ccflags} -mmacosx-version-min=${DEP_OSX_TARGET}")
            set(_mpfr_build_tgt "--build=${_mpfr_build_arch}-apple-darwin")
        endif()

        if (CMAKE_CROSSCOMPILING)
            # TOOLCHAIN_PREFIX should be defined in the toolchain file
            if (DEFINED TOOLCHAIN_PREFIX AND NOT "${TOOLCHAIN_PREFIX}" STREQUAL "")
                set(_cross_compile_arg --host=${TOOLCHAIN_PREFIX})
            elseif (DEFINED _mpfr_host_arch AND NOT "${_mpfr_host_arch}" STREQUAL "")
                set(_cross_compile_arg --host=${_mpfr_host_arch}-apple-darwin)
            endif ()
        endif ()

        ExternalProject_Add(dep_MPFR
            URL https://www.mpfr.org/mpfr-4.2.2/mpfr-4.2.2.tar.bz2
            URL_HASH SHA256=9ad62c7dc910303cd384ff8f1f4767a655124980bb6d8650fe62c815a231bb7b
            DOWNLOAD_DIR ${DEP_DOWNLOAD_DIR}/MPFR
            BUILD_IN_SOURCE ON
            CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env
                              CFLAGS=${_mpfr_ccflags}
                              CXXFLAGS=${_mpfr_ccflags}
                              ./configure ${_cross_compile_arg} --prefix=${DESTDIR} --enable-shared=no --enable-static=yes --with-gmp=${DESTDIR} ${_mpfr_build_tgt}
            BUILD_COMMAND make -j
            INSTALL_COMMAND make install
            DEPENDS dep_GMP
        )
    else ()
        # Keep original behavior on other platforms.
        set(_cross_compile_arg "")
        if (CMAKE_CROSSCOMPILING)
            # TOOLCHAIN_PREFIX should be defined in the toolchain file
            set(_cross_compile_arg --host=${TOOLCHAIN_PREFIX})
        endif ()

        ExternalProject_Add(dep_MPFR
            URL https://www.mpfr.org/mpfr-4.2.2/mpfr-4.2.2.tar.bz2
            URL_HASH SHA256=9ad62c7dc910303cd384ff8f1f4767a655124980bb6d8650fe62c815a231bb7b
            DOWNLOAD_DIR ${DEP_DOWNLOAD_DIR}/MPFR
            BUILD_IN_SOURCE ON
            CONFIGURE_COMMAND autoreconf -f -i && 
                              env "CFLAGS=${_gmp_ccflags}" "CXXFLAGS=${_gmp_ccflags}" ./configure ${_cross_compile_arg} --prefix=${DESTDIR} --enable-shared=no --enable-static=yes --with-gmp=${DESTDIR} ${_gmp_build_tgt}
            BUILD_COMMAND make -j
            INSTALL_COMMAND make install
            DEPENDS dep_GMP
        )
    endif ()
endif ()
