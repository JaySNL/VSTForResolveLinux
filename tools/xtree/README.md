# xtree — what is actually inside the editor window

    gcc -o xtree xtree.c -lX11
    ./xtree                 # the whole root tree
    ./xtree 0x4600003       # one window and its children

Prints each window's geometry, map state, the event masks any client selected on it, and where
the input focus is.

It exists because "the GUI shows but the knobs do nothing" has several possible causes and they
look identical from the outside. One dump separated them on 2026-08-25:

    0x4600003  736x532  viewable                                   <- our parent
      0x5a00000  736x532  viewable  +KeyPress                      <- yabridge's wrapper
        0x5600005  3840x2789 viewable +ButtonPress +KeyPress        "yabridge plugin"

    input focus: 0x4600003

The plugin's own window had asked for ButtonPress and KeyPress and was viewable, which rules out
a masking or mapping fault. The focus was on the parent and never moved down - and a Windows
plugin under Wine ignores input while its window is inactive. That is a one-line answer that
would otherwise have been three guesses.
