get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)
add_executable(${_TEST_NAME}_tests
	${_TEST_NAME}_tests.cpp
	test_helpers.cpp
	test_helpers.hpp
	test_cooling.cpp
	test_extrusion_entity.cpp
	test_extrusion_processor.cpp
	test_fill.cpp
	test_flow.cpp
	test_gcode_timing.cpp
	test_gcodewriter.cpp
	test_model.cpp
	test_multifilament.cpp
	test_multi_nozzle_layer_height.cpp
	test_perimeters.cpp
	test_print.cpp
	test_printobject.cpp
	test_skirt_brim.cpp
	test_slicing_pipeline_hook.cpp
	test_support_material.cpp
	test_bed_temperature.cpp
	test_trianglemesh.cpp
	test_wipe_tower.cpp
	)
# libslic3r_cgal links CGAL privately, so the GMP/MPFR libraries are needed at link time.
# They are not propagated to this test target; find them under the deps prefix or the
# system paths, and only add them when found to avoid linking a NOTFOUND placeholder.
find_library(GMP_LIB  NAMES libgmp-10  gmp  PATHS "${CMAKE_PREFIX_PATH}/lib")
find_library(MPFR_LIB NAMES libmpfr-4 mpfr PATHS "${CMAKE_PREFIX_PATH}/lib")
if (GMP_LIB AND MPFR_LIB)
    target_link_libraries(${_TEST_NAME}_tests test_common libslic3r Catch2::Catch2WithMain OpenSSL::Crypto ${GMP_LIB} ${MPFR_LIB})
else()
    target_link_libraries(${_TEST_NAME}_tests test_common libslic3r Catch2::Catch2WithMain OpenSSL::Crypto)
endif()
if (WIN32)
    target_link_libraries(${_TEST_NAME}_tests bcrypt.lib)
endif()
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

orcaslicer_copy_test_dlls()

orcaslicer_discover_tests(${_TEST_NAME}_tests)
