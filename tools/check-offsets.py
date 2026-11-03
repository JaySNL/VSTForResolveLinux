#!/usr/bin/env python3
"""Check the bridge's vtable offsets against the Resolve you actually have installed.

Every constant the bridge patches is a byte offset into one object: the vtable of
`BMDStereoDelay` in `libBMDAudioPlugins.so`. Blackmagic rebuilds that class on every release, so
a new Resolve can move a slot without changing anything a user would notice - and a bridge that
patches a moved slot replaces the wrong method. That is not a crash you can read; it is drag and
drop that stops working, or a knob that renames the effect.

So this reads the offsets out of the library instead of trusting them. It needs nothing but
python3: no binutils, no debugger, no index, no build.

    tools/check-offsets.py                                   # the installed Resolve
    tools/check-offsets.py /path/to/libBMDAudioPlugins.so    # any other copy
    tools/check-offsets.py --dump                            # every slot, not just ours

The member offsets (`this+0x150`, `this+0x218` and the rest) are not constants any more: the
bridge reads them out of the library's instructions when it loads (src/member_layout.cpp and
src/chain_lock_fix.cpp). This script decodes them by the same rules and reports what the bridge
will read, and whether it will refuse to load - 21.1.2 moved every one of them by eight.
"""

import bisect
import os
import struct
import subprocess
import sys

BASELINE = ("generated from DaVinci Resolve Studio 21.0.0.0048; 21.0.4.0005 runs on\n             the same numbers - the tester's machine is on it")
DEFAULT_LIBRARY = "/opt/resolve/libs/libBMDAudioPlugins.so"
VTABLE = "_ZTV14BMDStereoDelay"

# The slots the bridge patches or reads, as measured on DaVinci Resolve Studio 21.0.4.0005.
# offset -> (mangled symbol, what the bridge calls it)
#
# A name here is the MANGLED one on purpose. Demangling needs c++filt, which is one more thing to
# have installed, and a mangled comparison is the stricter test anyway: it carries the argument
# types, so an overload that changed signature is caught instead of passing.
LABELS = {
    0x0c0: "NotifyParameterUpdate",
    0x1c0: "ResetHistory",
    0x250: "InitializeEffectEdit",
    0x258: "HasEditor",
    0x260: "GetEffectEdit",
    0x278: "CloseEffectEdit",
    0x2c8: "GetNumberOfParameters",
    0x2d0: "GetParameterName",
    0x2d8: "GetControlType",
    0x310: "SetParameterValue",
    0x318: "GetParameterValue",
    0x460: "GenerateUserInterface",
    0x478: "GetPluginLatency",
    0x4d0: "Process",
    0x690: "Process (thunk)",
    0x6a0: "ResetHistory (thunk)",
    0x6b8: "SetUserEffectName",
    0x718: "HasEditor (thunk)",
    0x7b0: "InitializeEffectEdit (thunk)",
    0x7b8: "GetEffectEdit (thunk)",
    0x7c0: "UpdateEffectEditTitle",
    0x7d0: "CloseEffectEdit (thunk)",
    0x830: "HideSubWindows",
    0x8f0: "GetNumberOfParameters (thunk)",
    0x8f8: "GetParameterName (thunk)",
    0x900: "GetControlType (thunk)",
    0x910: "SetParameterValue (thunk)",
    0x930: "GetParameterValue (thunk)",
    0x990: "StorePreset (thunk)",
    0x998: "LoadPreset (thunk)",
    0xa68: "SetDirty",
    0xa70: "IsDirty",
}

# Generated from DaVinci Resolve Studio 21.0.4.0005 by --emit-expected. Do not hand-edit:
# a name typed from memory passes the eye and fails the comparison.
EXPECTED = {
    0x0c0: "_ZN18BMDAudioPluginImpl21NotifyParameterUpdateEj",
    0x1c0: "_ZN18BMDAudioPluginImpl12ResetHistoryEb",
    0x250: "_ZN18BMDAudioPluginImpl20InitializeEffectEditEPKcPv",
    0x258: "_ZNK18BMDAudioPluginImpl9HasEditorEv",
    0x260: "_ZNK18BMDAudioPluginImpl13GetEffectEditEv",
    0x278: "_ZN18BMDAudioPluginImpl15CloseEffectEditEv",
    0x2c8: "_ZNK18BMDAudioPluginImpl21GetNumberOfParametersEv",
    0x2d0: "_ZNK18BMDAudioPluginImpl16GetParameterNameEj",
    0x2d8: "_ZNK18BMDAudioPluginImpl14GetControlTypeEi",
    0x310: "_ZN14BMDStereoDelay17SetParameterValueEjf",
    0x318: "_ZNK18BMDAudioPluginImpl17GetParameterValueEj",
    0x460: "_ZN14BMDStereoDelay21GenerateUserInterfaceEv",
    0x478: "_ZNK14BMDAudioPluginI14BMDStereoDelayE16GetPluginLatencyEv",
    0x4d0: "_ZN14BMDStereoDelay7ProcessEPK30AudioPluginTimebaseInformationPPfS4_m",
    0x690: "_ZThn32_N14BMDStereoDelay7ProcessEPK30AudioPluginTimebaseInformationPPfS4_m",
    0x6a0: "_ZThn32_N18BMDAudioPluginImpl12ResetHistoryEb",
    0x6b8: "_ZN11AudioPlugin17SetUserEffectNameEPKw",
    0x718: "_ZThn32_NK18BMDAudioPluginImpl9HasEditorEv",
    0x7b0: "_ZThn32_N18BMDAudioPluginImpl20InitializeEffectEditEPKcPv",
    0x7b8: "_ZThn32_NK18BMDAudioPluginImpl13GetEffectEditEv",
    0x7c0: "_ZThn32_N18BMDAudioPluginImpl21UpdateEffectEditTitleEPKc",
    0x7d0: "_ZThn32_N18BMDAudioPluginImpl15CloseEffectEditEv",
    0x830: "_ZN11AudioPlugin14HideSubWindowsEv",
    0x8f0: "_ZThn32_NK18BMDAudioPluginImpl21GetNumberOfParametersEv",
    0x8f8: "_ZThn32_NK18BMDAudioPluginImpl16GetParameterNameEj",
    0x900: "_ZThn32_NK18BMDAudioPluginImpl14GetControlTypeEi",
    0x910: "_ZThn32_N14BMDStereoDelay17SetParameterValueEjf",
    0x930: "_ZThn32_NK18BMDAudioPluginImpl17GetParameterValueEj",
    0x990: "_ZThn32_N18BMDAudioPluginImpl11StorePresetER17AudioPluginPreset",
    0x998: "_ZThn32_N18BMDAudioPluginImpl10LoadPresetERK17AudioPluginPreset",
    0xa68: "_ZN11AudioPlugin8SetDirtyEb",
    0xa70: "_ZNK11AudioPlugin7IsDirtyEv",
}

# The whole vtable object is copied, so its size is a constant too.
EXPECTED_BYTES = 0xab8

# Member offsets: byte offsets into the OBJECT rather than into the vtable, so they are encoded
# inside instructions and no symbol names them. The bridge reads each one at load out of the one
# function that owns it, as an exact instruction form that must appear exactly as often as listed
# here - src/member_layout.cpp. This table is that file's, decoded the same way, so a run says what
# the bridge will read before Resolve is started. Keep the two in step.
#
# (what it is, owning function, bytes up to and including ModRM, displacement bytes, the values in
# 21.1.0 - one per match the bridge requires)
MEMBER_FORMS = [
    ("input channels", "_ZN18BMDAudioPluginImpl18UpdateChannelCountEii",
     bytes([0x48, 0x89, 0x83]), 4, [0x150]),
    ("output channels", "_ZN18BMDAudioPluginImpl18UpdateChannelCountEii",
     bytes([0x48, 0x89, 0xb3]), 4, [0x158]),
    ("bypass flag", "_ZN18BMDAudioPluginImpl6BypassEb",
     bytes([0x40, 0x88, 0xb7]), 4, [0x169]),
    ("dirty flag", "_ZN11AudioPlugin8SetDirtyEb",
     bytes([0x40, 0x88, 0xb7]), 4, [0x99]),
    ("AudioPlugin parent", "_ZN11AudioPlugin8SetDirtyEb",
     bytes([0x48, 0x8b, 0x7f]), 1, [0x40]),
    ("name record", "_ZNK14BMDAudioPluginI14BMDStereoDelayE13GetEffectNameEv",
     bytes([0x48, 0x8b, 0x86]), 4, [0x550]),
    ("resource tree", "_ZN14BMDStereoDelay21GenerateUserInterfaceEv",
     bytes([0x4d, 0x8d, 0xb5]), 4, [0x360]),
    ("panel width and height", "_ZN14BMDStereoDelay21GenerateUserInterfaceEv",
     bytes([0xf3, 0x41, 0x0f, 0x11, 0x85]), 4, [0x3a8, 0x3ac]),
]

# The lock fix reads its two offsets the same way, from the prologue of each half of the
# inversion, and refuses unless both halves agree - src/chain_lock_fix.cpp.
# (function, `this` register as ModRM r/m, bytes scanned)
LOCK_READERS = [
    ("_ZN18BMDAudioPluginImpl12ResetHistoryEb", 7, 0x60),
    ("_ZN18BMDAudioPluginImpl10PreProcessEPPflm", 3, 0x80),
]
LOCK_IN_21_1_0 = [0x218, 0x250]

# The gate that empties the effect list.
#
# GetBMDPluginInterface() asks the stock interface for its version and refuses to wrap anything
# when the answer is not this number - src/proxy.cpp does that on purpose, because wrapping an
# interface whose shape changed is worse than not wrapping it. The visible result of the refusal is
# an effect list with none of our plugins in it and a quiet line in the log, so it is worth reading
# the number straight out of the library.
# The size of one node in Resolve's plugin map.
#
# The bridge clones an entry into that map, so it has to build a node of exactly the right size.
# The number is not declared anywhere: __emplace_unique_key_args loads it into %edi immediately
# before `operator new`, so it is read from there. src/proxy.cpp says "the interface version gate
# (100) guards this number against a Resolve update" - that assumption is what this check tests,
# because 21.1 still reports 100.
NODE_SYMBOL = ("_ZNSt3__16__treeINS_12__value_typeI7QStringN21BMDAudioPluginFactory16Plugin"
               "DefinitionEEENS_19__map_value_compareIS2_S5_NS_4lessIS2_EELb1EEENS_9allocator"
               "IS5_EEE25__emplace_unique_key_argsIS2_JNS_4pairIKS2_S4_EEEEENSE_INS_15__tree_"
               "iteratorIS5_PNS_11__tree_nodeIS5_PvEElEEbEERKT_DpOT0_")
EXPECTED_NODE_BYTES = 0x68

# Resolve 21.1's gate on external Fairlight plugins.
#
# FLPluginHost::Initialize reads BMDPlugins.Path and loads the library named there. From 21.1 it
# first asks Resolve's core interface a yes/no question keyed on "Debug" and returns without doing
# anything when the answer is no, which empties the effect list with no error anywhere. The gate is
# `call *0x1d0(%rax)` -> `test %bpl,%bpl` -> `je`, and it is recognised here so a user is told which
# Resolve they have and what it needs.
GATE_LIBRARY = "libFairlightPage.so"
GATE_SYMBOL = "_ZN12FLPluginHost10InitializeEv"
GATE_TEST = bytes([0x40, 0x84, 0xed, 0x0f, 0x84])          # test %bpl,%bpl ; je rel32
GATE_QUERY = bytes([0xff, 0x90, 0xd0, 0x01, 0x00, 0x00, 0x89, 0xc5])  # call *0x1d0(%rax) ; mov %eax,%ebp
GATE_SCAN = 0x200

INTERFACE_VERSION_SYMBOL = "_ZNK22BMDPluginInterfaceImpl25GetPluginInterfaceVersionEv"
EXPECTED_INTERFACE_VERSION = 100

CONFIG = os.path.expanduser("~/.local/share/DaVinciResolve/configs/config-fairlight.dat")


R_X86_64_64 = 1
R_X86_64_RELATIVE = 8
STT_FUNC = 2


class Elf:
    """Just enough ELF64 to answer one question. No dependencies on purpose."""

    def __init__(self, path):
        with open(path, "rb") as handle:
            self.data = handle.read()
        if self.data[:4] != b"\x7fELF" or self.data[4] != 2:
            raise ValueError(f"{path} is not a 64-bit ELF file")
        (self.shoff,) = struct.unpack_from("<Q", self.data, 0x28)
        self.shentsize, self.shnum, self.shstrndx = struct.unpack_from("<HHH", self.data, 0x3a)
        self.sections = self._sections()

    def _section(self, index):
        base = self.shoff + index * self.shentsize
        name, kind, flags, addr, offset, size, link, info, align, entsize = struct.unpack_from(
            "<IIQQQQIIQQ", self.data, base)
        return dict(name=name, type=kind, addr=addr, offset=offset, size=size,
                    link=link, entsize=entsize)

    def _sections(self):
        raw = [self._section(i) for i in range(self.shnum)]
        strtab = raw[self.shstrndx]
        for section in raw:
            start = strtab["offset"] + section["name"]
            end = self.data.index(b"\0", start)
            section["label"] = self.data[start:end].decode("utf-8", "replace")
        return raw

    def by_label(self, label):
        for section in self.sections:
            if section["label"] == label:
                return section
        return None

    def symbols(self):
        """(address, size, type, name) for every defined symbol in every symbol table."""
        found = []
        for section in self.sections:
            if section["type"] not in (2, 11):  # SYMTAB, DYNSYM
                continue
            strtab = self.sections[section["link"]]
            count = section["size"] // 24
            for index in range(count):
                base = section["offset"] + index * 24
                name, info, other, shndx, value, size = struct.unpack_from(
                    "<IBBHQQ", self.data, base)
                if shndx == 0 or value == 0:
                    continue  # undefined, or an absolute we cannot place
                start = strtab["offset"] + name
                end = self.data.index(b"\0", start)
                text = self.data[start:end].decode("utf-8", "replace")
                if text:
                    found.append((value, size, info & 0xf, text))
        return found

    def relocations(self):
        """vtable slot address -> ("addr", target) or ("sym", index, addend)."""
        table = {}
        dynsym = self.by_label(".dynsym")
        for section in self.sections:
            if section["type"] != 4:  # RELA
                continue
            count = section["size"] // 24
            for index in range(count):
                offset, info, addend = struct.unpack_from(
                    "<QQq", self.data, section["offset"] + index * 24)
                kind = info & 0xffffffff
                if kind == R_X86_64_RELATIVE:
                    table[offset] = ("addr", addend)
                elif kind == R_X86_64_64:
                    table[offset] = ("sym", info >> 32, addend, dynsym)
        return table


def demangle(names):
    """Only for display, and only when c++filt happens to exist. Never used to compare."""
    try:
        out = subprocess.run(["c++filt"], input="\n".join(names), capture_output=True,
                             text=True, timeout=20).stdout.splitlines()
        if len(out) == len(names):
            return out
    except (OSError, subprocess.SubprocessError):
        pass
    return names


def resolve_version(skip=False):
    """Best effort. The version is not in a file, so it is scanned out of the main binary.

    It sits about 158 MB in on the machine this was written on, so the whole file is streamed
    rather than a hopeful prefix - a bounded read found nothing and reported "unknown", which is
    worse than slow. `--no-version` skips it. A miss prints "unknown" and never a guess.
    """
    import re as _re
    if skip:
        return None, None
    pattern = _re.compile(rb"\b(2[0-9]\.[0-9]+\.[0-9]+\.[0-9]{4})\b")
    for candidate in ("/opt/resolve/bin/resolve-real", "/opt/resolve/bin/resolve"):
        if not os.path.exists(candidate):
            continue
        try:
            with open(candidate, "rb") as handle:
                tail = b""
                while True:
                    chunk = handle.read(64 * 1024 * 1024)
                    if not chunk:
                        break
                    found = pattern.search(tail + chunk)
                    if found:
                        return found.group(1).decode(), candidate
                    tail = chunk[-32:]
        except OSError:
            continue
    return None, None


def fingerprint(path):
    import hashlib
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest(), os.path.getsize(path)


def read_vtable(path):
    elf = Elf(path)
    symbols = elf.symbols()

    vtable = next((s for s in symbols if s[3] == VTABLE), None)
    if vtable is None:
        raise SystemExit(f"{VTABLE} is not in {path} - the class was renamed or removed, which is "
                         f"a much bigger change than a moved slot.")
    address, size = vtable[0], vtable[1]

    # Functions, sorted, so an address can be placed inside the one that contains it.
    functions = sorted((s for s in symbols if s[2] == STT_FUNC and s[0]), key=lambda s: s[0])
    starts = [f[0] for f in functions]

    def name_at(target):
        index = bisect.bisect_right(starts, target) - 1
        if index < 0:
            return None
        start, length, _, text = functions[index]
        if length and target >= start + length:
            return None
        return text if target == start else f"{text}+0x{target - start:x}"

    relocations = elf.relocations()
    dynsym = elf.by_label(".dynsym")

    def symbol_name(index, section):
        base = section["offset"] + index * 24
        name, _info, _other, _shndx, _value, _size = struct.unpack_from("<IBBHQQ", elf.data, base)
        strtab = elf.sections[section["link"]]
        start = strtab["offset"] + name
        return elf.data[start:elf.data.index(b"\0", start)].decode("utf-8", "replace")

    slots = {}
    for offset in range(0, size, 8):
        entry = relocations.get(address + offset)
        if entry is None:
            slots[offset] = None
        elif entry[0] == "addr":
            slots[offset] = name_at(entry[1])
        else:
            slots[offset] = symbol_name(entry[1], entry[3] or dynsym)
    return size, slots



def find_form(blob, lead, disp_bytes):
    """Every displacement `lead` carries in `blob`, trying every byte offset - FindAll() in
    src/member_layout.cpp. A false match inside another instruction then shows up as one match
    too many, and the bridge refuses."""
    found = []
    for at in range(len(blob) - len(lead) - disp_bytes + 1):
        if blob[at:at + len(lead)] == lead:
            raw = blob[at + len(lead):at + len(lead) + disp_bytes]
            found.append(int.from_bytes(raw, "little", signed=True))
    return found


def first_disp32(blob, opcode, base):
    """The first `opcode disp32(%base)`, any REX - FindLea() and FindMovByte() in
    src/chain_lock_fix.cpp."""
    room = 7 if opcode == 0x8d else 6
    for at in range(len(blob) - room + 1):
        op = at + (1 if 0x40 <= blob[at] <= 0x4f else 0)
        if op + 6 > len(blob):
            continue
        modrm = blob[op + 1]
        if blob[op] == opcode and (modrm & 0xc0) == 0x80 and (modrm & 7) == base:
            return int.from_bytes(blob[op + 2:op + 6], "little", signed=True)
    return None


def check_members(path):
    """(what, offsets read, offsets in 21.1.0, why the bridge refuses or "") per member."""
    elf = Elf(path)
    rows = []
    read = {}
    for label, symbol, lead, disp_bytes, before in MEMBER_FORMS:
        blob = function_bytes(elf, symbol)
        found = find_form(blob, lead, disp_bytes)
        why = ""
        if not blob:
            why = f"{symbol} is not in this library"
        elif len(found) != len(before):
            why = f"the form appears {len(found)} times, the bridge needs {len(before)}"
        elif any(not 0 < value < 0x10000 for value in found):
            why = "a displacement that is not a member"
        read[label] = found
        rows.append((label, found, before, why))

    # The pairs that are one field each in Resolve's code - DecodeMemberLayout's cross-checks.
    def cross(label, ok, why):
        for index, row in enumerate(rows):
            if row[0] == label and not row[3] and not ok:
                rows[index] = row[:3] + (why,)
    channels = read.get("input channels", []), read.get("output channels", [])
    cross("output channels", len(channels[0]) == 1 and len(channels[1]) == 1
          and channels[1][0] == channels[0][0] + 8, "does not follow the input count by 8")
    panel = read.get("panel width and height", [])
    cross("panel width and height", len(panel) == 2 and panel[1] == panel[0] + 4,
          "height does not follow width by 4")
    # Pointers and vectors sit on 8 bytes, a float on 4; the two flags are bytes and unchecked.
    for label, align in (("input channels", 8), ("AudioPlugin parent", 8), ("name record", 8),
                         ("resource tree", 8), ("panel width and height", 4)):
        values = read.get(label, [])
        cross(label, bool(values) and values[0] % align == 0, f"not {align}-byte aligned")

    # The lock fix: the mutex and its flag, named identically by both halves of the inversion.
    halves = []
    for symbol, base, scan in LOCK_READERS:
        blob = function_bytes(elf, symbol)[:scan]
        halves.append((first_disp32(blob, 0x8d, base), first_disp32(blob, 0x8a, base)))
    lock = list(halves[0]) if None not in halves[0] else []
    why = ""
    if not lock:
        why = "ResetHistory does not read a mutex and a flag off `this`"
    elif halves[1] != halves[0]:
        why = "PreProcess names different offsets - the lock fix will not patch"
    rows.append(("the per-plugin lock, and its flag (lock fix)", lock, LOCK_IN_21_1_0, why))
    return rows



def function_bytes(elf, symbol):
    """The instruction bytes of one defined function, or b"" when it is not there."""
    place = next(((sym[0], sym[1]) for sym in elf.symbols() if sym[3] == symbol), None)
    if place is None or not place[1]:
        return b""
    address, length = place
    for section in elf.sections:
        if section["addr"] and section["addr"] <= address < section["addr"] + section["size"]:
            start = section["offset"] + (address - section["addr"])
            return elf.data[start:start + length]
    return b""


def node_size(path):
    """The allocation size passed to operator new: `mov $imm32,%edi` then `call`."""
    blob = function_bytes(Elf(path), NODE_SYMBOL)
    if not blob:
        return None
    for index in range(len(blob) - 6):
        if blob[index] == 0xbf and blob[index + 5] == 0xe8:
            return int.from_bytes(blob[index + 1:index + 5], "little")
    return None


def interface_version(path):
    """The constant that GetPluginInterfaceVersion returns, read without running anything.

    The body is a stack-guard preamble and one `mov $imm32,%eax`, so the constant is the immediate
    of the first such instruction. Returns None when the function is absent or does not have that
    shape, which is a finding in itself and not a zero.
    """
    elf = Elf(path)
    place = next(((sym[0], sym[1]) for sym in elf.symbols()
                  if sym[3] == INTERFACE_VERSION_SYMBOL), None)
    if place is None or not place[1]:
        return None
    address, length = place
    blob = b""
    for section in elf.sections:
        if section["addr"] and section["addr"] <= address < section["addr"] + section["size"]:
            start = section["offset"] + (address - section["addr"])
            blob = elf.data[start:start + length]
            break
    for index in range(len(blob) - 5):
        if blob[index] == 0xb8:  # mov $imm32, %eax
            return int.from_bytes(blob[index + 1:index + 5], "little")
    return None



def bridge_dependencies(bridge):
    """Every library libfxbridge.so needs, and whether this machine has it.

    Resolve dlopens the bridge and falls back to its own library WITHOUT SAYING SO when that
    fails, and a missing dependency is the commonest way for it to fail on a machine the bridge
    was not built on. The result looks exactly like an empty effect list.
    """
    elf = Elf(bridge)
    dynamic = elf.by_label(".dynamic")
    dynstr = elf.by_label(".dynstr")
    if dynamic is None or dynstr is None:
        return None
    needed = []
    for index in range(dynamic["size"] // 16):
        tag, value = struct.unpack_from("<Qq", elf.data, dynamic["offset"] + index * 16)
        if tag == 0:
            break
        if tag == 1:  # DT_NEEDED
            start = dynstr["offset"] + value
            needed.append(elf.data[start:elf.data.index(b"\0", start)].decode("utf-8", "replace"))

    cache = {}
    try:
        listing = subprocess.run(["ldconfig", "-p"], capture_output=True, text=True, timeout=20)
        for line in listing.stdout.splitlines():
            if "=>" in line:
                left, right = line.split("=>", 1)
                cache[left.strip().split()[0]] = right.strip()
    except (OSError, subprocess.SubprocessError):
        cache = {}

    search = ["/lib64", "/usr/lib64", "/lib/x86_64-linux-gnu", "/usr/lib/x86_64-linux-gnu",
              "/usr/lib", "/lib"]
    rows = []
    for name in needed:
        place = cache.get(name)
        if place is None:
            place = next((os.path.join(d, name) for d in search
                          if os.path.exists(os.path.join(d, name))), None)
        rows.append((name, place))
    return rows



def plugin_gate(audio_library):
    """Report 21.1's gate on external plugin libraries: (state, detail)."""
    path = os.path.join(os.path.dirname(audio_library), GATE_LIBRARY)
    if not os.path.exists(path):
        return "unknown", f"{path} is not there, so the gate could not be checked"
    try:
        elf = Elf(path)
        blob = function_bytes(elf, GATE_SYMBOL)[:GATE_SCAN]
    except (OSError, ValueError, struct.error):
        return "unknown", f"{path} could not be read"
    if not blob:
        return "unknown", f"{GATE_SYMBOL} is not in {GATE_LIBRARY}"
    gates = blob.count(GATE_TEST)
    queries = blob.count(GATE_QUERY)
    if gates == 0:
        return "open", "no gate - this Resolve loads an external plugin library on its own"
    if gates == 1 and queries == 1 and blob.find(GATE_QUERY) < blob.find(GATE_TEST):
        return "closed", f"the gate is at {GATE_SYMBOL}+0x{blob.find(GATE_TEST) + 3:x}"
    return "changed", (f"{gates} gate and {queries} query matches - the shape is not the one the "
                       f"preload knows, and it will refuse rather than guess")


def why_the_list_is_empty(path):
    """The things that hide every plugin, in the order they bite. Returns how many are wrong."""
    print("if the effect list is empty, these are the reasons, in order:")
    problems = 0

    version = interface_version(path)
    if version is None:
        problems += 1
        print(f"    interface version   COULD NOT READ - {INTERFACE_VERSION_SYMBOL} is not in this")
        print("                        library or has a shape this cannot read. That alone would")
        print("                        empty the list.")
    elif version != EXPECTED_INTERFACE_VERSION:
        problems += 1
        print(f"    interface version   {version}, and the bridge only wraps "
              f"{EXPECTED_INTERFACE_VERSION}.")
        print("                        THIS EMPTIES THE LIST. The bridge sees the mismatch and")
        print("                        forwards Resolve's own interface untouched, on purpose.")
        print("                        The log line is 'unexpected interface version, forwarding")
        print("                        untouched'.")
    else:
        print(f"    interface version   {version}  ok")

    if not os.path.exists(CONFIG):
        problems += 1
        print(f"    BMDPlugins.Path     {CONFIG} does not exist, so Resolve was never told to load")
        print("                        the bridge. Run build.sh, or add the line by hand.")
    else:
        line = None
        try:
            with open(CONFIG, encoding="utf-8", errors="replace") as handle:
                for text in handle:
                    if "BMDPlugins.Path" in text:
                        line = text.strip()
        except OSError as problem:
            line = f"(could not read it: {problem})"
        if line is None:
            problems += 1
            print("    BMDPlugins.Path     NOT SET in config-fairlight.dat. Resolve loads its own")
            print("                        library and the list holds only its own effects. An")
            print("                        update rewrites this file, so it is the first thing to")
            print("                        lose after a Resolve upgrade.")
        else:
            target = line.split("=", 1)[-1].strip()
            exists = os.path.exists(target)
            problems += 0 if exists else 1
            print(f"    BMDPlugins.Path     {target}")
            print(f"                        {'the file is there' if exists else 'THAT FILE DOES NOT EXIST'}")

    bridge = None
    if os.path.exists(CONFIG):
        try:
            with open(CONFIG, encoding="utf-8", errors="replace") as handle:
                for text in handle:
                    if "BMDPlugins.Path" in text:
                        bridge = text.split("=", 1)[-1].strip()
        except OSError:
            bridge = None
    if bridge and os.path.exists(bridge):
        # Is the file at BMDPlugins.Path actually our bridge? Resolve asks it for
        # GetBMDPluginInterface and falls back silently when the symbol is not there, so a stale
        # copy, a truncated download or the scanner binary put in the wrong place all look like
        # an empty list.
        try:
            exports = {sym[3] for sym in Elf(bridge).symbols()}
        except (OSError, ValueError, struct.error):
            exports = set()
        if "GetBMDPluginInterface" not in exports:
            problems += 1
            print("    the installed file  DOES NOT EXPORT GetBMDPluginInterface, so it is not the")
            print("                        bridge. Resolve asks for that symbol and falls back to")
            print("                        its own library when it is missing.")
        else:
            try:
                blob = open(bridge, "rb").read()
            except OSError:
                blob = b""
            build = ("v0.2.10 or newer" if b"off by default since v0.2.10" in blob
                     else "older than v0.2.10" if b"forwarding a locate" in blob else "unknown")
            watchdog = "yes" if b"SLOW - " in blob else "no (older than v0.2.12)"
            print(f"    the installed file  is the bridge, {build}, slow-block watchdog: {watchdog}")

        try:
            rows = bridge_dependencies(bridge)
        except (OSError, ValueError, struct.error):
            rows = None
        if rows is None:
            print("    bridge libraries    could not be read out of libfxbridge.so")
        else:
            gone = [name for name, place in rows if place is None]
            if gone:
                problems += 1
                print(f"    bridge libraries    MISSING {', '.join(gone)}")
                print("                        THIS EMPTIES THE LIST. Resolve dlopens the bridge and")
                print("                        falls back to its own library without saying so when")
                print("                        that fails.")
            else:
                print(f"    bridge libraries    all {len(rows)} present")

    state, detail = plugin_gate(path)
    if state == "open":
        print(f"    21.1 plugin gate    not present  ({detail})")
    elif state == "closed":
        problems += 1
        print(f"    21.1 plugin gate    CLOSED - {detail}.")
        print("                        THIS EMPTIES THE LIST. Resolve 21.1 refuses to load any")
        print("                        external Fairlight plugin library. Start Resolve through")
        print("                        ~/.local/share/BMDAudioPlugins/resolve-with-fxbridge, which")
        print("                        preloads libfxbridge-gate.so and clears the check in memory.")
    else:
        print(f"    21.1 plugin gate    {state} - {detail}")

    node = node_size(path)
    if node is None:
        problems += 1
        print("    map node size       COULD NOT READ - the map insert is not in this library under")
        print("                        the name this looks for. The bridge builds a node by hand,")
        print("                        so a changed name is a changed map.")
    elif node != EXPECTED_NODE_BYTES:
        problems += 1
        print(f"    map node size       0x{node:x}, and the bridge builds 0x{EXPECTED_NODE_BYTES:x}.")
        print("                        THIS EMPTIES THE LIST. Every entry the bridge inserts has")
        print("                        the wrong shape, so nothing usable reaches the menu.")
    else:
        print(f"    map node size       0x{node:x}  ok")

    cache = os.path.expanduser("~/.local/share/BMDAudioPlugins/fxbridge-scan-cache.tsv")
    if not os.path.exists(cache):
        print("    scan cache          not written yet, which is normal before the first start")
    else:
        try:
            with open(cache, encoding="utf-8", errors="replace") as handle:
                rows = sum(1 for text in handle if text.strip() and not text.startswith("#"))
            print(f"    scan cache          {rows} modules")
        except OSError:
            print("    scan cache          could not be read")
    print()
    return problems


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = {a for a in sys.argv[1:] if a.startswith("--")}
    path = args[0] if args else DEFAULT_LIBRARY

    if not os.path.exists(path):
        raise SystemExit(f"{path} does not exist. Pass the path to libBMDAudioPlugins.so.")

    size, slots = read_vtable(path)

    if "--emit-expected" in flags:
        # Regenerate the table from THIS library. Run it once against a Resolve you trust, paste
        # the result over EXPECTED, and the comparison is against measured names instead of
        # remembered ones.
        print("EXPECTED = {")
        for offset in sorted(LABELS):
            symbol = slots.get(offset)
            print(f'    0x{offset:03x}: "{symbol}",'
                  f'{"" if symbol else "  # NOTHING HERE"}')
        print("}")
        print(f"EXPECTED_BYTES = 0x{size:x}")
        return 0

    version, source = resolve_version("--no-version" in flags)
    digest, length = fingerprint(path)
    print(f"library      {path}")
    print(f"             {length} bytes, sha256 {digest[:16]}")
    print(f"Resolve      {version or 'unknown'}"
          f"{'  (read from ' + source + ')' if version else '  - could not read it from the binary'}")
    print(f"baseline     {BASELINE}")
    print()
    print(f"{VTABLE}   0x{size:x} bytes, {size // 8} slots")
    print(f"the bridge copies 0x{EXPECTED_BYTES:x} bytes", end="")
    print("  <-- SAME" if size == EXPECTED_BYTES else f"  <-- CHANGED, it is 0x{size:x} now")
    print()

    # Where each expected symbol actually sits now, so a moved slot can be reported as a new
    # number rather than as "gone".
    where = {}
    for offset, name in slots.items():
        if name:
            where.setdefault(name, []).append(offset)

    moved, missing, same = [], [], 0
    rows = []
    for offset in sorted(EXPECTED):
        wanted = EXPECTED[offset]
        label = LABELS.get(offset, "?")
        found = slots.get(offset)
        if found == wanted:
            same += 1
            rows.append((offset, label, "ok", ""))
        elif wanted in where:
            places = ", ".join(f"+0x{o:03x}" for o in where[wanted])
            moved.append((offset, where[wanted], label, wanted))
            rows.append((offset, label, "MOVED", f"now at {places}"))
        else:
            missing.append((offset, label, wanted, found))
            rows.append((offset, label, "GONE", f"this slot holds {found or '(empty)'}"))

    print(f"{'offset':8} {'the bridge calls it':32} {'verdict':8} note")
    print("-" * 100)
    for offset, label, verdict, note in rows:
        print(f"+0x{offset:03x}   {label:32} {verdict:8} {note}")

    print()
    print(f"{same} of {len(EXPECTED)} slots are where the bridge expects them.")

    if not moved and not missing and size == EXPECTED_BYTES:
        print("Nothing moved. This Resolve needs no remap.")
        print()
    else:
        if moved:
            print()
            print("These moved. Paste into src/proxy.cpp behind a version check:")
            print()
            for old, places, label, _sym in moved:
                where_now = ", ".join(f"0x{o:03x}" for o in places)
                print(f"    // {label}: +0x{old:03x} on 21.0.4, now {where_now}")
                print(f"    constexpr size_t k...Offset = 0x{places[0]:03x};")
        if missing:
            print()
            print("These are not in the vtable at all. A rename or a removed method, and each one")
            print("needs a decision rather than a new number:")
            for offset, label, wanted, found in missing:
                print(f"    +0x{offset:03x}  {label}  (wanted {wanted})")
        print()

    empty_list_problems = why_the_list_is_empty(path)

    print("member offsets, as the bridge reads them out of the instructions at load:")
    bad = 0
    for label, found, before, why in check_members(path):
        names = ", ".join(f"0x{value:x}" for value in found) or "-"
        if why:
            bad += 1
            print(f"    {label:46} {names:14} REFUSED - {why}")
        elif found != before:
            old = ", ".join(f"0x{value:x}" for value in before)
            print(f"    {label:46} {names:14} ok, moved from {old} (21.1.0)")
        else:
            print(f"    {label:46} {names:14} ok")
    if bad:
        print("    A REFUSED member means the bridge forwards Resolve's stock interface untouched:")
        print("    Resolve starts normally and the effect list has none of your plugins.")

    if "--dump" in flags:
        print()
        print("every slot:")
        names = [slots[o] or "" for o in sorted(slots)]
        human = demangle(names)
        for (offset, _), text in zip(sorted(slots.items()), human):
            print(f"  +0x{offset:03x}  {text}")

    return 1 if (moved or missing or bad or empty_list_problems
                 or size != EXPECTED_BYTES) else 0


if __name__ == "__main__":
    sys.exit(main())
