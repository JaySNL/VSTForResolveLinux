#!/bin/sh
# Builds and runs tools/bounded-save-test.cpp: the bounded project-save path, tested against the
# real host main thread with fake plugins. Needs only g++. Exit code 0 means every check passed.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

g++ -std=c++17 -O2 -Wall -Wextra -pthread -I "$root/src" \
    "$root/tools/bounded-save-test.cpp" "$root/src/bounded_save.cpp" "$root/src/host_thread.cpp" \
    -o "$work/bounded-save-test"
"$work/bounded-save-test"
