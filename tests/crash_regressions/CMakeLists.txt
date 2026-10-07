cmake_minimum_required(VERSION 3.16)
project(SnapmakerCrashRegressions LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
get_filename_component(ORCA_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
enable_testing()
include("${ORCA_SOURCE_DIR}/cmake/catch2.cmake")
find_package(Boost 1.84 REQUIRED COMPONENTS filesystem)

add_executable(printer_metadata_validation_tests
    "${ORCA_SOURCE_DIR}/tests/slic3rutils/PrinterMetadataValidation_test.cpp")
target_include_directories(printer_metadata_validation_tests PRIVATE "${ORCA_SOURCE_DIR}/src")
target_compile_definitions(printer_metadata_validation_tests PRIVATE TEST_RESOURCES_DIR="${ORCA_SOURCE_DIR}/resources")
target_link_libraries(printer_metadata_validation_tests PRIVATE Catch2::Catch2WithMain Boost::filesystem)
catch_discover_tests(printer_metadata_validation_tests)

add_executable(line_radius_tests LineRadius_test.cpp)
target_include_directories(line_radius_tests PRIVATE
    "${ORCA_SOURCE_DIR}/src"
    "${ORCA_SOURCE_DIR}/deps_src"
    "${ORCA_SOURCE_DIR}/deps_src/eigen"
    "${ORCA_SOURCE_DIR}/deps_src/libigl")
target_link_libraries(line_radius_tests PRIVATE Catch2::Catch2WithMain Boost::filesystem)
catch_discover_tests(line_radius_tests)
