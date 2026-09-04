cmake_minimum_required(VERSION 3.13)
project(Snapmaker_Orca-native)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include dev-utils for encoding check and other utilities
add_subdirectory(dev-utils)
# add_subdirectory(avrdude)
# Note: semver and hints are now included from deps_src/CMakeLists.txt

# Add MQTT library
add_subdirectory(mqtt)

# Set MQTT paths for use in slic3r GUI
set(PAHO_MQTT_CPP_PATH "${CMAKE_CURRENT_SOURCE_DIR}/mqtt" CACHE PATH "Paho MQTT C++ path")
set(PAHO_MQTT_C_PATH "${CMAKE_CURRENT_SOURCE_DIR}/mqtt/externals/paho-mqtt-c" CACHE PATH "Paho MQTT C path")

add_subdirectory(libslic3r)

if (SLIC3R_GUI)
    # imgui, imguizmo, and hidapi are now included from deps_src

    if(WIN32)
        message(STATUS "WXWIN environment set to: $ENV{WXWIN}")
    elseif(UNIX)
        set(wxWidgets_USE_UNICODE ON)
        # Linux: wxWidgets_USE_STATIC is set after wx-config is resolved (bundled wx is static;
        # distro wx is almost always shared-only and breaks wx-config --static=yes).
        if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
            if (SLIC3R_STATIC)
                set(wxWidgets_USE_STATIC ON)
            else ()
                set(wxWidgets_USE_STATIC OFF)
            endif ()
        endif ()
    endif()

    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # FindwxWidgets uses find_program(... ONLY_CMAKE_FIND_ROOT_PATH), so wx-config is not
        # discovered under CMAKE_PREFIX_PATH/bin unless we set wxWidgets_CONFIG_EXECUTABLE.
        # Bundled deps wx is GTK3 + static (see deps/wxWidgets/wxWidgets.cmake + build_linux.sh).
        if (wxWidgets_CONFIG_EXECUTABLE AND NOT EXISTS "${wxWidgets_CONFIG_EXECUTABLE}")
            unset(wxWidgets_CONFIG_EXECUTABLE CACHE)
        endif ()
        if (NOT wxWidgets_CONFIG_EXECUTABLE AND NOT DEFINED ENV{WX_CONFIG})
            set(_wx_search_prefixes "")
            if (NOT DEFINED SNAPMAKER_ORCA_TOP_SOURCE_DIR)
                get_filename_component(SNAPMAKER_ORCA_TOP_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" DIRECTORY)
            endif ()
            list(APPEND _wx_search_prefixes "${SNAPMAKER_ORCA_TOP_SOURCE_DIR}/deps/build/destdir/usr/local")
            foreach (_wx_p IN LISTS CMAKE_PREFIX_PATH)
                list(APPEND _wx_search_prefixes "${_wx_p}")
            endforeach ()
            list(REMOVE_DUPLICATES _wx_search_prefixes)
            set(_wx_cfg_candidates
                wx-config
                wx-config-3.2
                wx-config-3.1
                wx-config-3.0)
            set(_wx_cfg_done FALSE)
            foreach (_wx_prefix ${_wx_search_prefixes})
                if (_wx_cfg_done OR NOT EXISTS "${_wx_prefix}/bin")
                    continue()
                endif ()
                foreach (_wx_name ${_wx_cfg_candidates})
                    if (EXISTS "${_wx_prefix}/bin/${_wx_name}")
                        set(wxWidgets_CONFIG_EXECUTABLE "${_wx_prefix}/bin/${_wx_name}" CACHE FILEPATH
                            "wxWidgets wx-config (Orca deps / CMAKE_PREFIX_PATH)" FORCE)
                        message(STATUS "Using wx-config: ${wxWidgets_CONFIG_EXECUTABLE}")
                        set(_wx_cfg_done TRUE)
                        break()
                    endif ()
                endforeach ()
                if (NOT _wx_cfg_done)
                    file(GLOB _wx_glob_cfg
                        LIST_DIRECTORIES false
                        "${_wx_prefix}/bin/wx-config-*"
                        "${_wx_prefix}/bin/wxgtk*u-*-config"
                        "${_wx_prefix}/bin/wxgtk*-*-config")
                    list(SORT _wx_glob_cfg)
                    if (_wx_glob_cfg)
                        list(GET _wx_glob_cfg 0 _wx_one)
                        set(wxWidgets_CONFIG_EXECUTABLE "${_wx_one}" CACHE FILEPATH
                            "wxWidgets wx-config (glob under prefix bin)" FORCE)
                        message(STATUS "Using wx-config: ${wxWidgets_CONFIG_EXECUTABLE}")
                        set(_wx_cfg_done TRUE)
                    endif ()
                    unset(_wx_glob_cfg)
                    unset(_wx_one)
                endif ()
                if (_wx_cfg_done)
                    break()
                endif ()
            endforeach ()
            unset(_wx_cfg_done)
            unset(_wx_cfg_candidates)
            unset(_wx_prefix)
            unset(_wx_p)
            unset(_wx_name)
            unset(_wx_search_prefixes)
        endif ()

        # FindwxWidgets's own find_program(... ONLY_CMAKE_FIND_ROOT_PATH) skips normal PATH when
        # CMAKE_FIND_ROOT_PATH is empty, so /usr/bin/wx-config is never found unless we set it here.
        if (NOT wxWidgets_CONFIG_EXECUTABLE OR NOT EXISTS "${wxWidgets_CONFIG_EXECUTABLE}")
            if (NOT DEFINED SNAPMAKER_ORCA_TOP_SOURCE_DIR)
                get_filename_component(SNAPMAKER_ORCA_TOP_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" DIRECTORY)
            endif ()
            # Prefer any wx-config* from deps (find_program NAMES order can pick /usr/bin first).
            file(GLOB _orca_deps_wx_cfgs LIST_DIRECTORIES false
                "${SNAPMAKER_ORCA_TOP_SOURCE_DIR}/deps/build/destdir/usr/local/bin/wx-config*"
                "${SNAPMAKER_ORCA_TOP_SOURCE_DIR}/deps/build/destdir/usr/local/bin/wxgtk*config")
            list(SORT _orca_deps_wx_cfgs)
            if (_orca_deps_wx_cfgs)
                list(GET _orca_deps_wx_cfgs 0 _orca_wx_cfg_pick)
                set(wxWidgets_CONFIG_EXECUTABLE "${_orca_wx_cfg_pick}" CACHE FILEPATH
                    "wxWidgets wx-config (deps install)" FORCE)
                message(STATUS "Using wx-config: ${wxWidgets_CONFIG_EXECUTABLE}")
            else ()
                find_program(_orca_wx_config_sys
                    NAMES wx-config wx-config-3.2 wx-config-3.1 wx-config-3.0
                    PATHS
                        "${SNAPMAKER_ORCA_TOP_SOURCE_DIR}/deps/build/destdir/usr/local/bin"
                        /usr/local/bin
                        /usr/bin
                    NO_CMAKE_FIND_ROOT_PATH
                )
                if (_orca_wx_config_sys)
                    set(wxWidgets_CONFIG_EXECUTABLE "${_orca_wx_config_sys}" CACHE FILEPATH
                        "wxWidgets wx-config (PATH / standard locations)" FORCE)
                    message(STATUS "Using wx-config: ${wxWidgets_CONFIG_EXECUTABLE}")
                endif ()
                unset(_orca_wx_config_sys CACHE)
            endif ()
            unset(_orca_deps_wx_cfgs)
            unset(_orca_wx_cfg_pick)
        endif ()

        # Orca dependency wx is static; match wx-config selection to avoid empty --libs.
        if (wxWidgets_CONFIG_EXECUTABLE)
            if (NOT DEFINED SNAPMAKER_ORCA_TOP_SOURCE_DIR)
                get_filename_component(SNAPMAKER_ORCA_TOP_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" DIRECTORY)
            endif ()
            string(FIND "${wxWidgets_CONFIG_EXECUTABLE}" "${SNAPMAKER_ORCA_TOP_SOURCE_DIR}/deps/build" _wx_bundled_idx)
            if (NOT _wx_bundled_idx LESS 0)
                set(wxWidgets_USE_STATIC ON)
                message(STATUS "wxWidgets from bundled deps: wxWidgets_USE_STATIC=ON")
            else ()
                set(wxWidgets_USE_STATIC OFF)
                if (SLIC3R_STATIC)
                    message(WARNING "SLIC3R_STATIC is ON but wx-config is not from bundled deps; "
                        "using shared system wx. For a static stack like build_linux.sh, build deps (-d) "
                        "so wx installs under deps/build/destdir/usr/local.")
                endif ()
            endif ()
            unset(_wx_bundled_idx)
        endif ()

        set(wxWidgets_CONFIG_OPTIONS "--toolkit=gtk${SLIC3R_GTK}")
        if (wxWidgets_CONFIG_EXECUTABLE)
            execute_process(
                COMMAND sh "${wxWidgets_CONFIG_EXECUTABLE}" ${wxWidgets_CONFIG_OPTIONS} --cxxflags
                RESULT_VARIABLE _wx_cxxflags_rc
                OUTPUT_QUIET ERROR_QUIET)
            if (NOT _wx_cxxflags_rc EQUAL 0)
                execute_process(
                    COMMAND sh "${wxWidgets_CONFIG_EXECUTABLE}" --toolkit=gtk3 --cxxflags
                    RESULT_VARIABLE _wx_gtk3_rc
                    OUTPUT_QUIET ERROR_QUIET)
                if (_wx_gtk3_rc EQUAL 0)
                    set(wxWidgets_CONFIG_OPTIONS "--toolkit=gtk3")
                    message(STATUS "wx-config: gtk${SLIC3R_GTK} failed; using --toolkit=gtk3 (typical for Orca deps)")
                else ()
                    execute_process(
                        COMMAND sh "${wxWidgets_CONFIG_EXECUTABLE}" --toolkit=gtk2 --cxxflags
                        RESULT_VARIABLE _wx_gtk2_rc
                        OUTPUT_QUIET ERROR_QUIET)
                    if (_wx_gtk2_rc EQUAL 0)
                        set(wxWidgets_CONFIG_OPTIONS "--toolkit=gtk2")
                        message(STATUS "wx-config: gtk${SLIC3R_GTK} failed; using --toolkit=gtk2")
                    endif ()
                    unset(_wx_gtk2_rc)
                endif ()
                unset(_wx_gtk3_rc)
            endif ()
            unset(_wx_cxxflags_rc)
        endif ()

        # FindwxWidgets verifies each -lfoo with find_library(); ensure prefix lib dirs are searched.
        foreach (_wx_lp IN LISTS CMAKE_PREFIX_PATH)
            foreach (_wx_sub lib lib64)
                if (EXISTS "${_wx_lp}/${_wx_sub}")
                    list(APPEND CMAKE_LIBRARY_PATH "${_wx_lp}/${_wx_sub}")
                endif ()
            endforeach ()
        endforeach ()
        if (DEFINED SNAPMAKER_ORCA_TOP_SOURCE_DIR AND EXISTS "${SNAPMAKER_ORCA_TOP_SOURCE_DIR}/deps/build/destdir/usr/local/lib")
            list(APPEND CMAKE_LIBRARY_PATH "${SNAPMAKER_ORCA_TOP_SOURCE_DIR}/deps/build/destdir/usr/local/lib")
        endif ()
        if (DEFINED SNAPMAKER_ORCA_TOP_SOURCE_DIR AND EXISTS "${SNAPMAKER_ORCA_TOP_SOURCE_DIR}/deps/build/destdir/usr/local/lib64")
            list(APPEND CMAKE_LIBRARY_PATH "${SNAPMAKER_ORCA_TOP_SOURCE_DIR}/deps/build/destdir/usr/local/lib64")
        endif ()
        # wx-config often emits -L/usr/lib/x86_64-linux-gnu; FindwxWidgets still uses find_library()
        # with short names — ensure typical distro multiarch dirs are on CMAKE_LIBRARY_PATH.
        foreach (_orca_sys_lib IN ITEMS
                /usr/lib/x86_64-linux-gnu
                /usr/lib/aarch64-linux-gnu
                /usr/lib64
                /usr/lib)
            if (EXISTS "${_orca_sys_lib}")
                list(APPEND CMAKE_LIBRARY_PATH "${_orca_sys_lib}")
            endif ()
        endforeach ()
        unset(_orca_sys_lib)
        if (CMAKE_LIBRARY_PATH)
            list(REMOVE_DUPLICATES CMAKE_LIBRARY_PATH)
        endif ()
        unset(_wx_lp)
        unset(_wx_sub)

        if (NOT wxWidgets_CONFIG_EXECUTABLE OR NOT EXISTS "${wxWidgets_CONFIG_EXECUTABLE}")
            message(FATAL_ERROR "wxWidgets: no usable wx-config. On Debian/Ubuntu install e.g. "
                "libwxgtk3.2-dev (and webview package), or build Orca deps: ./build_linux.sh -d")
        endif ()

        if (SLIC3R_WX_STABLE)
            find_package(wxWidgets 3.0 REQUIRED COMPONENTS base core adv html gl aui net media webview)
        else ()
            find_package(wxWidgets 3.1 REQUIRED COMPONENTS base core adv html gl aui net media webview)
        endif ()
    else ()
        find_package(wxWidgets 3.1 REQUIRED COMPONENTS html adv gl core base webview aui net media)
    endif ()

    if(UNIX)
        message(STATUS "wx-config path: ${wxWidgets_CONFIG_EXECUTABLE}")
    endif()

    include(${wxWidgets_USE_FILE})

    find_package(JPEG QUIET)

    string(REGEX MATCH "wxpng" WX_PNG_BUILTIN ${wxWidgets_LIBRARIES})
    if (PNG_FOUND AND NOT WX_PNG_BUILTIN)
        list(FILTER wxWidgets_LIBRARIES EXCLUDE REGEX png)
        list(APPEND wxWidgets_LIBRARIES ${PNG_LIBRARIES})
    endif ()

    string(REGEX MATCH "wxjpeg" WX_JPEG_BUILTIN ${wxWidgets_LIBRARIES})
    if (JPEG_FOUND AND NOT WX_JPEG_BUILTIN)
        list(FILTER wxWidgets_LIBRARIES EXCLUDE REGEX jpeg)
        list(APPEND wxWidgets_LIBRARIES ${JPEG_LIBRARIES})
    endif ()

    string(REGEX MATCH "wxexpat" WX_EXPAT_BUILTIN ${wxWidgets_LIBRARIES})
    if (EXPAT_FOUND AND NOT WX_EXPAT_BUILTIN)
        list(FILTER wxWidgets_LIBRARIES EXCLUDE REGEX expat)
        list(APPEND wxWidgets_LIBRARIES ${EXPAT_LIBRARIES})
    endif ()
    
    # This is an issue in the new wxWidgets cmake build, doesn't deal with librt
    find_library(LIBRT rt)
    if(LIBRT)
        list(APPEND wxWidgets_LIBRARIES ${LIBRT})
    endif()

    # This fixes a OpenGL linking issue on OSX. wxWidgets cmake build includes
    # wrong libs for opengl in the link line and it does not link to it by himself.
    # libslic3r_gui will link to opengl anyway, so lets override wx
    list(FILTER wxWidgets_LIBRARIES EXCLUDE REGEX OpenGL)

#    list(REMOVE_ITEM wxWidgets_LIBRARIES oleacc)
    message(STATUS "wx libs: ${wxWidgets_LIBRARIES}")

    add_subdirectory(slic3r)
endif()

if(ORCA_TOOLS)
    # Snapmaker_Orca_profile_validator
    if(APPLE)
        add_executable(Snapmaker_Orca_profile_validator MACOSX_BUNDLE dev-utils/Snapmaker_Orca_profile_validator.cpp)
        set_target_properties(Snapmaker_Orca_profile_validator PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_BUNDLE_NAME "Snapmaker_Orca Profile Validator"
            MACOSX_BUNDLE_BUNDLE_VERSION "${SLIC3R_VERSION}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${SLIC3R_VERSION}"
            MACOSX_BUNDLE_IDENTIFIER "com.Snapmaker.Snapmaker_Orca.profile-validator"
            MACOSX_BUNDLE_COPYRIGHT "Copyright © 2024-2026 Snapmaker. All rights reserved."
            MACOSX_BUNDLE_GUI_IDENTIFIER "com.Snapmaker.Snapmaker_Orca.profile-validator"
        )
    else()
        add_executable(Snapmaker_Orca_profile_validator dev-utils/Snapmaker_Orca_profile_validator.cpp)
    endif()
    target_link_libraries(Snapmaker_Orca_profile_validator libslic3r boost_headeronly libcurl OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(Snapmaker_Orca_profile_validator PRIVATE -DBOOST_ALL_NO_LIB -DBOOST_USE_WINAPI_VERSION=0x602 -DBOOST_SYSTEM_USE_UTF8)
    if(WIN32)
        target_link_libraries(Snapmaker_Orca_profile_validator bcrypt.lib)
    endif()
endif()

# Create a slic3r executable
# Process mainfests for various platforms.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/dev-utils/platform/msw/Snapmaker_Orca.rc.in ${CMAKE_CURRENT_BINARY_DIR}/Snapmaker_Orca.rc @ONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/dev-utils/platform/msw/Snapmaker_Orca.manifest.in ${CMAKE_CURRENT_BINARY_DIR}/Snapmaker_Orca.manifest @ONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/dev-utils/platform/osx/Info.plist.in ${CMAKE_CURRENT_BINARY_DIR}/Info.plist @ONLY)
if (WIN32)
    add_library(Snapmaker_Orca SHARED Snapmaker_Orca.cpp Snapmaker_Orca.hpp dev-utils/BaseException.cpp dev-utils/BaseException.h dev-utils/StackWalker.cpp dev-utils/StackWalker.h sentry_wrapper/SentryWrapper.hpp sentry_wrapper/SentryWrapper.cpp bury_cfg/bury_point.hpp bury_cfg/bury_point.cpp common_func/common_func.hpp common_func/common_func.cpp)
    target_compile_definitions(Snapmaker_Orca PRIVATE BURY_EXPORTS)
else ()
    add_executable(Snapmaker_Orca Snapmaker_Orca.cpp Snapmaker_Orca.hpp sentry_wrapper/SentryWrapper.hpp sentry_wrapper/SentryWrapper.cpp bury_cfg/bury_point.hpp bury_cfg/bury_point.cpp common_func/common_func.hpp common_func/common_func.cpp)
endif ()

if (MINGW)
    target_link_options(Snapmaker_Orca PUBLIC "-Wl,-allow-multiple-definition")
    set_target_properties(Snapmaker_Orca PROPERTIES PREFIX "")
endif (MINGW)

if (NOT WIN32 AND NOT APPLE)
    # Binary name on unix like systems (Linux, Unix)
    set_target_properties(Snapmaker_Orca PROPERTIES OUTPUT_NAME "snapmaker-orca")
    set(SLIC3R_APP_CMD "snapmaker-orca")
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/dev-utils/platform/unix/build_linux_image.sh.in ${CMAKE_CURRENT_BINARY_DIR}/build_linux_image.sh USE_SOURCE_PERMISSIONS @ONLY)
endif ()

target_link_libraries(Snapmaker_Orca libslic3r cereal::cereal)
# target_include_directories(OrcaSlicer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/slic3r)
if (APPLE)
#    add_compile_options(-stdlib=libc++)
#    add_definitions(-DBOOST_THREAD_DONT_USE_CHRONO -DBOOST_NO_CXX11_RVALUE_REFERENCES -DBOOST_THREAD_USES_MOVE)
    # -liconv: boost links to libiconv by default
    target_link_libraries(Snapmaker_Orca "-liconv -framework IOKit" "-framework CoreFoundation" "-framework AVFoundation" "-framework AVKit" "-framework CoreMedia" "-framework VideoToolbox" -lc++)

elseif (MSVC)
    # Manifest is provided through Snapmaker_Orca.rc, don't generate your own.
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /MANIFEST:NO")
else ()
    # Boost on Raspberry-Pi does not link to pthreads explicitely.
    target_link_libraries(Snapmaker_Orca ${CMAKE_DL_LIBS} -lstdc++ Threads::Threads pangoft2-1.0)
endif ()

# Add the Slic3r GUI library, libcurl, OpenGL and GLU libraries.
if (SLIC3R_GUI)
#    target_link_libraries(Snapmaker_Orca ws2_32 uxtheme setupapi libslic3r_gui ${wxWidgets_LIBRARIES})
target_link_libraries(Snapmaker_Orca libslic3r_gui)
    if (MSVC)
        # Generate debug symbols even in release mode.
        target_link_options(Snapmaker_Orca PUBLIC "$<$<CONFIG:RELEASE>:/DEBUG>")
        target_link_libraries(Snapmaker_Orca user32.lib Setupapi.lib)
    elseif (MINGW)
        target_link_libraries(Snapmaker_Orca ws2_32 uxtheme setupapi)
    elseif (APPLE)
        target_link_libraries(Snapmaker_Orca "-framework OpenGL")
    else ()
        target_link_libraries(Snapmaker_Orca -ldl)
    endif ()
    #if (WIN32)
    #    find_library(PSAPI_LIB NAMES Psapi)
    #    target_link_libraries(Snapmaker_Orca ${PSAPI_LIB})
    #endif ()
endif ()

# Sentry crash reporting integration
if (SLIC3R_SENTRY)
    target_compile_definitions(Snapmaker_Orca PUBLIC SLIC3R_SENTRY)
    target_link_libraries(Snapmaker_Orca sentry::sentry)
endif()

# On Windows, a shim application is required to produce a console / non console version of the Slic3r application.
# Also the shim may load the Mesa software OpenGL renderer if the default renderer does not support OpenGL 2.0 and higher.
if (WIN32)
    if (MINGW)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -municode")
    endif()

    
    add_executable(Snapmaker_Orca_app_gui WIN32 Snapmaker_Orca_app_msvc.cpp sentry_wrapper/SentryWrapper.cpp common_func/common_func.cpp ${CMAKE_CURRENT_BINARY_DIR}/Snapmaker_Orca.rc)
    # Generate debug symbols even in release mode.
    if(MSVC)
        target_link_options(Snapmaker_Orca_app_gui PUBLIC "$<$<CONFIG:RELEASE>:/DEBUG>")
    endif()
    target_compile_definitions(Snapmaker_Orca_app_gui PRIVATE -DSLIC3R_WRAPPER_NOCONSOLE)
    add_dependencies(Snapmaker_Orca_app_gui Snapmaker_Orca)
    set_target_properties(Snapmaker_Orca_app_gui PROPERTIES OUTPUT_NAME "snapmaker-orca")
    
    if (SLIC3R_SENTRY)
        target_link_libraries(Snapmaker_Orca_app_gui PRIVATE boost_headeronly sentry::sentry nlohmann_json Snapmaker_Orca)
    else()
        target_link_libraries(Snapmaker_Orca_app_gui PRIVATE boost_headeronly nlohmann_json Snapmaker_Orca)
    endif()
    if (SLIC3R_SENTRY)
        target_compile_definitions(Snapmaker_Orca_app_gui PRIVATE SLIC3R_SENTRY)
    endif()
endif ()

# Link the resources dir to where Slic3r GUI expects it
set(output_dlls_Release "")
set(output_dlls_Debug "")
set(output_dlls_RelWithDebInfo "")
if (WIN32)
    # This has to be a separate target due to the windows command line lenght limits
    add_custom_target(COPY_DLLS ALL DEPENDS Snapmaker_Orca)

    if (CMAKE_CONFIGURATION_TYPES)
        foreach (CONF ${CMAKE_CONFIGURATION_TYPES})
            file(TO_NATIVE_PATH "${CMAKE_CURRENT_BINARY_DIR}/${CONF}" WIN_CONF_OUTPUT_DIR)
            file(TO_NATIVE_PATH "${CMAKE_CURRENT_BINARY_DIR}/${CONF}/resources" WIN_RESOURCES_SYMLINK)
            add_custom_command(TARGET Snapmaker_Orca POST_BUILD
                COMMAND if exist "${WIN_CONF_OUTPUT_DIR}" "("
                        if not exist "${WIN_RESOURCES_SYMLINK}" "("
                            mklink /J "${WIN_RESOURCES_SYMLINK}" "${SLIC3R_RESOURCES_DIR_WIN}"
                        ")"
                    ")"
                COMMENT "Symlinking the resources directory into the build tree"
                VERBATIM
            )
        endforeach ()

        # For multi-config generators (Visual Studio), copy DLLs for all configurations
        foreach (CONF ${CMAKE_CONFIGURATION_TYPES})
            if ("${CONF}" STREQUAL "Debug")
                Snapmaker_Orca_copy_dlls(COPY_DLLS "${CONF}" "d" output_dlls_Debug)
            elseif ("${CONF}" STREQUAL "Release")
                Snapmaker_Orca_copy_dlls(COPY_DLLS "${CONF}" "" output_dlls_Release)
            elseif ("${CONF}" STREQUAL "RelWithDebInfo")
                Snapmaker_Orca_copy_dlls(COPY_DLLS "${CONF}" "" output_dlls_RelWithDebInfo)
            else()
                # MinSizeRel or other configs
                Snapmaker_Orca_copy_dlls(COPY_DLLS "${CONF}" "" _unused_dlls)
            endif()
        endforeach ()
    else ()
        # Single-config generators (Ninja, Makefiles)
        file(TO_NATIVE_PATH "${CMAKE_CURRENT_BINARY_DIR}/resources" WIN_RESOURCES_SYMLINK)
        add_custom_command(TARGET Snapmaker_Orca POST_BUILD
            COMMAND if not exist "${WIN_RESOURCES_SYMLINK}" "(" mklink /J "${WIN_RESOURCES_SYMLINK}" "${SLIC3R_RESOURCES_DIR_WIN}" ")"
            COMMENT "Symlinking the resources directory into the build tree"
            VERBATIM
        )
        # Copy DLLs for single-config generators
        if ("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
            Snapmaker_Orca_copy_dlls(COPY_DLLS "Debug" "d" output_dlls_Debug)
        else()
            Snapmaker_Orca_copy_dlls(COPY_DLLS "${CMAKE_BUILD_TYPE}" "" output_dlls_Release)
        endif()
    endif ()


else ()
    if (APPLE AND NOT CMAKE_MACOSX_BUNDLE)
        # On OSX, the name of the binary matches the name of the Application.
        add_custom_command(TARGET Snapmaker_Orca POST_BUILD
            COMMAND ln -sf Snapmaker_Orca snapmaker-orca
            WORKING_DIRECTORY "$<TARGET_FILE_DIR:Snapmaker_Orca>"
            VERBATIM)
        # Copy Sentry crashpad_handler to executable directory for non-bundle builds (e.g., Xcode debugging)
        if (SLIC3R_SENTRY)
            add_custom_command(TARGET Snapmaker_Orca POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_PREFIX_PATH}/bin/crashpad_handler" "$<TARGET_FILE_DIR:Snapmaker_Orca>/crashpad_handler"
                COMMAND sh -c "codesign --force --sign - '$<TARGET_FILE_DIR:Snapmaker_Orca>/crashpad_handler' 2>/dev/null || true"
                COMMENT "Copying crashpad_handler for Sentry crash reporting"
                VERBATIM)
            # Copy libsentry.dylib to executable directory for non-bundle builds
            add_custom_command(TARGET Snapmaker_Orca POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_PREFIX_PATH}/lib/libsentry.dylib" "$<TARGET_FILE_DIR:Snapmaker_Orca>/libsentry.dylib"
                COMMAND sh -c "codesign --force --sign - '$<TARGET_FILE_DIR:Snapmaker_Orca>/libsentry.dylib' 2>/dev/null || true"
                COMMENT "Copying libsentry.dylib for Sentry crash reporting"
                VERBATIM)
        endif()
    else ()
        # Linux/Unix: CLI::setup resolves resources as <binary>/../../resources (Multi-Config:
        # build/src/Release/snapmaker-orca -> build/src/resources). Without this symlink, OnInit
        # fails wxCHECK_MSG(wxDirExists(resources_dir)) and wxEntry returns -1 (exit 255).
        if (UNIX AND NOT APPLE)
            add_custom_command(TARGET Snapmaker_Orca POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E rm -f "$<TARGET_FILE_DIR:Snapmaker_Orca>/../resources"
                COMMAND ${CMAKE_COMMAND} -E create_symlink "${SLIC3R_RESOURCES_DIR}" "$<TARGET_FILE_DIR:Snapmaker_Orca>/../resources"
                COMMENT "Symlinking resources directory for Linux dev build"
                VERBATIM)
        else ()
            add_custom_command(TARGET Snapmaker_Orca POST_BUILD
                WORKING_DIRECTORY "$<TARGET_FILE_DIR:Snapmaker_Orca>"
                VERBATIM)
        endif ()
    endif ()
    if (XCODE)
        # Because of Debug/Release/etc. configurations (similar to MSVC) the slic3r binary is located in an extra level
        set(BIN_RESOURCES_DIR "${CMAKE_CURRENT_BINARY_DIR}/resources")
        set(BIN_CONF_DIR "Debug")
    else ()
        set(BIN_RESOURCES_DIR "${CMAKE_CURRENT_BINARY_DIR}/../resources")
    endif ()
    if (CMAKE_MACOSX_BUNDLE)
        set(MACOSX_BUNDLE_ICON_FILE Icon.icns)
        set_target_properties(Snapmaker_Orca PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_BUNDLE_NAME "Snapmaker Orca"
            MACOSX_BUNDLE_SHORT_VERSION_STRING ${Snapmaker_VERSION}
            MACOSX_BUNDLE_COPYRIGHT "Copyright © 2024-2026 Snapmaker. All rights reserved.")
        # Get the bundle directory by going up from the executable to the .app bundle
        if (CMAKE_CONFIGURATION_TYPES)
            set(BIN_RESOURCES_DIR "$<TARGET_FILE_DIR:Snapmaker_Orca>/../Resources")
        else()
            set(BIN_RESOURCES_DIR "$<TARGET_FILE_DIR:Snapmaker_Orca>/../Resources")
        endif()
        # Create symlink for macOS bundle Resources directory
        # Remove existing Resources if it exists (could be from previous build), then create symlink
        add_custom_command(TARGET Snapmaker_Orca POST_BUILD
            COMMAND sh -c "rm -rf \"$<TARGET_FILE_DIR:Snapmaker_Orca>/../Resources\" || true"
            COMMAND ${CMAKE_COMMAND} -E create_symlink "${SLIC3R_RESOURCES_DIR}" "$<TARGET_FILE_DIR:Snapmaker_Orca>/../Resources"
            COMMENT "Symlinking the resources directory into the build tree"
            VERBATIM)
        # Copy Sentry crashpad_handler to MacOS directory (same directory as executable)
        if (SLIC3R_SENTRY)
            add_custom_command(TARGET Snapmaker_Orca POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_PREFIX_PATH}/bin/crashpad_handler" "$<TARGET_FILE_DIR:Snapmaker_Orca>/crashpad_handler"
                COMMAND sh -c "codesign --force --sign - '$<TARGET_FILE_DIR:Snapmaker_Orca>/crashpad_handler' 2>/dev/null || true"
                COMMENT "Copying and signing crashpad_handler for Sentry crash reporting"
                VERBATIM)
            # Copy libsentry.dylib to Frameworks directory
            add_custom_command(TARGET Snapmaker_Orca POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:Snapmaker_Orca>/../Frameworks"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_PREFIX_PATH}/lib/libsentry.dylib" "$<TARGET_FILE_DIR:Snapmaker_Orca>/../Frameworks/libsentry.dylib"
                COMMAND sh -c "codesign --force --sign - '$<TARGET_FILE_DIR:Snapmaker_Orca>/../Frameworks/libsentry.dylib' 2>/dev/null || true"
                COMMENT "Copying and signing libsentry.dylib for Sentry crash reporting"
                VERBATIM)
            # Update the rpath for libsentry.dylib
            add_custom_command(TARGET Snapmaker_Orca POST_BUILD
                COMMAND install_name_tool -change "@rpath/libsentry.dylib" "@executable_path/../Frameworks/libsentry.dylib" "$<TARGET_FILE:Snapmaker_Orca>" || true            
                COMMENT "Updating rpath for libsentry.dylib"
                VERBATIM)
        endif()
    endif()
endif ()

# Slic3r binary install target. Default build type is release in case no CMAKE_BUILD_TYPE is provided.
if( ("${CMAKE_BUILD_TYPE}" STREQUAL "Release") OR ("${CMAKE_BUILD_TYPE}" STREQUAL "") )
    set (build_type "Release")
    set(CMAKE_BUILD_POSTFIX "")
elseif ("${CMAKE_BUILD_TYPE}" STREQUAL "RelWithDebInfo")
    set (build_type "RelWithDebInfo")
    set(CMAKE_BUILD_POSTFIX "")
else()
    set (build_type "Debug")
    set(CMAKE_BUILD_POSTFIX "d")
endif()
message(STATUS "libslic3r-CMAKE_BUILD_TYPE: ${build_type}")
message(STATUS "CMAKE_CURRENT_BINARY_DIR: ${CMAKE_CURRENT_BINARY_DIR}")
if (WIN32)
    install(TARGETS Snapmaker_Orca RUNTIME DESTINATION ".")
    if (MSVC)
        install(TARGETS Snapmaker_Orca_app_gui RUNTIME DESTINATION ".")
    endif ()
    install(FILES ${output_dlls_${build_type}} DESTINATION ".")
else ()
    install(TARGETS Snapmaker_Orca RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" BUNDLE DESTINATION ${CMAKE_INSTALL_BINDIR})
endif ()
