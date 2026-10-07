get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)

add_executable(${_TEST_NAME}_tests
	${_TEST_NAME}_tests.cpp
	test_3mf.cpp
	test_plate_print_history.cpp
	test_bambu_3mf_export.cpp
	test_bambu_flow_support.cpp
	test_bambu_import_renames.cpp
	test_bambu_nil_override.cpp
	test_untrusted_input.cpp
	test_custom_models.cpp
	test_model_browser_rules.cpp
	test_arrange_support_clearance.cpp
	test_appconfig_trailing_newline.cpp
	test_memory_guard_policy.cpp
	test_aabbindirect.cpp
	test_ams_lookup.cpp
	test_assembly_face_pick.cpp
	test_assembly_exact_footprint.cpp
	test_curve_pick.cpp
	test_chameleon_brim.cpp
	test_paint_depth.cpp
	test_paint_depth_clamp.cpp
	test_paint_penetration.cpp
	test_color_mix_solver.cpp
	test_color_split.cpp
	test_code_emboss.cpp
	test_emboss_bend.cpp
	test_inline_shapes.cpp
	test_inline_text.cpp
	test_simple_shape.cpp
	test_image_fill.cpp
	test_image_row_transform.cpp
	test_image_row_dialog.cpp
	test_clipper_offset.cpp
	test_clipper_utils.cpp
	test_config.cpp
	test_contour_z.cpp
	test_curved_cut.cpp
	test_draw_cut.cpp
	test_edgegrid.cpp
	test_cut_recipe.cpp
	test_config_include.cpp
	test_custom_gcode.cpp
	test_default_bed_type.cpp
	test_dual_nozzle_group.cpp
	test_ec_retraction.cpp
	test_bed_exclude_area.cpp
	test_cut_retraction_range.cpp
	test_wipe_tower_no_sparse_gcode.cpp
	test_gcodewriter_feedrate_guard.cpp
	# test_ec_retraction.cpp slices a real two-filament plate, so it needs the
	# Slic3r::Test slicing helpers that otherwise only fff_print_tests links.
	../fff_print/test_data.cpp
	test_identity_filament_map.cpp
	test_bambu_extruder_map.cpp
	test_bambu_dual_nozzle_sync.cpp
	test_bambu_idle_precool.cpp
	test_export_stl_part.cpp
	test_step_export.cpp
	test_cad_edit.cpp
	test_elephant_foot_compensation.cpp
	test_fill_bed_pack.cpp
	test_elegoo_cc2_profiles.cpp
	test_elegoo_cc2_toolchange.cpp
	test_flexi_joint.cpp
	test_fuzzy_skin.cpp
	test_geometry.cpp
	test_h2c_rack_nozzle_change.cpp
	test_h2c_rack_park_preheat.cpp
	test_h2d_byobject_toolchange.cpp
	test_h2d_timelapse.cpp
	test_extruder_areas.cpp
	test_by_object_max_clearance.cpp
	test_filament_map_reapply.cpp
	test_h2_preload_filaments.cpp
	test_mixed_filament.cpp
	test_mixed_filament_cli_gates.cpp
	test_mixed_nozzle.cpp
	test_mixed_filament_color_golden.cpp
	test_triangle_selector.cpp
	test_trianglemesh_slicer.cpp
	test_locked_zag_patterns.cpp
	test_locked_zag_identity_probe.cpp
	test_layer_time_speed_smoothing.cpp
	test_local_z_order_optimizer.cpp
	test_placeholder_parser.cpp
	test_polygon.cpp
	test_preset_diff.cpp
	test_print_settings_carry.cpp
	test_filament_variant_autoenable.cpp
	test_preset_load_determinism.cpp
	test_profile_load_util.cpp
	test_project_preset_switch.cpp
	test_mutable_polygon.cpp
	test_mutable_priority_queue.cpp
	test_gltf.cpp
	test_obj_color_match.cpp
	test_scale_to_volume.cpp
	test_slice_compare.cpp
	test_stl.cpp
	test_support_set.cpp
	test_mesh_edit.cpp
	test_mesh_sculpt.cpp
	test_mesh_remesh.cpp
	test_mesh_round.cpp
	test_quad_remesh.cpp
	test_meshboolean.cpp
	test_lay_on_face.cpp
	# test_marchingsquares.cpp
	test_timeutils.cpp
	test_utils.cpp
	test_instance_lock.cpp
	TestMD5.cpp
	test_voronoi.cpp
    test_optimizers.cpp
    test_wipe_tower.cpp
    test_wipe_tower_estimate.cpp
    test_bbl_rib_tower.cpp
    test_tower_interface.cpp
    test_wipe_path.cpp
    test_toolordering_cyclic.cpp
    test_shortest_path.cpp
    test_gcode_import.cpp
    test_cost_estimate.cpp
    test_align_math.cpp
    # test_png_io.cpp
    test_indexed_triangle_set.cpp
    ../libnest2d/printer_parts.cpp
	)

if (TARGET OpenVDB::openvdb)
    target_sources(${_TEST_NAME}_tests PRIVATE test_hollowing.cpp)
endif()
    
target_link_libraries(${_TEST_NAME}_tests test_common libslic3r OpenSSL::Crypto)
if (WIN32)
    target_link_libraries(${_TEST_NAME}_tests bcrypt.lib crypt32.lib)
endif()
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

if (WIN32)
	if ("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
	Snapmaker_Orca_copy_dlls(COPY_DLLS "Debug" "d" output_dlls_Debug)
	elseif("${CMAKE_BUILD_TYPE}" STREQUAL "RelWithDebInfo")
	Snapmaker_Orca_copy_dlls(COPY_DLLS "RelWithDebInfo" "" output_dlls_Release)
	else()
	Snapmaker_Orca_copy_dlls(COPY_DLLS "Release" "" output_dlls_Release)
	endif()
endif()

# catch_discover_tests(${_TEST_NAME}_tests TEST_PREFIX "${_TEST_NAME}: ")
add_test(${_TEST_NAME}_tests ${_TEST_NAME}_tests ${CATCH_EXTRA_ARGS})
