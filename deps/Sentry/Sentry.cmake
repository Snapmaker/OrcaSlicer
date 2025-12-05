set(_sentry_platform_flags
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON
  -DSENTRY_BUILD_TESTS=OFF
  -DSENTRY_EXAMPLES=OFF
  -DSENTRY_CRASHPAD_BACKEND=ON
  -DSENTRY_ENABLE_INSTALL=ON
)

# Platform-specific CMake generator and flags
set(_sentry_cmake_generator "")
set(_sentry_build_config "Release")

if (WIN32)
  # Windows: build shared libs so we get sentry.dll
  set(_sentry_platform_flags  ${_sentry_platform_flags}
    -DSENTRY_TRANSPORT_WINHTTP=ON
    -DSENTRY_BUILD_SHARED_LIBS=ON
    -DCMAKE_BUILD_TYPE:STRING=RelWithDebInfo
    -DCMAKE_C_FLAGS_RELWITHDEBINFO:STRING=/Zi /O2
    -DCMAKE_CXX_FLAGS_RELWITHDEBINFO:STRING=/Zi /O2
    -DCMAKE_EXE_LINKER_FLAGS:STRING=/DEBUG
    -DCMAKE_SHARED_LINKER_FLAGS:STRING=/DEBUG
  )
  if (MSVC)
    set(_sentry_cmake_generator -G "Visual Studio 17 2022")
  endif()
elseif (APPLE)
  # macOS: Use Unix Makefiles (install will put libs in ${DESTDIR}/bin or lib)
  set(_sentry_platform_flags 
    ${_sentry_platform_flags}
    -DSENTRY_TRANSPORT_CURL=ON
    -DSENTRY_BUILD_SHARED_LIBS=OFF
    -DCMAKE_BUILD_TYPE:STRING=RelWithDebInfo
    -DCMAKE_C_FLAGS:STRING=-g -O2
    -DCMAKE_CXX_FLAGS:STRING=-g -O2
  )
  set(_sentry_cmake_generator -G "Unix Makefiles")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # Linux: Use Unix Makefiles
  set(_sentry_platform_flags 
    ${_sentry_platform_flags}
    -DSENTRY_TRANSPORT_CURL=ON
    -DSENTRY_BUILD_SHARED_LIBS=OFF
    -DCMAKE_BUILD_TYPE:STRING=RelWithDebInfo
    -DCMAKE_C_FLAGS:STRING=-g -O2
    -DCMAKE_CXX_FLAGS:STRING=-g -O2
  )
  set(_sentry_cmake_generator -G "Unix Makefiles")
endif ()

Snapmaker_Orca_add_cmake_project(Sentry
  GIT_REPOSITORY      https://github.com/getsentry/sentry-native.git
  GIT_TAG             0.12.1
  PATCH_COMMAND       ${GIT_EXECUTABLE} submodule update --init --recursive && ${CMAKE_COMMAND} -S external/crashpad -B external/crashpad/build ${_sentry_cmake_generator} -DCMAKE_BUILD_TYPE=Release && ${CMAKE_COMMAND} --build external/crashpad/build --config Release
  CMAKE_ARGS
    ${_sentry_cmake_generator}
    -DCMAKE_INSTALL_DATADIR:STRING=share
    ${_sentry_platform_flags}
)

if (MSVC)
    add_debug_dep(dep_Sentry)
endif ()
