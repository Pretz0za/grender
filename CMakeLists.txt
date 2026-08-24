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
option(GRENDER_ENABLE_STEP_PROFILING
    "Time every gviz embedder Step() call in the example apps and print avg/min/max on exit" OFF)
option(GRENDER_FORCE_O3
    "Compile grender and gviz with -O3 regardless of CMAKE_BUILD_TYPE" OFF)
set(GRENDER_GVIZ_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../gviz" CACHE PATH
    "Path to the gviz repository (provides gviz public headers and the gviz library)")

if(GRENDER_FORCE_O3)
    # Directory-scoped, so it covers grender, every example, and (applied
    # before gviz's add_subdirectory() below) gviz itself uniformly -- same
    # reasoning as the EMSCRIPTEN exception-handling flags further down.
    # Added via add_compile_options rather than overwriting CMAKE_CXX_FLAGS,
    # so it lands after CMAKE_CXX_FLAGS_<CONFIG> on the compiler command
    # line and its -O3 wins regardless of CMAKE_BUILD_TYPE (Debug's -O0
    # included).
    if(MSVC)
        add_compile_options(/O2)
    else()
        add_compile_options(-O3)
    endif()
endif()

if(EMSCRIPTEN)
    # Both gviz and grender lean on C++ exceptions for error handling
    # (gviz::NoLayoutError, gviz::NotATreeError, ...), but Emscripten
    # disables exception catching by default for code size/speed. Applied
    # here (before gviz's add_subdirectory() below) so it covers gviz,
    # grender, and every example uniformly -- directory-scoped
    # add_compile_options/add_link_options propagate to subdirectories added
    # afterward.
    #
    # Deliberately the OLD JS-based exception model (DISABLE_EXCEPTION_CATCHING=0),
    # not -fwasm-exceptions: the new wasm-EH proposal is documented as
    # incompatible with -sASYNCIFY (needed below for the WebGPU adapter/device
    # request), and emcc's silent fallback when both were requested together
    # linked our -fwasm-exceptions object files against the *legacy* prebuilt
    # libc++abi/libunwind -- an ABI mismatch that corrupted memory at runtime
    # (a wasm32 build genuinely crashed while the equivalent native build was
    # fine, always compile+link this the same way on both sides instead of
    # letting the two mechanisms disagree silently).
    add_compile_options(-sDISABLE_EXCEPTION_CATCHING=0)
    add_link_options(-sDISABLE_EXCEPTION_CATCHING=0)

    # ------------------------------------------------------------------------------
    # wgpu-native and GLFW (native-only, see the else() branch below) are
    # replaced by Emscripten's own ports: emdawnwebgpu implements webgpu.h on
    # top of the browser's WebGPU, and -sUSE_GLFW=3 is Emscripten's built-in
    # GLFW3 emulation over a <canvas>. Both targets keep their native names
    # (wgpu_native, glfw) as INTERFACE libraries carrying the right
    # emcc flags, so every target_link_libraries() call site below is
    # unchanged between platforms.
    # ------------------------------------------------------------------------------
    add_library(wgpu_native INTERFACE)
    target_compile_options(wgpu_native INTERFACE --use-port=emdawnwebgpu)
    target_link_options(wgpu_native INTERFACE --use-port=emdawnwebgpu -sASYNCIFY)

    add_library(glfw INTERFACE)
    target_compile_options(glfw INTERFACE -sUSE_GLFW=3)
    target_link_options(glfw INTERFACE -sUSE_GLFW=3)
else()
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
endif()

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
# Dear ImGui + ImPlot: every on-screen overlay (stats charts, vertex list,
# vertex-info panel, caption banner, command console) is an ImGui/ImPlot
# window drawn on top of the WebGPU surface via the official imgui_impl_wgpu
# + imgui_impl_glfw backends -- lifecycle wiring lives in grRenderer.cpp next
# to the GLFW/WebGPU device it depends on; window-building code lives in
# grStats.cpp/grListOverlay.cpp/grVertexOverlay.cpp/grCaption.cpp/
# grConsole.cpp, one per overlay, unchanged from before this migration. Only
# the two graph instanced draw calls and the .obj picture-in-picture panel
# stay native WebGPU (grRenderer.cpp / grObjOverlay.cpp).
#
# Vendored as extracted source under third-party/ (see scripts/setup-deps.sh),
# the same pinned-not-FetchContent convention as GLFW/wgpu-native. Unlike
# GLFW, upstream ImGui/ImPlot ship no CMakeLists.txt of their own, so there's
# no add_subdirectory() step -- their sources are just added to the grender
# target directly below.
# ------------------------------------------------------------------------------
set(GRENDER_IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third-party/imgui" CACHE PATH
    "Path to extracted Dear ImGui source tree")
set(GRENDER_IMPLOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third-party/implot" CACHE PATH
    "Path to extracted ImPlot source tree")

if(NOT EXISTS "${GRENDER_IMGUI_DIR}/imgui.cpp")
    message(FATAL_ERROR
        "Dear ImGui not found at ${GRENDER_IMGUI_DIR}.\n"
        "Run ./scripts/setup-deps.sh once, or set -DGRENDER_IMGUI_DIR=...")
endif()
if(NOT EXISTS "${GRENDER_IMPLOT_DIR}/implot.cpp")
    message(FATAL_ERROR
        "ImPlot not found at ${GRENDER_IMPLOT_DIR}.\n"
        "Run ./scripts/setup-deps.sh once, or set -DGRENDER_IMPLOT_DIR=...")
endif()

add_library(imgui STATIC
    "${GRENDER_IMGUI_DIR}/imgui.cpp"
    "${GRENDER_IMGUI_DIR}/imgui_draw.cpp"
    "${GRENDER_IMGUI_DIR}/imgui_tables.cpp"
    "${GRENDER_IMGUI_DIR}/imgui_widgets.cpp"
    "${GRENDER_IMGUI_DIR}/backends/imgui_impl_glfw.cpp"
    "${GRENDER_IMGUI_DIR}/backends/imgui_impl_wgpu.cpp"
    "${GRENDER_IMPLOT_DIR}/implot.cpp"
    "${GRENDER_IMPLOT_DIR}/implot_items.cpp"
)
target_include_directories(imgui PUBLIC
    "${GRENDER_IMGUI_DIR}"
    "${GRENDER_IMGUI_DIR}/backends"
    "${GRENDER_IMPLOT_DIR}"
)
target_link_libraries(imgui PUBLIC glfw wgpu_native)
if(NOT EMSCRIPTEN)
    # Exactly one of these must be defined for imgui_impl_wgpu.cpp when not
    # targeting Emscripten (its own emdawnwebgpu port is auto-detected by the
    # backend instead) -- see imgui_impl_wgpu.h. grender pins wgpu-native
    # (scripts/setup-deps.sh), not Dawn.
    target_compile_definitions(imgui PUBLIC IMGUI_IMPL_WEBGPU_BACKEND_WGPU)
endif()
if(NOT MSVC)
    # Vendored code: don't hold it to grender's own -Wall -Wextra below.
    target_compile_options(imgui PRIVATE -w)
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
    src/grCaption.cpp
    src/grPCA.cpp
    src/grObjMesh.cpp
    src/grObjOverlay.cpp
    src/grTextureMap.cpp
    src/grProfiling.cpp
)

if(GRENDER_ENABLE_PROFILING)
    target_compile_definitions(grender PRIVATE GRENDER_ENABLE_PROFILING)
endif()

if(GRENDER_ENABLE_STEP_PROFILING)
    # PUBLIC, not PRIVATE like GRENDER_ENABLE_PROFILING above: step timing is
    # measured at the Step() call sites in the example apps themselves (see
    # include/grender/grStepProfiling.h), not inside the library, so the
    # define has to propagate to every target that links grender.
    target_compile_definitions(grender PUBLIC GRENDER_ENABLE_STEP_PROFILING)
endif()

if(APPLE)
    target_sources(grender PRIVATE src/grSurfaceCocoa.mm src/grMenuCocoa.mm)
    target_link_libraries(grender PRIVATE "-framework AppKit")
elseif(EMSCRIPTEN)
    target_sources(grender PRIVATE src/grSurfaceWasm.cpp src/grPlatformMenu.cpp)
else()
    target_sources(grender PRIVATE src/grPlatformMenu.cpp)
endif()

target_include_directories(grender
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PUBLIC ${GRENDER_GVIZ_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third-party
    PRIVATE "$ENV{HOME}/include"
)

if(NOT EMSCRIPTEN)
    # Not needed on wasm: grPCA.cpp's LAPACK-dependent (>3D) path is compiled
    # out under __EMSCRIPTEN__ (see grPCA.cpp) since no wasm-targeting demo
    # uses a >3D embedding yet.
    find_library(GRENDER_OPENBLAS openblas HINTS "$ENV{HOME}/lib")
    if(NOT GRENDER_OPENBLAS)
        message(FATAL_ERROR "OpenBLAS not found in $ENV{HOME}/lib (needed for 4D PCA)")
    endif()
endif()

target_link_libraries(grender
    PUBLIC wgpu_native
    PRIVATE glfw imgui
)
if(GRENDER_OPENBLAS)
    target_link_libraries(grender PRIVATE ${GRENDER_OPENBLAS})
endif()

if(NOT MSVC)
    target_compile_options(grender PRIVATE -Wall -Wextra)
endif()

# ------------------------------------------------------------------------------
# Examples
# ------------------------------------------------------------------------------
if(GRENDER_BUILD_EXAMPLES AND TARGET gviz)
    add_executable(sierpinskiDemo examples/sierpinskiDemo.cpp)
    target_link_libraries(sierpinskiDemo PRIVATE grender gviz)
    if(EMSCRIPTEN)
        target_link_options(sierpinskiDemo PRIVATE
            # Growing memory (ALLOW_MEMORY_GROWTH) from inside an
            # Asyncify-wrapped call is a known-fragile combination -- our
            # per-frame emscripten_set_main_loop_arg callback ends up
            # Asyncify-wrapped too (it's invoked through an indirect
            # dynCall, and Asyncify's default instrumentation conservatively
            # wraps every indirect-call target it can't prove is
            # unreachable from the async adapter/device request), so a
            # mid-frame malloc that needs to grow memory can corrupt state.
            # A large enough fixed initial heap avoids ever growing during
            # a normal session instead of fighting that interaction.
            -sINITIAL_MEMORY=134217728
            -sALLOW_MEMORY_GROWTH=1
            -sASSERTIONS=2
            -g
        )
        set_target_properties(sierpinskiDemo PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/wasm")
    endif()

    # webDemo: the interactive front-end (embedder/graph selection, live
    # tuning -- see wasm/webDemo.html + wasm/webDemoUI.js). Unlike
    # sierpinskiDemo it has no fixed content of its own; every graph and
    # embedder run is driven from JS through examples/webDemo.cpp's
    # EMSCRIPTEN_KEEPALIVE exports, so it also needs malloc/free exported
    # (the JS side allocates on the wasm heap to hand over edge arrays) and
    # ccall/cwrap/UTF8ToString kept in the runtime (the latter for reading
    # webLastError() back as a JS string).
    add_executable(webDemo examples/webDemo.cpp)
    target_link_libraries(webDemo PRIVATE grender gviz)
    if(EMSCRIPTEN)
        target_link_options(webDemo PRIVATE
            # See sierpinskiDemo's identical block above for why these are
            # sized/set this way.
            -sINITIAL_MEMORY=134217728
            -sALLOW_MEMORY_GROWTH=1
            -sASSERTIONS=2
            -g
            -sEXPORTED_FUNCTIONS=_main,_malloc,_free
            # HEAPU32/HEAPF64: wasm/webDemoUI.js writes edge arrays directly
            # into wasm memory through these views before calling
            # webGraphStageJSON (see stageFromSchema there) -- grep that file
            # for `Module.HEAP` before adding a third one here.
            # FS: FORCE_FILESYSTEM below only guarantees the filesystem code
            # itself is linked in, not that Module.FS is reachable from JS --
            # that still needs FS listed here explicitly, same as any other
            # runtime method.
            -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,HEAPU32,HEAPF64,FS
            # FORCE_FILESYSTEM: guarantees Module.FS (writeFile/unlink) is
            # present regardless of what gviz::io ends up pulling in on its
            # own -- webDemoUI.js's .gexf/.edges upload path writes the
            # uploaded file into MEMFS before webStageFromFile reads it back
            # via gviz::io::LoadFromEdgesFile/LoadFromGexfFile.
            -sFORCE_FILESYSTEM=1
        )
        set_target_properties(webDemo PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/wasm")
    endif()
endif()

# The rest of the example apps (GRIP/ForceAtlas/dataset/texture demos) are
# native-only for now: they read gviz's data/ tree straight off disk and
# haven't been ported to wasm's virtual filesystem or Emscripten's
# preload-file packaging.
if(GRENDER_BUILD_EXAMPLES AND TARGET gviz AND NOT EMSCRIPTEN)
    add_executable(gripDemo examples/gripDemo.cpp)
    target_compile_definitions(gripDemo PRIVATE
        GRENDER_GVIZ_DATA_DIR="${GRENDER_GVIZ_DIR}/data")
    target_link_libraries(gripDemo PRIVATE grender gviz)

    add_executable(millionDemo examples/millionDemo.cpp)
    target_link_libraries(millionDemo PRIVATE grender gviz)

    add_executable(treeDemo examples/treeDemo.cpp)
    target_link_libraries(treeDemo PRIVATE grender gviz)

    add_executable(rtTraceDemo examples/rtTraceDemo.cpp)
    target_link_libraries(rtTraceDemo PRIVATE grender gviz)

    add_executable(tutteDemo examples/tutteDemo.cpp)
    target_link_libraries(tutteDemo PRIVATE grender gviz)

    add_executable(forceEmbedderDemo examples/forceEmbedderDemo.cpp)
    target_compile_definitions(forceEmbedderDemo PRIVATE
        GRENDER_GVIZ_DATA_DIR="${GRENDER_GVIZ_DIR}/data")
    target_link_libraries(forceEmbedderDemo PRIVATE grender gviz)

    add_executable(kamadaKawaiDemo examples/kamadaKawaiDemo.cpp)
    target_compile_definitions(kamadaKawaiDemo PRIVATE
        GRENDER_GVIZ_DATA_DIR="${GRENDER_GVIZ_DIR}/data")
    target_link_libraries(kamadaKawaiDemo PRIVATE grender gviz)

    add_executable(textureMapDemo examples/textureMapDemo.cpp)
    target_link_libraries(textureMapDemo PRIVATE grender gviz)

    add_executable(textureMapUnitTest examples/textureMapUnitTest.cpp)
    # This test reaches past grender's public API into grInternal.h directly
    # (see textureMapUnitTest.cpp) to exercise grTextureMapComputeUV/
    # ComputeFaceValidity, the pure-CPU helpers split out for exactly this --
    # no gviz/GPU types involved. grInternal.h now pulls in imgui.h/implot.h
    # too (for the overlay declarations every other grInternal.h consumer
    # needs), so this target needs imgui's include dirs even though it never
    # calls into ImGui itself.
    target_include_directories(textureMapUnitTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(textureMapUnitTest PRIVATE grender gviz imgui)
endif()
