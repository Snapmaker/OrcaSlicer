cmake_minimum_required(VERSION 3.15)
project(SnapmakerCLI VERSION 1.0.0 LANGUAGES NONE)

# ── Paths (absolute, adjust for your build) ──────────────────────────
set(BUILD_DIR    "C:/workDir/programing/SnapMakerOracaRequirements/OrcaSlicer/build/src/Release")
set(RESOURCES    "C:/workDir/programing/SnapMakerOracaRequirements/OrcaSlicer/resources")
set(TOOL_DIR     "C:/workDir/programing/SnapMakerOracaRequirements/OrcaSlicer/tools/cli_test_tool")

# ── Bundle name ──────────────────────────────────────────────────────
set(CPACK_PACKAGE_NAME "Snapmaker_CLI_Package")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "Snapmaker-CLI-Package-${PROJECT_VERSION}")
set(CPACK_GENERATOR "ZIP")
set(CPACK_VERBATIM_VARIABLES ON)

# ── Install: CLI binary + DLLs ──────────────────────────────────────
file(GLOB CLI_FILES "${BUILD_DIR}/*")
install(FILES ${CLI_FILES} DESTINATION ".")

# ── Install: essential resources only ───────────────────────────────
install(DIRECTORY "${RESOURCES}/profiles"   DESTINATION "resources")
install(DIRECTORY "${RESOURCES}/calib"      DESTINATION "resources")
install(DIRECTORY "${RESOURCES}/printers"   DESTINATION "resources")
install(DIRECTORY "${RESOURCES}/fonts"      DESTINATION "resources")

# ── Install: test tool web app ──────────────────────────────────────
install(FILES "${TOOL_DIR}/app.py" DESTINATION "tools/cli_test_tool")
install(DIRECTORY "${TOOL_DIR}/templates" DESTINATION "tools/cli_test_tool")

# ── Install: Electron portable exe (if built) ───────────────────────
set(ELECTRON_EXE "${TOOL_DIR}/electron/dist/Snapmaker-CLI-Test-Tool-1.0.0-x64.exe")
if(EXISTS "${ELECTRON_EXE}")
  install(FILES "${ELECTRON_EXE}" DESTINATION "tools")
endif()

include(CPack)
