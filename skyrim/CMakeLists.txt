cmake_minimum_required(VERSION 3.23)

set(NAME "FasterLoadscreens")
set(VERSION 1.7.1)

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
  set(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "")
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL" CACHE STRING "")
else()
  message(FATAL_ERROR ">> Variable VCPKG_ROOT is not set!")
endif()

project(${NAME} VERSION ${VERSION} LANGUAGES CXX)
message(">>> Building project '${PROJECT_NAME}' v${PROJECT_VERSION}")

add_compile_definitions(
    _CRT_SECURE_NO_WARNINGS
    SKYRIMSE
)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)

# CommonLibSSE-NG (via vcpkg, colorglass registry)
find_package(CommonLibSSE CONFIG REQUIRED)

# MinHook (loading-loop + scene-ready-hold hooks)
find_package(minhook CONFIG REQUIRED)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    "${SOURCE_DIR}/*.cpp"
    "${SOURCE_DIR}/*.h"
)

add_commonlibsse_plugin(${PROJECT_NAME} SOURCES ${SOURCES})

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE "${SOURCE_DIR}/PCH.h")

target_include_directories(${PROJECT_NAME} PRIVATE
    "${SOURCE_DIR}"
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    minhook::minhook
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W3
        /permissive-
        /Zc:preprocessor
        /Zc:__cplusplus
        /Zi
    )
    target_link_options(${PROJECT_NAME} PRIVATE
        /DEBUG
        /OPT:REF
        /OPT:ICF
    )
endif()

message(">>> ${PROJECT_NAME} configured successfully")
