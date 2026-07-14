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

# ------------------------------------------------------------------------------
# wgpu-native: prebuilt WebGPU implementation (webgpu.h C API).
# Place an extracted release under third-party/wgpu-native (see scripts/setup-deps.sh).
# ------------------------------------------------------------------------------
set(GRENDER_WGPU_NATIVE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third-party/wgpu-native" CACHE PATH
    "Path to extracted wgpu-native release (must contain include/ and lib/)")

if(NOT EXISTS "${GRENDER_WGPU_NATIVE_DIR}/include/webgpu/webgpu.h")
    message(FATAL_ERROR
        "wgpu-native not found at ${GRENDER_WGPU_NATIVE_DIR}.\n"
        "Run ./scripts/setup-deps.sh once, or set -DGRENDER_WGPU_NATIVE_DIR=...")
endif()

add_library(wgpu_native STATIC IMPORTED)
if(WIN32)
    set_target_properties(wgpu_native PROPERTIES
        IMPORTED_LOCATION "${GRENDER_WGPU_NATIVE_DIR}/lib/wgpu_native.lib")
else()
    set_target_properties(wgpu_native PROPERTIES
        IMPORTED_LOCATION "${GRENDER_WGPU_NATIVE_DIR}/lib/libwgpu_native.a")
endif()
target_include_directories(wgpu_native INTERFACE "${GRENDER_WGPU_NATIVE_DIR}/include")

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
# Place the GLFW source tree under third-party/glfw (see scripts/setup-deps.sh).
# ------------------------------------------------------------------------------
set(GRENDER_GLFW_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third-party/glfw" CACHE PATH
    "Path to GLFW source tree")

if(NOT EXISTS "${GRENDER_GLFW_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "GLFW not found at ${GRENDER_GLFW_DIR}.\n"
        "Run ./scripts/setup-deps.sh once, or set -DGRENDER_GLFW_DIR=...")
endif()

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${GRENDER_GLFW_DIR}" glfw-build EXCLUDE_FROM_ALL)

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
    src/grPCA.c
)

if(APPLE)
    target_sources(grender PRIVATE src/grSurfaceCocoa.m src/grMenuCocoa.m)
    target_link_libraries(grender PRIVATE "-framework AppKit")
else()
    target_sources(grender PRIVATE src/grPlatformMenu.c)
endif()

target_include_directories(grender
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PUBLIC ${GRENDER_GVIZ_DIR}/include
    PRIVATE "$ENV{HOME}/include"
)

find_library(GRENDER_OPENBLAS openblas HINTS "$ENV{HOME}/lib")
if(NOT GRENDER_OPENBLAS)
    message(FATAL_ERROR "OpenBLAS not found in $ENV{HOME}/lib (needed for 4D PCA)")
endif()

target_link_libraries(grender
    PUBLIC wgpu_native
    PRIVATE glfw
    PRIVATE ${GRENDER_OPENBLAS}
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

    add_executable(datasetDemo examples/datasetDemo.c)
    target_compile_definitions(datasetDemo PRIVATE
        GRENDER_GVIZ_DATA_DIR="${GRENDER_GVIZ_DIR}/data")
    target_link_libraries(datasetDemo PRIVATE grender graphvis)

    add_executable(treeDemo examples/treeDemo.c)
    target_link_libraries(treeDemo PRIVATE grender graphvis)

    add_executable(tutteDemo examples/tutteDemo.c)
    target_link_libraries(tutteDemo PRIVATE grender graphvis)
endif()
