# The paho MQTT client is POSIX-only here: the regression test forks a child process so a crash
# of the client under test cannot take the Catch2 runner down.
if (NOT UNIX)
    return()
endif()

get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)
add_executable(${_TEST_NAME}_tests SSLContextFailure_test.cpp)

# Deliberately not test_common: that force-includes catch_main.hpp, which pulls in libslic3r,
# and this test needs nothing but Catch2 and the MQTT library the application links.
target_link_libraries(${_TEST_NAME}_tests PRIVATE Catch2::Catch2WithMain paho-mqttpp3-static)
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

catch_discover_tests(${_TEST_NAME}_tests TEST_PREFIX "${_TEST_NAME}: " EXTRA_ARGS ${CATCH_EXTRA_ARGS})
