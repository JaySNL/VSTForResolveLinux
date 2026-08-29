#include "plugin_scan.h"

#include "vst3_plugin.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

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

// The numeric half of every effect id we register. It is the source effect's id, kept because the
// create hook substitutes our key for the source key and the stock factory needs that number.
constexpr const char kEffectIdSuffix[] = ":1112360057";

// How deep to walk. yabridge nests one folder per vendor, so three is already generous.
constexpr int kMaxDepth = 4;

bool IsDirectory(const std::string& path)
{
    struct stat info;
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool IsFile(const std::string& path)
{
    struct stat info;
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

std::string HomeDirectory()
{
    const char* const home = std::getenv("HOME");
    return home != nullptr ? std::string(home) : std::string();
}

// The file's stem: no directory, no extension.
std::string Stem(const std::string& path)
{
    const size_t slash = path.find_last_of('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot != 0) {
        name.erase(dot);
    }
    return name;
}

bool EndsWith(const std::string& text, const char* suffix)
{
    const size_t length = std::strlen(suffix);
    return text.size() >= length && text.compare(text.size() - length, length, suffix) == 0;
}

// A shared library that ships beside plugins but is not one. yabridge's chainloader, Carla's
// helper libraries and its process bridges all live in the same folders as real VST2 plugins, and
// every one of them exports nothing a host can use.
//
// Loading one is not harmless: dlopen runs its constructors. So this filter is on names, before
// anything is opened, and it is deliberately blunt.
bool LooksLikeSupportLibrary(const std::string& file_name)
{
    if (file_name.compare(0, 3, "lib") == 0) {
        return true;
    }
    static const char* const markers[] = {"bridge", "interposer", "chainloader"};
    std::string lowered = file_name;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* marker : markers) {
        if (lowered.find(marker) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// The library inside a bundle, or the file itself when it is the flat variant. The cache stamps
// this rather than the bundle folder: replacing the library inside a bundle does not change the
// folder's modification time, so stamping the folder would serve stale class names forever.
std::string Vst3BinaryOf(const std::string& bundle);

// A VST3 bundle is a directory: Name.vst3/Contents/x86_64-linux/Name.so. yabridge builds the same
// shape around a Windows plugin, with an extra x86_64-win folder beside it that we never touch.
// The bundle directory is what the host is given, so this only has to confirm the Linux binary is
// there - an incomplete bundle in the menu is an entry that loads nothing.
bool Vst3BundleHasLinuxBinary(const std::string& bundle)
{
    const std::string folder = bundle + "/Contents/x86_64-linux";
    if (!IsDirectory(folder)) {
        return false;
    }
    DIR* const directory = opendir(folder.c_str());
    if (directory == nullptr) {
        return false;
    }
    bool found = false;
    while (const dirent* const item = readdir(directory)) {
        if (EndsWith(item->d_name, ".so")) {
            found = true;
            break;
        }
    }
    closedir(directory);
    return found;
}

struct Candidate {
    std::string path;
    PluginFormat format;
};

void ScanFolder(const std::string& folder, PluginFormat wanted, int depth,
                std::vector<Candidate>& out)
{
    if (depth > kMaxDepth) {
        return;
    }
    DIR* const directory = opendir(folder.c_str());
    if (directory == nullptr) {
        return;
    }
    while (const dirent* const item = readdir(directory)) {
        const std::string name = item->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        const std::string path = folder + "/" + name;

        if (wanted == PluginFormat::Vst3 && EndsWith(name, ".vst3")) {
            if (IsDirectory(path) && Vst3BundleHasLinuxBinary(path)) {
                out.push_back({path, PluginFormat::Vst3});
            } else if (IsFile(path)) {
                out.push_back({path, PluginFormat::Vst3});  // the flat, single-file variant
            }
            continue;  // never descend into a bundle
        }
        if (wanted == PluginFormat::Clap && EndsWith(name, ".clap")) {
            if (IsFile(path)) {
                out.push_back({path, PluginFormat::Clap});
                continue;
            }
        }
        if (wanted == PluginFormat::Vst2 && EndsWith(name, ".so") && IsFile(path)) {
            if (!LooksLikeSupportLibrary(name)) {
                out.push_back({path, PluginFormat::Vst2});
            }
            continue;
        }
        if (!IsDirectory(path)) {
            continue;
        }
        // A VST2 bundle is a folder called Name.vst holding the plugin at its top level. Carla's
        // ships a styles/ folder beside it with a Qt style plugin in it, and that .so is not a VST -
        // loading it would run a Qt plugin's constructors inside Resolve. So a bundle is read flat.
        if (depth > 0 && wanted == PluginFormat::Vst2 && EndsWith(folder, ".vst")) {
            continue;
        }
        // Another format's bundle is never a folder to walk into.
        if (EndsWith(name, ".vst3") || EndsWith(name, ".clap") ||
            (wanted != PluginFormat::Vst2 && EndsWith(name, ".vst"))) {
            continue;
        }
        ScanFolder(path, wanted, depth + 1, out);
    }
    closedir(directory);
}

// The folders each format is installed in. The environment variables come first, because a user
// who set one means it.
void RootsFor(PluginFormat format, std::vector<std::string>& roots)
{
    const std::string home = HomeDirectory();
    const char* variable = nullptr;
    const char* fixed[4] = {nullptr, nullptr, nullptr, nullptr};
    const char* under_home = nullptr;

    if (format == PluginFormat::Clap) {
        variable = "CLAP_PATH";
        under_home = "/.clap";
        fixed[0] = "/usr/lib/clap";
        fixed[1] = "/usr/local/lib/clap";
    } else if (format == PluginFormat::Vst3) {
        variable = "VST3_PATH";
        under_home = "/.vst3";
        fixed[0] = "/usr/lib/vst3";
        fixed[1] = "/usr/local/lib/vst3";
    } else if (format == PluginFormat::Vst2) {
        variable = "VST_PATH";
        under_home = "/.vst";
        fixed[0] = "/usr/lib/vst";
        fixed[1] = "/usr/local/lib/vst";
        fixed[2] = "/usr/lib/lxvst";
        fixed[3] = "/usr/local/lib/lxvst";
    }

    if (variable != nullptr) {
        const char* const value = std::getenv(variable);
        if (value != nullptr && value[0] != '\0') {
            std::string all = value;
            size_t start = 0;
            while (start <= all.size()) {
                const size_t colon = all.find(':', start);
                const std::string one =
                    all.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
                if (!one.empty()) {
                    roots.push_back(one);
                }
                if (colon == std::string::npos) {
                    break;
                }
                start = colon + 1;
            }
        }
    }
    if (under_home != nullptr && !home.empty()) {
        roots.push_back(home + under_home);
    }
    if (format == PluginFormat::Vst2 && !home.empty()) {
        roots.push_back(home + "/.lxvst");
    }
    for (const char* one : fixed) {
        if (one != nullptr) {
            roots.push_back(one);
        }
    }
}

const char* FormatName(PluginFormat format)
{
    switch (format) {
        case PluginFormat::Clap: return "CLAP";
        case PluginFormat::Vst2: return "VST2";
        case PluginFormat::Vst3: return "VST3";
        default: return "unknown";
    }
}

// Which formats reach the menu.
//
// All three are listed. VST3 used to be scanned but withheld, because CreateHostedPlugin could not
// host it; that host landed on 2026-08-25 (IPlugFrame + IRunLoop for the editor, IConnectionPoint
// to join component and controller) and Windows VST3 now loads through yabridge as well.
//
// Leaving the old default in place cost a whole diagnosis: Resolve started without the variable
// set, twenty-one VST3 entries silently vanished from a menu that had listed them minutes before,
// and the missing entries read as a regression in the scanner rather than as this switch.
// Set FXBRIDGE_SCAN_FORMATS to a subset such as "clap,vst2" to narrow it again.
bool FormatIsListed(PluginFormat format)
{
    const char* const setting = std::getenv("FXBRIDGE_SCAN_FORMATS");
    const std::string list = setting != nullptr && setting[0] != '\0' ? setting : "clap,vst2,vst3";
    switch (format) {
        case PluginFormat::Clap: return list.find("clap") != std::string::npos;
        case PluginFormat::Vst2: return list.find("vst2") != std::string::npos;
        case PluginFormat::Vst3: return list.find("vst3") != std::string::npos;
        default: return false;
    }
}

// Which plugins inside a shell reach the menu.
//
// A shell is one file that publishes many plugins. The Waves WaveShell publishes 718, and listing
// all of them would bury the other fifty-five entries in the effect menu. So a shell is expanded
// only through a filter file, one pattern per line, matched as a substring of the class name:
//
//   ~/.local/share/BMDAudioPlugins/fxbridge-shell-allow.txt
//
// A line starting with # is a comment. With no file, a shell contributes its first class only -
// the old behaviour, and never a menu full of plugins nobody asked for. An empty file means the
// same thing, deliberately: "allow nothing" has to be sayable.
// ---------------------------------------------------------------------------
// The scan cache
//
// Reading a VST3's class names means opening the module, and for a Windows plugin that means
// yabridge starting a Wine host. Measured on a tester's machine on 2026-08-29, with a Windows
// plugin collection: enabling this bridge took Resolve from 229 threads to 706, from 9.5 GB of
// system memory to 22.8 GB, and left three cores spinning - with no project open. The count that
// named it was `pgrep -fc yabridge-host`: **336**. Resolve's own resident set had barely moved,
// because the memory was in 336 other processes.
//
// So the scan remembers what it found. A module is opened only when this file has nothing for it,
// or when its size or modification time has changed - which is to say, after it is installed or
// updated, and never again. A normal start opens nothing at all.
//
// The format is one header line per module, then one line per class:
//
//   M<TAB>path<TAB>size<TAB>mtime<TAB>count
//   C<TAB>class name<TAB>subcategory
//
// A line that does not parse is skipped rather than fatal: a stale or truncated cache must cost a
// rescan, never a failed start.
// ---------------------------------------------------------------------------

std::string Vst3BinaryOf(const std::string& bundle)
{
    const std::string folder = bundle + "/Contents/x86_64-linux";
    if (!IsDirectory(folder)) {
        return bundle;
    }
    DIR* const directory = opendir(folder.c_str());
    if (directory == nullptr) {
        return bundle;
    }
    std::string found;
    while (const dirent* const item = readdir(directory)) {
        if (EndsWith(item->d_name, ".so")) {
            found = folder + "/" + item->d_name;
            break;
        }
    }
    closedir(directory);
    return found.empty() ? bundle : found;
}

// ---------------------------------------------------------------------------
// The deny list
//
// The cache makes a normal start cheap, but it cannot help the FIRST start, and that is the one
// that hurts on a large collection: every module has to be opened once. A plugin that hangs the
// scan is worse still - there is no way past it except moving the file.
//
// So: one substring per line, matched against the module's full path, applied BEFORE anything is
// opened. Asked for by a tester on 2026-08-29 whose first scan started 336 Wine hosts.
//
//   ~/.local/share/BMDAudioPlugins/fxbridge-scan-deny.txt
//
// Matching is deliberately dull, and the generated file says so: a plain case-insensitive
// substring of the path. Not a glob and not a regular expression - "*" matches a literal asterisk.
// Anything cleverer is a second thing to learn and a second thing to get wrong, and a path is
// already the most specific handle a person has.
// ---------------------------------------------------------------------------

std::string LoweredCopy(const std::string& text)
{
    std::string lowered = text;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lowered;
}

std::string ConfigPath(const char* file_name)
{
    const char* const home = std::getenv("HOME");
    if (home == nullptr) {
        return std::string();
    }
    return std::string(home) + "/.local/share/BMDAudioPlugins/" + file_name;
}

const std::vector<std::string>& ScanDenyList()
{
    static std::vector<std::string> patterns;
    static bool loaded = false;
    if (loaded) {
        return patterns;
    }
    loaded = true;
    const std::string path = ConfigPath("fxbridge-scan-deny.txt");
    if (path.empty()) {
        return patterns;
    }
    std::FILE* const file = std::fopen(path.c_str(), "re");
    if (file == nullptr) {
        return patterns;
    }
    char line[1024];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        std::string entry = line;
        while (!entry.empty() && (entry.back() == '\n' || entry.back() == '\r' ||
                                  entry.back() == ' ' || entry.back() == '\t')) {
            entry.pop_back();
        }
        size_t start = 0;
        while (start < entry.size() && (entry[start] == ' ' || entry[start] == '\t')) {
            ++start;
        }
        entry = entry.substr(start);
        if (entry.empty() || entry[0] == '#') {
            continue;
        }
        patterns.push_back(LoweredCopy(entry));
    }
    std::fclose(file);
    if (!patterns.empty()) {
        Log("scan: deny list holds %zu patterns", patterns.size());
    }
    return patterns;
}

bool ScanDenied(const std::string& path)
{
    const std::vector<std::string>& patterns = ScanDenyList();
    if (patterns.empty()) {
        return false;
    }
    const std::string lowered = LoweredCopy(path);
    for (const std::string& pattern : patterns) {
        if (lowered.find(pattern) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// Writes the file the first time, with everything this machine has, all commented out.
//
// A blank file that has to be filled in from memory is a file nobody fills in. This one is the
// list of what is actually installed, so using it is deleting a "#" rather than getting a path
// right by hand. It is written only when there is no file: an existing one is somebody's work and
// is never touched. Delete it to have it built again from the current scan.
void ScanDenyTemplateWrite(const std::vector<std::string>& paths)
{
    const std::string path = ConfigPath("fxbridge-scan-deny.txt");
    if (path.empty()) {
        return;
    }
    if (std::FILE* const existing = std::fopen(path.c_str(), "re")) {
        std::fclose(existing);
        return;
    }
    std::FILE* const file = std::fopen(path.c_str(), "we");
    if (file == nullptr) {
        return;
    }
    std::fprintf(file, "%s",
                 "# Plugins this bridge should not even open.\n"
                 "#\n"
                 "# One pattern per line. A pattern is a plain SUBSTRING of the plugin's full\n"
                 "# path, matched without regard to upper or lower case.\n"
                 "#\n"
                 "# It is not a glob and not a regular expression:\n"
                 "#     Waves          matches every path containing \"waves\"\n"
                 "#     /yabridge/     matches everything under a folder called yabridge\n"
                 "#     smartEQ4.vst3  matches that one plugin\n"
                 "#     *.vst3         matches NOTHING - the asterisk is a literal asterisk\n"
                 "#\n"
                 "# A line starting with # is a comment. Blank lines are ignored.\n"
                 "#\n"
                 "# Why this exists: reading a plugin's name means opening it, and opening a\n"
                 "# Windows plugin starts a Wine host. That happens once - what a module answers\n"
                 "# is cached - but the first start after installing one is the slow one, and a\n"
                 "# plugin that hangs the scan has no way past it otherwise.\n"
                 "#\n"
                 "# Everything found on this machine is listed below, commented out. Remove the\n"
                 "# \"# \" in front of a line to stop that plugin being opened. Changes take\n"
                 "# effect on the next Resolve start.\n"
                 "#\n"
                 "# This file is written once. Your edits are never overwritten - delete it to\n"
                 "# have it built again from the current scan.\n"
                 "\n");
    for (const std::string& entry : paths) {
        std::fprintf(file, "# %s\n", entry.c_str());
    }
    std::fclose(file);
    Log("scan: wrote a deny-list template with %zu plugins, all commented out", paths.size());
}

// --- the module that did not come back ---------------------------------------------------------
//
// Opening a plugin runs the plugin's own code inside Resolve. A plugin that hangs or faults there
// takes the whole start down with it, and every later start opens the same module again, in the
// same order, and dies in the same place. The scan can never get past its worst plugin, so the
// plugins after it are never seen either.
//
// So the scan writes down which module it is about to open, and rubs it out when that module
// answers. A name still written down at the next start belongs to a module that did not come
// back. It is recorded and skipped from then on, out loud - never silently, and never on a
// plugin that merely returned nothing.
//
// A start killed by hand while the scan is running blames whichever module was open at that
// moment. That is the price of not being able to tell one dead process from another, and it is
// why the skip is one line in a file the log names, and one delete away from being undone.

std::string ScanInFlightPath() { return ConfigPath("fxbridge-scan-open.txt"); }

std::string ScanBadPath() { return ConfigPath("fxbridge-scan-crashed.txt"); }

std::vector<std::string>& ScanBadList()
{
    static std::vector<std::string> bad;
    return bad;
}

// Written and closed before the module opens, so it survives a process that never returns.
void ScanInFlightMark(const std::string& path)
{
    const std::string file = ScanInFlightPath();
    if (file.empty()) {
        return;
    }
    if (std::FILE* const out = std::fopen(file.c_str(), "we")) {
        std::fprintf(out, "%s\n", path.c_str());
        std::fclose(out);
    }
}

void ScanInFlightClear()
{
    const std::string file = ScanInFlightPath();
    if (!file.empty()) {
        ::unlink(file.c_str());
    }
}

// Reads the note the previous start left, and the list of everything blamed before it.
void ScanBadLoad()
{
    std::vector<std::string>& bad = ScanBadList();
    bad.clear();

    const std::string list = ScanBadPath();
    if (!list.empty()) {
        if (std::FILE* const in = std::fopen(list.c_str(), "re")) {
            char line[1024];
            while (std::fgets(line, sizeof(line), in) != nullptr) {
                std::string entry = line;
                while (!entry.empty() && (entry.back() == '\n' || entry.back() == '\r')) {
                    entry.pop_back();
                }
                if (!entry.empty() && entry[0] != '#') {
                    bad.push_back(entry);
                }
            }
            std::fclose(in);
        }
    }

    const std::string flight = ScanInFlightPath();
    if (flight.empty()) {
        return;
    }
    std::string stranded;
    if (std::FILE* const in = std::fopen(flight.c_str(), "re")) {
        char line[1024];
        if (std::fgets(line, sizeof(line), in) != nullptr) {
            stranded = line;
            while (!stranded.empty() && (stranded.back() == '\n' || stranded.back() == '\r')) {
                stranded.pop_back();
            }
        }
        std::fclose(in);
    }
    ::unlink(flight.c_str());
    if (stranded.empty()) {
        return;
    }

    Log("scan: the last start stopped while it was opening %s. It is skipped from now on.",
        stranded.c_str());
    if (std::find(bad.begin(), bad.end(), stranded) == bad.end()) {
        bad.push_back(stranded);
        if (!list.empty()) {
            const bool fresh = std::fopen(list.c_str(), "re") == nullptr;
            if (std::FILE* const out = std::fopen(list.c_str(), "ae")) {
                if (fresh) {
                    std::fprintf(out, "%s",
                                 "# Plugins that stopped a Resolve start while they were being\n"
                                 "# opened. Each one was skipped from the scan after that.\n"
                                 "#\n"
                                 "# A start you killed by hand blames whatever was open at that\n"
                                 "# moment, so a line here is not proof of a broken plugin.\n"
                                 "# Delete a line to have that plugin tried again, or delete the\n"
                                 "# whole file to try all of them.\n");
                }
                std::fprintf(out, "%s\n", stranded.c_str());
                std::fclose(out);
            }
        }
    }
    Log("scan: delete %s to try it again", list.c_str());
}

bool ScanIsBad(const std::string& path)
{
    const std::vector<std::string>& bad = ScanBadList();
    return std::find(bad.begin(), bad.end(), path) != bad.end();
}

std::string ScanCachePath()
{
    const char* const home = std::getenv("HOME");
    if (home == nullptr) {
        return std::string();
    }
    return std::string(home) + "/.local/share/BMDAudioPlugins/fxbridge-scan-cache.tsv";
}

struct CachedModule {
    unsigned long long size = 0;
    unsigned long long mtime = 0;
    // How many classes the header promised. Kept so a truncated write can be told apart from a
    // module that genuinely has none, which are the same thing on disk and must not be.
    size_t expected = 0;
    std::vector<std::string> classes;
    std::vector<std::string> categories;
};

std::map<std::string, CachedModule>& ScanCache()
{
    static std::map<std::string, CachedModule> cache;
    return cache;
}

bool ModuleStamp(const std::string& path, unsigned long long& size, unsigned long long& mtime)
{
    // A VST3 bundle is a directory, so the stamp is taken from the binary inside it rather than
    // from the folder: a folder's mtime does not change when the library in it is replaced.
    const std::string binary = Vst3BinaryOf(path);
    struct stat info;
    if (stat(binary.empty() ? path.c_str() : binary.c_str(), &info) != 0) {
        return false;
    }
    size = static_cast<unsigned long long>(info.st_size);
    mtime = static_cast<unsigned long long>(info.st_mtime);
    return true;
}

std::vector<std::string> SplitTabs(const std::string& line)
{
    std::vector<std::string> fields;
    size_t start = 0;
    for (size_t at = 0; at <= line.size(); ++at) {
        if (at == line.size() || line[at] == '\t') {
            fields.push_back(line.substr(start, at - start));
            start = at + 1;
        }
    }
    return fields;
}

void ScanCacheLoad()
{
    const std::string path = ScanCachePath();
    if (path.empty()) {
        return;
    }
    std::FILE* const file = std::fopen(path.c_str(), "re");
    if (file == nullptr) {
        return;  // no cache is the normal first run, not a fault
    }
    char line[4096];
    std::string current;
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        std::string text = line;
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
            text.pop_back();
        }
        const std::vector<std::string> fields = SplitTabs(text);
        if (fields.size() == 5 && fields[0] == "M") {
            current = fields[1];
            CachedModule entry;
            entry.size = std::strtoull(fields[2].c_str(), nullptr, 10);
            entry.mtime = std::strtoull(fields[3].c_str(), nullptr, 10);
            entry.expected = static_cast<size_t>(std::strtoul(fields[4].c_str(), nullptr, 10));
            ScanCache()[current] = entry;
        } else if (fields.size() == 3 && fields[0] == "C" && !current.empty()) {
            CachedModule& entry = ScanCache()[current];
            entry.classes.push_back(fields[1]);
            entry.categories.push_back(fields[2]);
        }
    }
    std::fclose(file);
    // A record whose class count does not match its header is a truncated write: drop it, so the
    // module is opened once and the cache repairs itself rather than listing half a shell.
    //
    // A record promising zero classes is NOT that. It is a module that answered nothing - a native
    // Linux bundle sitting in the yabridge folder is the common case - and remembering that is the
    // whole point. Treating the two alike reopened ten modules on every start here, which on a
    // large collection is ten Wine hosts started to re-learn nothing.
    for (auto it = ScanCache().begin(); it != ScanCache().end();) {
        it = it->second.classes.size() != it->second.expected ? ScanCache().erase(it)
                                                              : std::next(it);
    }
    Log("scan: cache holds %zu modules", ScanCache().size());
}

void ScanCacheStore()
{
    const std::string path = ScanCachePath();
    if (path.empty()) {
        return;
    }
    const std::string temporary = path + ".new";
    std::FILE* const file = std::fopen(temporary.c_str(), "we");
    if (file == nullptr) {
        Log("scan: cannot write %s", temporary.c_str());
        return;
    }
    for (const auto& entry : ScanCache()) {
        std::fprintf(file, "M\t%s\t%llu\t%llu\t%zu\n", entry.first.c_str(), entry.second.size,
                     entry.second.mtime, entry.second.classes.size());
        for (size_t at = 0; at < entry.second.classes.size(); ++at) {
            std::fprintf(file, "C\t%s\t%s\n", entry.second.classes[at].c_str(),
                         at < entry.second.categories.size() ? entry.second.categories[at].c_str()
                                                             : "");
        }
    }
    std::fclose(file);
    // Renamed into place, never written over: a half-written cache read by the next start would
    // list half of somebody's shell and look like plugins had gone missing.
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        Log("scan: cannot replace %s", path.c_str());
    }
}

const std::vector<std::string>& ShellAllowList(bool& configured)
{
    static std::vector<std::string> patterns;
    static bool loaded = false;
    static bool present = false;
    if (!loaded) {
        loaded = true;
        const char* const home = std::getenv("HOME");
        if (home != nullptr) {
            const std::string path =
                std::string(home) + "/.local/share/BMDAudioPlugins/fxbridge-shell-allow.txt";
            if (std::FILE* const file = std::fopen(path.c_str(), "re")) {
                present = true;
                char line[512];
                while (std::fgets(line, sizeof(line), file) != nullptr) {
                    std::string entry = line;
                    while (!entry.empty() && (entry.back() == '\n' || entry.back() == '\r' ||
                                              entry.back() == ' ' || entry.back() == '\t')) {
                        entry.pop_back();
                    }
                    size_t start = 0;
                    while (start < entry.size() && (entry[start] == ' ' || entry[start] == '\t')) {
                        ++start;
                    }
                    entry = entry.substr(start);
                    if (entry.empty() || entry[0] == '#') {
                        continue;
                    }
                    patterns.push_back(entry);
                }
                std::fclose(file);
                Log("scan: shell filter has %zu patterns", patterns.size());
            }
        }
    }
    configured = present;
    return patterns;
}

bool ShellClassAllowed(const std::string& class_name, const std::vector<std::string>& patterns)
{
    for (const std::string& pattern : patterns) {
        if (class_name.find(pattern) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::vector<ScannedPlugin> g_plugins;
bool g_scanned = false;

void Scan()
{
    const PluginFormat formats[] = {PluginFormat::Clap, PluginFormat::Vst2, PluginFormat::Vst3};
    std::vector<Candidate> candidates;
    int skipped[3] = {0, 0, 0};

    for (size_t index = 0; index < sizeof(formats) / sizeof(formats[0]); ++index) {
        const PluginFormat format = formats[index];
        std::vector<std::string> roots;
        RootsFor(format, roots);

        std::vector<Candidate> found;
        for (const std::string& root : roots) {
            if (IsDirectory(root)) {
                ScanFolder(root, format, 0, found);
            }
        }
        if (!FormatIsListed(format)) {
            skipped[index] = static_cast<int>(found.size());
            continue;
        }
        candidates.insert(candidates.end(), found.begin(), found.end());
    }

    // The same file found under two roots is one plugin. Sorting by path also makes the scan
    // repeatable, which matters: the menu name is stored in the project, so an entry that changes
    // name between runs is an effect that stops loading.
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.path < b.path; });
    candidates.erase(std::unique(candidates.begin(), candidates.end(),
                                 [](const Candidate& a, const Candidate& b) {
                                     return a.path == b.path;
                                 }),
                     candidates.end());

    // One name per entry, and the name has to be unique: the key is built from it, and two effects
    // under one key means the second insert is dropped by the map.
    std::map<std::string, int> seen;
    // Read what previous starts already learned. Every module found here is one Wine host that
    // does not have to be started to answer a question it answered before.
    ScanCacheLoad();
    ScanBadLoad();

    // The escape hatch is written BEFORE the loop it is an escape from.
    //
    // It used to be written after, from the list the loop had built. That is exactly backwards:
    // a scan that finishes does not need a deny list, and a scan that hangs never wrote one. A
    // tester on v0.2.5 hung here, looked in the directory, and found nothing but the library -
    // no deny file to edit, no cache, and so no way past the module that stopped him. Nothing in
    // this list needs the loop: every path is known from the candidate scan above.
    std::vector<std::string> offered;
    offered.reserve(candidates.size());
    for (const Candidate& candidate : candidates) {
        offered.push_back(candidate.path);
    }
    ScanDenyTemplateWrite(offered);

    int from_cache = 0;
    int opened = 0;
    int denied = 0;
    int stopped = 0;

    // Said out loud before the work starts, because the work is invisible while it happens.
    // Opening one Windows VST3 through yabridge starts a Wine host, asks it, and shuts it down
    // again: 987 ms and 792 ms for the two Waves shells on the development machine. A first start
    // with a hundred of them is therefore a minute of a Resolve splash screen that does not move,
    // and a tester reasonably reported that as the program being stuck. Later starts read the
    // cache and open nothing.
    int to_open = 0;
    for (const Candidate& candidate : candidates) {
        if (candidate.format != PluginFormat::Vst3 || ScanDenied(candidate.path) ||
            ScanIsBad(candidate.path)) {
            continue;
        }
        unsigned long long size = 0;
        unsigned long long mtime = 0;
        auto found = ScanCache().find(candidate.path);
        if (!ModuleStamp(candidate.path, size, mtime) || found == ScanCache().end() ||
            found->second.size != size || found->second.mtime != mtime) {
            ++to_open;
        }
    }
    if (to_open > 0) {
        Log("scan: %d VST3 modules have to be opened. Each Windows one starts and stops a Wine "
            "host, so this start is slow and the next one is not.",
            to_open);
    }

    for (const Candidate& candidate : candidates) {
        // Before anything is opened. That is the whole point of this list: a plugin that hangs the
        // scan has to be stoppable without being opened first.
        if (ScanDenied(candidate.path)) {
            ++denied;
            continue;
        }
        if (ScanIsBad(candidate.path)) {
            ++stopped;
            continue;
        }
        std::string name = Stem(candidate.path);
        if (name.empty()) {
            continue;
        }
        // Two builds of one plugin - a .clap and a .vst3 of the same effect is the normal case -
        // share a stem, so the format tells them apart. A third of the same format gets a number.
        if (seen.find(name) != seen.end()) {
            const std::string tagged = name + " (" + FormatName(candidate.format) + ")";
            if (seen.find(tagged) == seen.end()) {
                name = tagged;
            } else {
                char suffix[32];
                std::snprintf(suffix, sizeof(suffix), " (%s %d)", FormatName(candidate.format),
                              ++seen[tagged]);
                name = name + suffix;
            }
        }
        // A VST3 file may be a shell: one file, many plugins. Ask it before naming the entry,
        // because a shell's file stem ("WaveShell1-VST3 17.1_x64") names nothing a user wants to
        // pick. Enumeration does not create anything, so this is cheap - 718 classes came back from
        // the WaveShell in 1.674 s, essentially all of it Wine starting behind yabridge.
        std::vector<std::string> classes;
        std::vector<std::string> declared;
        if (candidate.format == PluginFormat::Vst3) {
            unsigned long long size = 0;
            unsigned long long mtime = 0;
            const bool stamped = ModuleStamp(candidate.path, size, mtime);
            auto found = ScanCache().find(candidate.path);
            const bool fresh = stamped && found != ScanCache().end() &&
                               found->second.size == size && found->second.mtime == mtime;
            if (fresh) {
                classes = found->second.classes;
                declared = found->second.categories;
                ++from_cache;
            } else {
                // Named before the module opens, never only after. A module that hangs the scan
                // or takes the process down with it writes nothing of its own, so the last line
                // in the log has to be the one that says which module was being asked.
                Log("scan: opening %s", candidate.path.c_str());
                // On disk before the plugin's own code runs, and gone again the moment it
                // answers. Nothing between these two lines is allowed to assume it returns.
                ScanInFlightMark(candidate.path);
                const auto started = std::chrono::steady_clock::now();
                Vst3ListClasses(candidate.path.c_str(), classes, &declared);
                ScanInFlightClear();
                const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - started)
                                       .count();
                Log("scan: %s answered %zu classes in %lld ms", candidate.path.c_str(),
                    classes.size(), static_cast<long long>(spent));
                ++opened;
                // Stored even when it found nothing, so a module that cannot answer is asked
                // once rather than on every start.
                if (stamped) {
                    CachedModule entry;
                    entry.size = size;
                    entry.mtime = mtime;
                    entry.expected = classes.size();
                    entry.classes = classes;
                    entry.categories = declared;
                    ScanCache()[candidate.path] = entry;
                }
            }
        }

        if (classes.size() > 1) {
            bool configured = false;
            const std::vector<std::string>& patterns = ShellAllowList(configured);
            int taken = 0;
            for (size_t at = 0; at < classes.size(); ++at) {
                const std::string& class_name = classes[at];
                if (configured) {
                    if (!ShellClassAllowed(class_name, patterns)) {
                        continue;
                    }
                } else if (taken > 0) {
                    break;  // no filter: the first class only, never 718 menu entries
                }
                std::string entry = class_name;
                if (seen.find(entry) != seen.end()) {
                    char suffix[32];
                    std::snprintf(suffix, sizeof(suffix), " (%d)", ++seen[entry]);
                    entry += suffix;
                }
                ++seen[entry];
                ScannedPlugin plugin;
                plugin.path = candidate.path;
                plugin.name = entry;
                plugin.key = entry + kEffectIdSuffix;
                plugin.class_name = class_name;
                plugin.category = at < declared.size() ? declared[at] : std::string();
                plugin.format = candidate.format;
                g_plugins.push_back(plugin);
                ++taken;
            }
            Log("scan: %s is a shell with %zu plugins, %d listed", candidate.path.c_str(),
                classes.size(), taken);
            continue;
        }

        ++seen[name];
        ScannedPlugin plugin;
        plugin.path = candidate.path;
        plugin.name = name;
        plugin.key = name + kEffectIdSuffix;
        plugin.category = declared.empty() ? std::string() : declared.front();
        plugin.format = candidate.format;
        g_plugins.push_back(plugin);
    }

    std::sort(g_plugins.begin(), g_plugins.end(),
              [](const ScannedPlugin& a, const ScannedPlugin& b) { return a.name < b.name; });

    ScanCacheStore();
    Log("scan: %d modules answered from the cache, %d had to be opened, %d denied, %d skipped "
        "after stopping a previous start",
        from_cache, opened, denied, stopped);
    Log("scan: %zu plugins will be listed", g_plugins.size());
    for (const ScannedPlugin& plugin : g_plugins) {
        Log("scan:   %-5s %s", FormatName(plugin.format), plugin.path.c_str());
    }
    for (size_t index = 0; index < sizeof(formats) / sizeof(formats[0]); ++index) {
        if (skipped[index] > 0) {
            Log("scan: %d %s plugins found but not listed - see FXBRIDGE_SCAN_FORMATS",
                skipped[index], FormatName(formats[index]));
        }
    }
}

}  // namespace

const std::vector<ScannedPlugin>& ScannedPlugins()
{
    if (!g_scanned) {
        g_scanned = true;
        Scan();
    }
    return g_plugins;
}

void PluginScanSetLogger(void (*logger)(const char*))
{
    g_logger = logger;
}
