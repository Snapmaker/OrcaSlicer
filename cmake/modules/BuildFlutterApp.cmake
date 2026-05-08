# BuildFlutterApp.cmake
# Builds the Flutter application from source during the CMake build phase.
#
# Defines:
#   build_flutter_app(FLUTTER_APP_DIR <dir> OUTPUT_DIR <dir>)
#
# Outputs per platform:
#   macOS   → App.framework
#   Windows → flutter_app.dll + data/flutter_assets/
#   Linux   → lib/libflutter_app.so + data/flutter_assets/

function(build_flutter_app)
    set(oneValueArgs FLUTTER_APP_DIR OUTPUT_DIR)
    cmake_parse_arguments(BFA "" "${oneValueArgs}" "" ${ARGN})

    if(NOT BFA_FLUTTER_APP_DIR)
        set(BFA_FLUTTER_APP_DIR "${CMAKE_SOURCE_DIR}/flutter_app")
    endif()
    if(NOT BFA_OUTPUT_DIR)
        set(BFA_OUTPUT_DIR "${CMAKE_BINARY_DIR}/flutter_app")
    endif()

    # ── Auto-detect Flutter SDK ──────────────────────────────────────────
    set(_flutter_sdk "")
    if(DEFINED ENV{FLUTTER_HOME})
        set(_flutter_sdk "$ENV{FLUTTER_HOME}")
    endif()
    if(NOT _flutter_sdk AND APPLE)
        file(GLOB _homebrew_dirs "/opt/homebrew/Caskroom/flutter/[0-9]*")
        if(_homebrew_dirs)
            list(SORT _homebrew_dirs COMPARE NATURAL ORDER DESCENDING)
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

    if(NOT _flutter_sdk)
        message(FATAL_ERROR "Flutter SDK not found. Install Flutter or set FLUTTER_HOME.")
    endif()

    set(_flutter "${_flutter_sdk}/bin/flutter")
    message(STATUS "Flutter SDK: ${_flutter_sdk}")

    # ── macOS ────────────────────────────────────────────────────────────
    if(APPLE)
        set(_fw_src  "${BFA_FLUTTER_APP_DIR}/build/macos/Build/Products/Release/App.framework")
        set(_fw_dest "${BFA_OUTPUT_DIR}/App.framework")
        set(_stamp   "${BFA_OUTPUT_DIR}/.flutter_build_macos")

        add_custom_command(
            OUTPUT "${_stamp}"
            COMMAND "${_flutter}" build macos --release
            COMMAND ${CMAKE_COMMAND} -E make_directory "${BFA_OUTPUT_DIR}"
            COMMAND rm -rf "${_fw_dest}"
            COMMAND cp -R "${_fw_src}" "${_fw_dest}"
            COMMAND ${CMAKE_COMMAND} -E touch "${_stamp}"
            WORKING_DIRECTORY "${BFA_FLUTTER_APP_DIR}"
            COMMENT "Building Flutter app (macOS release)"
            DEPENDS
                "${BFA_FLUTTER_APP_DIR}/pubspec.yaml"
                "${BFA_FLUTTER_APP_DIR}/lib/main.dart"
        )
        add_custom_target(flutter_app ALL DEPENDS "${_stamp}")
        set(FLUTTER_APP_FRAMEWORK "${_fw_dest}" CACHE PATH "Built Flutter App.framework" FORCE)

    # ── Windows ──────────────────────────────────────────────────────────
    elseif(WIN32)
        # Flutter build produces a full MSBuild output. After install:
        #   build/windows/x64/runner/Release/data/app.so   (AOT)
        #   build/windows/x64/runner/Release/data/flutter_assets/
        #   build/windows/x64/runner/Release/data/icudtl.dat
        set(_release_dir "${BFA_FLUTTER_APP_DIR}/build/windows/x64/runner/Release")
        set(_stamp "${BFA_OUTPUT_DIR}/.flutter_build_windows")

        add_custom_command(
            OUTPUT "${_stamp}"
            COMMAND "${_flutter}" build windows --release
            COMMAND ${CMAKE_COMMAND} -E make_directory "${BFA_OUTPUT_DIR}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_release_dir}/data/app.so" "${BFA_OUTPUT_DIR}/flutter_app.dll"
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${_release_dir}/data" "${BFA_OUTPUT_DIR}/data"
            COMMAND ${CMAKE_COMMAND} -E touch "${_stamp}"
            WORKING_DIRECTORY "${BFA_FLUTTER_APP_DIR}"
            COMMENT "Building Flutter app (Windows release)"
            DEPENDS
                "${BFA_FLUTTER_APP_DIR}/pubspec.yaml"
                "${BFA_FLUTTER_APP_DIR}/lib/main.dart"
        )
        add_custom_target(flutter_app ALL DEPENDS "${_stamp}")
        set(FLUTTER_APP_DLL "${BFA_OUTPUT_DIR}/flutter_app.dll" CACHE PATH "Built Flutter app DLL" FORCE)
        set(FLUTTER_APP_DATA "${BFA_OUTPUT_DIR}/data" CACHE PATH "Built Flutter app data" FORCE)

    # ── Linux ────────────────────────────────────────────────────────────
    elseif(UNIX)
        set(_src_dir "${BFA_FLUTTER_APP_DIR}/build/linux/x64/release/bundle")
        set(_stamp   "${BFA_OUTPUT_DIR}/.flutter_build_linux")

        add_custom_command(
            OUTPUT "${_stamp}"
            COMMAND "${_flutter}" build linux --release
            COMMAND ${CMAKE_COMMAND} -E make_directory "${BFA_OUTPUT_DIR}/lib"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_src_dir}/lib/libapp.so" "${BFA_OUTPUT_DIR}/lib/libflutter_app.so"
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${_src_dir}/data" "${BFA_OUTPUT_DIR}/data"
            COMMAND ${CMAKE_COMMAND} -E touch "${_stamp}"
            WORKING_DIRECTORY "${BFA_FLUTTER_APP_DIR}"
            COMMENT "Building Flutter app (Linux release)"
            DEPENDS
                "${BFA_FLUTTER_APP_DIR}/pubspec.yaml"
                "${BFA_FLUTTER_APP_DIR}/lib/main.dart"
        )
        add_custom_target(flutter_app ALL DEPENDS "${_stamp}")
        set(FLUTTER_APP_SO "${BFA_OUTPUT_DIR}/lib/libflutter_app.so" CACHE PATH "Built Flutter app SO" FORCE)
        set(FLUTTER_APP_DATA "${BFA_OUTPUT_DIR}/data" CACHE PATH "Built Flutter app data" FORCE)
    endif()

    # ── Rebuild helper target (force re-run) ─────────────────────────────
    add_custom_target(flutter_app_rebuild
        COMMAND ${CMAKE_COMMAND} -E remove "${_stamp}"
        COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target flutter_app
        COMMENT "Forcing Flutter app rebuild"
    )

    message(STATUS "Flutter app: ${BFA_FLUTTER_APP_DIR} → ${BFA_OUTPUT_DIR}")
endfunction()
