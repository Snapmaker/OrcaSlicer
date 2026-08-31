get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)
add_executable(${_TEST_NAME}_tests
    ${_TEST_NAME}_tests_main.cpp
    )

target_link_libraries(${_TEST_NAME}_tests test_common libslic3r_gui libslic3r)
if (MSVC)
    target_link_libraries(${_TEST_NAME}_tests Setupapi.lib)
endif ()
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

add_executable(gateway_tests
    gateway_tests_main.cpp
    ConnectionProcessManager_test.cpp
    GatewayProtocol_test.cpp
    GatewayService_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/slic3r/Utils/ConnectionProcessManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/slic3r/Utils/GatewayProtocol.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/slic3r/Utils/GatewayService.cpp
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

catch_discover_tests(${_TEST_NAME}_tests TEST_PREFIX "${_TEST_NAME}: ")
catch_discover_tests(gateway_tests TEST_PREFIX "gateway: ")
# add_test(${_TEST_NAME}_tests ${_TEST_NAME}_tests "${CATCH_EXTRA_ARGS} exclude:[NotWorking]")
