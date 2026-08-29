// fxbridge-scan - build the plugin cache before Resolve is started, and show it happening.
//
// Why this exists as a separate program:
//
//   1. The scan is slow exactly once, and the once used to happen inside Resolve's splash screen.
//      Opening a Windows VST3 through yabridge starts a Wine host, which costs about a third of a
//      second here and one to three seconds on a tester's machine. Multiplied by a few hundred
//      plugins that is minutes of an application that looks hung, with nothing to read and nothing
//      to do. Here it is minutes of a progress bar, which is the same wait and a different
//      experience.
//
//   2. Reading a plugin's name runs that plugin's code. A plugin that faults while being read
//      takes the host process down with it. When the host process is this program, that costs a
//      restart of a command; when it is Resolve, it costs a session. The scan learns the same
//      thing either way, and the cache it leaves behind is read by the bridge inside Resolve.
//
//   3. It can be interrupted. The cache is written after every module, so Ctrl-C is safe and the
//      next run continues where this one stopped.
//
// It writes exactly what the bridge writes, to the same place, because it is the same code.

#include "plugin_scan.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;

Clock::time_point g_started;
bool g_verbose = false;
bool g_tty = false;
int g_drawn = 0;

// Whole minutes and seconds. A scan of a large collection is measured in minutes and nobody wants
// three decimal places on an estimate.
std::string Duration(long long seconds)
{
    char text[32];
    if (seconds >= 60) {
        std::snprintf(text, sizeof(text), "%lldm%02llds", seconds / 60, seconds % 60);
    } else {
        std::snprintf(text, sizeof(text), "%llds", seconds);
    }
    return text;
}

// The bundle name without its directory or its extension. The full path is what goes in the log;
// on one terminal line it is noise that pushes out the part that changes.
std::string ShortName(const char* path)
{
    if (path == nullptr) {
        return std::string();
    }
    std::string name = path;
    const size_t slash = name.find_last_of('/');
    if (slash != std::string::npos) {
        name = name.substr(slash + 1);
    }
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        name = name.substr(0, dot);
    }
    if (name.size() > 34) {
        name = name.substr(0, 33) + "…";
    }
    return name;
}

void Progress(int done, int total, const char* path)
{
    if (total <= 0) {
        return;
    }
    if (done == 0 && path == nullptr) {
        g_started = Clock::now();
        std::printf("%d plugin modules to read. This happens once - after it, Resolve starts on\n"
                    "the cache and opens nothing. Ctrl-C is safe; the next run continues here.\n\n",
                    total);
        std::fflush(stdout);
        return;
    }

    const long long elapsed =
        std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - g_started).count();
    const int percent = done * 100 / total;

    // Only meaningful once something has finished, and only honest as a rough figure: the modules
    // are not equally slow. A native Linux plugin answers in under a millisecond and a Windows one
    // takes seconds, so the estimate moves about early and settles as the run goes on.
    std::string left = "done";
    if (done > 0 && done < total) {
        left = "~" + Duration(elapsed * (total - done) / done) + " left";
    }

    char bar[25];
    const int filled = done * 24 / total;
    for (int i = 0; i < 24; ++i) {
        bar[i] = i < filled ? '#' : '.';
    }
    bar[24] = '\0';

    char line[256];
    const int width =
        std::snprintf(line, sizeof(line), "  [%s] %3d%%  %d/%d  %s elapsed  %s  %s", bar,
                      percent, done, total, Duration(elapsed).c_str(), left.c_str(),
                      ShortName(path).c_str());

    if (g_tty) {
        // Redrawn in place. The previous line is painted over rather than cleared with an escape
        // sequence, so this behaves the same in a terminal that does not speak ANSI.
        std::printf("\r%s", line);
        for (int i = width; i < g_drawn; ++i) {
            std::putchar(' ');
        }
        g_drawn = width;
        std::fflush(stdout);
    } else if (done == total || done % 25 == 0) {
        // Piped into a file: one line every twenty-five modules, and one at the end. A carriage
        // return per module would make a log file that is one very long line.
        std::printf("%s\n", line);
        std::fflush(stdout);
    }
}

// Ctrl-C is a person, not a plugin.
//
// The scan writes down which modules are open so that a run which never comes back can name what
// stopped it. A run stopped on purpose would leave the same evidence and blame whichever eight
// modules happened to be open, so the note is deleted on the way out. unlink is safe to call from
// a signal handler; building the path is not, which is why it is taken before the handler is
// installed.
char g_in_flight[512];

extern "C" void Stopped(int)
{
    if (g_in_flight[0] != '\0') {
        ::unlink(g_in_flight);
    }
    // Not exit(): the scan is inside threads that are inside a plugin's own code, and running
    // static destructors underneath them is a worse ending than this one.
    ::_exit(130);
}

void Logger(const char* line)
{
    if (g_verbose) {
        std::printf("%s\n", line);
        std::fflush(stdout);
        return;
    }
    // Without --verbose, the per-module chatter is what the progress line already says. Everything
    // else is a decision the run made and has to be visible: what it skipped, and why.
    if (std::strncmp(line, "scan: opening ", 14) == 0) {
        return;
    }
    if (std::strstr(line, " answered ") != nullptr) {
        return;
    }
    // The final roll-call: one line per plugin found, which on a large collection is several
    // hundred lines of things that went right. The count says the same thing.
    if (std::strncmp(line, "scan:   ", 8) == 0) {
        return;
    }
    if (g_tty && g_drawn > 0) {
        std::printf("\r");
        for (int i = 0; i < g_drawn; ++i) {
            std::putchar(' ');
        }
        std::printf("\r");
        g_drawn = 0;
    }
    std::printf("%s\n", line);
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--verbose" || arg == "-v") {
            g_verbose = true;
        } else if (arg == "--help" || arg == "-h") {
            std::printf(
                "fxbridge-scan - read every plugin once and write the cache Resolve then uses.\n"
                "\n"
                "  --verbose   every line the scan logs, rather than a progress bar\n"
                "  --help      this\n"
                "\n"
                "The cache, the deny list and the skip list all live in\n"
                "~/.local/share/BMDAudioPlugins/. Ctrl-C is safe: the cache is written after\n"
                "every module and the next run continues where this one stopped.\n"
                "\n"
                "FXBRIDGE_SCAN_THREADS sets how many modules are opened at once (default 8).\n");
            return 0;
        } else {
            std::fprintf(stderr, "fxbridge-scan: unknown option %s\n", arg.c_str());
            return 2;
        }
    }

    g_tty = isatty(STDOUT_FILENO) != 0;

    // The plugins talk too, and not to us.
    //
    // yabridge writes its own diagnostics straight to stderr - one paragraph per bundle that holds
    // no Windows module, which is what a native Linux VST3 in the yabridge directory looks like.
    // Those land in the middle of the progress line and tear it in half. They are not noise to be
    // thrown away, though: if a plugin faults while it is being read, its last words are in there.
    // So stderr goes to a file and the file is named up front.
    std::string errors;
    if (!g_verbose) {
        if (const char* const home = std::getenv("HOME")) {
            errors = std::string(home) + "/.local/share/BMDAudioPlugins/fxbridge-scan-errors.log";
            const int fd = ::open(errors.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                ::dup2(fd, STDERR_FILENO);
                ::close(fd);
            } else {
                errors.clear();
            }
        }
    }
    if (!errors.empty()) {
        std::printf("Anything the plugins themselves print goes to\n  %s\n\n", errors.c_str());
    }

    std::snprintf(g_in_flight, sizeof(g_in_flight), "%s", PluginScanInFlightPath());
    std::signal(SIGINT, Stopped);
    std::signal(SIGTERM, Stopped);

    g_started = Clock::now();
    PluginScanSetLogger(Logger);
    PluginScanSetProgress(Progress);

    const size_t found = ScannedPlugins().size();
    const long long elapsed =
        std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - g_started).count();
    if (g_tty && g_drawn > 0) {
        std::printf("\n");
    }
    std::printf("\n%zu plugins ready, in %s. Resolve reads this cache and opens nothing.\n", found,
                Duration(elapsed).c_str());
    return 0;
}

// The bridge's editor callbacks, which this program never reaches.
//
// vst3_plugin.cpp and plugin_window.cpp call back into proxy.cpp when a plugin's own window opens,
// closes or moves a control. proxy.cpp is the library Resolve loads and has no place in a command,
// so those four symbols are defined here instead. Nothing in a scan opens an editor: the scan asks
// a factory for its class names and closes the module again.
struct HostedPlugin;
extern "C" void BridgeEditorWasClosedByUser(unsigned long) {}
extern "C" void BridgeArmEditorTrace() {}
extern "C" void BridgeEditorReassert() {}
void BridgeParameterChangedByEditor(HostedPlugin*, unsigned int) {}
