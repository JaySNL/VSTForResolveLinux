#!/usr/bin/env python3
"""Index DaVinci Resolve's audio backend: every exported symbol and every vtable slot.

Why this exists: the offsets a plugin bridge needs - which vtable slot is Process, where a
class keeps its channel counts - were being found one at a time by disassembling around a
crash. Every one of them is already in the binary's relocations. This reads them all.

Ground truth for the method: the 21 offsets the bridge uses on BMDStereoDelay were found by
hand over two evenings. This extractor reproduces all 21 exactly. See `rmap vtable BMDStereoDelay`.

The vtable slots are zero in the file; a PIE shared object carries each target in an
R_X86_64_RELATIVE relocation whose addend is the target address. So: read the relocations,
map addresses back to symbols, and the layout falls out.

Offsets are reported FROM THE VTABLE SYMBOL, which is what the bridge's constants use.
The primary vptr points at symbol+0x10 (past offset-to-top and the typeinfo pointer).
"""
import bisect, os, re, sqlite3, subprocess, sys

LIBRARIES = [
    "/opt/resolve/libs/libBMDAudioPlugins.so",
    "/opt/resolve/libs/libFairlightPage.so",
    "/opt/resolve/libs/libFairlightPanelAPI.so",
    "/opt/resolve/bin/resolve",
]

DB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "resolve-map.sqlite")


def demangle_many(names):
    """One c++filt for the whole batch; per-name would take hours at this scale."""
    if not names:
        return []
    out = subprocess.run(["c++filt"], input="\n".join(names),
                         capture_output=True, text=True).stdout.splitlines()
    # c++filt is line-for-line, but guard against a short read rather than mispairing.
    if len(out) != len(names):
        out = out + names[len(out):]
    return out


def read_symbols(so):
    syms = []
    out = subprocess.run(["nm", "-D", "--defined-only", "--no-sort", "-S", so],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        f = line.split()
        # "addr [size] type name"
        if len(f) == 4:
            addr, size, kind, name = f[0], f[1], f[2], f[3]
        elif len(f) == 3:
            addr, size, kind, name = f[0], "0", f[1], f[2]
        else:
            continue
        try:
            syms.append((int(addr, 16), int(size, 16), kind, name))
        except ValueError:
            continue
    syms.sort()
    return syms


def read_relocations(so):
    """offset -> target, as ("sym", mangled_name) or ("addr", address).

    Two forms matter and reading only one of them is the mistake this comment exists to stop.
    A vtable slot in these binaries is overwhelmingly R_X86_64_64, which names its target
    symbol; R_X86_64_RELATIVE, which carries a bare address in the addend, is the minority.
    Filtering to RELATIVE alone found 135 slots across 2166 vtables - a silent 99% miss that
    looked like a successful run.
    """
    rel = {}
    out = subprocess.run(["readelf", "-r", "-W", so], capture_output=True, text=True).stdout
    for line in out.splitlines():
        f = line.split()
        if len(f) < 4 or not re.fullmatch(r"[0-9a-f]{4,16}", f[0]):
            continue
        try:
            offset = int(f[0], 16)
        except ValueError:
            continue
        kind = f[2]
        if kind == "R_X86_64_RELATIVE":
            try:
                rel[offset] = ("addr", int(f[3], 16))
            except ValueError:
                pass
        elif kind in ("R_X86_64_64", "R_X86_64_GLOB_DAT", "R_X86_64_JUMP_SLOT") and len(f) >= 5:
            rel[offset] = ("sym", f[4])
    return rel


def main():
    if os.path.exists(DB):
        os.remove(DB)
    db = sqlite3.connect(DB)
    db.executescript("""
        CREATE TABLE symbol (
            library TEXT, address INTEGER, size INTEGER, kind TEXT,
            mangled TEXT, name TEXT);
        CREATE TABLE vtable_slot (
            library TEXT, class TEXT, vtable_mangled TEXT,
            offset INTEGER, target_mangled TEXT, target TEXT,
            is_thunk INTEGER);
        CREATE INDEX symbol_name ON symbol(name);
        CREATE INDEX symbol_mangled ON symbol(mangled);
        CREATE INDEX symbol_addr ON symbol(library, address);
        CREATE INDEX vtable_class ON vtable_slot(class);
        CREATE INDEX vtable_target ON vtable_slot(target);
        CREATE INDEX vtable_offset ON vtable_slot(offset);
    """)

    for so in LIBRARIES:
        if not os.path.exists(so):
            print("skipped (absent): %s" % so, file=sys.stderr)
            continue
        short = os.path.basename(so)
        print("%s ..." % short, flush=True)

        syms = read_symbols(so)
        addrs = [a for a, _, _, _ in syms]
        mangled = [n for _, _, _, n in syms]
        names = demangle_many(mangled)
        db.executemany("INSERT INTO symbol VALUES (?,?,?,?,?,?)",
                       [(short, a, s, k, m, d)
                        for (a, s, k, m), d in zip(syms, names)])
        print("  %d symbols" % len(syms), flush=True)

        rel = read_relocations(so)
        print("  %d relocations" % len(rel), flush=True)

        by_mangled = {m: d for m, d in zip(mangled, names)}

        # A relocation target, as (mangled, readable). An address is resolved to the symbol
        # that contains it; a symbol reference is demangled, or left as-is when it is imported
        # from another library and so absent from this one's table.
        def resolve(target):
            kind, value = target
            if kind == "sym":
                readable = by_mangled.get(value)
                if readable is None:
                    readable = demangle_many([value])[0]
                return value, readable
            i = bisect.bisect_right(addrs, value) - 1
            if i < 0:
                return None, None
            base = addrs[i]
            if value - base > 0x100000:      # too far to be inside that symbol
                return None, None
            return mangled[i], (names[i] if base == value
                                else "%s+0x%x" % (names[i], value - base))

        rows = []
        vtables = [(a, m, d) for (a, _, _, m), d in zip(syms, names) if m.startswith("_ZTV")]
        for vaddr, vmangled, vname in vtables:
            cls = vname[len("vtable for "):] if vname.startswith("vtable for ") else vmangled

            # A vtable ends where the next symbol begins, and nothing else bounds it.
            #
            # .data.rel.ro is packed solid with relocations, so a walk that stops after N
            # consecutive unrelocated slots never stops: one vtable runs straight into the
            # next. That produced 3,839,112 slots from 2,166 vtables - about 1,770 slots each -
            # which is the shape of a missing boundary, not of a large class.
            i = bisect.bisect_right(addrs, vaddr)
            limit = addrs[i] if i < len(addrs) else vaddr + 0x4000
            offset = 0
            while vaddr + offset < limit:
                target = rel.get(vaddr + offset)
                if target is not None:
                    tm, tn = resolve(target)
                    if tn is not None:
                        rows.append((short, cls, vmangled, offset, tm, tn,
                                     1 if tn.startswith("non-virtual thunk") or
                                          tn.startswith("virtual thunk") else 0))
                offset += 8
        db.executemany("INSERT INTO vtable_slot VALUES (?,?,?,?,?,?,?)", rows)
        widest = max((r[3] for r in rows), default=0)
        print("  %d vtables, %d slots, widest +0x%x" % (len(vtables), len(rows), widest),
              flush=True)
        db.commit()

    n_sym = db.execute("SELECT COUNT(*) FROM symbol").fetchone()[0]
    n_slot = db.execute("SELECT COUNT(*) FROM vtable_slot").fetchone()[0]
    n_cls = db.execute("SELECT COUNT(DISTINCT class) FROM vtable_slot").fetchone()[0]
    print("\n%s\n  %d symbols, %d classes, %d vtable slots" % (DB, n_sym, n_cls, n_slot))
    db.close()


if __name__ == "__main__":
    main()
