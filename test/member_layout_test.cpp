// Runs DecodeMemberLayout - the code the bridge ships - over function bytes that
// tools/member-layout-test.sh cut out of a libBMDAudioPlugins.so file, and compares what it reads
// with the offsets it is given. No Resolve needed, so a new build can be checked from its installer.
#include "../src/member_layout.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

void Print(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::printf("\n");
}

std::vector<unsigned char> Slurp(const std::string& path) {
    std::vector<unsigned char> bytes;
    if (FILE* const file = std::fopen(path.c_str(), "rb")) {
        unsigned char buffer[4096];
        size_t got = 0;
        while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
            bytes.insert(bytes.end(), buffer, buffer + got);
        }
        std::fclose(file);
    }
    return bytes;
}

// An empty file reads as "not exported", the same as a failed dlsym in the bridge.
CodeRange Range(const std::vector<unsigned char>& bytes) {
    CodeRange range;
    if (!bytes.empty()) {
        range.begin = bytes.data();
        range.size = bytes.size();
    }
    return range;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 11) {
        std::fprintf(stderr, "usage: %s DIR IN OUT BYPASS DIRTY PARENT NAME TREE WIDTH HEIGHT\n",
                     argv[0]);
        return 2;
    }
    const std::string dir = argv[1];
    const auto channels = Slurp(dir + "/_ZN18BMDAudioPluginImpl18UpdateChannelCountEii.bin");
    const auto bypass = Slurp(dir + "/_ZN18BMDAudioPluginImpl6BypassEb.bin");
    const auto dirty = Slurp(dir + "/_ZN11AudioPlugin8SetDirtyEb.bin");
    const auto name = Slurp(dir + "/_ZNK14BMDAudioPluginI14BMDStereoDelayE13GetEffectNameEv.bin");
    const auto panel = Slurp(dir + "/_ZN14BMDStereoDelay21GenerateUserInterfaceEv.bin");

    MemberCode code;
    code.update_channel_count = Range(channels);
    code.bypass = Range(bypass);
    code.set_dirty = Range(dirty);
    code.get_effect_name = Range(name);
    code.generate_user_interface = Range(panel);

    MemberLayout layout;
    if (!DecodeMemberLayout(code, &layout, Print)) {
        std::printf("FAIL: the layout was refused\n");
        return 1;
    }

    const size_t got[] = {layout.input_channels, layout.output_channels, layout.bypass_flag,
                          layout.dirty_flag,     layout.parent,          layout.name_record,
                          layout.resource_tree,  layout.panel_width,     layout.panel_height};
    const char* const names[] = {"input channels", "output channels", "bypass flag",
                                 "dirty flag",     "parent",          "name record",
                                 "resource tree",  "panel width",     "panel height"};
    int failures = 0;
    for (int i = 0; i < 9; ++i) {
        const size_t want = std::strtoul(argv[2 + i], nullptr, 16);
        if (got[i] != want) {
            std::printf("FAIL: %s read +0x%zx, expected +0x%zx\n", names[i], got[i], want);
            ++failures;
        }
    }
    if (failures != 0) {
        std::printf("%d member(s) wrong\n", failures);
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
