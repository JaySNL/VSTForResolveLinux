// Proves the lock order patch against an installed Resolve, without running Resolve.
//
// It loads libBMDAudioPlugins.so, runs the same detection the bridge runs, and patches the
// vtables in this throw-away process. What it answers: are the two offsets still readable out of
// the instructions, do the two functions agree about them, and does the pointer actually appear
// in a vtable. A run that prints "not patching" is a build we leave alone.
//
//   cc -O2 -o /tmp/lockfix-check tools/lockfix-check.cpp src/chain_lock_fix.cpp -ldl
//   /tmp/lockfix-check [path to libBMDAudioPlugins.so]
#include "../src/chain_lock_fix.h"

#include <dlfcn.h>

#include <cstdarg>
#include <cstdio>

namespace {

const char* kDefaultLibrary = "/opt/resolve/libs/libBMDAudioPlugins.so";
void Report(const char* format, ...)
{
    std::va_list args;
    va_start(args, format);
    std::vfprintf(stdout, format, args);
    va_end(args);
    std::fputc('\n', stdout);
}

}  // namespace

int main(int argc, char** argv)
{
    const char* const path = argc > 1 ? argv[1] : kDefaultLibrary;
    void* const handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        std::fprintf(stderr, "dlopen(%s) failed: %s\n", path, dlerror());
        return 2;
    }
    std::printf("library: %s\n", path);
    return InstallChainLockFix(handle, Report) ? 0 : 1;
}
