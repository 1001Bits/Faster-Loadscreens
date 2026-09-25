cmake_minimum_required(VERSION 3.23)

set(NAME "LoadingScreens")
set(VERSION 2.1.1)

message(">>> Start build for '${NAME}' v${VERSION}")

set(ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(SOURCE_DIR "${ROOT_DIR}/src")
set(BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}")

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(FATAL_ERROR ">> In-source builds not allowed.")
endif()

# Setup vcpkg BEFORE project(). Preserve an explicitly supplied/cached toolchain
# so an existing build tree does not suddenly require the environment variable.
set(VCPKG_ROOT "" CACHE PATH "Path to a vcpkg checkout")
if(NOT VCPKG_ROOT AND DEFINED ENV{VCPKG_ROOT} AND NOT "$ENV{VCPKG_ROOT}" STREQUAL "")
  file(TO_CMAKE_PATH "$ENV{VCPKG_ROOT}" _vcpkg_from_environment)
  set(VCPKG_ROOT "${_vcpkg_from_environment}" CACHE PATH "Path to a vcpkg checkout" FORCE)
endif()

if(VCPKG_ROOT)
  if(NOT EXISTS "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    message(FATAL_ERROR ">> Invalid VCPKG_ROOT: ${VCPKG_ROOT}")
  endif()
  set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE FILEPATH "")
elseif(NOT DEFINED CMAKE_TOOLCHAIN_FILE OR CMAKE_TOOLCHAIN_FILE STREQUAL "")
  message(FATAL_ERROR
    ">> vcpkg was not found. Set VCPKG_ROOT, pass -DVCPKG_ROOT=C:/vcpkg, "
    "or pass an existing vcpkg CMAKE_TOOLCHAIN_FILE."
  )
endif()
set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "")
set(VCPKG_OVERLAY_TRIPLETS "${CMAKE_CURRENT_SOURCE_DIR}/triplets" CACHE STRING "")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "")

# CommonLibF4 (alandtse fork — supports F4, NG, and VR in one build).
#
# This dependency is deliberately fetch-free: builds must select an existing,
# reviewed checkout either with -DCOMMONLIBF4_PATH=... or the environment
# variable of the same name.  A conventional in-tree checkout is also accepted.
set(COMMONLIBF4_PATH "" CACHE PATH "Path to an existing alandtse/CommonLibF4 checkout")
set(COMMONLIBF4_EXPECTED_REVISION
    "2b64114a449ebdcaa82d33d83d8df9bb8d8092d5")
option(COMMONLIBF4_ALLOW_DIRTY
    "Allow local CommonLibF4 source modifications (non-reproducible developer build)"
    OFF)
if(NOT COMMONLIBF4_PATH)
  set(_commonlibf4_candidates)
  if(DEFINED ENV{COMMONLIBF4_PATH} AND NOT "$ENV{COMMONLIBF4_PATH}" STREQUAL "")
    list(APPEND _commonlibf4_candidates "$ENV{COMMONLIBF4_PATH}")
  endif()
  list(APPEND _commonlibf4_candidates
    "${ROOT_DIR}/extern/CommonLibF4"
    "${ROOT_DIR}/extern/CommonLibF4/CommonLibF4"
  )
  foreach(_candidate IN LISTS _commonlibf4_candidates)
    if(EXISTS "${_candidate}/CMakeLists.txt")
      file(REAL_PATH "${_candidate}" _commonlibf4_resolved)
      set(COMMONLIBF4_PATH "${_commonlibf4_resolved}" CACHE PATH
          "Path to an existing alandtse/CommonLibF4 checkout" FORCE)
      break()
    endif()
  endforeach()
endif()

if(NOT COMMONLIBF4_PATH OR NOT EXISTS "${COMMONLIBF4_PATH}/CMakeLists.txt")
  message(FATAL_ERROR
    ">> CommonLibF4 was not found. This project never downloads dependencies at configure time.\n"
    "   Pass an existing alandtse/CommonLibF4 checkout explicitly:\n"
    "     cmake -S . -B build -DCOMMONLIBF4_PATH=\"C:/src/CommonLibF4\"\n"
    "   or set the COMMONLIBF4_PATH environment variable before configuring."
  )
endif()

find_program(GIT_EXECUTABLE NAMES git REQUIRED)
execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${COMMONLIBF4_PATH}" rev-parse HEAD
    RESULT_VARIABLE _commonlibf4_git_result
    OUTPUT_VARIABLE _commonlibf4_revision
    ERROR_VARIABLE _commonlibf4_git_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT _commonlibf4_git_result EQUAL 0)
  message(FATAL_ERROR
    ">> COMMONLIBF4_PATH is not inside a readable Git checkout:\n"
    "   ${_commonlibf4_git_error}"
  )
endif()
if(NOT _commonlibf4_revision STREQUAL "${COMMONLIBF4_EXPECTED_REVISION}")
  message(FATAL_ERROR
    ">> Unsupported CommonLibF4 revision ${_commonlibf4_revision}.\n"
    "   Checkout the reviewed revision ${COMMONLIBF4_EXPECTED_REVISION}."
  )
endif()
execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${COMMONLIBF4_PATH}" status --porcelain -- .
    RESULT_VARIABLE _commonlibf4_status_result
    OUTPUT_VARIABLE _commonlibf4_changes
    ERROR_VARIABLE _commonlibf4_status_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT _commonlibf4_status_result EQUAL 0)
  message(FATAL_ERROR
    ">> Unable to inspect CommonLibF4 worktree:\n"
    "   ${_commonlibf4_status_error}"
  )
endif()
if(_commonlibf4_changes AND NOT COMMONLIBF4_ALLOW_DIRTY)
  message(FATAL_ERROR
    ">> CommonLibF4 has local source changes, so this build would not be reproducible.\n"
    "   Use a clean checkout at ${COMMONLIBF4_EXPECTED_REVISION}, or explicitly set\n"
    "   -DCOMMONLIBF4_ALLOW_DIRTY=ON for a non-release developer build."
  )
endif()
if(_commonlibf4_changes)
  message(WARNING ">>> CommonLibF4 contains explicitly allowed local changes")
  set(_commonlibf4_release_eligible FALSE)
else()
  set(_commonlibf4_release_eligible TRUE)
endif()
message(">>> Using CommonLibF4 at: ${COMMONLIBF4_PATH}")
message(">>> CommonLibF4 revision: ${_commonlibf4_revision}")

# Build the one DLL against flat, next-generation, and VR CommonLib interfaces.
set(ENABLE_FALLOUT_F4 ON CACHE BOOL "Enable Fallout 4 flat support" FORCE)
set(ENABLE_FALLOUT_NG ON CACHE BOOL "Enable Fallout 4 Next-Gen support" FORCE)
set(ENABLE_FALLOUT_VR ON CACHE BOOL "Enable Fallout 4 VR support" FORCE)
set(F4SE_SUPPORT_XBYAK ON CACHE BOOL "Enable xbyak trampoline support" FORCE)

project(${NAME} VERSION ${VERSION} LANGUAGES CXX)
message(">>> Building project '${PROJECT_NAME}' v${PROJECT_VERSION}")

add_compile_definitions(
    _CRT_SECURE_NO_WARNINGS
    _SILENCE_CXX23_ALIGNED_STORAGE_DEPRECATION_WARNING
    FALLOUT4
)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)

add_subdirectory(${COMMONLIBF4_PATH} "CommonLibF4" EXCLUDE_FROM_ALL)

find_path(SIMPLEINI_INCLUDE_DIRS "SimpleIni.h")
find_package(minhook CONFIG REQUIRED)

set(SOURCES
    "${SOURCE_DIR}/CellWorldspaceGuard.cpp"
    "${SOURCE_DIR}/CellWorldspaceGuard.h"
    "${SOURCE_DIR}/D3D11Compositor.cpp"
    "${SOURCE_DIR}/D3D11Compositor.h"
    "${SOURCE_DIR}/DDSOverlayCodec.h"
    "${SOURCE_DIR}/DoorPrefetch.cpp"
    "${SOURCE_DIR}/DoorPrefetch.h"
    "${SOURCE_DIR}/WorldspacePreload.cpp"
    "${SOURCE_DIR}/WorldspacePreload.h"
    "${SOURCE_DIR}/WorldspacePreloadState.h"
    "${SOURCE_DIR}/GameSettingTweaks.cpp"
    "${SOURCE_DIR}/GameSettingTweaks.h"
    "${SOURCE_DIR}/LoadingScreenManager.cpp"
    "${SOURCE_DIR}/LoadingScreenManager.h"
    "${SOURCE_DIR}/PapyrusOptimizer.cpp"
    "${SOURCE_DIR}/PapyrusOptimizer.h"
    "${SOURCE_DIR}/PCH.h"
    "${SOURCE_DIR}/RuntimePolicy.h"
    "${SOURCE_DIR}/PerformancePatches.cpp"
    "${SOURCE_DIR}/PerformancePatches.h"
    "${SOURCE_DIR}/PreloadDiagnostics.cpp"
    "${SOURCE_DIR}/PreloadDiagnostics.h"
    "${SOURCE_DIR}/SaveGamePrefetch.cpp"
    "${SOURCE_DIR}/SaveGamePrefetch.h"
    "${SOURCE_DIR}/VRCompositorHelper.cpp"
    "${SOURCE_DIR}/VRCompositorHelper.h"
    "${SOURCE_DIR}/main.cpp"
)

add_library(${PROJECT_NAME} SHARED ${SOURCES})

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE "${SOURCE_DIR}/PCH.h")

target_include_directories(${PROJECT_NAME} PRIVATE
    "${SOURCE_DIR}"
    ${SIMPLEINI_INCLUDE_DIRS}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    CommonLibF4::CommonLibF4
    minhook::minhook
    winmm
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

if(MSVC)
    # Keep a local address map for crash analysis and private runtime probes.
    # This developer artifact is never included in the release archive.
    target_link_options(${PROJECT_NAME} PRIVATE "/MAP:$<TARGET_FILE_DIR:${PROJECT_NAME}>/LoadingScreens.map")
    target_compile_options(${PROJECT_NAME} PRIVATE
        /MP4
        /W4
        /permissive-
        /Zc:preprocessor
        /Zc:__cplusplus
    )
endif()

# Post build
add_custom_command(
    TARGET ${PROJECT_NAME}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ROOT_DIR}/package/Data/F4SE/Plugins"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ROOT_DIR}/package/Data/MCM/Config/FasterLoadscreens"
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}> "${ROOT_DIR}/package/Data/F4SE/Plugins/"
    COMMENT "Packaging ${PROJECT_NAME}..."
)

# A release archive is always assembled from a new staging directory.  It uses
# the DLL produced by this target, the source-controlled MCM files, and exactly
# the supported DDS asset set; no file from an older archive can leak in.
set(RELEASE_ARCHIVE "${ROOT_DIR}/dist/Faster-Loadscreens-${PROJECT_VERSION}.zip")
set(RELEASE_PACKAGE_SCRIPT "${ROOT_DIR}/cmake/PackageRelease.cmake")
set(RELEASE_VALIDATE_SCRIPT "${ROOT_DIR}/cmake/ValidateReleaseInputs.cmake")

if(_commonlibf4_release_eligible)
    add_custom_target(package_release
        DEPENDS ${PROJECT_NAME}
        COMMAND ${CMAKE_COMMAND}
            "-DPROJECT_ROOT=${ROOT_DIR}"
            "-DBUILD_ROOT=${BUILD_DIR}"
            "-DBUILD_CONFIG=$<CONFIG>"
            "-DDLL_PATH=$<TARGET_FILE:${PROJECT_NAME}>"
            "-DSTAGE_DIR=${BUILD_DIR}/package-stage/$<CONFIG>"
            "-DARCHIVE_PATH=${RELEASE_ARCHIVE}"
            -P "${RELEASE_PACKAGE_SCRIPT}"
        BYPRODUCTS "${RELEASE_ARCHIVE}"
        COMMENT "Creating clean release archive ${RELEASE_ARCHIVE}"
        VERBATIM
    )
else()
    message(WARNING
        ">>> package_release is unavailable because CommonLibF4 has "
        "explicitly allowed local changes")
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(policy_tests
        "${ROOT_DIR}/tests/PolicyTests.cpp"
        "${SOURCE_DIR}/DDSOverlayCodec.h"
        "${SOURCE_DIR}/RuntimePolicy.h"
    )
    target_compile_features(policy_tests PRIVATE cxx_std_23)
    target_include_directories(policy_tests PRIVATE "${SOURCE_DIR}")
    add_test(NAME policy_logic COMMAND policy_tests)

    add_executable(worldspace_preload_tests "${ROOT_DIR}/tests/WorldspacePreloadTests.cpp")
    target_compile_features(worldspace_preload_tests PRIVATE cxx_std_23)
    target_include_directories(worldspace_preload_tests PRIVATE "${SOURCE_DIR}")
    add_test(NAME worldspace_preload_lifetime COMMAND worldspace_preload_tests)

    # Private-desktop integration probe; built only by naming this target.
    add_library(worldspace_probe SHARED EXCLUDE_FROM_ALL "${ROOT_DIR}/tools/WorldspaceRuntimeProbe.cpp")
    target_compile_features(worldspace_probe PRIVATE cxx_std_23)
    target_include_directories(worldspace_probe PRIVATE "${SOURCE_DIR}")
    target_link_libraries(worldspace_probe PRIVATE CommonLibF4::CommonLibF4)
    set_target_properties(worldspace_probe PROPERTIES OUTPUT_NAME "FLSWorldspaceProbe")

    add_test(
        NAME release_inputs
        COMMAND ${CMAKE_COMMAND}
            "-DPROJECT_ROOT=${ROOT_DIR}"
            -P "${RELEASE_VALIDATE_SCRIPT}"
    )

    if(_commonlibf4_release_eligible)
        # Exercise the same clean staging/archive code as the release target.
        # This test is intentionally independent of any pre-existing dist file.
        add_test(
            NAME release_archive
            COMMAND ${CMAKE_COMMAND}
                "-DPROJECT_ROOT=${ROOT_DIR}"
                "-DBUILD_ROOT=${BUILD_DIR}"
                "-DBUILD_CONFIG=$<CONFIG>"
                "-DDLL_PATH=$<TARGET_FILE:${PROJECT_NAME}>"
                "-DSTAGE_DIR=${BUILD_DIR}/ctest-package-stage/$<CONFIG>"
                "-DARCHIVE_PATH=${BUILD_DIR}/ctest-dist/Faster-Loadscreens-${PROJECT_VERSION}.zip"
                -P "${RELEASE_PACKAGE_SCRIPT}"
        )
        set_tests_properties(release_archive PROPERTIES RUN_SERIAL TRUE)
    endif()
endif()

message(">>> ${PROJECT_NAME} configured successfully")
