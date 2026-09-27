get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)

add_executable(${_TEST_NAME}_tests
    ${_TEST_NAME}_tests.cpp
    test_3mf.cpp
    test_aabbindirect.cpp
    test_appconfig.cpp
    test_arachne_walls.cpp
    test_arrange.cpp
    test_bambu_networking.cpp
    test_buildvolume.cpp
    test_calib.cpp
    test_clipper_offset.cpp
    test_clipper_utils.cpp
    test_config.cpp
    test_config_variant_expansion.cpp
    test_custom_gcode.cpp
    test_toolordering_nozzle_group.cpp
    test_preset_bundle_loading.cpp
    test_preset_setting_id.cpp
    test_preset_diff.cpp
    test_vendor_cache.cpp
    test_profile_load_util.cpp
    test_preset_options.cpp
    test_elephant_foot_compensation.cpp
    test_fill_corner_smoothing.cpp
    test_filament_mixer.cpp
    test_fill_plane_path.cpp
    test_geometry.cpp
    test_mixed_filament.cpp
    test_mixed_filament_color_golden.cpp
    test_local_z_order_optimizer.cpp
    test_multimaterial_segmentation.cpp
    test_placeholder_parser.cpp
    test_polygon.cpp
    test_mutable_polygon.cpp
    test_mutable_priority_queue.cpp
    test_minimum_spanning_tree.cpp
    test_nozzle_volume_type.cpp
    test_nozzle_filament_presets.cpp # Snapmaker Orca: filament presets follow the nozzle size of their tool head
    test_first_run_presets.cpp # Snapmaker Orca: the presets the setup wizard leaves for a Snapmaker U1
    test_snapmaker_flow_compat.cpp # Snapmaker Orca: older project layouts of the per flow type filament keys
    test_sswcp_protocol.cpp # Snapmaker Orca: device protocol conversion (upstream #807)
    test_snapmaker_hf_profiles.cpp # Snapmaker Orca: High Flow columns of the shipped U1 profiles
    test_step.cpp
    test_stl.cpp
    test_triangle_selector.cpp
    test_meshboolean.cpp
    test_mesh_lod.cpp # Snapmaker Orca: level rule and reduced meshes of the render LOD (upstream #737)
    test_marchingsquares.cpp
    test_lay_on_face.cpp
    test_model.cpp
    test_utils.cpp
    test_timeutils.cpp
    test_voronoi.cpp
    test_wipe_tower_estimate.cpp
    test_wipe_tower.cpp
    test_wipe_path.cpp
    test_optimizers.cpp
    test_ordering_strategies.cpp
    # test_png_io.cpp
    test_indexed_triangle_set.cpp
    TestMD5.cpp
    ../libnest2d/printer_parts.cpp
    )

if (SLIC3R_CAD)
    target_sources(${_TEST_NAME}_tests PRIVATE
        test_caddocument.cpp
        test_sketchconstraints.cpp
        test_sketchedit.cpp
        test_sketchprofile.cpp
        test_sketchimport.cpp
        test_sketchinference.cpp
        test_slvs_constraints.cpp)
endif ()

if (TARGET OpenVDB::openvdb)
    target_sources(${_TEST_NAME}_tests PRIVATE test_hollowing.cpp)
endif()

target_link_libraries(${_TEST_NAME}_tests test_common libslic3r Catch2::Catch2WithMain OpenSSL::Crypto)
if (WIN32)
    target_link_libraries(${_TEST_NAME}_tests bcrypt.lib crypt32.lib)
endif()
target_include_directories(${_TEST_NAME}_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

orcaslicer_copy_test_dlls()

orcaslicer_discover_tests(${_TEST_NAME}_tests)
