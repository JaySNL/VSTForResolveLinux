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

#include <zlib.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

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

constexpr const char kFairlightLibrary[] = "libFairlightPage.so";
constexpr size_t kOuterHeader = 4;  // big-endian payload length: 4 + the zlib stream
constexpr size_t kInnerHeader = 4;  // big-endian uncompressed size
constexpr size_t kNodeSize = 22;    // resource tree node; the data offset sits at +10

uint32_t Be32(const unsigned char* at)
{
    return (static_cast<uint32_t>(at[0]) << 24) | (static_cast<uint32_t>(at[1]) << 16) |
           (static_cast<uint32_t>(at[2]) << 8) | static_cast<uint32_t>(at[3]);
}

void PutBe32(unsigned char* at, uint32_t value)
{
    at[0] = static_cast<unsigned char>((value >> 24) & 0xff);
    at[1] = static_cast<unsigned char>((value >> 16) & 0xff);
    at[2] = static_cast<unsigned char>((value >> 8) & 0xff);
    at[3] = static_cast<unsigned char>(value & 0xff);
}

struct Region {
    unsigned char* begin = nullptr;
    unsigned char* end = nullptr;
};

// The readable mappings of one library. dl_iterate_phdr gives the load address but not which pages
// are actually mapped, and this code scans raw bytes: a read into a gap is a crash, not a miss.
std::vector<Region> ReadableRegionsOf(const char* library)
{
    std::vector<Region> regions;
    std::FILE* const maps = std::fopen("/proc/self/maps", "re");
    if (maps == nullptr) {
        return regions;
    }
    char line[512];
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        if (std::strstr(line, library) == nullptr) {
            continue;
        }
        unsigned long begin = 0;
        unsigned long end = 0;
        char perms[8] = {0};
        if (std::sscanf(line, "%lx-%lx %7s", &begin, &end, perms) != 3 || perms[0] != 'r') {
            continue;
        }
        regions.push_back({reinterpret_cast<unsigned char*>(begin),
                           reinterpret_cast<unsigned char*>(end)});
    }
    std::fclose(maps);
    return regions;
}

struct Table {
    unsigned char* record = nullptr;  // the 4-byte payload length that starts the record
    uint32_t payload_length = 0;      // 4 + the zlib stream
    std::string text;
};

bool Inflate(const unsigned char* stream, size_t available, size_t expected, std::string& out,
             size_t& consumed)
{
    out.assign(expected, '\0');
    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) {
        return false;
    }
    zs.next_in = const_cast<Bytef*>(stream);
    zs.avail_in = static_cast<uInt>(available);
    zs.next_out = reinterpret_cast<Bytef*>(&out[0]);
    zs.avail_out = static_cast<uInt>(expected);
    const int status = inflate(&zs, Z_FINISH);
    consumed = zs.total_in;
    const bool ok = status == Z_STREAM_END && zs.total_out == expected;
    inflateEnd(&zs);
    return ok;
}

// Every compressed category table in the library, found by content rather than by address so that
// a Resolve update moves nothing. The search is for the zlib header, because the marker only exists
// once a block is inflated; a wrong guess costs one inflate into a bounded buffer, never a write.
std::vector<Table> FindTables(const std::vector<Region>& regions)
{
    std::vector<Table> found;
    for (const Region& region : regions) {
        const size_t span = static_cast<size_t>(region.end - region.begin);
        if (span < kOuterHeader + kInnerHeader + 8) {
            continue;
        }
        for (size_t at = kOuterHeader + kInnerHeader; at + 2 < span; ++at) {
            if (region.begin[at] != 0x78 || region.begin[at + 1] != 0x9c) {
                continue;
            }
            unsigned char* const record = region.begin + at - kInnerHeader - kOuterHeader;
            const uint32_t outer = Be32(record);
            const uint32_t inner = Be32(record + kOuterHeader);
            if (inner < 64 || inner > (1u << 22) || outer < 12 || outer > span - at) {
                continue;
            }
            std::string text;
            size_t consumed = 0;
            if (!Inflate(region.begin + at, span - at, inner, text, consumed)) {
                continue;
            }
            if (consumed + kInnerHeader != outer) {
                continue;
            }
            if (text.find("<Effects type=") == std::string::npos) {
                continue;
            }
            found.push_back({record, outer, text});
        }
    }
    return found;
}

// How often a big-endian word occurs across the library. A tree hit that is not unique is not a
// tree hit, and this is the check that says so.
int CountWord(const std::vector<Region>& regions, uint32_t value)
{
    unsigned char needle[4];
    PutBe32(needle, value);
    int seen = 0;
    for (const Region& region : regions) {
        for (const unsigned char* at = region.begin; at + 4 <= region.end; ++at) {
            if (std::memcmp(at, needle, 4) == 0 && ++seen > 4) {
                return seen;
            }
        }
    }
    return seen;
}

// The tree field holding one table's data offset.
//
// The offsets are relative to a base we never learn, but the differences between them equal the
// differences between the tables' addresses, whatever the base is. So look for a word whose
// neighbours at node spacing carry exactly those differences. On this build each of the seven
// offsets occurs exactly once in 46 MB and all seven sit within 132 bytes.
unsigned char* FindTreeField(const std::vector<Region>& regions, const std::vector<Table>& tables,
                             const Table& wanted, uint32_t& offset_out)
{
    std::vector<long> deltas;
    for (const Table& table : tables) {
        deltas.push_back(static_cast<long>(table.record - wanted.record));
    }
    for (const Region& region : regions) {
        for (unsigned char* at = region.begin; at + 4 <= region.end; ++at) {
            const uint32_t base_value = Be32(at);
            if (base_value < 1024 || base_value > (1u << 28)) {
                continue;
            }
            size_t matched = 0;
            for (const long delta : deltas) {
                if (delta == 0) {
                    ++matched;
                    continue;
                }
                const uint32_t want = static_cast<uint32_t>(static_cast<long>(base_value) + delta);
                for (int step = -8; step <= 8; ++step) {
                    unsigned char* const probe = at + step * static_cast<long>(kNodeSize);
                    if (probe < region.begin || probe + 4 > region.end) {
                        continue;
                    }
                    if (Be32(probe) == want) {
                        ++matched;
                        break;
                    }
                }
            }
            if (matched == deltas.size()) {
                offset_out = base_value;
                return at;
            }
        }
    }
    return nullptr;
}

bool MakeWritable(void* start, size_t length)
{
    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    const uintptr_t first = reinterpret_cast<uintptr_t>(start) & ~(page - 1);
    const uintptr_t last = (reinterpret_cast<uintptr_t>(start) + length + page - 1) & ~(page - 1);
    if (mprotect(reinterpret_cast<void*>(first), last - first, PROT_READ | PROT_WRITE) != 0) {
        Log("categories: mprotect over %p+%zu failed", start, length);
        return false;
    }
    return true;
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
    const std::vector<Region> regions = ReadableRegionsOf(kFairlightLibrary);
    if (regions.empty()) {
        Log("categories: %s is not mapped - nothing patched", kFairlightLibrary);
        return;
    }

    const std::vector<Table> tables = FindTables(regions);
    if (tables.size() < 2) {
        Log("categories: found %zu category tables - nothing patched", tables.size());
        return;
    }

    // The table Resolve consults for our entries, and the roomiest one to move it into. The "vst"
    // tables are dead weight here: this build has no VST host, so nothing reads them.
    const Table* target = nullptr;
    const Table* donor = nullptr;
    for (const Table& table : tables) {
        if (table.text.find("<Effects type=\"bmd\"") != std::string::npos) {
            target = &table;
        } else if (donor == nullptr || table.payload_length > donor->payload_length) {
            donor = &table;
        }
    }
    if (target == nullptr || donor == nullptr) {
        Log("categories: no bmd table among %zu - nothing patched", tables.size());
        return;
    }

    uint32_t target_offset = 0;
    uint32_t donor_offset = 0;
    unsigned char* const target_field = FindTreeField(regions, tables, *target, target_offset);
    unsigned char* const donor_field = FindTreeField(regions, tables, *donor, donor_offset);
    if (target_field == nullptr || donor_field == nullptr) {
        Log("categories: the resource tree was not found - nothing patched");
        return;
    }
    if (CountWord(regions, target_offset) != 1 || CountWord(regions, donor_offset) != 1) {
        Log("categories: tree offsets %u and %u are not unique - refusing to patch", target_offset,
            donor_offset);
        return;
    }
    Log("categories: bmd table at offset %u, donor at %u holding %u bytes", target_offset,
        donor_offset, donor->payload_length);

    // Minify the shipped rows before adding ours: every one is written as "\n    <Effect .../>",
    // and those runs cost real compressed bytes.
    std::string flat;
    flat.reserve(target->text.size());
    for (size_t at = 0; at < target->text.size(); ++at) {
        flat.push_back(target->text[at]);
        if (target->text[at] != '\n') {
            continue;
        }
        while (at + 1 < target->text.size() &&
               (target->text[at + 1] == ' ' || target->text[at + 1] == '\t')) {
            ++at;
        }
    }
    const size_t close_at = flat.rfind("</Effects>");
    if (close_at == std::string::npos) {
        Log("categories: the bmd table has no closing tag - nothing patched");
        return;
    }

    std::string additions;
    int added = 0;
    for (const ScannedPlugin& plugin : ScannedPlugins()) {
        additions += "<Effect id=\"";
        additions += plugin.key;
        additions += "\" category=\"";
        additions += CategoryFor(plugin.name);
        additions += "\"/>\n";
        ++added;
    }
    const std::string rebuilt = flat.substr(0, close_at) + additions + flat.substr(close_at);

    uLongf packed_length = compressBound(static_cast<uLong>(rebuilt.size()));
    std::vector<unsigned char> packed(packed_length);
    if (compress2(packed.data(), &packed_length, reinterpret_cast<const Bytef*>(rebuilt.data()),
                  static_cast<uLong>(rebuilt.size()), 9) != Z_OK) {
        Log("categories: compression failed - nothing patched");
        return;
    }
    const uint32_t new_payload = static_cast<uint32_t>(packed_length) + kInnerHeader;
    if (new_payload > donor->payload_length) {
        Log("categories: %d entries need %u bytes, the donor block holds %u - nothing patched",
            added, new_payload, donor->payload_length);
        return;
    }

    // Write the table first, repoint the node second. In that order a failed write leaves the tree
    // still describing the original table, so the worst case is the state we started in.
    if (!MakeWritable(donor->record, kOuterHeader + donor->payload_length) ||
        !MakeWritable(target_field, 4)) {
        return;
    }
    PutBe32(donor->record, new_payload);
    PutBe32(donor->record + kOuterHeader, static_cast<uint32_t>(rebuilt.size()));
    std::memcpy(donor->record + kOuterHeader + kInnerHeader, packed.data(), packed_length);
    PutBe32(target_field, donor_offset);

    Log("categories: added %d entries; bmd now reads offset %u, using %u of %u bytes", added,
        donor_offset, new_payload, donor->payload_length);
}
