// Putting bridged plugins into Fairlight's effect categories instead of "Uncategorized".
//
// This is an LD_PRELOAD library, not part of libfxbridge.so, and the reason is timing.
//
// Resolve decides an effect's category by looking its "<name>:<id>" up in a table compiled into
// libFairlightPage.so as the Qt resource ":/FLSystem/Plugin Metadata". An effect in no table has no
// category. Ours were in no table, which is the whole of the Uncategorized problem.
//
// Editing that table in the mapped library was tried and does not work. The only table Resolve
// consults for our entries is the "bmd" one, and it holds 594 compressed bytes where our entries
// need roughly 950. It cannot grow in place, because the next resource starts 598 bytes on.
// Relocating it needs the resource tree, and the tree's data offsets are not present as static
// data in the file: a seven-way offset signature search over 8.7 million candidate words found no
// compact run of nodes anywhere in the 46 MB library.
//
// So the patch moved to the one moment the tree is handed over for free. libFairlightPage.so
// imports qRegisterResourceData from Qt5Core:
//
//   U qRegisterResourceData(int, unsigned char const*, unsigned char const*, unsigned char const*)@Qt_5
//
// A preloaded definition of that symbol is called instead, with the tree, the name block and the
// data block as arguments. We rebuild the data block with a larger "bmd" table, shift the offsets
// of every file node that sat after it, and hand the copies to the real Qt function.
//
// THIS DOES NOT REACH THE CATEGORY TABLE, and the measurements say why. Interception works: 68
// registrations pass through here every start, the tree walk is correct once Qt's flag bits are
// read the right way round (Compressed is 0x01 and Directory is 0x02, not the reverse), and 101 of
// 101 compressed entries inflate. But no registration comes from libFairlightPage.so - the callers
// are resolve, libfraunhoferdcp, libBMDDavUI and Qt itself - and no resource anywhere in the
// process carries a file named "FLSystem" or "Plugin Metadata". Opening the Fairlight page adds no
// registration either.
//
// So the category tables are not Qt resources. They are Blackmagic's own data that happens to share
// Qt's [length][size][zlib] layout, referenced from code by a PC-relative instruction: there is no
// relocation anywhere in the library pointing into that region, so there is no pointer to repoint.
// Reaching them means rewriting an instruction displacement in a root-owned file that a Resolve
// update replaces. That is a decision, not a detail, so this file stops here.
//
// The unversioned definition here satisfies the versioned @Qt_5 reference: glibc treats a
// definition in an object with no version definitions as matching any requested version.
//
// What this file does NOT know is which plugins exist. It runs before libfxbridge.so is loaded and
// cannot call the scanner, so the two halves meet through a file the bridge writes each scan:
//
//   ~/.local/share/BMDAudioPlugins/fxbridge-categories.txt      "<key>\t<category code>" per line
//
// That means a newly added plugin is categorised from the restart after next. It is a real
// limitation, stated rather than hidden; the alternative is a second scanner that can disagree
// with the first.
//
// Failure is always a pass-through. Every step that cannot be completed hands Qt the original
// pointers unchanged, because a broken resource block costs the whole Fairlight page.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dlfcn.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <zlib.h>

namespace {

// Silent unless asked. The shim currently finds nothing to patch - see the note at the top of this
// file - so its chatter would be 100 lines of noise per start for no benefit. Set FXBRIDGE_SHIM_LOUD
// to bring the diagnostics back when picking this up again.
bool Verbose()
{
    static const bool on = std::getenv("FXBRIDGE_SHIM_LOUD") != nullptr;
    return on;
}

void Log(const char* format, ...) __attribute__((format(printf, 1, 2)));

void Log(const char* format, ...)
{
    if (!Verbose()) {
        return;
    }
    va_list args;
    va_start(args, format);
    std::fprintf(stderr, "[fxshim] ");
    std::vfprintf(stderr, format, args);
    std::fprintf(stderr, "\n");
    va_end(args);
}

uint16_t Be16(const unsigned char* at)
{
    return static_cast<uint16_t>((at[0] << 8) | at[1]);
}

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

// A Qt resource tree node is 14 bytes in version 1 and 22 from version 2, where the extra eight
// carry a last-modified stamp. In both, a node begins name offset (4), flags (2); a directory then
// has child count (4) and first child (4), and a file has country (2), language (2) and its data
// offset (4). So the data offset always sits at +10.
size_t NodeSize(int version)
{
    return version >= 2 ? 22u : 14u;
}

// Qt's own values, and the order is the opposite of the obvious guess: Compressed is bit 0 and
// Directory is bit 1. Reading them the other way round made every root node look like a file, so
// the walk stopped at node 0 and reported "1 files" for all 68 registrations.
constexpr uint16_t kFlagCompressed = 0x01;
constexpr uint16_t kFlagDirectory = 0x02;
constexpr uint16_t kFlagCompressedZstd = 0x04;

// Every file node in the tree, found by walking from the root rather than by scanning, because the
// node count is not stored anywhere.
void CollectFileNodes(const unsigned char* tree, int version, uint32_t node, std::vector<uint32_t>& out,
                      int depth, uint32_t& highest_node)
{
    if (node > highest_node) {
        highest_node = node;
    }
    if (depth > 32 || out.size() > 65536) {
        return;
    }
    const unsigned char* const at = tree + static_cast<size_t>(node) * NodeSize(version);
    const uint16_t flags = Be16(at + 4);
    if ((flags & kFlagDirectory) != 0) {
        const uint32_t count = Be32(at + 6);
        const uint32_t first = Be32(at + 10);
        if (count > 65536) {
            return;
        }
        for (uint32_t i = 0; i < count; ++i) {
            CollectFileNodes(tree, version, first + i, out, depth + 1, highest_node);
        }
        return;
    }
    out.push_back(node);
}

std::string Inflate(const unsigned char* stream, size_t available, size_t expected)
{
    std::string out(expected, '\0');
    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) {
        return std::string();
    }
    zs.next_in = const_cast<Bytef*>(stream);
    zs.avail_in = static_cast<uInt>(available);
    zs.next_out = reinterpret_cast<Bytef*>(&out[0]);
    zs.avail_out = static_cast<uInt>(expected);
    const int status = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);
    if (status != Z_STREAM_END || zs.total_out != expected) {
        return std::string();
    }
    return out;
}

// key -> category code, as written by the bridge after its scan.
const std::vector<std::pair<std::string, std::string>>& Wanted()
{
    static std::vector<std::pair<std::string, std::string>> rows;
    static bool loaded = false;
    if (loaded) {
        return rows;
    }
    loaded = true;
    const char* const home = std::getenv("HOME");
    if (home == nullptr) {
        return rows;
    }
    const std::string path =
        std::string(home) + "/.local/share/BMDAudioPlugins/fxbridge-categories.txt";
    std::FILE* const file = std::fopen(path.c_str(), "re");
    if (file == nullptr) {
        Log("no category list at %s - nothing to add", path.c_str());
        return rows;
    }
    char line[1024];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }
        char* const tab = std::strchr(line, '\t');
        if (tab == nullptr) {
            continue;
        }
        *tab = '\0';
        std::string category = tab + 1;
        while (!category.empty() && (category.back() == '\n' || category.back() == '\r')) {
            category.pop_back();
        }
        if (category.empty()) {
            continue;
        }
        rows.emplace_back(line, category);
    }
    std::fclose(file);
    Log("category list holds %zu entries", rows.size());
    return rows;
}

using RegisterFn = bool (*)(int, const unsigned char*, const unsigned char*, const unsigned char*);

RegisterFn RealRegister()
{
    static RegisterFn real = reinterpret_cast<RegisterFn>(
        dlsym(RTLD_NEXT, "_Z21qRegisterResourceDataiPKhS0_S0_"));
    return real;
}

}  // namespace

// The interception itself. C++ linkage on purpose: the mangled name has to match Qt's.
bool qRegisterResourceData(int version, const unsigned char* tree, const unsigned char* names,
                           const unsigned char* data)
{
    {
        // Who is registering. If libFairlightPage never appears here, its call is not reaching us
        // and the whole approach is wrong - which is worth knowing before tuning anything else.
        Dl_info info{};
        if (dladdr(__builtin_return_address(0), &info) != 0 && info.dli_fname != nullptr) {
            const char* const slash = std::strrchr(info.dli_fname, '/');
            Log("caller: %s", slash != nullptr ? slash + 1 : info.dli_fname);
        }
    }
    RegisterFn const real = RealRegister();
    if (real == nullptr) {
        // Nothing sensible to do: without the real function the resource cannot be registered at
        // all, and returning false silently would strip Resolve of its own UI resources.
        std::fprintf(stderr, "[fxshim] qRegisterResourceData not found behind us - resource lost\n");
        return false;
    }

    const std::vector<std::pair<std::string, std::string>>& wanted = Wanted();
    if (wanted.empty() || tree == nullptr || data == nullptr || version < 1 || version > 3) {
        return real(version, tree, names, data);
    }

    // Decode this resource's file names. A Qt name entry is length (2), hash (4), then that many
    // UTF-16BE characters. If ":/FLSystem/Plugin Metadata" is a Qt resource at all, its file names
    // are in one of these blocks; if no block ever holds them, the table is not a Qt resource and
    // this whole approach is aimed at the wrong mechanism.
    std::vector<uint32_t> files;
    uint32_t highest_node = 0;
    CollectFileNodes(tree, version, 0, files, 0, highest_node);
    static int dumped = 0;
    if (dumped < 3) {
        ++dumped;
        char hex[160] = {0};
        for (int i = 0; i < 40 && i * 3 + 3 < (int)sizeof(hex); ++i) {
            std::snprintf(hex + i * 3, 4, "%02x ", tree[i]);
        }
        Log("tree[0..40] = %s", hex);
    }
    if (files.size() > 100) {
        int flag_counts[8] = {0};
        for (const uint32_t node : files) {
            const uint16_t f = Be16(tree + static_cast<size_t>(node) * NodeSize(version) + 4);
            flag_counts[f & 7]++;
        }
        Log("flags histogram over %zu files: 0=%d 1=%d 2=%d 3=%d 4=%d 5=%d 6=%d 7=%d",
            files.size(), flag_counts[0], flag_counts[1], flag_counts[2], flag_counts[3],
            flag_counts[4], flag_counts[5], flag_counts[6], flag_counts[7]);
    }
    if (names != nullptr) {
        for (const uint32_t node : files) {
            const uint32_t name_offset = Be32(tree + static_cast<size_t>(node) * NodeSize(version));
            const uint16_t length = Be16(names + name_offset);
            if (length == 0 || length > 128) {
                continue;
            }
            std::string plain;
            for (uint16_t i = 0; i < length; ++i) {
                const uint16_t ch = Be16(names + name_offset + 6 + i * 2);
                plain.push_back(ch < 128 ? static_cast<char>(ch) : '?');
            }
            if (plain.find("Metadata") != std::string::npos ||
                plain.find("FLSystem") != std::string::npos ||
                plain.find("Effects") != std::string::npos) {
                Log("  name of interest: \"%s\"", plain.c_str());
            }
        }
    }
    Log("register: version %d, %zu files, highest node %u", version, files.size(), highest_node);
    if (files.empty()) {
        return real(version, tree, names, data);
    }

    // Find the "bmd" table among this block's files, and the end of the block while we are here.
    const size_t node_size = NodeSize(version);
    uint32_t target_offset = 0;
    uint32_t target_node = 0;
    std::string target_text;
    bool found = false;
    uint32_t block_end = 0;
    int compressed_seen = 0;
    int inflated_ok = 0;
    int texts_seen = 0;

    for (const uint32_t node : files) {
        const unsigned char* const at = tree + static_cast<size_t>(node) * node_size;
        const uint32_t offset = Be32(at + 10);
        const uint32_t length = Be32(data + offset);
        if (offset + 4 + length > block_end) {
            block_end = offset + 4 + length;
        }
        const uint16_t flags = Be16(at + 4);
        if (found || length < 12 || (flags & kFlagCompressed) == 0 ||
            (flags & kFlagCompressedZstd) != 0) {
            continue;
        }
        const unsigned char* const payload = data + offset + 4;
        const uint32_t expanded = Be32(payload);
        if (expanded < 64 || expanded > (1u << 22)) {
            continue;
        }
        ++compressed_seen;
        const std::string text = Inflate(payload + 4, length - 4, expanded);
        if (text.empty()) {
            continue;
        }
        ++inflated_ok;
        if (text.find("<?xml") != std::string::npos) {
            ++texts_seen;
        }
        if (text.find("<Effects type=\"bmd\"") == std::string::npos) {
            if (text.find("<Effects type=") != std::string::npos) {
                Log("  saw a sibling Effects table (%zu bytes) but not the bmd one", text.size());
            }
            continue;
        }
        target_offset = offset;
        target_node = node;
        target_text = text;
        found = true;
    }

    if (!found) {
        if (files.size() > 50) {
            Log("  no bmd table here: %d compressed, %d inflated, %d texts seen", compressed_seen,
                inflated_ok, texts_seen);
        }
        return real(version, tree, names, data);
    }
    Log("found the bmd table: node %u, offset %u, %zu bytes of XML", target_node, target_offset,
        target_text.size());

    const size_t close_at = target_text.rfind("</Effects>");
    if (close_at == std::string::npos) {
        return real(version, tree, names, data);
    }

    std::string additions;
    for (const auto& row : wanted) {
        additions += "<Effect id=\"";
        additions += row.first;
        additions += "\" category=\"";
        additions += row.second;
        additions += "\"/>\n";
    }
    const std::string rebuilt =
        target_text.substr(0, close_at) + additions + target_text.substr(close_at);

    uLongf packed_length = compressBound(static_cast<uLong>(rebuilt.size()));
    std::vector<unsigned char> packed(packed_length);
    if (compress2(packed.data(), &packed_length, reinterpret_cast<const Bytef*>(rebuilt.data()),
                  static_cast<uLong>(rebuilt.size()), 9) != Z_OK) {
        return real(version, tree, names, data);
    }

    const uint32_t old_record = 4 + Be32(data + target_offset);
    const uint32_t new_payload = static_cast<uint32_t>(packed_length) + 4;
    const uint32_t new_record = 4 + new_payload;
    const int64_t shift = static_cast<int64_t>(new_record) - static_cast<int64_t>(old_record);

    // The blocks are heap copies that are never freed: Qt keeps the pointers for the lifetime of
    // the process, and a resource root outlives every caller here.
    auto* const new_data = new unsigned char[block_end + (shift > 0 ? shift : 0) + 16];
    std::memcpy(new_data, data, target_offset);
    PutBe32(new_data + target_offset, new_payload);
    PutBe32(new_data + target_offset + 4, static_cast<uint32_t>(rebuilt.size()));
    std::memcpy(new_data + target_offset + 8, packed.data(), packed_length);
    const uint32_t tail_from = target_offset + old_record;
    if (block_end > tail_from) {
        std::memcpy(new_data + target_offset + new_record, data + tail_from, block_end - tail_from);
    }

    // Every file that lived after the table moves by the same amount.
    const size_t tree_bytes = (static_cast<size_t>(highest_node) + 1) * node_size;
    auto* const new_tree = new unsigned char[tree_bytes];
    std::memcpy(new_tree, tree, tree_bytes);
    int moved = 0;
    for (const uint32_t node : files) {
        unsigned char* const at = new_tree + static_cast<size_t>(node) * node_size;
        const uint32_t offset = Be32(at + 10);
        if (offset > target_offset) {
            PutBe32(at + 10, static_cast<uint32_t>(static_cast<int64_t>(offset) + shift));
            ++moved;
        }
    }

    Log("added %zu entries: record %u -> %u bytes, %d later files shifted by %+lld",
        wanted.size(), old_record, new_record, moved, static_cast<long long>(shift));
    return real(version, new_tree, names, new_data);
}
