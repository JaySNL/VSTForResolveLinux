#!/bin/sh
# Build the Fairlight FX bridge.
set -e
root=$(cd "$(dirname "$0")" && pwd)
out="$root/build"
mkdir -p "$out"
g++ -std=c++17 -shared -fPIC -O2 -Wall -Wextra \
    -I "$root/third_party/clap/include" \
    -I /usr/include/carla/includes \
    -o "$out/libfxbridge.so" \
    "$root/src/proxy.cpp" "$root/src/clap_host.cpp" "$root/src/carla_host.cpp" "$root/src/vst2_host.cpp" "$root/src/plugin_window.cpp" -ldl -lX11
# Install as part of the build.
#
# A build that is not installed is a build that is not being tested. Keeping these separate cost a
# crash report against a library that was two commits old, and a diagnosis of a bug that had
# already been fixed. Resolve loads this file at startup, so a restart is still required - but at
# least the file on disk is always the file that was just compiled.
install_dir="$HOME/.local/share/BMDAudioPlugins"
mkdir -p "$install_dir"
cp "$out/libfxbridge.so" "$install_dir/libfxbridge.so"

echo "built and installed $install_dir/libfxbridge.so"
