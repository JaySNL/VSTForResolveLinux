#include "member_layout.h"

// Measured on DaVinci Resolve Studio 21.1.2.0010, instruction by instruction, against
// libBMDAudioPlugins.so - the table in CHANGELOG.md under v0.3.1. 21.1.2 only: see the header.
bool ReadMemberLayout(void*, MemberLayout* out, void (*log)(const char*, ...)) {
    out->input_channels = 0x158;
    out->output_channels = 0x160;
    out->bypass_flag = 0x171;
    out->dirty_flag = 0xa1;
    out->parent = 0x40;
    out->name_record = 0x558;
    out->resource_tree = 0x368;
    out->panel_width = 0x3b0;
    out->panel_height = 0x3b4;
    log("layout: FIXED for Resolve 21.1.2 (v0.3.1.1) - channels +0x%zx/+0x%zx, bypass +0x%zx, "
        "dirty +0x%zx, parent +0x%zx, name +0x%zx, panel tree +0x%zx size +0x%zx/+0x%zx",
        out->input_channels, out->output_channels, out->bypass_flag, out->dirty_flag,
        out->parent, out->name_record, out->resource_tree, out->panel_width, out->panel_height);
    return true;
}
