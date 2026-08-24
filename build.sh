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
echo "built $out/libfxbridge.so"
