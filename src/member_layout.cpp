#include "member_layout.h"

#include <dlfcn.h>
#include <link.h>

#include <cstdint>
#include <cstring>

namespace {

// One instruction form: every byte up to and including ModRM, then a displacement.
//
// The registers are part of the form on purpose. A Resolve that only renamed a register is then
// refused as well, which costs a release. Accepting a match that is not the member costs a
// crashed session, or a project saved with garbage in it.
struct Form {
    const char* text;  // as objdump prints it, for the log
    unsigned char lead[5];
    size_t lead_bytes;
    size_t disp_bytes;  // 1 or 4
};

const Form kStoreRaxToRbx = {"mov %rax,disp32(%rbx)", {0x48, 0x89, 0x83}, 3, 4};
const Form kStoreRsiToRbx = {"mov %rsi,disp32(%rbx)", {0x48, 0x89, 0xb3}, 3, 4};
const Form kStoreSilToRdi = {"mov %sil,disp32(%rdi)", {0x40, 0x88, 0xb7}, 3, 4};
const Form kLoadRdiFromRdi = {"mov disp8(%rdi),%rdi", {0x48, 0x8b, 0x7f}, 3, 1};
const Form kLoadRaxFromRsi = {"mov disp32(%rsi),%rax", {0x48, 0x8b, 0x86}, 3, 4};
const Form kLeaR14FromR13 = {"lea disp32(%r13),%r14", {0x4d, 0x8d, 0xb5}, 3, 4};
const Form kStoreXmm0ToR13 = {"movss %xmm0,disp32(%r13)", {0xf3, 0x41, 0x0f, 0x11, 0x85}, 5, 4};

// Nothing in the stock effect is this large. A displacement outside (0, kLargestMember) is not a
// member, whatever bytes it was read from.
constexpr long kLargestMember = 0x10000;

// Nor is any of the five functions: the largest, GenerateUserInterface, is 10456 bytes in both
// 21.1.0 and 21.1.2. A symbol size past this is not one of them, and is not scanned.
constexpr size_t kLargestFunction = 0x10000;

// Every displacement `form` carries in `code`, in order, storing at most `room` of them; returns
// how many there are. Every byte offset is tried, not only instruction starts, so a false match
// inside another instruction's operands shows up as one match too many and is refused.
size_t FindAll(const CodeRange& code, const Form& form, long* found, size_t room) {
    size_t count = 0;
    const size_t whole = form.lead_bytes + form.disp_bytes;
    for (size_t at = 0; code.begin != nullptr && at + whole <= code.size; ++at) {
        if (std::memcmp(code.begin + at, form.lead, form.lead_bytes) != 0) {
            continue;
        }
        long disp = 0;
        if (form.disp_bytes == 1) {
            disp = static_cast<int8_t>(code.begin[at + form.lead_bytes]);
        } else {
            int32_t wide = 0;
            std::memcpy(&wide, code.begin + at + form.lead_bytes, sizeof(wide));
            disp = wide;
        }
        if (count < room) {
            found[count] = disp;
        }
        ++count;
    }
    return count;
}

// Exactly `wanted` matches of `form` in `code`, each a plausible member offset, or a log line
// naming the member and false.
bool Expect(const CodeRange& code, const Form& form, size_t wanted, long* found,
            const char* member, const char* function, void (*log)(const char*, ...)) {
    if (code.begin == nullptr) {
        log("layout: %s - %s is not exported", member, function);
        return false;
    }
    const size_t count = FindAll(code, form, found, wanted);
    if (count != wanted) {
        log("layout: %s - `%s` appears %zu times in %s, expected %zu", member, form.text, count,
            function, wanted);
        return false;
    }
    for (size_t i = 0; i < wanted; ++i) {
        if (found[i] <= 0 || found[i] >= kLargestMember) {
            log("layout: %s - %s reads %+ld, which is not a member", member, function, found[i]);
            return false;
        }
    }
    return true;
}

// A function's code, sized by its own entry in the dynamic symbol table.
//
// dladdr1 answers with the nearest symbol, which is not necessarily the one asked for. So the entry
// must start exactly where dlsym said and be a plain function; an IFUNC resolver or a symbol that
// merely contains the address would size the scan from someone else's bytes.
CodeRange Lookup(void* handle, const char* symbol) {
    CodeRange range;
    void* const address = dlsym(handle, symbol);
    if (address == nullptr) {
        return range;
    }
    Dl_info info;
    void* found = nullptr;
    if (dladdr1(address, &info, &found, RTLD_DL_SYMENT) == 0 || found == nullptr ||
        info.dli_saddr != address) {
        return range;
    }
    const auto* const entry = static_cast<const ElfW(Sym)*>(found);
    if (ELF64_ST_TYPE(entry->st_info) != STT_FUNC || entry->st_size == 0 ||
        entry->st_size > kLargestFunction) {
        return range;
    }
    range.begin = static_cast<const unsigned char*>(address);
    range.size = entry->st_size;
    return range;
}

}  // namespace

bool DecodeMemberLayout(const MemberCode& code, MemberLayout* out, void (*log)(const char*, ...)) {
    long input = 0;
    long output = 0;
    long bypass = 0;
    long dirty = 0;
    long parent = 0;
    long name = 0;
    long tree = 0;
    long panel[2] = {0, 0};

    const char* const channels_fn = "BMDAudioPluginImpl::UpdateChannelCount";
    const char* const dirty_fn = "AudioPlugin::SetDirty";
    const char* const panel_fn = "BMDStereoDelay::GenerateUserInterface";
    if (!Expect(code.update_channel_count, kStoreRaxToRbx, 1, &input, "input channels",
                channels_fn, log) ||
        !Expect(code.update_channel_count, kStoreRsiToRbx, 1, &output, "output channels",
                channels_fn, log) ||
        !Expect(code.bypass, kStoreSilToRdi, 1, &bypass, "bypass flag",
                "BMDAudioPluginImpl::Bypass", log) ||
        !Expect(code.set_dirty, kStoreSilToRdi, 1, &dirty, "dirty flag", dirty_fn, log) ||
        !Expect(code.set_dirty, kLoadRdiFromRdi, 1, &parent, "parent", dirty_fn, log) ||
        !Expect(code.get_effect_name, kLoadRaxFromRsi, 1, &name, "name record",
                "BMDAudioPlugin<BMDStereoDelay>::GetEffectName", log) ||
        !Expect(code.generate_user_interface, kLeaR14FromR13, 1, &tree, "resource tree",
                panel_fn, log) ||
        !Expect(code.generate_user_interface, kStoreXmm0ToR13, 2, panel, "panel size", panel_fn,
                log)) {
        return false;
    }

    // The pairs that are one field each in Resolve's own code. If these disagree, the forms
    // matched something else.
    if (output != input + 8) {
        log("layout: output channels at +0x%lx do not follow input channels at +0x%lx", output,
            input);
        return false;
    }
    if (panel[1] != panel[0] + 4) {
        log("layout: panel height at +0x%lx does not follow width at +0x%lx", panel[1], panel[0]);
        return false;
    }

    // The members with no partner to check against still have a shape: the compiler puts a
    // pointer or a vector on 8 bytes and a float on 4. A match that breaks that is not the member.
    // The two flags are single bytes, so nothing can be said about theirs.
    const struct {
        const char* member;
        long at;
        long align;
    } aligned[] = {{"input channels", input, 8}, {"parent", parent, 8},
                   {"name record", name, 8},     {"resource tree", tree, 8},
                   {"panel width", panel[0], 4}};
    for (const auto& check : aligned) {
        if (check.at % check.align != 0) {
            log("layout: %s at +0x%lx is not %ld-byte aligned", check.member, check.at,
                check.align);
            return false;
        }
    }

    out->input_channels = static_cast<size_t>(input);
    out->output_channels = static_cast<size_t>(output);
    out->bypass_flag = static_cast<size_t>(bypass);
    out->dirty_flag = static_cast<size_t>(dirty);
    out->parent = static_cast<size_t>(parent);
    out->name_record = static_cast<size_t>(name);
    out->resource_tree = static_cast<size_t>(tree);
    out->panel_width = static_cast<size_t>(panel[0]);
    out->panel_height = static_cast<size_t>(panel[1]);
    log("layout: channels +0x%zx/+0x%zx, bypass +0x%zx, dirty +0x%zx, parent +0x%zx, "
        "name +0x%zx, panel tree +0x%zx size +0x%zx/+0x%zx",
        out->input_channels, out->output_channels, out->bypass_flag, out->dirty_flag,
        out->parent, out->name_record, out->resource_tree, out->panel_width, out->panel_height);
    return true;
}

bool ReadMemberLayout(void* handle, MemberLayout* out, void (*log)(const char*, ...)) {
    MemberCode code;
    code.update_channel_count = Lookup(handle, "_ZN18BMDAudioPluginImpl18UpdateChannelCountEii");
    code.bypass = Lookup(handle, "_ZN18BMDAudioPluginImpl6BypassEb");
    code.set_dirty = Lookup(handle, "_ZN11AudioPlugin8SetDirtyEb");
    code.get_effect_name =
        Lookup(handle, "_ZNK14BMDAudioPluginI14BMDStereoDelayE13GetEffectNameEv");
    code.generate_user_interface = Lookup(handle, "_ZN14BMDStereoDelay21GenerateUserInterfaceEv");
    return DecodeMemberLayout(code, out, log);
}
