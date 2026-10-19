# Runs the real snapmaker-orca binary, so it needs the built app and resources/, not just build/tests.
# The CI unit-test job only receives build/tests, so the test carries the RequiresApp label that
# scripts/run_unit_tests.sh excludes. Run it with `ctest -C Release -L RequiresApp`. It also exits 77
# (skipped) when the binary is missing.

find_program(ORCA_CLI_TEST_PYTHON NAMES python3)
if (NOT ORCA_CLI_TEST_PYTHON)
    message(STATUS "python3 not found, not registering the CLI tests")
    return()
endif ()

add_test(NAME cli_strict_mode
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_strict.sh $<TARGET_FILE:Snapmaker_Orca> ${ORCA_CLI_TEST_PYTHON})
set_tests_properties(cli_strict_mode PROPERTIES
    LABELS "CLI;RequiresApp"
    SKIP_RETURN_CODE 77
    TIMEOUT 900)

# --uptodate keeps the High Flow column of a project filament whose refreshing file has none.
add_test(NAME cli_uptodate_high_flow
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_uptodate_high_flow.sh $<TARGET_FILE:Snapmaker_Orca> ${ORCA_CLI_TEST_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set_tests_properties(cli_uptodate_high_flow PROPERTIES
    LABELS "CLI;RequiresApp"
    SKIP_RETURN_CODE 77
    TIMEOUT 900)

add_test(NAME cli_project_missing_keys
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_project_missing_keys.sh $<TARGET_FILE:Snapmaker_Orca> ${ORCA_CLI_TEST_PYTHON}
        ${CMAKE_SOURCE_DIR}/resources/profiles/BBL)
set_tests_properties(cli_project_missing_keys PROPERTIES
    LABELS "CLI;RequiresApp"
    SKIP_RETURN_CODE 77
    TIMEOUT 900)
