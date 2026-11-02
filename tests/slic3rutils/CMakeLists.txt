# slic3rutils tests require libslic3r_gui (contains Http.cpp).
# Skip entirely when GUI is disabled.
if (NOT SLIC3R_GUI)
    return()
endif()

get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)
add_executable(${_TEST_NAME}_tests
    ${_TEST_NAME}_tests_main.cpp
    SnapLogClient_test.cpp
    SnapshotService_test.cpp
    PlateController_test.cpp
    ProjectSession_test.cpp
    ExportSendCoordinator_test.cpp
    SelectionQuery_test.cpp
    ObjectEditCommands_test.cpp
    ArrangementService_test.cpp
    MixingGateKeeper_test.cpp
    FilamentService_test.cpp
    CalibrationFlow_test.cpp
    LayoutManager_test.cpp
    SceneRefreshCoordinator_test.cpp
    PlaterEventRouter_test.cpp
    )

# Link repair: on Windows libslic3r_gui imports app-only helper symbols
# (Slic3r::sentryReportLog / bury_point BURY_API / common_func / CBaseException),
# which are otherwise provided solely by the app targets in src/CMakeLists.txt.
# Without these the test executable cannot link in a GUI build (SLIC3R_SENTRY=ON).
target_sources(${_TEST_NAME}_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/sentry_wrapper/SentryWrapper.cpp
    ${CMAKE_SOURCE_DIR}/src/bury_cfg/bury_point.cpp
    ${CMAKE_SOURCE_DIR}/src/common_func/common_func.cpp
    ${CMAKE_SOURCE_DIR}/src/dev-utils/BaseException.cpp
    ${CMAKE_SOURCE_DIR}/src/dev-utils/StackWalker.cpp
    )
set_source_files_properties(${CMAKE_SOURCE_DIR}/src/bury_cfg/bury_point.cpp
    PROPERTIES COMPILE_DEFINITIONS BURY_EXPORTS)
if (SLIC3R_SENTRY AND TARGET sentry::sentry)
    target_link_libraries(${_TEST_NAME}_tests PRIVATE sentry::sentry)
endif ()
# This target links libslic3r_gui, whose BitmapCache.cpp already compiles the
# nanoSVG rasterizer implementation — drop test_nanosvg_impl (headless-only
# provider) to avoid LNK2005 duplicate-symbol errors in GUI builds.
list(REMOVE_ITEM TEST_LINK_LIBS test_nanosvg_impl)
# PlateController_test includes PartPlate.hpp (wx headers); wx include dirs are
# PRIVATE to libslic3r_gui (find_package runs in src/ scope). Rebuild them from
# the global findwxWidgets cache entries instead of re-running find_package.
if (WIN32 AND wxWidgets_ROOT_DIR AND wxWidgets_LIB_DIR)
    target_include_directories(${_TEST_NAME}_tests PRIVATE
        "${wxWidgets_ROOT_DIR}/include"
        "${wxWidgets_LIB_DIR}/mswu")
endif ()

find_package(JPEG REQUIRED)
find_package(PNG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(libnoise REQUIRED)
find_package(Qhull 7.2 QUIET)

if (SLIC3R_WX_STABLE)
    find_package(wxWidgets 3.0 REQUIRED COMPONENTS base core adv html gl aui net media webview)
else ()
    find_package(wxWidgets 3.1 REQUIRED COMPONENTS base core adv html gl aui net media webview)
endif ()
include(${wxWidgets_USE_FILE})

if(UNIX AND NOT APPLE)
    target_link_options(${_TEST_NAME}_tests PRIVATE "-Wl,--no-as-needed")
endif()
if(TARGET libslic3r_gui)
    target_link_libraries(${_TEST_NAME}_tests PRIVATE libslic3r_gui ${TEST_LINK_LIBS})
else()
    target_link_libraries(${_TEST_NAME}_tests PRIVATE ${TEST_LINK_LIBS})
endif()
if (MSVC)
    target_link_libraries(${_TEST_NAME}_tests PRIVATE Setupapi.lib)
endif ()
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

add_executable(gateway_tests
    gateway_tests_main.cpp
    ConnectionProcessManager_test.cpp
    GatewayProtocol_test.cpp
    GatewayService_test.cpp
    GatewayDevice_test.cpp
    GatewayFlutterResources_test.cpp
    GatewayAccount_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/slic3r/Utils/ConnectionProcessManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/slic3r/Utils/GatewayProtocol.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/slic3r/Utils/GatewayService.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/slic3r/Utils/GatewayDevice.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/slic3r/Utils/GatewayAccount.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/slic3r/Utils/Http.cpp
    )
target_link_libraries(gateway_tests test_common libslic3r boost_libs libcurl OpenSSL::SSL OpenSSL::Crypto)
target_compile_definitions(gateway_tests PRIVATE GATEWAY_FLUTTER_BUNDLE="${CMAKE_CURRENT_SOURCE_DIR}/../../resources/web/flutter_web/main.dart.js")
if (MSVC)
    target_compile_definitions(gateway_tests PRIVATE _WIN32_WINNT=0x0601 NOMINMAX)
endif ()
set_property(TARGET gateway_tests PROPERTY FOLDER "tests")

if (WIN32)
    if ("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
        Snapmaker_Orca_copy_dlls(COPY_DLLS "Debug" "d" output_dlls_Debug)
    elseif("${CMAKE_BUILD_TYPE}" STREQUAL "RelWithDebInfo")
        Snapmaker_Orca_copy_dlls(COPY_DLLS "RelWithDebInfo" "" output_dlls_Release)
    else()
        Snapmaker_Orca_copy_dlls(COPY_DLLS "Release" "" output_dlls_Release)
    endif()
endif()

catch_discover_tests(${_TEST_NAME}_tests TEST_PREFIX "${_TEST_NAME}: " EXTRA_ARGS ${CATCH_EXTRA_ARGS})
catch_discover_tests(gateway_tests TEST_PREFIX "gateway: " EXTRA_ARGS ${CATCH_EXTRA_ARGS})
# add_test(${_TEST_NAME}_tests ${_TEST_NAME}_tests "${CATCH_EXTRA_ARGS} exclude:[NotWorking]")

set(_FILAMENT_TEST_NAME filament_dropdown_mapping)
add_executable(${_FILAMENT_TEST_NAME}_tests FilamentDropDown_test.cpp)
target_compile_definitions(${_FILAMENT_TEST_NAME}_tests PRIVATE wxDEBUG_LEVEL=0)
target_include_directories(${_FILAMENT_TEST_NAME}_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/libslic3r
    ${PROJECT_SOURCE_DIR}/deps_src
    ${Boost_INCLUDE_DIR}
    ${wxWidgets_INCLUDE_DIRS})
if (WIN32 AND wxWidgets_LIB_DIR AND wxWidgets_CONFIGURATION)
    target_include_directories(${_FILAMENT_TEST_NAME}_tests PRIVATE
        "${wxWidgets_LIB_DIR}/${wxWidgets_CONFIGURATION}")
endif ()
target_link_libraries(${_FILAMENT_TEST_NAME}_tests PRIVATE
    Catch2::Catch2WithMain
    ${wxWidgets_LIBRARIES}
    JPEG::JPEG
    PNG::PNG
    ZLIB::ZLIB
    ${EXPAT_LIBRARIES})
set_property(TARGET ${_FILAMENT_TEST_NAME}_tests PROPERTY FOLDER "tests")
catch_discover_tests(${_FILAMENT_TEST_NAME}_tests TEST_PREFIX "${_FILAMENT_TEST_NAME}: " EXTRA_ARGS ${CATCH_EXTRA_ARGS})

set(_FILAMENT_SORT_TEST_NAME filament_sort)
# The shipped allow-list is a data contract with PresetUpdater (which deploys it) and with the
# loader under test, so the suite reads the real file instead of a copy that could drift from it.
set(_FILAMENT_ALLOW_LIST_FILE ${PROJECT_SOURCE_DIR}/resources/profiles/Snapmaker/filament/filament_allow_list.json)
file(TO_CMAKE_PATH "${_FILAMENT_ALLOW_LIST_FILE}" _FILAMENT_ALLOW_LIST_FILE)
add_executable(${_FILAMENT_SORT_TEST_NAME}_tests
    FilamentSort_test.cpp
    ${PROJECT_SOURCE_DIR}/src/slic3r/GUI/FilamentSort.cpp)
target_compile_definitions(${_FILAMENT_SORT_TEST_NAME}_tests PRIVATE
    wxDEBUG_LEVEL=0
    "FILAMENT_ALLOW_LIST_FILE=R\"(${_FILAMENT_ALLOW_LIST_FILE})\"")
target_include_directories(${_FILAMENT_SORT_TEST_NAME}_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/libslic3r
    ${PROJECT_SOURCE_DIR}/deps_src
    ${Boost_INCLUDE_DIR}
    ${wxWidgets_INCLUDE_DIRS})
if (WIN32 AND wxWidgets_LIB_DIR AND wxWidgets_CONFIGURATION)
    target_include_directories(${_FILAMENT_SORT_TEST_NAME}_tests PRIVATE
        "${wxWidgets_LIB_DIR}/${wxWidgets_CONFIGURATION}")
endif ()
target_link_libraries(${_FILAMENT_SORT_TEST_NAME}_tests PRIVATE
    Catch2::Catch2WithMain
    ${wxWidgets_LIBRARIES})
set_property(TARGET ${_FILAMENT_SORT_TEST_NAME}_tests PROPERTY FOLDER "tests")
catch_discover_tests(${_FILAMENT_SORT_TEST_NAME}_tests TEST_PREFIX "${_FILAMENT_SORT_TEST_NAME}: " EXTRA_ARGS ${CATCH_EXTRA_ARGS})
