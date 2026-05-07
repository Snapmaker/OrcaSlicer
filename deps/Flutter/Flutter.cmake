# Flutter engine artifacts — copy from Flutter SDK into deps
# Auto-detects Flutter SDK from: FLUTTER_SDK_PATH, FLUTTER_HOME, Homebrew, or $PATH
#
# Per-platform artifacts:
#   macOS   → FlutterMacOS.framework
#   Windows → flutter_windows.h + .dll + .lib, flutter_engine.dll
#   Linux   → flutter_linux.h + .so, flutter_engine.so

# ── Flutter SDK auto-detection (shared) ─────────────────────────────────

if(NOT FLUTTER_SDK_PATH)
    if(DEFINED ENV{FLUTTER_HOME})
        set(FLUTTER_SDK_PATH "$ENV{FLUTTER_HOME}" CACHE PATH "Path to Flutter SDK root")
    endif()
endif()

if(NOT FLUTTER_SDK_PATH AND APPLE)
    file(GLOB FLUTTER_HOMEBREW_DIRS "/opt/homebrew/Caskroom/flutter/*")
    if(FLUTTER_HOMEBREW_DIRS)
        list(GET FLUTTER_HOMEBREW_DIRS 0 _flutter_latest)
        set(FLUTTER_SDK_PATH "${_flutter_latest}/flutter" CACHE PATH "Path to Flutter SDK root")
    endif()
endif()

if(NOT FLUTTER_SDK_PATH)
    find_program(FLUTTER_EXECUTABLE flutter)
    if(FLUTTER_EXECUTABLE)
        get_filename_component(_flutter_bin "${FLUTTER_EXECUTABLE}" DIRECTORY)
        get_filename_component(FLUTTER_SDK_PATH "${_flutter_bin}" DIRECTORY)
        set(FLUTTER_SDK_PATH "${FLUTTER_SDK_PATH}" CACHE PATH "Path to Flutter SDK root")
    endif()
endif()

if(NOT FLUTTER_SDK_PATH)
    message(FATAL_ERROR "Flutter SDK not found. Install Flutter or pass -DFLUTTER_SDK_PATH=/path/to/flutter")
endif()

set(_engine_cache "${FLUTTER_SDK_PATH}/bin/cache/artifacts/engine")

# ── macOS ───────────────────────────────────────────────────────────────

if (APPLE)
    # Handle both arm64 and x86_64 on macOS
    if(CMAKE_OSX_ARCHITECTURES)
        if("${CMAKE_OSX_ARCHITECTURES}" MATCHES "arm64" AND "${CMAKE_OSX_ARCHITECTURES}" MATCHES "x86")
            set(_flutter_arch "macos-arm64_x86_64")
        elseif("${CMAKE_OSX_ARCHITECTURES}" MATCHES "arm64")
            set(_flutter_arch "macos-arm64_x86_64")
        elseif("${CMAKE_OSX_ARCHITECTURES}" MATCHES "x86")
            set(_flutter_arch "macos-arm64_x86_64")
        else()
            set(_flutter_arch "macos-arm64_x86_64")
        endif()
    else()
        set(_flutter_arch "macos-arm64_x86_64")
    endif()

    set(_flutter_fw_src "${_engine_cache}/darwin-x64/FlutterMacOS.xcframework/${_flutter_arch}/FlutterMacOS.framework")
    set(_flutter_fw_dest "${DESTDIR}/FlutterMacOS.framework")

    if(NOT EXISTS "${_flutter_fw_src}")
        message(FATAL_ERROR "FlutterMacOS.framework not found at ${_flutter_fw_src}. Run 'flutter precache --macos' first.")
    endif()

    add_custom_target(dep_Flutter ALL
        COMMAND ${CMAKE_COMMAND} -E echo "Copying FlutterMacOS.framework to deps..."
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${_flutter_fw_src}" "${_flutter_fw_dest}"
        COMMENT "Copying FlutterMacOS.framework → ${_flutter_fw_dest}"
    )
    message(STATUS "Flutter deps (macOS): FlutterMacOS.framework from ${FLUTTER_SDK_PATH}")

# ── Windows ─────────────────────────────────────────────────────────────

elseif (WIN32)
    set(_flutter_engine_dir "${_engine_cache}/windows-x64")
    set(_flutter_client "${_flutter_engine_dir}/cpp_client_wrapper")

    set(_flutter_headers "${_flutter_client}/include/flutter")
    set(_flutter_dll "${_flutter_engine_dir}/flutter_windows.dll")
    set(_flutter_lib "${_flutter_engine_dir}/flutter_windows.dll.lib")
    set(_flutter_engine_dll "${_flutter_engine_dir}/flutter_engine.dll")
    set(_flutter_icudtl "${_flutter_engine_dir}/icudtl.dat")

    if(NOT EXISTS "${_flutter_headers}")
        message(FATAL_ERROR "Flutter Windows artifacts not found at ${_flutter_headers}. Run 'flutter precache --windows' first.")
    endif()

    add_custom_target(dep_Flutter ALL
        # C++ wrapper headers (flutter_engine.h, flutter_view_controller.h, …)
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${_flutter_headers}" "${DESTDIR}/include/flutter"
        # C API headers at engine root (flutter_windows.h, flutter_export.h, …)
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_flutter_engine_dir}/flutter_windows.h" "${DESTDIR}/include/flutter/flutter_windows.h"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_flutter_engine_dir}/flutter_export.h" "${DESTDIR}/include/flutter/flutter_export.h"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_flutter_engine_dir}/flutter_messenger.h" "${DESTDIR}/include/flutter/flutter_messenger.h"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_flutter_engine_dir}/flutter_plugin_registrar.h" "${DESTDIR}/include/flutter/flutter_plugin_registrar.h"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_flutter_engine_dir}/flutter_texture_registrar.h" "${DESTDIR}/include/flutter/flutter_texture_registrar.h"
        # Import library
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_flutter_lib}" "${DESTDIR}/lib/flutter_windows.dll.lib"
        # Embedder DLL (at root, not in cpp_client_wrapper)
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_flutter_dll}" "${DESTDIR}/bin/flutter_windows.dll"
        # ICU data
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_flutter_icudtl}" "${DESTDIR}/bin/icudtl.dat"
        COMMENT "Copying Flutter Windows engine → ${DESTDIR}"
    )
    # flutter_engine.dll — optional, removed in newer Flutter SDKs (3.38+)
    if(EXISTS "${_flutter_engine_dll}")
        add_custom_command(TARGET dep_Flutter POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_flutter_engine_dll}" "${DESTDIR}/bin/flutter_engine.dll"
        )
    endif()
    message(STATUS "Flutter deps (Windows): engine from ${FLUTTER_SDK_PATH}")

# ── Linux ───────────────────────────────────────────────────────────────

elseif (UNIX AND NOT APPLE)
    set(_flutter_engine_dir "${_engine_cache}/linux-x64")
    set(_flutter_hdr_dir "${_flutter_engine_dir}/flutter_linux")
    set(_flutter_so "${_flutter_engine_dir}/libflutter_linux_gtk.so")
    set(_flutter_engine_so "${_flutter_engine_dir}/libflutter_engine.so")
    set(_flutter_icudtl "${_flutter_engine_dir}/icudtl.dat")

    if(NOT EXISTS "${_flutter_hdr_dir}")
        message(FATAL_ERROR "Flutter Linux artifacts not found at ${_flutter_hdr_dir}. Run 'flutter precache --linux' first.")
    endif()

    add_custom_target(dep_Flutter ALL
        # Headers — preserve flutter_linux/ dir for internal #include paths
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${_flutter_hdr_dir}" "${DESTDIR}/include/flutter/flutter_linux"
        # Shared libs
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_flutter_so}" "${DESTDIR}/lib/libflutter_linux_gtk.so"
        # ICU data
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_flutter_icudtl}" "${DESTDIR}/bin/icudtl.dat"
        COMMENT "Copying Flutter Linux engine → ${DESTDIR}"
    )
    # libflutter_engine.so — optional, removed in newer Flutter SDKs (3.38+)
    if(EXISTS "${_flutter_engine_so}")
        add_custom_command(TARGET dep_Flutter POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_flutter_engine_so}" "${DESTDIR}/lib/libflutter_engine.so"
        )
    endif()
    message(STATUS "Flutter deps (Linux): engine from ${FLUTTER_SDK_PATH}")
endif()
