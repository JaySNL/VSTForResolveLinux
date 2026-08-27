#include "plugin_state.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

void (*g_logger)(const char*) = nullptr;

void Log(const char* message)
{
    if (g_logger != nullptr) {
        g_logger(message);
    }
}

// The directory the store writes into, created on first use. Alongside the library itself, so a
// user who removes the bridge removes its files too.
std::string StoreDirectory()
{
    const char* const home = std::getenv("HOME");
    if (home == nullptr || home[0] == '\0') {
        return std::string();
    }
    std::string directory = std::string(home) + "/.local/share/BMDAudioPlugins";
    mkdir(directory.c_str(), 0755);
    directory += "/state";
    mkdir(directory.c_str(), 0755);
    return directory;
}

// FNV-1a. Not a security hash - it is here so that two plugins whose names sanitise to the same
// text still get two files.
uint64_t Hash(const std::string& text)
{
    uint64_t value = 1469598103934665603ULL;
    for (const char character : text) {
        value ^= static_cast<unsigned char>(character);
        value *= 1099511628211ULL;
    }
    return value;
}

}  // namespace

void StateStoreSetLogger(void (*logger)(const char*)) { g_logger = logger; }

bool StateStoreEnabled()
{
    const char* const setting = std::getenv("FXBRIDGE_STATE_STORE");
    return setting != nullptr && setting[0] == '1';
}

std::string StateStoreKey(const char* path, const char* class_name)
{
    if (path == nullptr || path[0] == '\0') {
        return std::string();
    }
    std::string full = path;
    if (class_name != nullptr && class_name[0] != '\0') {
        full += "#";
        full += class_name;
    }

    // The readable half is for a human reading `ls`; the hash is what keeps it unique.
    std::string readable;
    for (const char character : full) {
        const bool plain = (character >= 'a' && character <= 'z') ||
                           (character >= 'A' && character <= 'Z') ||
                           (character >= '0' && character <= '9');
        readable += plain ? character : '_';
    }
    if (readable.size() > 64) {
        readable = readable.substr(readable.size() - 64);
    }

    char suffix[24] = {0};
    std::snprintf(suffix, sizeof(suffix), "-%016lx", static_cast<unsigned long>(Hash(full)));
    return readable + suffix;
}

bool StateStoreRead(const std::string& key, std::vector<uint8_t>& out)
{
    const std::string directory = StoreDirectory();
    if (directory.empty() || key.empty()) {
        return false;
    }
    const std::string file = directory + "/" + key + ".bin";
    std::FILE* const handle = std::fopen(file.c_str(), "rb");
    if (handle == nullptr) {
        return false;  // no saved state is the normal case, not a fault
    }
    out.clear();
    uint8_t buffer[4096];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), handle)) > 0) {
        out.insert(out.end(), buffer, buffer + read);
    }
    std::fclose(handle);
    return !out.empty();
}

// Written to a temporary name and renamed into place. A half-written state file is worse than a
// missing one: the plugin parses it as its own, and what happens next belongs to the plugin.
bool StateStoreWrite(const std::string& key, const std::vector<uint8_t>& bytes)
{
    const std::string directory = StoreDirectory();
    if (directory.empty() || key.empty() || bytes.empty()) {
        return false;
    }
    const std::string file = directory + "/" + key + ".bin";
    const std::string temporary = file + ".new";
    std::FILE* const handle = std::fopen(temporary.c_str(), "wb");
    if (handle == nullptr) {
        Log("state: cannot open the store for writing");
        return false;
    }
    const size_t written = std::fwrite(bytes.data(), 1, bytes.size(), handle);
    const bool flushed = std::fclose(handle) == 0;
    if (written != bytes.size() || !flushed) {
        std::remove(temporary.c_str());
        Log("state: the store was not written in full");
        return false;
    }
    if (std::rename(temporary.c_str(), file.c_str()) != 0) {
        std::remove(temporary.c_str());
        return false;
    }
    return true;
}
