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

What it does NOT check: the member offsets (`this+0x150`, `this+0x218` and the rest). Those live
in instruction encodings, not in the symbol table, and finding them needs a disassembler. They are
listed at the end so nobody reads a clean run as "everything is verified".
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

# Read out of `src/proxy.cpp`, and NOT checked here. Each is a byte offset into the object rather
# than into the vtable, so it is encoded inside an instruction and only a disassembler finds it.
MEMBERS_NOT_CHECKED = [
    (0x150, "input channel count", "BMDAudioPluginImpl::UpdateChannelCount writes it"),
    (0x158, "output channel count", "BMDAudioPluginImpl::UpdateChannelCount writes it"),
    (0x169, "bypass flag", "read every block"),
    (0x99, "dirty flag", "SetDirty writes it"),
    (0x40, "AudioPlugin parent", "the base subobject"),
    (0x550, "name record", "the label in the effect list"),
    (0x360, "resource tree", "the panel"),
    (0x3a8, "panel width", "the panel"),
    (0x3ac, "panel height", "the panel"),
]

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

    print("NOT CHECKED by this script - member offsets, which live inside instructions and need a")
    print("disassembler. A clean run above does not clear these:")
    for offset, label, why in MEMBERS_NOT_CHECKED:
        print(f"    this+0x{offset:03x}  {label:24} ({why})")

    if "--dump" in flags:
        print()
        print("every slot:")
        names = [slots[o] or "" for o in sorted(slots)]
        human = demangle(names)
        for (offset, _), text in zip(sorted(slots.items()), human):
            print(f"  +0x{offset:03x}  {text}")

    return 1 if (moved or missing or size != EXPECTED_BYTES) else 0


if __name__ == "__main__":
    sys.exit(main())
