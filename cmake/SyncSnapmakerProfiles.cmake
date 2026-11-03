# Check out the profiles revision recorded by the superproject and mirror its resources.
# Usage: cmake -DSUB=<submodule> -DDST=<profiles> -DVERSION_STAMP=<file> -P <script>
# Updating to the configured branch's latest commit requires a manual update --remote.
cmake_minimum_required(VERSION 3.13)

foreach(_required SUB DST VERSION_STAMP)
    if("${${_required}}" STREQUAL "")
        message(FATAL_ERROR "SyncSnapmakerProfiles.cmake: -D${_required}=<path> is required")
    endif()
endforeach()

get_filename_component(SUB "${SUB}" ABSOLUTE)
get_filename_component(DST "${DST}" ABSOLUTE)
get_filename_component(_repository "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
file(RELATIVE_PATH _sub_path "${_repository}" "${SUB}")
if(_sub_path MATCHES "^\\.\\.(/|$)" OR IS_ABSOLUTE "${_sub_path}")
    message(FATAL_ERROR "Profiles submodule must be inside ${_repository}")
endif()

# Lock the source before the destination across concurrent builds.
if(WIN32)
    set(_lock_root "$ENV{TEMP}")
else()
    set(_lock_root "$ENV{TMPDIR}")
endif()
if(NOT _lock_root)
    set(_lock_root "/tmp")
endif()
string(SHA256 _source_lock_key "${SUB}")
string(SHA256 _destination_lock_key "${DST}")
file(LOCK "${_lock_root}/snapmaker-profiles-source-${_source_lock_key}.lock" GUARD PROCESS TIMEOUT 300)
file(LOCK "${_lock_root}/snapmaker-profiles-sync-${_destination_lock_key}.lock" GUARD PROCESS TIMEOUT 300)

find_package(Git REQUIRED)
function(profiles_git operation output)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${_repository}" ${ARGN}
        OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr RESULT_VARIABLE _result
        OUTPUT_STRIP_TRAILING_WHITESPACE TIMEOUT 300)
    if(NOT _result STREQUAL "0")
        message(FATAL_ERROR "Profiles ${operation} failed (${_result}).\n${_stdout}\n${_stderr}")
    endif()
    set(${output} "${_stdout}" PARENT_SCOPE)
endfunction()

# Initialize missing submodules and fetch missing commits, without following the remote branch tip.
profiles_git(sync _git_output submodule sync -- "${_sub_path}")
profiles_git(update _git_output submodule update --init --checkout --recursive -- "${_sub_path}")

if(NOT EXISTS "${SUB}/Snapmaker.json")
    message(FATAL_ERROR "Missing ${SUB}/Snapmaker.json")
endif()

# PresetUpdater deploys these rule files by fixed path (bundle/filament/<name>);
# a layout drift in the Profiles repo silently breaks that deployment.
foreach(_rule filament_hot_bed_nozzles.json filament_compatibility.json filament_allow_list.json)
    if(NOT EXISTS "${SUB}/Snapmaker/filament/${_rule}")
        message(FATAL_ERROR "Missing ${SUB}/Snapmaker/filament/${_rule}")
    endif()
endforeach()

# Validate all source directories before deleting or copying destination files.
set(_dirs Snapmaker/filament Snapmaker/machine Snapmaker/process)
set(_expected Snapmaker.json)
foreach(_dir IN LISTS _dirs)
    file(GLOB_RECURSE _files LIST_DIRECTORIES false RELATIVE "${SUB}" "${SUB}/${_dir}/*")
    if(NOT _files)
        message(FATAL_ERROR "No preset files found in ${SUB}/${_dir}")
    endif()
    list(APPEND _expected ${_files})
endforeach()

# Remove stale files only within the managed preset directories.
foreach(_dir IN LISTS _dirs)
    file(GLOB_RECURSE _files LIST_DIRECTORIES false RELATIVE "${DST}" "${DST}/${_dir}/*")
    foreach(_file IN LISTS _files)
        if(NOT _file IN_LIST _expected)
            file(REMOVE "${DST}/${_file}")
        endif()
    endforeach()
endforeach()

# COPYONLY preserves timestamps when file contents are unchanged.
foreach(_file IN LISTS _expected)
    configure_file("${SUB}/${_file}" "${DST}/${_file}" COPYONLY)
endforeach()

# Report and record the revision only when it changes.
profiles_git(revision _sha -C "${SUB}" rev-parse HEAD)
set(_previous "")
if(EXISTS "${VERSION_STAMP}")
    file(READ "${VERSION_STAMP}" _previous)
    string(STRIP "${_previous}" _previous)
endif()
if(NOT _sha STREQUAL _previous)
    message(STATUS "Snapmaker profiles: submodule revision ${_sha}")
    file(WRITE "${VERSION_STAMP}" "${_sha}\n")
endif()
