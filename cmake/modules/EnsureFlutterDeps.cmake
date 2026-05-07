# EnsureFlutterDeps.cmake
# Auto-populate Flutter engine artifacts from Flutter SDK into deps
# if they don't already exist in CMAKE_PREFIX_PATH.
#
# Call after CMAKE_PREFIX_PATH is set up:
#   ensure_flutter_deps()

function(ensure_flutter_deps)
    # ── Auto-detect Flutter SDK ──────────────────────────────────────────
    set(_flutter_sdk "")

    if(DEFINED ENV{FLUTTER_HOME})
        set(_flutter_sdk "$ENV{FLUTTER_HOME}")
    endif()

    if(NOT _flutter_sdk AND APPLE)
        file(GLOB _homebrew_dirs "/opt/homebrew/Caskroom/flutter/*")
        if(_homebrew_dirs)
            list(GET _homebrew_dirs 0 _latest)
            set(_flutter_sdk "${_latest}/flutter")
        endif()
    endif()

    if(NOT _flutter_sdk)
        find_program(_flutter_exe flutter)
        if(_flutter_exe)
            get_filename_component(_bin_dir "${_flutter_exe}" DIRECTORY)
            get_filename_component(_flutter_sdk "${_bin_dir}" DIRECTORY)
        endif()
    endif()

    set(_engine_cache "${_flutter_sdk}/bin/cache/artifacts/engine")

    # ── macOS ────────────────────────────────────────────────────────────
    if(APPLE)
        if(NOT EXISTS "${CMAKE_PREFIX_PATH}/FlutterMacOS.framework")
            if(NOT _flutter_sdk)
                message(FATAL_ERROR "Flutter SDK not found and FlutterMacOS.framework not in deps. Install Flutter or build deps first.")
            endif()

            if(CMAKE_OSX_ARCHITECTURES)
                if("${CMAKE_OSX_ARCHITECTURES}" MATCHES "arm64" AND "${CMAKE_OSX_ARCHITECTURES}" MATCHES "x86")
                    set(_fw_arch "macos-arm64_x86_64")
                elseif("${CMAKE_OSX_ARCHITECTURES}" MATCHES "arm64")
                    set(_fw_arch "macos-arm64_x86_64")
                else()
                    set(_fw_arch "macos-arm64_x86_64")
                endif()
            else()
                set(_fw_arch "macos-arm64_x86_64")
            endif()

            set(_fw_src "${_engine_cache}/darwin-x64/FlutterMacOS.xcframework/${_fw_arch}/FlutterMacOS.framework")
            if(NOT EXISTS "${_fw_src}")
                message(FATAL_ERROR "FlutterMacOS.framework not found at ${_fw_src}. Run 'flutter precache --macos' first.")
            endif()

            message(STATUS "Populating FlutterMacOS.framework from Flutter SDK → deps")
            file(COPY "${_fw_src}" DESTINATION "${CMAKE_PREFIX_PATH}/FlutterMacOS.framework")
        endif()

    # ── Windows ──────────────────────────────────────────────────────────
    elseif(WIN32)
        if(NOT EXISTS "${CMAKE_PREFIX_PATH}/include/flutter/flutter_windows.h")
            if(NOT _flutter_sdk)
                message(FATAL_ERROR "Flutter SDK not found and Flutter Windows headers not in deps. Install Flutter or build deps first.")
            endif()

            set(_hdr_src "${_engine_cache}/windows-x64/cpp_client_wrapper/include/flutter")
            set(_dll_src "${_engine_cache}/windows-x64/flutter_windows.dll")
            set(_lib_src "${_engine_cache}/windows-x64/flutter_windows.lib")

            if(NOT EXISTS "${_hdr_src}")
                message(FATAL_ERROR "Flutter Windows artifacts not found at ${_hdr_src}. Run 'flutter precache --windows' first.")
            endif()

            message(STATUS "Populating Flutter Windows engine from Flutter SDK → deps")
            file(COPY "${_hdr_src}" DESTINATION "${CMAKE_PREFIX_PATH}/include/flutter")
            file(COPY "${_dll_src}" DESTINATION "${CMAKE_PREFIX_PATH}/bin/")
            file(COPY "${_lib_src}" DESTINATION "${CMAKE_PREFIX_PATH}/lib/")
            if(EXISTS "${_engine_cache}/windows-x64/flutter_engine.dll")
                file(COPY "${_engine_cache}/windows-x64/flutter_engine.dll"
                     DESTINATION "${CMAKE_PREFIX_PATH}/bin/")
            endif()
            file(COPY "${_engine_cache}/windows-x64/icudtl.dat"
                 DESTINATION "${CMAKE_PREFIX_PATH}/bin/")
        endif()

    # ── Linux ────────────────────────────────────────────────────────────
    elseif(UNIX AND NOT APPLE)
        if(NOT EXISTS "${CMAKE_PREFIX_PATH}/include/flutter/flutter_linux.h")
            if(NOT _flutter_sdk)
                message(FATAL_ERROR "Flutter SDK not found and Flutter Linux headers not in deps. Install Flutter or build deps first.")
            endif()

            set(_hdr_dir "${_engine_cache}/linux-x64/flutter_linux")
            set(_so_src  "${_engine_cache}/linux-x64/libflutter_linux_gtk.so")

            if(NOT EXISTS "${_hdr_dir}")
                message(FATAL_ERROR "Flutter Linux artifacts not found at ${_hdr_dir}. Run 'flutter precache --linux' first.")
            endif()

            message(STATUS "Populating Flutter Linux engine from Flutter SDK → deps")
            # Preserve flutter_linux/ directory name for internal #include paths
            file(COPY "${_hdr_dir}/" DESTINATION "${CMAKE_PREFIX_PATH}/include/flutter/flutter_linux")
            file(COPY "${_so_src}" DESTINATION "${CMAKE_PREFIX_PATH}/lib/")
            if(EXISTS "${_engine_cache}/linux-x64/libflutter_engine.so")
                file(COPY "${_engine_cache}/linux-x64/libflutter_engine.so"
                     DESTINATION "${CMAKE_PREFIX_PATH}/lib/")
            endif()
            file(COPY "${_engine_cache}/linux-x64/icudtl.dat"
                 DESTINATION "${CMAKE_PREFIX_PATH}/bin/")
        endif()
    endif()
endfunction()
