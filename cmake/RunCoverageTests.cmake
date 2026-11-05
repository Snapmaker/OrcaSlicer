# Run the coverage test suite while optionally tolerating known test failures.
# This keeps the lcov capture/generation commands runnable so a report is still
# available for CI; callers remain responsible for gating on the original test
# result when COVERAGE_IGNORE_TEST_FAILURES is enabled.

if(NOT DEFINED COVERAGE_CTEST_COMMAND)
    message(FATAL_ERROR "COVERAGE_CTEST_COMMAND is required")
endif()
if(NOT DEFINED COVERAGE_BINARY_DIR)
    message(FATAL_ERROR "COVERAGE_BINARY_DIR is required")
endif()

set(command
    "${COVERAGE_CTEST_COMMAND}"
    --test-dir "${COVERAGE_BINARY_DIR}"
    --output-on-failure
    --timeout 3600
    -E "\\[trace\\]"
)

execute_process(COMMAND ${command} RESULT_VARIABLE result)

if(result EQUAL 0)
    return()
endif()

if(COVERAGE_IGNORE_TEST_FAILURES)
    message(WARNING "Coverage tests exited with ${result}; continuing to generate the report")
    return()
endif()

message(FATAL_ERROR "Coverage tests exited with ${result}")