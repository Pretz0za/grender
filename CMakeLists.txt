cmake_minimum_required(VERSION 3.20)
project(grender LANGUAGES C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(APPLE)
    enable_language(OBJC)
endif()

option(GRENDER_BUILD_EXAMPLES "Build grender example apps" ON)
set(GRENDER_GVIZ_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../gviz" CACHE PATH
    "Path to the gviz repository (provides gviz public headers and the graphvis library)")

include(FetchContent)

# ------------------------------------------------------------------------------
# wgpu-native: prebuilt WebGPU implementation (webgpu.h C API).
# Pinned release; see README for the upgrade procedure.
# ------------------------------------------------------------------------------
set(WGPU_NATIVE_VERSION "v29.0.1.1")

if(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(WGPU_NATIVE_PLATFORM "macos-aarch64")
    else()
        set(WGPU_NATIVE_PLATFORM "macos-x86_64")
    endif()
elseif(WIN32)
    set(WGPU_NATIVE_PLATFORM "windows-x86_64-msvc")
else()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(WGPU_NATIVE_PLATFORM "linux-aarch64")
    else()
        set(WGPU_NATIVE_PLATFORM "linux-x86_64")
    endif()
endif()

FetchContent_Declare(wgpu_native
    URL "https://github.com/gfx-rs/wgpu-native/releases/download/${WGPU_NATIVE_VERSION}/wgpu-${WGPU_NATIVE_PLATFORM}-release.zip"
)
FetchContent_MakeAvailable(wgpu_native)

add_library(wgpu_native STATIC IMPORTED)
if(WIN32)
    set_target_properties(wgpu_native PROPERTIES
        IMPORTED_LOCATION "${wgpu_native_SOURCE_DIR}/lib/wgpu_native.lib")
else()
    set_target_properties(wgpu_native PROPERTIES
        IMPORTED_LOCATION "${wgpu_native_SOURCE_DIR}/lib/libwgpu_native.a")
endif()
target_include_directories(wgpu_native INTERFACE "${wgpu_native_SOURCE_DIR}/include")

if(APPLE)
    target_link_libraries(wgpu_native INTERFACE
        "-framework CoreFoundation"
        "-framework QuartzCore"
        "-framework Metal"
    )
elseif(WIN32)
    target_link_libraries(wgpu_native INTERFACE
        d3dcompiler ws2_32 userenv bcrypt ntdll opengl32 Propsys RuntimeObject)
else()
    target_link_libraries(wgpu_native INTERFACE m dl)
endif()

# ------------------------------------------------------------------------------
# GLFW: windowing and input for native targets.
# ------------------------------------------------------------------------------
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(glfw
    URL "https://github.com/glfw/glfw/releases/download/3.4/glfw-3.4.zip"
)
FetchContent_MakeAvailable(glfw)

# ------------------------------------------------------------------------------
# gviz: the embedding library whose public API grender consumes.
# Built here (with its own options) so examples can link against it; the
# grender library itself only needs the public headers.
# ------------------------------------------------------------------------------
if(GRENDER_BUILD_EXAMPLES AND EXISTS "${GRENDER_GVIZ_DIR}/CMakeLists.txt")
    set(ENABLE_ASAN OFF CACHE BOOL "" FORCE)
    add_subdirectory("${GRENDER_GVIZ_DIR}" gviz-build EXCLUDE_FROM_ALL)
endif()

# ------------------------------------------------------------------------------
# grender library
# ------------------------------------------------------------------------------
add_library(grender STATIC
    src/grRenderer.c
    src/grCamera.c
    src/grTopology.c
    src/grStats.c
)

if(APPLE)
    target_sources(grender PRIVATE src/grSurfaceCocoa.m)
endif()

target_include_directories(grender
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PUBLIC ${GRENDER_GVIZ_DIR}/include
)

target_link_libraries(grender
    PUBLIC wgpu_native
    PRIVATE glfw
)

if(NOT MSVC)
    target_compile_options(grender PRIVATE -Wall -Wextra)
endif()

# ------------------------------------------------------------------------------
# Examples
# ------------------------------------------------------------------------------
if(GRENDER_BUILD_EXAMPLES AND TARGET graphvis)
    add_executable(gripDemo examples/gripDemo.c)
    target_link_libraries(gripDemo PRIVATE grender graphvis)

    add_executable(millionDemo examples/millionDemo.c)
    target_link_libraries(millionDemo PRIVATE grender graphvis)
endif()
