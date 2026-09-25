cmake_minimum_required(VERSION 3.23)

foreach(_variable IN ITEMS
        PROJECT_ROOT BUILD_ROOT BUILD_CONFIG DLL_PATH STAGE_DIR ARCHIVE_PATH)
    if(NOT DEFINED ${_variable} OR "${${_variable}}" STREQUAL "")
        message(FATAL_ERROR "${_variable} is required")
    endif()
endforeach()

if(NOT BUILD_CONFIG STREQUAL "Release")
    message(FATAL_ERROR
        "Release packaging requires a Release configuration; got '${BUILD_CONFIG}'")
endif()

get_filename_component(PROJECT_ROOT "${PROJECT_ROOT}" ABSOLUTE)
get_filename_component(BUILD_ROOT "${BUILD_ROOT}" ABSOLUTE)
get_filename_component(DLL_PATH "${DLL_PATH}" ABSOLUTE)
get_filename_component(STAGE_DIR "${STAGE_DIR}" ABSOLUTE)
get_filename_component(ARCHIVE_PATH "${ARCHIVE_PATH}" ABSOLUTE)

if(NOT EXISTS "${DLL_PATH}")
    message(FATAL_ERROR "Built plugin DLL is missing: ${DLL_PATH}")
endif()
get_filename_component(_dll_name "${DLL_PATH}" NAME)
if(NOT _dll_name STREQUAL "LoadingScreens.dll")
    message(FATAL_ERROR "Expected LoadingScreens.dll, got '${_dll_name}'")
endif()

# Guard both cleanup paths even when this script is invoked by hand with bad
# -D input. Staging must be a child of this CMake build tree; the ZIP may live
# under either the source tree (the release target) or build tree (CTest).
cmake_path(IS_PREFIX BUILD_ROOT "${STAGE_DIR}" NORMALIZE _stage_under_build)
cmake_path(IS_PREFIX PROJECT_ROOT "${ARCHIVE_PATH}" NORMALIZE _archive_under_project)
cmake_path(IS_PREFIX BUILD_ROOT "${ARCHIVE_PATH}" NORMALIZE _archive_under_build)
get_filename_component(_archive_extension "${ARCHIVE_PATH}" LAST_EXT)
string(TOLOWER "${_archive_extension}" _archive_extension)
if(NOT _stage_under_build OR STAGE_DIR STREQUAL BUILD_ROOT)
    message(FATAL_ERROR "Refusing staging directory outside the build tree: ${STAGE_DIR}")
endif()
if((NOT _archive_under_project AND NOT _archive_under_build) OR
   ARCHIVE_PATH STREQUAL PROJECT_ROOT OR ARCHIVE_PATH STREQUAL BUILD_ROOT OR
   NOT _archive_extension STREQUAL ".zip")
    message(FATAL_ERROR "Refusing unsafe release archive path: ${ARCHIVE_PATH}")
endif()

include("${PROJECT_ROOT}/cmake/ValidateReleaseInputs.cmake")

set(_staged_data "${STAGE_DIR}/Data")
set(_staged_plugin_dir "${_staged_data}/F4SE/Plugins")
set(_staged_mcm_dir "${_staged_data}/MCM/Config/FasterLoadscreens")
set(_staged_asset_dir "${_staged_data}/Textures/LoadingScreens")

file(REMOVE_RECURSE "${STAGE_DIR}")
file(REMOVE "${ARCHIVE_PATH}")
file(MAKE_DIRECTORY
    "${_staged_plugin_dir}"
    "${_staged_mcm_dir}"
    "${_staged_asset_dir}"
)

configure_file("${DLL_PATH}" "${_staged_plugin_dir}/LoadingScreens.dll" COPYONLY)
configure_file("${PROJECT_ROOT}/package/Data/MCM/Config/FasterLoadscreens/config.json"
               "${_staged_mcm_dir}/config.json" COPYONLY)
configure_file("${PROJECT_ROOT}/package/Data/MCM/Config/FasterLoadscreens/settings.ini"
               "${_staged_mcm_dir}/settings.ini" COPYONLY)
foreach(_dds IN LISTS _dds_files)
    get_filename_component(_dds_name "${_dds}" NAME)
    configure_file("${_dds}" "${_staged_asset_dir}/${_dds_name}" COPYONLY)
endforeach()

get_filename_component(_archive_dir "${ARCHIVE_PATH}" DIRECTORY)
file(MAKE_DIRECTORY "${_archive_dir}")

find_program(_powershell_executable NAMES pwsh powershell REQUIRED)

# MSBuild custom targets can leave data on inherited stdin. PowerShell treats
# that as pipeline input to the script and emits a non-terminating parameter-
# binding error even though the archive is created. Give the child an explicit
# empty input stream so packaging is clean and deterministic in every launcher.
set(_archive_stdin "${STAGE_DIR}/.archive-stdin")
file(WRITE "${_archive_stdin}" "")

# ZipArchive is fed a sorted manifest and fixed metadata. This avoids
# libarchive's varying access-time extras and makes repeated packages with
# identical inputs byte-for-byte equal when built with the same toolchain.
execute_process(
    COMMAND "${_powershell_executable}"
        -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass
        -File "${PROJECT_ROOT}/cmake/CreateDeterministicArchive.ps1"
        -StageDir "${STAGE_DIR}"
        -ArchivePath "${ARCHIVE_PATH}"
    RESULT_VARIABLE _archive_result
    OUTPUT_VARIABLE _archive_output
    ERROR_VARIABLE _archive_error
    INPUT_FILE "${_archive_stdin}"
)
file(REMOVE "${_archive_stdin}")
if(NOT _archive_output STREQUAL "")
    string(STRIP "${_archive_output}" _archive_output)
    message(STATUS "Archive tool: ${_archive_output}")
endif()
if(NOT _archive_result EQUAL 0 OR NOT _archive_error STREQUAL "" OR
   NOT EXISTS "${ARCHIVE_PATH}")
    string(STRIP "${_archive_error}" _archive_error)
    message(FATAL_ERROR
        "Release archive creation failed (${_archive_result}): ${ARCHIVE_PATH}\n${_archive_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar tf "${ARCHIVE_PATH}"
    OUTPUT_VARIABLE _archive_listing
    ERROR_VARIABLE _archive_error
    RESULT_VARIABLE _list_result
)
if(NOT _list_result EQUAL 0)
    message(FATAL_ERROR "Could not inspect release archive: ${_archive_error}")
endif()
string(REPLACE "\r\n" ";" _archive_entries "${_archive_listing}")
string(REPLACE "\n" ";" _archive_entries "${_archive_entries}")
list(FILTER _archive_entries EXCLUDE REGEX "^$")
list(LENGTH _archive_entries _archive_entry_count)
if(NOT _archive_entry_count EQUAL 83)
    message(FATAL_ERROR "Archive should contain exactly 83 files, found ${_archive_entry_count}")
endif()

set(_required_archive_entries
    "F4SE/Plugins/LoadingScreens.dll"
    "MCM/Config/FasterLoadscreens/config.json"
    "MCM/Config/FasterLoadscreens/settings.ini"
)
foreach(_required_entry IN LISTS _required_archive_entries)
    if(NOT _required_entry IN_LIST _archive_entries)
        message(FATAL_ERROR "Release archive is missing ${_required_entry}")
    endif()
endforeach()

foreach(_entry IN LISTS _archive_entries)
    if(NOT _entry MATCHES "^(F4SE|MCM|Textures)/")
        message(FATAL_ERROR
            "Release files must be at the archive root without a Data wrapper: ${_entry}")
    endif()
endforeach()

file(SIZE "${ARCHIVE_PATH}" _archive_size)
message(STATUS "Created ${ARCHIVE_PATH} (${_archive_entry_count} files, ${_archive_size} bytes)")
