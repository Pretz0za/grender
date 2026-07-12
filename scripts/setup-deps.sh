#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
THIRD_PARTY="$ROOT/third-party"
WGPU_DIR="$THIRD_PARTY/wgpu-native"
GLFW_DIR="$THIRD_PARTY/glfw"

WGPU_NATIVE_VERSION="v29.0.1.1"
GLFW_VERSION="3.4"

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

have_wgpu() {
    [[ -f "$WGPU_DIR/include/webgpu/webgpu.h" ]]
}

have_glfw() {
    [[ -f "$GLFW_DIR/CMakeLists.txt" ]]
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

echo "Dependencies ready:"
echo "  wgpu-native -> $WGPU_DIR"
echo "  GLFW        -> $GLFW_DIR"
