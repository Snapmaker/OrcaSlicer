# FlutterMacOS.framework — copy from Flutter SDK into deps
# Auto-detects Flutter SDK from: FLUTTER_SDK_PATH, FLUTTER_HOME, Homebrew, or $PATH

if(NOT FLUTTER_SDK_PATH)
    # 1) Explicit FLUTTER_HOME env var (set by subosito/flutter-action on CI)
    if(DEFINED ENV{FLUTTER_HOME})
        set(FLUTTER_SDK_PATH "$ENV{FLUTTER_HOME}" CACHE PATH "Path to Flutter SDK root")
    endif()
endif()

if(NOT FLUTTER_SDK_PATH)
    # 2) Homebrew (macOS dev machines)
    file(GLOB FLUTTER_HOMEBREW_DIRS "/opt/homebrew/Caskroom/flutter/*")
    if(FLUTTER_HOMEBREW_DIRS)
        list(GET FLUTTER_HOMEBREW_DIRS 0 _flutter_latest)
        set(FLUTTER_SDK_PATH "${_flutter_latest}/flutter" CACHE PATH "Path to Flutter SDK root")
    endif()
endif()

if(NOT FLUTTER_SDK_PATH)
    # 3) Find flutter on $PATH and resolve symlinks to SDK root
    find_program(FLUTTER_EXECUTABLE flutter)
    if(FLUTTER_EXECUTABLE)
        get_filename_component(_flutter_bin "${FLUTTER_EXECUTABLE}" DIRECTORY)
        get_filename_component(FLUTTER_SDK_PATH "${_flutter_bin}" DIRECTORY)
        set(FLUTTER_SDK_PATH "${FLUTTER_SDK_PATH}" CACHE PATH "Path to Flutter SDK root")
    endif()
endif()

if(NOT FLUTTER_SDK_PATH)
    message(WARNING "Flutter SDK not found. Install Flutter or pass -DFLUTTER_SDK_PATH=/path/to/flutter")
else()
    set(_flutter_xcframework_dir "${FLUTTER_SDK_PATH}/bin/cache/artifacts/engine/darwin-x64/FlutterMacOS.xcframework/macos-arm64_x86_64/FlutterMacOS.framework")
    set(_flutter_framework_dest "${DESTDIR}/FlutterMacOS.framework")

    if(EXISTS "${_flutter_xcframework_dir}")
        add_custom_target(dep_Flutter ALL
            COMMAND ${CMAKE_COMMAND} -E echo "Copying FlutterMacOS.framework to deps..."
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${_flutter_xcframework_dir}" "${_flutter_framework_dest}"
            COMMENT "Copying FlutterMacOS.framework into ${_flutter_framework_dest}"
        )
        message(STATUS "Flutter deps: will copy FlutterMacOS.framework from ${FLUTTER_SDK_PATH}")
    else()
        message(WARNING "FlutterMacOS.framework not found at ${_flutter_xcframework_dir}. Run 'flutter precache' first.")
    endif()
endif()
