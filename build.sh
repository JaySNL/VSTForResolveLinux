#!/bin/sh
# Build the Fairlight FX bridge.
set -e
root=$(cd "$(dirname "$0")" && pwd)
out="$root/build"
mkdir -p "$out"
g++ -std=c++17 -shared -fPIC -O2 -Wall -Wextra \
    -I "$root/third_party/clap/include" \
    -o "$out/libfxbridge.so" \
    "$root/src/proxy.cpp" "$root/src/clap_host.cpp" -ldl -lX11
echo "built $out/libfxbridge.so"
