#include "plugin_scan.h"

#include "vst3_plugin.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sys/stat.h>
#include <dirent.h>

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
    for (const Candidate& candidate : candidates) {
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
            Vst3ListClasses(candidate.path.c_str(), classes, &declared);
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
