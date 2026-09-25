cmake_minimum_required(VERSION 3.23)

if(NOT DEFINED PROJECT_ROOT OR PROJECT_ROOT STREQUAL "")
    message(FATAL_ERROR "PROJECT_ROOT is required")
endif()

get_filename_component(PROJECT_ROOT "${PROJECT_ROOT}" ABSOLUTE)
set(_mcm_dir "${PROJECT_ROOT}/package/Data/MCM/Config/FasterLoadscreens")
set(_config_path "${_mcm_dir}/config.json")
set(_settings_path "${_mcm_dir}/settings.ini")
set(_asset_dir "${PROJECT_ROOT}/release/Faster Loadscreens/Textures/LoadingScreens")
set(_main_source_path "${PROJECT_ROOT}/src/main.cpp")
set(_d3d_source_path "${PROJECT_ROOT}/src/D3D11Compositor.cpp")
set(_game_settings_source_path "${PROJECT_ROOT}/src/GameSettingTweaks.cpp")
set(_door_prefetch_source_path "${PROJECT_ROOT}/src/DoorPrefetch.cpp")
set(_cmake_source_path "${PROJECT_ROOT}/CMakeLists.txt")
set(_loading_manager_source_path "${PROJECT_ROOT}/src/LoadingScreenManager.cpp")
set(_runtime_policy_source_path "${PROJECT_ROOT}/src/RuntimePolicy.h")
set(_performance_header_path "${PROJECT_ROOT}/src/PerformancePatches.h")
set(_performance_source_path "${PROJECT_ROOT}/src/PerformancePatches.cpp")

foreach(_required_path IN ITEMS
        "${_config_path}"
        "${_settings_path}"
        "${_asset_dir}"
        "${_main_source_path}"
        "${_d3d_source_path}"
        "${_game_settings_source_path}"
        "${_door_prefetch_source_path}"
        "${_cmake_source_path}"
        "${_loading_manager_source_path}"
        "${_runtime_policy_source_path}"
        "${_performance_header_path}"
        "${_performance_source_path}")
    if(NOT EXISTS "${_required_path}")
        message(FATAL_ERROR "Required release input is missing: ${_required_path}")
    endif()
endforeach()

# The plugin suppresses VaultTecLogo_mc dynamically only while its custom
# presentation owns the native LoadingMenu. Shipping an Interface override
# would also change native/fallback mode and can leave a spinner-less or
# otherwise stale movie behind after an upgrade. The release intentionally has
# no SWF at all.
file(GLOB_RECURSE _packaged_interface_files
    LIST_DIRECTORIES false
    "${PROJECT_ROOT}/package/Data/Interface/*")
if(_packaged_interface_files)
    message(FATAL_ERROR
        "The release must not ship Interface overrides: ${_packaged_interface_files}")
endif()

# Parse the MCM document with CMake's JSON parser, then check the settings that
# the DLL and the shipped INI rely on.  This catches malformed hand edits as
# well as an accidentally copied config for a different mod.
file(READ "${_config_path}" _config_json)
string(JSON _mod_name ERROR_VARIABLE _json_error GET "${_config_json}" modName)
if(NOT _json_error STREQUAL "NOTFOUND")
    message(FATAL_ERROR "Invalid MCM JSON (${_config_path}): ${_json_error}")
endif()
if(NOT _mod_name STREQUAL "FasterLoadscreens")
    message(FATAL_ERROR "MCM modName must be FasterLoadscreens, got '${_mod_name}'")
endif()

string(JSON _content_type ERROR_VARIABLE _json_error
    TYPE "${_config_json}" content)
if(NOT _json_error STREQUAL "NOTFOUND" OR
   NOT _content_type STREQUAL "ARRAY")
    message(FATAL_ERROR "MCM config 'content' must be an array: ${_json_error}")
endif()
string(JSON _content_count ERROR_VARIABLE _json_error
    LENGTH "${_config_json}" content)
if(NOT _json_error STREQUAL "NOTFOUND" OR _content_count LESS 1)
    message(FATAL_ERROR "MCM config must contain a non-empty content array: ${_json_error}")
endif()

set(_required_mcm_ids
    "iLoadingScreenMode:Main"
    "bPreloadSaveOnConfirm:Main"
    "bVanillaFades:Main"
    "fMinSecondsForLoadFadeIn:Main"
    "fLoadGameFadeSecs:Main"
    "fFadeToBlackFadeSeconds:Main"
    "fAutoDoorFadeSecs:Main"
    "fNormalDoorFadeSecs:Main"
    "fNormalDoorFadeWait:Main"
)
set(_retired_mcm_sections
    "Door Preload"
)

# Walk the actual content array. A textual search can be fooled by an ID copied
# into help text while the corresponding control is missing, and it cannot
# detect duplicate controls which write the same ModSetting.
math(EXPR _content_last "${_content_count} - 1")
set(_mcm_ids)
foreach(_content_index RANGE 0 ${_content_last})
    string(JSON _entry_type ERROR_VARIABLE _entry_error
        TYPE "${_config_json}" content ${_content_index})
    if(NOT _entry_error STREQUAL "NOTFOUND" OR
       NOT _entry_type STREQUAL "OBJECT")
        message(FATAL_ERROR
            "MCM content[${_content_index}] must be an object: ${_entry_error}")
    endif()

    string(JSON _control_type_kind ERROR_VARIABLE _type_kind_error
        TYPE "${_config_json}" content ${_content_index} type)
    if(NOT _type_kind_error STREQUAL "NOTFOUND" OR
       NOT _control_type_kind STREQUAL "STRING")
        message(FATAL_ERROR
            "MCM content[${_content_index}] requires a string 'type': "
            "${_type_kind_error}")
    endif()
    string(JSON _control_type ERROR_VARIABLE _type_error
        GET "${_config_json}" content ${_content_index} type)
    if(NOT _type_error STREQUAL "NOTFOUND" OR
       _control_type STREQUAL "")
        message(FATAL_ERROR
            "MCM content[${_content_index}] requires a string 'type': ${_type_error}")
    endif()

    if(_control_type STREQUAL "section")
        string(JSON _section_text_type ERROR_VARIABLE _section_text_type_error
            TYPE "${_config_json}" content ${_content_index} text)
        if(NOT _section_text_type_error STREQUAL "NOTFOUND" OR
           NOT _section_text_type STREQUAL "STRING")
            message(FATAL_ERROR
                "MCM section content[${_content_index}] requires string text")
        endif()
        string(JSON _section_text GET
            "${_config_json}" content ${_content_index} text)
        if(_section_text IN_LIST _retired_mcm_sections)
            message(FATAL_ERROR
                "MCM content exposes retired section '${_section_text}'")
        endif()
    endif()

    string(JSON _id_type ERROR_VARIABLE _id_type_error
        TYPE "${_config_json}" content ${_content_index} id)
    if(_id_type_error STREQUAL "NOTFOUND")
        if(_control_type STREQUAL "section" OR
           _control_type STREQUAL "spacer")
            message(FATAL_ERROR
                "MCM layout entry content[${_content_index}] "
                "(${_control_type}) must not have a setting id")
        endif()
        if(NOT _id_type STREQUAL "STRING")
            message(FATAL_ERROR
                "MCM content[${_content_index}].id must be a string")
        endif()
        string(JSON _entry_id GET
            "${_config_json}" content ${_content_index} id)
        if(_entry_id STREQUAL "")
            message(FATAL_ERROR
                "MCM content[${_content_index}].id cannot be empty")
        endif()
        if(_entry_id IN_LIST _mcm_ids)
            message(FATAL_ERROR
                "MCM config contains duplicate setting id '${_entry_id}'")
        endif()
        list(APPEND _mcm_ids "${_entry_id}")
    elseif(NOT _control_type STREQUAL "section" AND
           NOT _control_type STREQUAL "spacer")
        message(FATAL_ERROR
            "MCM control content[${_content_index}] (${_control_type}) "
            "is missing its setting id")
    endif()

    if(NOT _control_type STREQUAL "section" AND
       NOT _control_type STREQUAL "spacer")
        string(JSON _help_type ERROR_VARIABLE _help_type_error
            TYPE "${_config_json}" content ${_content_index} help)
        if(NOT _help_type_error STREQUAL "NOTFOUND" OR
           NOT _help_type STREQUAL "STRING")
            message(FATAL_ERROR
                "MCM control content[${_content_index}] requires string help text")
        endif()
        string(JSON _help_text GET
            "${_config_json}" content ${_content_index} help)
        string(REGEX MATCHALL "[.!?]" _help_sentence_marks "${_help_text}")
        list(LENGTH _help_sentence_marks _help_sentence_count)
        if(_help_sentence_count GREATER 1)
            message(FATAL_ERROR
                "MCM control content[${_content_index}] help must be no more than one sentence: '${_help_text}'")
        endif()
    endif()
endforeach()

foreach(_id IN LISTS _required_mcm_ids)
    if(NOT _id IN_LIST _mcm_ids)
        message(FATAL_ERROR
            "MCM content array is missing required setting id '${_id}'")
    endif()
endforeach()
list(LENGTH _required_mcm_ids _required_mcm_id_count)
list(LENGTH _mcm_ids _actual_mcm_id_count)
if(NOT _actual_mcm_id_count EQUAL _required_mcm_id_count)
    message(FATAL_ERROR
        "MCM must expose exactly ${_required_mcm_id_count} supported controls; found ${_actual_mcm_id_count}: ${_mcm_ids}")
endif()

set(_retired_mcm_ids
    "bPreloadExteriorGates:DoorPrefetch"
    "iExteriorGateDistanceCells:DoorPrefetch"
    "bPrefetchCellOnCrosshairDoor:DoorPrefetch"
    "iExteriorGridRadius:DoorPrefetch"
    "bPreloadLinkedAreas:GameSettings"
    "bNativeExteriorArrivalCellOnly:GameSettings"
    "iTeleportPreloadDistance:GameSettings"
    "iExtendedRange:DoorPrefetch"
    "iMenuLoadCellPreload:GameSettings"
)
foreach(_id IN LISTS _retired_mcm_ids)
    if(_id IN_LIST _mcm_ids)
        message(FATAL_ERROR
            "MCM content exposes retired unsafe setting id '${_id}'")
    endif()
endforeach()

# The benchmark Stage action writes a live MCM override, so it must obey the
# same retired-key policy as the packaged defaults.
set(_benchmark_harness_path
    "${PROJECT_ROOT}/tools/Fallout4PreloadBenchmark.ps1")
file(READ "${_benchmark_harness_path}" _benchmark_harness)
string(REPLACE "\r\n" "\n" _benchmark_harness "${_benchmark_harness}")
foreach(_key IN ITEMS
        "bPrefetchCellOnCrosshairDoor"
        "iExteriorGridRadius"
        "iExtendedRange"
        "iMenuLoadCellPreload")
    string(REGEX MATCH
        "(^|\n)[ \t]*${_key}[ \t]*="
        _retired_harness_entry "${_benchmark_harness}")
    if(NOT _retired_harness_entry STREQUAL "")
        message(FATAL_ERROR
            "Benchmark harness still writes retired MCM key '${_key}'")
    endif()
endforeach()

file(READ "${_settings_path}" _settings_ini)
string(REPLACE "\r\n" "\n" _settings_ini "${_settings_ini}")
set(_required_ini_keys
    "iLoadingScreenMode"
    "iBenchmarkMode"
    "bPreloadSaveOnConfirm"
    "bPreloadExteriorGates"
    "iExteriorGateDistanceCells"
    "bVanillaFades"
    "fMinSecondsForLoadFadeIn"
    "fLoadGameFadeSecs"
    "fFadeToBlackFadeSeconds"
    "fAutoDoorFadeSecs"
    "fNormalDoorFadeSecs"
    "fNormalDoorFadeWait"
    "bPreloadDiagnostics"
)
foreach(_key IN LISTS _required_ini_keys)
    string(REGEX MATCHALL
        "(^|\n)[ \t]*${_key}[ \t]*="
        _entry_matches
        "${_settings_ini}"
    )
    list(LENGTH _entry_matches _entry_count)
    if(_entry_count EQUAL 0)
        message(FATAL_ERROR "MCM settings.ini is missing active key '${_key}'")
    endif()
    if(NOT _entry_count EQUAL 1)
        message(FATAL_ERROR
            "MCM settings.ini must contain exactly one active '${_key}' entry; found ${_entry_count}"
        )
    endif()
endforeach()

foreach(_key IN ITEMS
        "bPrefetchCellOnCrosshairDoor"
        "iExteriorGridRadius"
        "bPreloadLinkedAreas"
        "bNativeExteriorArrivalCellOnly"
        "iTeleportPreloadDistance"
        "iExtendedRange"
        "iMenuLoadCellPreload")
    string(REGEX MATCH
        "(^|\n)[ \t]*${_key}[ \t]*="
        _retired_entry_match
        "${_settings_ini}")
    if(NOT _retired_entry_match STREQUAL "")
        message(FATAL_ERROR
            "MCM settings.ini still activates retired key '${_key}'")
    endif()
endforeach()

# Guard the exterior-only full-plugin release arm and explicit fade baseline.
set(_required_release_defaults
    "iLoadingScreenMode:3"
    "iBenchmarkMode:1"
    "bPreloadSaveOnConfirm:1"
    "bPreloadExteriorGates:1"
    "iExteriorGateDistanceCells:0"
    "bVanillaFades:1"
    "fMinSecondsForLoadFadeIn:1.5"
    "fLoadGameFadeSecs:1.0"
    "fFadeToBlackFadeSeconds:1.0"
    "fAutoDoorFadeSecs:0.5"
    "fNormalDoorFadeSecs:0.4"
    "fNormalDoorFadeWait:0.01"
    "bPreloadDiagnostics:0"
)
foreach(_key_and_value IN LISTS _required_release_defaults)
    string(REPLACE ":" ";" _pair "${_key_and_value}")
    list(GET _pair 0 _key)
    list(GET _pair 1 _expected)
    string(REPLACE "." "\\." _expected_regex "${_expected}")
    string(REGEX MATCH
        "(^|\n)[ \t]*${_key}[ \t]*=[ \t]*${_expected_regex}[ \t]*(\n|$)"
        _default_match
        "${_settings_ini}"
    )
    if(_default_match STREQUAL "")
        message(FATAL_ERROR
            "MCM settings.ini must ship release policy ${_key}=${_expected}"
        )
    endif()
endforeach()

# These switches are deliberately compiled rather than exposed as unsupported
# MCM toggles. Validate the initializer policy as release input so a source edit
# cannot silently turn the speed floor off, re-enable the unsafe live
# PresentThread rewrite, or ship the one-core loader regression.
file(READ "${_performance_header_path}" _performance_header)
set(_required_performance_defaults
    "untieSpeedFromFPS:true"
    "disableiFPSClamp:true"
    "disableBlackLoadingScreens:false"
    "disableVSyncWhileLoading:true"
    "disable3DModel:true"
    "disableAnimationOnLoadingScreens:true"
    "yieldCPUDuringLoading:false"
    "oneThreadWhileLoading:false"
)
foreach(_field_and_value IN LISTS _required_performance_defaults)
    string(REPLACE ":" ";" _pair "${_field_and_value}")
    list(GET _pair 0 _field)
    list(GET _pair 1 _expected)
    string(REGEX MATCH
        "bool[ \t]+${_field}[ \t]*=[ \t]*${_expected}[ \t]*;"
        _performance_default_match
        "${_performance_header}"
    )
    if(_performance_default_match STREQUAL "")
        message(FATAL_ERROR
            "PerformanceConfig must default ${_field}=${_expected}")
    endif()
endforeach()

# The explicit shipped INI value and both source fallbacks must agree. This
# keeps a missing/user-reset INI in full mode and makes timing-only mode opt-in.
file(READ "${_main_source_path}" _main_source)
foreach(_benchmark_source_default IN ITEMS
        "int benchmarkMode = 1;"
        "\"Main\", \"iBenchmarkMode\", 1")
    string(FIND "${_main_source}" "${_benchmark_source_default}"
        _benchmark_default_position)
    if(_benchmark_default_position EQUAL -1)
        message(FATAL_ERROR
            "Source benchmark default is not full mode: "
            "missing '${_benchmark_source_default}'")
    endif()
endforeach()

# Measurement mode must remain an observer, not merely a visual-off variant of
# the full plugin. It also acts as the fail-closed coexistence path when the
# distinct March VRLoadingScreens.dll is present.
foreach(_passive_requirement IN ITEMS
        "Policy::ShouldUsePassiveMeasurement("
        "VRLoadingScreens.dll"
        "s_passiveMeasurementOnly"
        "return ProcessPassiveEvent(a_event)"
        "PASSIVE_MEASURE ready:"
        "PASSIVE_MEASURE attempt=")
    string(FIND "${_main_source}" "${_passive_requirement}"
        _passive_requirement_position)
    if(_passive_requirement_position EQUAL -1)
        message(FATAL_ERROR
            "Passive measurement contract is missing: "
            "'${_passive_requirement}'")
    endif()
endforeach()

# VR full mode must retain March's load-time policy around the restored visual
# lifecycle. Current tips remain an intentional presentation-only difference.
file(READ "${_loading_manager_source_path}" _loading_manager_source)
foreach(_vr_march_parity_requirement IN ITEMS
        "Policy::ShouldApplyLegacyLoadBudgets("
        "Policy::ShouldStartLoadHeartbeat("
        "Policy::ShouldArmVRTimerPatches("
        "VR March parity: deferred timer/iFPSClamp"
        "CompositeTipsIntoBg()")
    string(FIND "${_main_source}${_loading_manager_source}"
        "${_vr_march_parity_requirement}"
        _vr_march_parity_requirement_position)
    if(_vr_march_parity_requirement_position EQUAL -1)
        message(FATAL_ERROR
            "VR March performance parity contract is missing: "
            "'${_vr_march_parity_requirement}'")
    endif()
endforeach()

string(FIND "${_main_source}" "PASSIVE_MEASURE ready:"
    _passive_early_return_position)
foreach(_active_only_initializer IN ITEMS
        "F4SE::AllocTrampoline(256);"
        "spdlog::flush_every(std::chrono::seconds(1));"
        "PreloadDiagnostics::Install()"
        "loadingManager.Init("
        "CellWorldspaceGuard::Install()")
    string(FIND "${_main_source}" "${_active_only_initializer}"
        _active_only_initializer_position)
    if(_active_only_initializer_position EQUAL -1 OR
       _active_only_initializer_position LESS _passive_early_return_position)
        message(FATAL_ERROR
            "Active-only initializer escaped ahead of passive return: "
            "'${_active_only_initializer}'")
    endif()
endforeach()

# Fallout's VR startup LoadingMenu requires the state constructed by
# BackgroundScreenModel::InitModels. RET-patching that initializer causes a
# repeatable null dereference at RVA 0x9E4F3D. Release validation permanently
# rejects the unsafe patch while requiring the safe per-frame selection NOP.
file(READ "${_performance_source_path}" _performance_source)
string(FIND "${_performance_source}"
    "config.disableiFPSClamp && !isNG && !isVR"
    _vr_ifpsclamp_deferred_position)
if(_vr_ifpsclamp_deferred_position EQUAL -1)
    message(FATAL_ERROR
        "VR iFPSClamp must remain deferred until the first successful save CLOSE")
endif()
foreach(_unsafe_vr_model_patch IN ITEMS
        "kVRInitModelsPrologue"
        "InitModels_Offset_VR"
        "BackgroundScreenModel::InitModels RET")
    string(FIND "${_performance_source}" "${_unsafe_vr_model_patch}"
        _unsafe_vr_model_patch_position)
    if(NOT _unsafe_vr_model_patch_position EQUAL -1)
        message(FATAL_ERROR
            "Unsafe VR model-initializer patch returned: "
            "'${_unsafe_vr_model_patch}'")
    endif()
endforeach()
foreach(_safe_vr_model_requirement IN ITEMS
        "kVRSetForegroundModelCall"
        "VR: SetForegroundModel CALL NOP5 applied")
    string(FIND "${_performance_source}" "${_safe_vr_model_requirement}"
        _safe_vr_model_requirement_position)
    if(_safe_vr_model_requirement_position EQUAL -1)
        message(FATAL_ERROR
            "Safe VR model-selection suppression is missing: "
            "'${_safe_vr_model_requirement}'")
    endif()
endforeach()

# Keep every policy whose release default was selected by the Fallout/VR
# benchmark aligned with its missing-key fallback in the DLL. A stale base INI
# is fixed by a clean install; a missing key must not silently resurrect the
# legacy inverse defaults (trimmed fades, crosshair preload, or linked preload
# off).
set(_required_config_fallbacks
    "\"Main\", \"iLoadingScreenMode\", 3"
    "\"Main\", \"bPreloadSaveOnConfirm\", true"
    "ini.GetBoolValue(\"Main\", \"bVanillaFades\", true)"
    "\"Main\", \"fMinSecondsForLoadFadeIn\", 1.5"
    "\"Main\", \"fLoadGameFadeSecs\", 1.0"
    "\"Main\", \"fFadeToBlackFadeSeconds\", 1.0"
    "\"Main\", \"fAutoDoorFadeSecs\", 0.5"
    "\"Main\", \"fNormalDoorFadeSecs\", 0.4"
    "\"Main\", \"fNormalDoorFadeWait\", 0.01"
    "\"DoorPrefetch\", \"bPreloadExteriorGates\", true"
    "\"DoorPrefetch\", \"iExteriorGateDistanceCells\", 0"
)
foreach(_fallback IN LISTS _required_config_fallbacks)
    string(FIND "${_main_source}" "${_fallback}" _fallback_position)
    if(_fallback_position EQUAL -1)
        message(FATAL_ERROR
            "Source fallback is missing or disagrees with release policy: "
            "'${_fallback}'")
    endif()
endforeach()

# Interior preloading is a hard retirement boundary, not a default. Require the
# active full-mode setting write, prohibit the removed mutation knobs/call target,
# and ensure the native callsite hook cannot return to the production target.
file(READ "${_game_settings_source_path}" _game_settings_source)
string(FIND "${_game_settings_source}"
    "SehSetBinary(\"bPreloadLinkedAreas:General\", false"
    _forced_native_off_position)
if(_forced_native_off_position EQUAL -1)
    message(FATAL_ERROR
        "GameSettingTweaks must force bPreloadLinkedAreas=false in full mode")
endif()
foreach(_retired_setting_write IN ITEMS
        "fTeleportPreloadDistance:General"
        "uInterior Cell Buffer:General")
    string(FIND "${_game_settings_source}" "${_retired_setting_write}"
        _retired_setting_write_position)
    if(NOT _retired_setting_write_position EQUAL -1)
        message(FATAL_ERROR
            "Retired interior setting mutation returned: ${_retired_setting_write}")
    endif()
endforeach()

foreach(_retired_source_read IN ITEMS
        "\"GameSettings\", \"iMenuLoadCellPreload\""
        "\"DoorPrefetch\", \"iExtendedRange\"")
    string(FIND "${_main_source}" "${_retired_source_read}"
        _retired_source_read_position)
    if(NOT _retired_source_read_position EQUAL -1)
        message(FATAL_ERROR
            "Source still reads retired MCM setting: '${_retired_source_read}'")
    endif()
endforeach()

string(FIND "${_main_source}"
    "DoorPrefetch::SetExtendedRay(false, 2.0f);"
    _retired_extended_ray_position)
if(_retired_extended_ray_position EQUAL -1)
    message(FATAL_ERROR
        "Production config must force the retired extended ray off")
endif()

string(FIND "${_main_source}" "if (timingOnly)" _passive_branch_position)
string(FIND "${_main_source}" "        StartLiveConfigWatcher();"
    _live_watcher_call_position)
if(_passive_branch_position EQUAL -1 OR
   _live_watcher_call_position EQUAL -1 OR
   _live_watcher_call_position LESS _passive_branch_position)
    message(FATAL_ERROR
        "The live MCM watcher must start only after passive measurement returns")
endif()

file(READ "${_door_prefetch_source_path}" _door_prefetch_source)
string(FIND "${_door_prefetch_source}" "PreloadInterior"
    _interior_call_target_position)
if(NOT _interior_call_target_position EQUAL -1)
    message(FATAL_ERROR
        "DoorPrefetch must not resolve, call, or advertise an interior preload entry")
endif()

# The engine helper retired in 2.3.24 reads g_TES's active GridArray before it
# consults the requested worldspace, so it is not a valid cross-world residency
# predicate for the ambient gate poller. Keep the executable dependency and its
# false-skip decision out of every release.
foreach(_retired_gate_probe IN ITEMS
        "AllCellsInGridLoadedFn"
        "kAllCellsInGridLoadedIDNG"
        "s_allCellsInGridLoaded"
        "native-arrival-grid-loaded")
    string(FIND "${_door_prefetch_source}" "${_retired_gate_probe}"
        _retired_gate_probe_position)
    if(NOT _retired_gate_probe_position EQUAL -1)
        message(FATAL_ERROR
            "Context-invalid cross-world gate probe returned: "
            "${_retired_gate_probe}")
    endif()
endforeach()

foreach(_required_gate_contract IN ITEMS
        "ResolveDoorDestination(a_refr, currentDestination)"
        "WorldspacePreload::Submit(tes, destWS, centerX, centerY, s_preloadWorld)"
        "Policy::ShouldArmExteriorPresentationEvidence("
        "s_exteriorPresentationCorrectionReady.load("
        "postSubmissionDestination"
        "residencyProof={},")
    string(FIND "${_door_prefetch_source}" "${_required_gate_contract}"
        _required_gate_contract_position)
    if(_required_gate_contract_position EQUAL -1)
        message(FATAL_ERROR
            "Cross-world-safe exterior gate contract is missing: "
            "${_required_gate_contract}")
    endif()
endforeach()

file(READ "${PROJECT_ROOT}/src/WorldspacePreload.cpp" _worldspace_preload_source)
string(FIND "${_worldspace_preload_source}"
    "a_preload(a_tes, a_world, a_x, a_y, true);" _queue_only_call_position)
if(_queue_only_call_position EQUAL -1)
    message(FATAL_ERROR "Worldspace preparation must retain the native queue-only submission")
endif()

foreach(_required_legacy_presentation_contract IN ITEMS
        "HookedLegacyPositionShowLoadingMenu"
        "InstallLegacyPositionExteriorShowHook();"
        "isOG163"
        "isVR1272"
        "0xE9AC60"
        "0xF03BD0"
        "DoorPrefetch::SetExteriorPresentationCorrectionReady("
        "Policy::ShouldCorrectPositionExteriorShow(")
    string(FIND "${_main_source}"
        "${_required_legacy_presentation_contract}"
        _required_legacy_presentation_contract_position)
    if(_required_legacy_presentation_contract_position EQUAL -1)
        message(FATAL_ERROR
            "OG/VR gate-presentation contract is missing: "
            "${_required_legacy_presentation_contract}")
    endif()
endforeach()

file(READ "${_cmake_source_path}" _cmake_source)
string(REGEX MATCH
    "set\\(VERSION[ \t]+([0-9]+)\\.([0-9]+)\\.([0-9]+)\\)"
    _project_version_match "${_cmake_source}")
if(_project_version_match STREQUAL "")
    string(REGEX MATCH
        "set\\(VERSION[ \t]+([0-9]+)\\.([0-9]+)\\)"
        _project_version_match "${_cmake_source}")
    if(_project_version_match STREQUAL "")
        message(FATAL_ERROR
            "Could not parse the release version from CMakeLists.txt")
    endif()
    set(_release_version_major "${CMAKE_MATCH_1}")
    set(_release_version_minor "${CMAKE_MATCH_2}")
    set(_release_version_patch 0)
else()
    set(_release_version_major "${CMAKE_MATCH_1}")
    set(_release_version_minor "${CMAKE_MATCH_2}")
    set(_release_version_patch "${CMAKE_MATCH_3}")
endif()
set(_expected_plugin_version
    "v.PluginVersion({ ${_release_version_major}, ${_release_version_minor}, ${_release_version_patch}, 0 });")
string(FIND "${_main_source}" "${_expected_plugin_version}"
    _plugin_version_position)
if(_plugin_version_position EQUAL -1)
    message(FATAL_ERROR
        "F4SE exported plugin version does not match CMake release version: "
        "expected '${_expected_plugin_version}'")
endif()

string(FIND "${_cmake_source}" "NativePreloadPolicy"
    _native_policy_target_position)
if(NOT _native_policy_target_position EQUAL -1)
    message(FATAL_ERROR
        "NativePreloadPolicy must remain outside the production target")
endif()

foreach(_retired_main_key IN ITEMS
        "bPrefetchCellOnCrosshairDoor"
        "iExteriorGridRadius"
        "bPreloadLinkedAreas"
        "bNativeExteriorArrivalCellOnly"
        "iTeleportPreloadDistance")
    string(FIND "${_main_source}" "\"${_retired_main_key}\""
        _retired_main_key_position)
    if(NOT _retired_main_key_position EQUAL -1)
        message(FATAL_ERROR
            "main.cpp still reads or publishes retired key '${_retired_main_key}'")
    endif()
endforeach()

# VR spinner removal belongs to the native custom-presentation lifecycle and
# must preserve/restore the SWF's actual prior Boolean. Flat presentation may
# use its dedicated GPU proof/replay mask instead; it removes the spinner corner
# before publication and does not require unsafe NG UI-singleton access.
file(READ "${_loading_manager_source_path}" _loading_manager_source)
foreach(_spinner_source_requirement IN ITEMS
        "SetNativeLoadingSpinnerVisible(false)"
        "GetNativeLoadingSpinnerVisible(m_spinnerOriginalVisible)"
        "SetNativeLoadingSpinnerVisible(m_spinnerOriginalVisible)"
        "HideNativeLoadingSpinnerForCustomScreen"
        "RestoreNativeLoadingSpinner"
        "Policy::ShouldEnableMode3TipCapture("
        "spinner isolation=GPU-mask")
    string(FIND "${_loading_manager_source}"
        "${_spinner_source_requirement}" _spinner_source_position)
    if(_spinner_source_position EQUAL -1)
        message(FATAL_ERROR
            "LoadingScreenManager is missing spinner lifecycle requirement "
            "'${_spinner_source_requirement}'")
    endif()
endforeach()

# The VR release contract is the March 5 v1.0 lifecycle, with only the current
# exact Scaleform tip/level composite substituted for its submitted-eye crop.
# Keep the speed and handoff boundaries source-visible so a future readiness
# gate cannot silently reintroduce the long-load/title-flash regression.
foreach(_march_manager_requirement IN ITEMS
        "m_vrMarchDelayMs = Policy::VRMarchRenderDelayMs("
        "VR custom presentation shown at native OPEN"
        "UpdateBackgroundOverlayTexture(combined)"
        "RE::Console::ExecuteCommand(\"player.moveto player\")"
        "Policy::kVRMarchPostCloseHoldMs"
        "CompositeTipsIntoBg()")
    string(FIND "${_loading_manager_source}"
        "${_march_manager_requirement}" _march_manager_position)
    if(_march_manager_position EQUAL -1)
        message(FATAL_ERROR
            "March VR manager contract is missing "
            "'${_march_manager_requirement}'")
    endif()
endforeach()

string(FIND "${_loading_manager_source}"
    "m_vrMarchPresentationAt" _delayed_vr_overlay_position)
if(NOT _delayed_vr_overlay_position EQUAL -1)
    message(FATAL_ERROR
        "VR custom presentation must own native OPEN immediately; "
        "do not restore the delayed m_vrMarchPresentationAt gate")
endif()

foreach(_march_main_requirement IN ITEMS
        "kVRMainLoopCallsite = 0xD8405E"
        "TickVRMarchMainLoop()")
    string(FIND "${_main_source}" "${_march_main_requirement}"
        _march_main_position)
    if(_march_main_position EQUAL -1)
        message(FATAL_ERROR
            "March direct main-loop contract is missing "
            "'${_march_main_requirement}'")
    endif()
endforeach()

file(READ "${_d3d_source_path}" _d3d_source)
foreach(_march_submit_requirement IN ITEMS
        "if (self && eye == 1 &&"
        "self->m_deferredNOPPending.load(std::memory_order_acquire)"
        "VRCompositorHelper::ApplySceneFade();"
        "March timing owns presentation")
    string(FIND "${_d3d_source}" "${_march_submit_requirement}"
        _march_submit_position)
    if(_march_submit_position EQUAL -1)
        message(FATAL_ERROR
            "March right-eye Submit contract is missing "
            "'${_march_submit_requirement}'")
    endif()
endforeach()

# Flat mode-3 native-minimal screens do not need a tip capture, but disabling
# that capture must retain the exact owner/content/serial until an opaque black
# Present succeeds. Keep this cross-component regression contract visible in
# release validation; the ordinary disable path must continue to invalidate.
file(READ "${_runtime_policy_source_path}" _runtime_policy_source)
foreach(_flat_minimal_manager_requirement IN ITEMS
        "ShouldPreserveFlatMinimalSelectionWithoutTipCapture("
        "DisableTipsCapturePreservingFlatNativeSelection()")
    string(FIND "${_loading_manager_source}"
        "${_flat_minimal_manager_requirement}" _flat_minimal_manager_position)
    if(_flat_minimal_manager_position EQUAL -1)
        message(FATAL_ERROR
            "Flat native-minimal OPEN contract is missing "
            "'${_flat_minimal_manager_requirement}'")
    endif()
endforeach()
foreach(_flat_minimal_compositor_requirement IN ITEMS
        "ConfigureTipsExtraction(enabled, false);"
        "ConfigureTipsExtraction(false, true);"
        "owner/content/serial retained for solid-black")
    string(FIND "${_d3d_source}"
        "${_flat_minimal_compositor_requirement}"
        _flat_minimal_compositor_position)
    if(_flat_minimal_compositor_position EQUAL -1)
        message(FATAL_ERROR
            "Flat native-minimal compositor contract is missing "
            "'${_flat_minimal_compositor_requirement}'")
    endif()
endforeach()
string(FIND "${_runtime_policy_source}" "!a_nativeOwnerKnown"
    _flat_owner_gate_position)
if(_flat_owner_gate_position EQUAL -1)
    message(FATAL_ERROR
        "Flat freeze policy must retain its native-owner requirement")
endif()

# Release mode 2/3 depends on this complete art set.  Only DDS files are
# accepted, and the magic check rejects renamed/corrupt inputs early.
file(GLOB _asset_entries LIST_DIRECTORIES false "${_asset_dir}/*")
set(_dds_files)
foreach(_asset IN LISTS _asset_entries)
    get_filename_component(_extension "${_asset}" EXT)
    string(TOLOWER "${_extension}" _extension)
    if(NOT _extension STREQUAL ".dds")
        message(FATAL_ERROR "Unexpected non-DDS release asset: ${_asset}")
    endif()
    file(READ "${_asset}" _dds_magic LIMIT 4 HEX)
    string(TOUPPER "${_dds_magic}" _dds_magic)
    if(NOT _dds_magic STREQUAL "44445320")
        message(FATAL_ERROR "Invalid DDS header in release asset: ${_asset}")
    endif()
    # DDS stores height then width as little-endian uint32 values at offsets
    # 12 and 16. The VR presentation expects the curated 2:1 landscape set;
    # reject a square/portrait file instead of silently stretching it in-game.
    file(READ "${_asset}" _dds_dimensions OFFSET 12 LIMIT 8 HEX)
    string(TOUPPER "${_dds_dimensions}" _dds_dimensions)
    if(NOT _dds_dimensions STREQUAL "0004000000080000")
        message(FATAL_ERROR
            "Loading-screen DDS must be 2048x1024 landscape: ${_asset}"
        )
    endif()

    # The runtime VR path supports only the legacy formats below. Validate the
    # complete curated payload here so a truncated file or unsupported FourCC
    # cannot pass packaging and fail asynchronously in the first game session.
    file(READ "${_asset}" _dds_header_size OFFSET 4 LIMIT 4 HEX)
    file(READ "${_asset}" _dds_mip_count OFFSET 28 LIMIT 4 HEX)
    file(READ "${_asset}" _dds_pf_size OFFSET 76 LIMIT 4 HEX)
    file(READ "${_asset}" _dds_fourcc OFFSET 84 LIMIT 4 HEX)
    string(TOUPPER "${_dds_header_size}" _dds_header_size)
    string(TOUPPER "${_dds_mip_count}" _dds_mip_count)
    string(TOUPPER "${_dds_pf_size}" _dds_pf_size)
    string(TOUPPER "${_dds_fourcc}" _dds_fourcc)
    if(NOT _dds_header_size STREQUAL "7C000000" OR
       NOT _dds_pf_size STREQUAL "20000000")
        message(FATAL_ERROR "Malformed DDS header in release asset: ${_asset}")
    endif()

    set(_expected_dds_size 0)
    if(_dds_fourcc STREQUAL "44585431") # DXT1 / BC1
        if(_dds_mip_count STREQUAL "00000000" OR
           _dds_mip_count STREQUAL "01000000")
            set(_expected_dds_size 1048704)
        elseif(_dds_mip_count STREQUAL "0C000000")
            set(_expected_dds_size 1398248)
        endif()
    elseif(_dds_fourcc STREQUAL "44585435") # DXT5 / BC3
        if(_dds_mip_count STREQUAL "00000000" OR
           _dds_mip_count STREQUAL "01000000")
            set(_expected_dds_size 2097280)
        elseif(_dds_mip_count STREQUAL "0C000000")
            set(_expected_dds_size 2796368)
        endif()
    endif()
    file(SIZE "${_asset}" _actual_dds_size)
    if(_expected_dds_size EQUAL 0 OR
       NOT _actual_dds_size EQUAL _expected_dds_size)
        message(FATAL_ERROR
            "Unsupported or truncated release DDS "
            "(FourCC=${_dds_fourcc}, mips=${_dds_mip_count}, "
            "size=${_actual_dds_size}, expected=${_expected_dds_size}): "
            "${_asset}")
    endif()
    list(APPEND _dds_files "${_asset}")
endforeach()
list(SORT _dds_files)
list(LENGTH _dds_files _dds_count)
if(NOT _dds_count EQUAL 80)
    message(FATAL_ERROR "Expected exactly 80 loading-screen DDS assets, found ${_dds_count}")
endif()

message(STATUS
    "Release inputs valid: structured MCM JSON, unique INI defaults, "
    "performance policy, and ${_dds_count} landscape 2048x1024 DDS assets"
)
