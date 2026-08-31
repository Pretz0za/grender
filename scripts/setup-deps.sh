#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
THIRD_PARTY="$ROOT/third-party"
WGPU_DIR="$THIRD_PARTY/wgpu-native"
GLFW_DIR="$THIRD_PARTY/glfw"
IMGUI_DIR="$THIRD_PARTY/imgui"
IMPLOT_DIR="$THIRD_PARTY/implot"

WGPU_NATIVE_VERSION="v29.0.1.1"
GLFW_VERSION="3.4"
# Dear ImGui + ImPlot: source vendored the same way GLFW is (extracted tree
# under third-party/, built directly as grender sources -- upstream ImGui
# ships no CMakeLists.txt of its own, so unlike GLFW there's no
# add_subdirectory step; CMakeLists.txt just adds the needed .cpp files to
# the grender target directly). Pinned so an unrelated dependency bump can't
# silently change overlay behavior/rendering.
IMGUI_VERSION="v1.91.9b"
IMPLOT_VERSION="v0.16"

case "$(uname -s)" in
Darwin)
    case "$(uname -m)" in
    arm64|aarch64) WGPU_PLATFORM="macos-aarch64" ;;
    *) WGPU_PLATFORM="macos-x86_64" ;;
    esac
    ;;
Linux)
    case "$(uname -m)" in
    arm64|aarch64) WGPU_PLATFORM="linux-aarch64" ;;
    *) WGPU_PLATFORM="linux-x86_64" ;;
    esac
    ;;
MINGW*|MSYS*|CYGWIN*) WGPU_PLATFORM="windows-x86_64-msvc" ;;
*)
    echo "Unsupported OS for automatic wgpu-native download." >&2
    exit 1
    ;;
esac

WGPU_URL="https://github.com/gfx-rs/wgpu-native/releases/download/${WGPU_NATIVE_VERSION}/wgpu-${WGPU_PLATFORM}-release.zip"
GLFW_URL="https://github.com/glfw/glfw/releases/download/${GLFW_VERSION}/glfw-${GLFW_VERSION}.zip"
IMGUI_URL="https://github.com/ocornut/imgui/archive/refs/tags/${IMGUI_VERSION}.zip"
IMPLOT_URL="https://github.com/epezent/implot/archive/refs/tags/${IMPLOT_VERSION}.zip"

have_wgpu() {
    [[ -f "$WGPU_DIR/include/webgpu/webgpu.h" ]]
}

have_glfw() {
    [[ -f "$GLFW_DIR/CMakeLists.txt" ]]
}

have_imgui() {
    [[ -f "$IMGUI_DIR/imgui.cpp" && -f "$IMGUI_DIR/backends/imgui_impl_glfw.cpp" \
        && -f "$IMGUI_DIR/backends/imgui_impl_wgpu.cpp" ]]
}

have_implot() {
    [[ -f "$IMPLOT_DIR/implot.cpp" ]]
}

import_from_build_cache() {
    local build_deps="$ROOT/build/_deps"
    if have_wgpu && have_glfw; then
        return 0
    fi
    if [[ ! -d "$build_deps" ]]; then
        return 1
    fi
    if ! have_wgpu && [[ -d "$build_deps/wgpu_native-src/include/webgpu" ]]; then
        echo "Importing wgpu-native from $build_deps/wgpu_native-src"
        mkdir -p "$THIRD_PARTY"
        rm -rf "$WGPU_DIR"
        cp -R "$build_deps/wgpu_native-src" "$WGPU_DIR"
    fi
    if ! have_glfw && [[ -f "$build_deps/glfw-src/CMakeLists.txt" ]]; then
        echo "Importing GLFW from $build_deps/glfw-src"
        mkdir -p "$THIRD_PARTY"
        rm -rf "$GLFW_DIR"
        cp -R "$build_deps/glfw-src" "$GLFW_DIR"
    fi
}

download_and_extract() {
    local url="$1"
    local dest="$2"
    local tmp
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' RETURN

    echo "Downloading $url"
    curl -fsSL "$url" -o "$tmp/archive.zip"
    rm -rf "$dest"
    mkdir -p "$dest"
    unzip -q "$tmp/archive.zip" -d "$tmp/extract"

    local inner
    inner="$(find "$tmp/extract" -mindepth 1 -maxdepth 1 -type d | head -1)"
    if [[ -z "$inner" ]]; then
        echo "Archive did not contain a top-level directory: $url" >&2
        exit 1
    fi
    cp -R "$inner/." "$dest/"
}

mkdir -p "$THIRD_PARTY"
import_from_build_cache

if ! have_wgpu; then
    download_and_extract "$WGPU_URL" "$WGPU_DIR"
fi

if ! have_glfw; then
    download_and_extract "$GLFW_URL" "$GLFW_DIR"
fi

if ! have_imgui; then
    download_and_extract "$IMGUI_URL" "$IMGUI_DIR"
    # imgui_impl_wgpu.cpp (as of $IMGUI_VERSION) only aliases the removed
    # WGPUProgrammableStageDescriptor type / adds WGPUVertexAttribute's new
    # leading nextInChain field for the Dawn backend -- but wgpu-native
    # (pinned above) has already adopted that same unified webgpu-headers
    # struct shape for its plain "WGPU" backend too, so building against it
    # with IMGUI_IMPL_WEBGPU_BACKEND_WGPU fails without this patch. Re-check
    # this against imgui's release notes on the next IMGUI_VERSION bump --
    # if upstream has caught up, this patch (and this whole block) can go.
    echo "Patching imgui_impl_wgpu.cpp for wgpu-native ${WGPU_NATIVE_VERSION}'s struct layout"
    patch -p1 -d "$IMGUI_DIR" < "$THIRD_PARTY/patches/imgui_impl_wgpu.patch"
fi

if ! have_implot; then
    download_and_extract "$IMPLOT_URL" "$IMPLOT_DIR"
fi

echo "Dependencies ready:"
echo "  wgpu-native -> $WGPU_DIR"
echo "  GLFW        -> $GLFW_DIR"
echo "  Dear ImGui  -> $IMGUI_DIR"
echo "  ImPlot      -> $IMPLOT_DIR"
