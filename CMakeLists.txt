cmake_minimum_required(VERSION 3.23)

set(NAME "LoadingScreens")
set(VERSION 1.5.0)

message(">>> Start build for '${NAME}' v${VERSION}")

set(ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(SOURCE_DIR "${ROOT_DIR}/src")
set(BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}")

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(FATAL_ERROR ">> In-source builds not allowed.")
endif()

# Setup vcpkg BEFORE project()
macro(set_from_environment VARIABLE)
  if(NOT DEFINED ${VARIABLE} AND DEFINED ENV{${VARIABLE}})
    set(${VARIABLE} $ENV{${VARIABLE}})
  endif()
endmacro()

set_from_environment(VCPKG_ROOT)
if(DEFINED VCPKG_ROOT)
  set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
  set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "")
  set(VCPKG_OVERLAY_TRIPLETS "${CMAKE_CURRENT_SOURCE_DIR}/triplets" CACHE STRING "")
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "")
else()
  message(FATAL_ERROR ">> Variable VCPKG_ROOT is not set!")
endif()

# CommonLibF4 (alandtse fork — supports F4, NG, and VR in one build)
set(COMMONLIBF4_PATH "C:/Development/Community Shaders FO4/skyrim-community-shaders-dev/extern/CommonLibF4/CommonLibF4" CACHE STRING "")
if(NOT EXISTS "${COMMONLIBF4_PATH}/CMakeLists.txt")
  message(FATAL_ERROR ">> CommonLibF4 not found at ${COMMONLIBF4_PATH}")
endif()
message(">>> Using CommonLibF4 at: ${COMMONLIBF4_PATH}")

# Enable multi-runtime support (F4 + VR, skip NG for now)
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

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    "${SOURCE_DIR}/*.cpp"
    "${SOURCE_DIR}/*.h"
)

add_library(${PROJECT_NAME} SHARED ${SOURCES})

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)

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

if(CMAKE_GENERATOR MATCHES "Visual Studio")
    target_compile_options(${PROJECT_NAME} PRIVATE
        /MP4
        /W3
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
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ROOT_DIR}/package/Data/MCM/Config/VRLoadingScreens"
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}> "${ROOT_DIR}/package/Data/F4SE/Plugins/"
    COMMENT "Packaging ${PROJECT_NAME}..."
)

message(">>> ${PROJECT_NAME} configured successfully")
