# slic3rutils tests require libslic3r_gui (contains Http.cpp).
# Skip entirely when GUI is disabled.
if (NOT SLIC3R_GUI)
    return()
endif()

get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)
add_executable(${_TEST_NAME}_tests
    ${_TEST_NAME}_tests_main.cpp
    SnapLogClient_test.cpp
    )

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
    GatewayMachineSnapshot_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/slic3r/Utils/ConnectionProcessManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/slic3r/Utils/GatewayProtocol.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/slic3r/Utils/GatewayService.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/slic3r/Utils/GatewayDevice.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/slic3r/GUI/GatewayMachineSnapshot.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/slic3r/Utils/Http.cpp
    )
target_link_libraries(gateway_tests test_common libslic3r boost_libs libcurl OpenSSL::SSL OpenSSL::Crypto)
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
