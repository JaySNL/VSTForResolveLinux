// Where a Fairlight effect's category actually comes from.
//
// Measured on 2026-08-26, after three other explanations were tried and disproved:
//
//   - FairlightFXConfiguration.xml <CategoryMask> - written for our keys, kept across a restart,
//     and ignored. Resolve records the mask there, it does not read it to decide a category.
//   - FXConfiguration.xml <Category> - written for our keys, parsed at startup, and then *erased*.
//     Resolve dropped both entries and rewrote the file down to an empty <Effects version="1.4"/>,
//     because it cannot validate a VST it has no host for.
//   - Resolve's own VST host - absent. There is no VSTPluginMain and no GetPluginFactory anywhere
//     under /opt/resolve, against a control of two in this library. VstScanner exists as a binary
//     but its config path resolves empty, so nothing drives it.
//
// What decides it is a lookup table compiled into libFairlightPage.so as the Qt resource
// ":/FLSystem/Plugin Metadata" - three zlib-compressed XML documents:
//
//   <Effects type="bmd">     35 entries   <Effect id="De-Esser:1112360051" category="nr"/>
//   <Effects type="vst">  1,225 entries   <Effect id="ERA 6 De-Breath:1682076214" category="nr"/>
//   <Effects type="au">   1,312 entries
//
// An effect whose "<name>:<id>" is in no table gets no category. That is the whole mechanism, and
// it is why soothe2, smartEQ4, every CrumplePop, Dragonfly, Airwindows and all of Waves sit under
// Uncategorized: none of them is on Blackmagic's list. It was never a fault in this bridge.
//
// Why the VST table and not the built-in one:
//
// Our entries are clones of a built-in effect, so the "bmd" table is where they belong. It has 590
// compressed bytes and our 56 entries need 754 - even after minifying the table's own indentation,
// two test entries need 609. It cannot grow: the next resource starts right after it, and moving
// the blob means rewriting the resource tree's data offset, which is pointer surgery inside a live
// process. The "vst" table sitting beside it has 16,896 bytes and 16,844 is enough for all 56,
// so the same edit fits there with no relocation and no tree changes.
//
// That leaves one thing unproven, and it is deliberately left to the run rather than argued: does
// Resolve consult the VST table for entries that came from the built-in factory? If it does, this
// costs one in-place write. If it does not, the log line below says so plainly and the fallback is
// the tree-offset patch. Nothing here is destructive either way - the VST table is dead weight on
// a Linux build with no VST host, so overwriting it cannot cost a category that anything still uses.

#include "fx_categories.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "plugin_scan.h"

namespace {

void (*g_logger)(const char*) = nullptr;

void Log(const char* format, ...) __attribute__((format(printf, 1, 2)));

void Log(const char* format, ...)
{
    if (g_logger == nullptr) {
        return;
    }
    char line[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    g_logger(line);
}

// Which category code each plugin gets.
//
// The codes are Resolve's own, read out of the tables it ships: nr, eq, dyn, dist, rvb, ambi,
// guitar, bass, tool, chan, meter, inst, dly, mod, pitch, img, multi, surr, mst, other. Matching is
// on the menu name, longest-intent-first, and anything unrecognised becomes "other" rather than
// being left out - an entry with a wrong-but-plausible category is still easier to find than one in
// a list of fifty-six Uncategorized.
const char* CategoryFor(const std::string& name)
{
    struct Rule {
        const char* fragment;
        const char* category;
    };
    // Order matters: the first fragment that appears in the name wins.
    static const Rule rules[] = {
        // Cleaners and de-noisers.
        {"ERA6_", "nr"},        {"ERA 6", "nr"},         {"CrumplePop", "nr"},
        {"Accentize", "nr"},    {"soothe", "nr"},        {"SoundApp", "nr"},
        {"Clarity Vx", "nr"},   {"DeBreath", "nr"},      {"DeEsser", "nr"},
        {"Sibilance", "nr"},    {"NS1", "nr"},
        // Equalisers.
        {"smartEQ", "eq"},      {"Smooth Operator", "eq"}, {"LinEQ", "eq"},
        {"F6", "eq"},           {"Silk Vocal", "eq"},
        // Level and dynamics.
        {"smartcomp", "dyn"},   {"smartComp", "dyn"},    {"LinMB", "dyn"},
        {"DPR-402", "dyn"},     {"MaxxVolume", "dyn"},   {"PlaylistRider", "dyn"},
        {"Vocal Rider", "dyn"}, {"PSE", "dyn"},
        // Voice channel strips.
        {"RVox", "chan"},       {"CLA Vocals", "chan"},  {"JJP-Vocals", "chan"},
        {"Butch Vig Vocals", "chan"}, {"GW MixCentric", "chan"},
        // Pitch and formant.
        {"Vocal Bender", "pitch"}, {"OVox", "pitch"},
        // The rest.
        {"PAZ", "meter"},       {"MaxxBass", "bass"},    {"PRS ", "guitar"},
        {"Center", "img"},
        {"Dragonfly", "rvb"},   {"Reverb", "rvb"},
        {"WaveShell", "multi"}, {"StudioRack", "multi"}, {"Carla", "tool"},
        {"pp-master", "mst"},   {"pp-track", "chan"},
        {"VoiceChanger", "other"}, {"Airwindows", "other"},
    };
    for (const Rule& rule : rules) {
        if (name.find(rule.fragment) != std::string::npos) {
            return rule.category;
        }
    }
    return "other";
}

}  // namespace

void FxCategoriesSetLogger(void (*logger)(const char*))
{
    g_logger = logger;
}

void FxCategoriesApply()
{
    // Write the category list where the preload shim can find it.
    //
    // This used to patch the mapped library directly. That is abandoned, and the reason is worth
    // keeping: the only table Resolve consults for our entries is the "bmd" one, which has 594
    // compressed bytes against the ~950 our entries need, and it cannot grow because the next
    // resource begins 598 bytes later. Relocating it needs the resource tree, and the tree's data
    // offsets are not present as static data in the file - a seven-way offset signature search over
    // 8.7 million candidate words found no compact run of nodes.
    //
    // So the patch moved to where the tree is handed over for free: an LD_PRELOAD shim on
    // qRegisterResourceData, which libFairlightPage imports from Qt5Core. The shim runs before this
    // library is loaded and cannot call ScannedPlugins(), so the two halves meet through this file.
    //
    // It is written every scan and read at the next start, which means a plugin added today is
    // categorised from the restart after next. That is a real limitation and not a bug to chase:
    // the alternative is teaching the shim to scan, and two scanners disagreeing is worse than one
    // restart of lag.
    const char* const home = std::getenv("HOME");
    if (home == nullptr) {
        Log("categories: no HOME - the category list was not written");
        return;
    }
    const std::string path = std::string(home) + "/.local/share/BMDAudioPlugins/fxbridge-categories.txt";

    std::FILE* const file = std::fopen(path.c_str(), "we");
    if (file == nullptr) {
        Log("categories: cannot write %s", path.c_str());
        return;
    }
    std::fprintf(file, "# effect key<TAB>category code. Written by the bridge, read by the preload\n");
    std::fprintf(file, "# shim. Codes are Resolve's own: nr eq dyn dist rvb dly mod pitch meter\n");
    std::fprintf(file, "# multi chan mst tool inst bass guitar img surr ambi other.\n");
    int written = 0;
    for (const ScannedPlugin& plugin : ScannedPlugins()) {
        std::fprintf(file, "%s\t%s\n", plugin.key.c_str(), CategoryFor(plugin.name));
        ++written;
    }
    std::fclose(file);
    Log("categories: wrote %d entries to %s", written, path.c_str());
}
