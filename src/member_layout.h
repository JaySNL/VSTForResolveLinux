// Where the stock effect keeps the members this bridge reads and writes.
//
// Every one of these used to be a constant, copied out of a disassembly. Between 21.1.0 and
// 21.1.2, Resolve added eight bytes to AudioPlugin between +0x50 and +0x68, and every member past
// that point moved with it. The constants still compiled and still matched their comments, and all of them pointed
// eight bytes short. Most of those reads are only wrong when wrong; the panel strip writes
// through one.
//
// So the numbers are read at load, out of the same instructions the constants were copied from,
// and each one must be found exactly once in the function that owns it. A Resolve that moved a
// member still works. A Resolve that changed the shape of one of these functions is refused, and
// the bridge forwards the stock interface untouched rather than guess.
#ifndef FXBRIDGE_MEMBER_LAYOUT_H
#define FXBRIDGE_MEMBER_LAYOUT_H

#include <cstddef>

struct MemberLayout {
    // BMDAudioPluginImpl::UpdateChannelCount - `mov %rax,disp(%rbx)` then `mov %rsi,disp(%rbx)`,
    // both 64-bit. From the primary object: 0x150 and 0x158 in 21.1.0, 0x158 and 0x160 in 21.1.2.
    size_t input_channels = 0;
    size_t output_channels = 0;
    // BMDAudioPluginImpl::Bypass(bool) - `mov %sil,disp(%rdi)`, its only writer. 0x169, then 0x171.
    size_t bypass_flag = 0;
    // AudioPlugin::SetDirty(bool) - `mov %sil,disp(%rdi)` for the flag, then `mov disp8(%rdi),%rdi`
    // for the parent it marks next. From the AudioPlugin sub-object: 0x99, then 0xa1. The parent
    // has stayed at 0x40.
    size_t dirty_flag = 0;
    size_t parent = 0;
    // BMDAudioPlugin<BMDStereoDelay>::GetEffectName - `mov disp(%rsi),%rax`. `this` is in %rsi
    // because the string comes back through a hidden buffer in %rdi. 0x550, then 0x558.
    size_t name_record = 0;
    // BMDStereoDelay::GenerateUserInterface, which keeps `this` in %r13 - `lea disp(%r13),%r14`
    // for the resource tree, then two `movss %xmm0,disp(%r13)` for the default width and height.
    // 0x360, 0x3a8 and 0x3ac, then 0x368, 0x3b0 and 0x3b4.
    size_t resource_tree = 0;
    size_t panel_width = 0;
    size_t panel_height = 0;
};

// One function's machine code.
struct CodeRange {
    const unsigned char* begin = nullptr;
    size_t size = 0;
};

// The functions the layout is read out of. Kept apart from the lookup, so the decoding can be
// run against a library file without loading it - see tools/member-layout-test.sh.
struct MemberCode {
    CodeRange update_channel_count;
    CodeRange bypass;
    CodeRange set_dirty;
    CodeRange get_effect_name;
    CodeRange generate_user_interface;
};

// Fills `out` from `code`. False when any member is not found exactly once, or a cross-check
// disagrees, with one log line naming which; `out` is left as it was then.
bool DecodeMemberLayout(const MemberCode& code, MemberLayout* out, void (*log)(const char*, ...));

// Finds the five functions in the library behind `handle` (libBMDAudioPlugins.so), sizes each
// one from its own dynamic symbol table entry, and decodes them.
bool ReadMemberLayout(void* handle, MemberLayout* out, void (*log)(const char*, ...));

#endif
