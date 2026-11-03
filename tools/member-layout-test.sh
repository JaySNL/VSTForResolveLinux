#!/bin/sh
# Runs src/member_layout.cpp - the code the bridge ships - against a libBMDAudioPlugins.so file,
# without starting Resolve, and compares the member offsets it reads with the ones given.
#
#   tools/member-layout-test.sh LIBRARY IN OUT BYPASS DIRTY PARENT NAME TREE WIDTH HEIGHT
#
# Each function is cut out of the file by its dynamic symbol's address and size: the same range
# the bridge reads in memory through dladdr1. Verified by disassembly, then by this script:
#
#   21.1.0.0014  0x150 0x158 0x169 0x99 0x40 0x550 0x360 0x3a8 0x3ac
#   21.1.2.0010  0x158 0x160 0x171 0xa1 0x40 0x558 0x368 0x3b0 0x3b4
#
# A new Resolve can be checked straight from its installer, nothing installed:
#   7z x -oOUT DaVinci_Resolve_Studio_X_Linux.run libs/libBMDAudioPlugins.so
set -e
[ $# -eq 10 ] || { sed -n '2,6p' "$0" >&2; exit 2; }
lib=$1
shift
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

python3 -I - "$lib" "$work" <<'EOF'
import subprocess
import sys

lib, work = sys.argv[1], sys.argv[2]
want = [
    "_ZN18BMDAudioPluginImpl18UpdateChannelCountEii",
    "_ZN18BMDAudioPluginImpl6BypassEb",
    "_ZN11AudioPlugin8SetDirtyEb",
    "_ZNK14BMDAudioPluginI14BMDStereoDelayE13GetEffectNameEv",
    "_ZN14BMDStereoDelay21GenerateUserInterfaceEv",
]
run = lambda *cmd: subprocess.run(cmd, capture_output=True, text=True, check=True).stdout

# objdump -h: Idx Name Size VMA LMA File-off Algn
sections = []
for line in run("objdump", "-h", lib).splitlines():
    f = line.split()
    if len(f) >= 7 and f[0].isdigit():
        sections.append((int(f[3], 16), int(f[2], 16), int(f[5], 16)))

symbols = {}
for line in run("nm", "-D", "-S", "--defined-only", lib).splitlines():
    f = line.split()
    if len(f) == 4:
        symbols[f[3].split("@")[0]] = (int(f[0], 16), int(f[1], 16))

with open(lib, "rb") as handle:
    for name in want:
        data = b""
        if name in symbols:
            address, size = symbols[name]
            for vma, length, offset in sections:
                if vma <= address < vma + length:
                    handle.seek(offset + address - vma)
                    data = handle.read(size)
                    break
        if not data:
            print(f"{name} not found in {lib}")
        with open(f"{work}/{name}.bin", "wb") as out:
            out.write(data)
EOF

g++ -std=c++17 -O2 -Wall -Wextra -o "$work/member_layout_test" \
    "$root/test/member_layout_test.cpp" "$root/src/member_layout.cpp" -ldl
"$work/member_layout_test" "$work" "$@"
