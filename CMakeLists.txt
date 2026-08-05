cmake_minimum_required(VERSION 3.20)
project(grender LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The grender library itself is pure C++20, but examples/*.c (ported in a
# follow-up pass) are still C, so C must stay enabled at configure time or
# CMake refuses to generate targets referencing those sources at all.
enable_language(C)

if(APPLE)
    # Both OBJC and OBJCXX: GLFW's own Cocoa backend (third-party/glfw) is
    # plain Objective-C (.m) and needs OBJC enabled or CMake silently
    # compiles those sources with the C++ front end instead, which rejects
    # GLFW's C-style implicit void*-to-T* conversions. grender's own
    # grSurfaceCocoa.mm/grMenuCocoa.mm are Objective-C++ (.mm) because they
    # include grInternal.h, which pulls in gviz's C++ headers transitively --
    # plain Objective-C has no access to the C++ standard library.
    enable_language(OBJC)
    enable_language(OBJCXX)
endif()

option(GRENDER_BUILD_EXAMPLES "Build grender example apps" ON)
option(GRENDER_ENABLE_PROFILING
    "Time every rendered frame and print avg/p50/p95/p99/max on exit" OFF)
set(GRENDER_GVIZ_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../gviz" CACHE PATH
    "Path to the gviz repository (provides gviz public headers and the gviz library)")

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
    src/grRenderer.cpp
    src/grCamera.cpp
    src/grTopology.cpp
    src/grStats.cpp
    src/grVertexOverlay.cpp
    src/grListOverlay.cpp
    src/grConsole.cpp
    src/grPCA.cpp
    src/grObjMesh.cpp
    src/grObjOverlay.cpp
    src/grTextureMap.cpp
    src/grProfiling.cpp
)

if(GRENDER_ENABLE_PROFILING)
    target_compile_definitions(grender PRIVATE GRENDER_ENABLE_PROFILING)
endif()

if(APPLE)
    target_sources(grender PRIVATE src/grSurfaceCocoa.mm src/grMenuCocoa.mm)
    target_link_libraries(grender PRIVATE "-framework AppKit")
else()
    target_sources(grender PRIVATE src/grPlatformMenu.cpp)
endif()

target_include_directories(grender
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PUBLIC ${GRENDER_GVIZ_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third-party
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
if(GRENDER_BUILD_EXAMPLES AND TARGET gviz)
    add_executable(gripDemo examples/gripDemo.c)
    target_compile_definitions(gripDemo PRIVATE
        GRENDER_GVIZ_DATA_DIR="${GRENDER_GVIZ_DIR}/data")
    target_link_libraries(gripDemo PRIVATE grender gviz)

    add_executable(millionDemo examples/millionDemo.c)
    target_link_libraries(millionDemo PRIVATE grender gviz)

    add_executable(datasetDemo examples/datasetDemo.c)
    target_compile_definitions(datasetDemo PRIVATE
        GRENDER_GVIZ_DATA_DIR="${GRENDER_GVIZ_DIR}/data")
    target_link_libraries(datasetDemo PRIVATE grender gviz)

    add_executable(treeDemo examples/treeDemo.c)
    target_link_libraries(treeDemo PRIVATE grender gviz)

    add_executable(tutteDemo examples/tutteDemo.c)
    target_link_libraries(tutteDemo PRIVATE grender gviz)

    add_executable(forceEmbedderDemo examples/forceEmbedderDemo.c)
    target_compile_definitions(forceEmbedderDemo PRIVATE
        GRENDER_GVIZ_DATA_DIR="${GRENDER_GVIZ_DIR}/data")
    target_link_libraries(forceEmbedderDemo PRIVATE grender gviz)

    add_executable(textureMapDemo examples/textureMapDemo.c)
    target_link_libraries(textureMapDemo PRIVATE grender gviz)

    add_executable(textureMapUnitTest examples/textureMapUnitTest.c)
    target_include_directories(textureMapUnitTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(textureMapUnitTest PRIVATE grender gviz)
endif()
