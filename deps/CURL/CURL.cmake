set(_curl_platform_flags 
  -DENABLE_IPV6:BOOL=ON
  -DENABLE_VERSIONED_SYMBOLS:BOOL=ON
  -DENABLE_THREADED_RESOLVER:BOOL=ON
  -DENABLE_MANUAL:BOOL=OFF
  -DCURL_DISABLE_LDAP:BOOL=ON
  -DCURL_DISABLE_LDAPS:BOOL=ON
  -DCURL_DISABLE_RTSP:BOOL=ON
  -DCURL_DISABLE_DICT:BOOL=ON
  -DCURL_DISABLE_TELNET:BOOL=ON
  -DCURL_DISABLE_POP3:BOOL=ON
  -DCURL_DISABLE_IMAP:BOOL=ON
  -DCURL_DISABLE_SMB:BOOL=ON
  -DCURL_DISABLE_SMTP:BOOL=ON
  -DCURL_DISABLE_GOPHER:BOOL=ON
  -DCURL_DISABLE_TFTP:BOOL=ON
  -DCURL_DISABLE_MQTT:BOOL=ON
  #-DHTTP_ONLY=ON

  -DCMAKE_USE_GSSAPI:BOOL=OFF
  -DCMAKE_USE_LIBSSH2:BOOL=OFF
  -DUSE_RTMP:BOOL=OFF
  -DUSE_NGHTTP2:BOOL=OFF
  -DUSE_MBEDTLS:BOOL=OFF
)

if (WIN32)
  #set(_curl_platform_flags  ${_curl_platform_flags} -DCMAKE_USE_SCHANNEL=ON)
  set(_curl_platform_flags  ${_curl_platform_flags} -DCMAKE_USE_OPENSSL=ON -DCURL_CA_PATH:STRING=none)
elseif (APPLE)
  set(_curl_platform_flags 
    
    ${_curl_platform_flags}

    #-DCMAKE_USE_SECTRANSP:BOOL=ON 
    -DCMAKE_USE_OPENSSL:BOOL=ON

    -DCURL_CA_PATH:STRING=none
  )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(_curl_platform_flags 

    ${_curl_platform_flags}

    -DCMAKE_USE_OPENSSL:BOOL=ON

    -DCURL_CA_PATH:STRING=none
    -DCURL_CA_BUNDLE:STRING=none
    -DCURL_CA_FALLBACK:BOOL=ON
  )
endif ()

if (BUILD_SHARED_LIBS)
  set(_curl_static OFF)
else()
  set(_curl_static ON)
endif()

# On non-Apple platforms, __builtin_available(macOS ...) is invalid C syntax.
# Pre-define HAVE_BUILTIN_AVAILABLE=0 to skip curl's try_compile test that
# would otherwise fail on Linux (GCC) and Windows (MSVC).
# On macOS, this feature is valid and should be detected normally.
if (APPLE)
  set(_curl_apple_flags "")
  set(_curl_patch_cmd "")
else()
  set(_curl_apple_flags -DHAVE_BUILTIN_AVAILABLE:INTERNAL=0)
  # CurlTests.c:568 uses __builtin_available(macOS 10.12, *) — Apple-Clang only.
  # GCC/MSVC don't define 'macOS'. Guard with #ifdef __APPLE__.
  # Use perl (not sed) for cross-platform -i compatibility (BSD sed needs -i '').
  set(_curl_patch_cmd PATCH_COMMAND perl -i -pe "if(/__builtin_available.*macOS/){s/^/#ifdef __APPLE__\\n/;s/\$/\\n#endif/}" CMake/CurlTests.c)
endif()

Snapmaker_Orca_add_cmake_project(CURL
  # GIT_REPOSITORY      https://github.com/curl/curl.git
  # GIT_TAG             curl-7_75_0
  URL                 https://github.com/curl/curl/archive/refs/tags/curl-7_75_0.zip
  URL_HASH            SHA256=a63ae025bb0a14f119e73250f2c923f4bf89aa93b8d4fafa4a9f5353a96a765a
  DEPENDS             ${ZLIB_PKG}
  ${_curl_patch_cmd}
  CMAKE_ARGS
    -DBUILD_TESTING:BOOL=OFF
    -DBUILD_CURL_EXE:BOOL=OFF
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DCURL_STATICLIB=${_curl_static}
    ${_curl_apple_flags}
    ${_curl_platform_flags}
)

if(NOT OPENSSL_FOUND)
  # (openssl may or may not be built)
  add_dependencies(dep_CURL ${OPENSSL_PKG})
endif()

if (MSVC)
    add_debug_dep(dep_CURL)
endif ()
