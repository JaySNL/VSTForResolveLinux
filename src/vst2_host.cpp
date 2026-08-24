#include "vst2_host.h"

#include "plugin_window.h"
#include "vst2_abi.h"

#include <dlfcn.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

void (*g_logger)(const char*) = nullptr;

void Log(const char* format, ...) __attribute__((format(printf, 1, 2)));

void Log(const char* format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (g_logger != nullptr) {
        g_logger(line);
    }
}

void* g_library = nullptr;
AEffect* g_effect = nullptr;
bool g_ready = false;
bool g_editor_open = false;
double g_sample_rate = 48000.0;
uint32_t g_max_frames = 8192;
uint32_t g_channel_count = 0;
char g_name[128] = {0};

// VST2 processReplacing may not read and write the same buffer, so the input is copied aside.
std::vector<float>& InputScratch()
{
    static std::vector<float> scratch;
    return scratch;
}

// What the plugin is allowed to ask us. Answering "I do not know" to an unknown opcode is correct
// and expected; refusing to answer the known ones is what makes plugins misbehave.
intptr_t HostCallback(AEffect* effect, int32_t opcode, int32_t index, intptr_t value, void* ptr,
                      float opt)
{
    (void)effect;
    (void)index;
    (void)value;
    (void)opt;

    switch (opcode) {
        case kAudioMasterVersion:
            return 2400;  // VST 2.4
        case kAudioMasterCurrentId:
            return g_effect != nullptr ? g_effect->uniqueID : 0;
        case kAudioMasterGetSampleRate:
            return static_cast<intptr_t>(g_sample_rate);
        case kAudioMasterGetBlockSize:
            return static_cast<intptr_t>(g_max_frames);
        case kAudioMasterGetCurrentProcessLevel:
            return 2;  // realtime
        case kAudioMasterGetVendorString:
            if (ptr != nullptr) {
                std::snprintf(static_cast<char*>(ptr), 64, "%s", "Jay's Desk");
            }
            return 1;
        case kAudioMasterGetProductString:
            if (ptr != nullptr) {
                std::snprintf(static_cast<char*>(ptr), 64, "%s", "Resolve FX Bridge");
            }
            return 1;
        case kAudioMasterGetVendorVersion:
            return 1;
        case kAudioMasterCanDo:
            // Only what is actually true. Claiming a capability we do not implement is worse than
            // saying no: the plugin will use it.
            if (ptr != nullptr) {
                const char* const what = static_cast<const char*>(ptr);
                if (std::strcmp(what, "sizeWindow") == 0 ||
                    std::strcmp(what, "supplyIdle") == 0) {
                    return 1;
                }
            }
            return 0;
        case kAudioMasterSizeWindow:
        case kAudioMasterUpdateDisplay:
        case kAudioMasterIdle:
        case kAudioMasterBeginEdit:
        case kAudioMasterEndEdit:
            return 1;
        default:
            return 0;
    }
}

intptr_t Dispatch(int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt)
{
    if (g_effect == nullptr || g_effect->dispatcher == nullptr) {
        return 0;
    }
    return g_effect->dispatcher(g_effect, opcode, index, value, ptr, opt);
}

}  // namespace

bool Vst2HostLoad(const char* path, double sample_rate, uint32_t max_frames)
{
    if (g_ready) {
        return true;
    }
    g_sample_rate = sample_rate;
    g_max_frames = max_frames;

    g_library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (g_library == nullptr) {
        Log("vst2: dlopen(%s) failed: %s", path, dlerror());
        return false;
    }

    // "VSTPluginMain" is the current name; "main" is what older Linux plugins export, and plenty
    // of them export nothing else.
    auto entry = reinterpret_cast<VstPluginMain>(dlsym(g_library, "VSTPluginMain"));
    if (entry == nullptr) {
        entry = reinterpret_cast<VstPluginMain>(dlsym(g_library, "main"));
    }
    if (entry == nullptr) {
        Log("vst2: %s exports neither VSTPluginMain nor main", path);
        return false;
    }

    g_effect = entry(HostCallback);
    if (g_effect == nullptr) {
        Log("vst2: the entry point returned no effect");
        return false;
    }
    if (g_effect->magic != kEffectMagic) {
        Log("vst2: %s is not a VST2 plugin (magic 0x%08x)", path,
            static_cast<unsigned>(g_effect->magic));
        g_effect = nullptr;
        return false;
    }
    if ((g_effect->flags & kEffFlagsCanReplacing) == 0 || g_effect->processReplacing == nullptr) {
        Log("vst2: %s has no processReplacing - too old to host", path);
        g_effect = nullptr;
        return false;
    }

    // The order matters: open, then rate and block size, then resume. A plugin that is told to
    // resume before it knows its rate will allocate for the wrong one.
    Dispatch(kEffOpen, 0, 0, nullptr, 0.0f);
    Dispatch(kEffSetSampleRate, 0, 0, nullptr, static_cast<float>(sample_rate));
    Dispatch(kEffSetBlockSize, 0, static_cast<intptr_t>(max_frames), nullptr, 0.0f);
    Dispatch(kEffMainsChanged, 0, 1, nullptr, 0.0f);

    char name[64] = {0};
    if (Dispatch(kEffGetEffectName, 0, 0, name, 0.0f) != 0 && name[0] != '\0') {
        std::snprintf(g_name, sizeof(g_name), "%s", name);
    } else {
        std::snprintf(g_name, sizeof(g_name), "%s", "VST2 plugin");
    }

    g_channel_count = g_effect->numOutputs > 0 ? static_cast<uint32_t>(g_effect->numOutputs) : 0;
    if (g_channel_count > 2) {
        g_channel_count = 2;  // Resolve hands this effect a stereo pair
    }
    if (g_channel_count == 0) {
        Log("vst2: \"%s\" reports no outputs", g_name);
        g_effect = nullptr;
        return false;
    }

    InputScratch().assign(static_cast<size_t>(g_channel_count) * g_max_frames, 0.0f);
    g_ready = true;

    Log("vst2: loaded \"%s\", %d in, %d out, %d parameters, %s editor, %.0f Hz",
        g_name, g_effect->numInputs, g_effect->numOutputs, g_effect->numParams,
        (g_effect->flags & kEffFlagsHasEditor) != 0 ? "has an" : "no", sample_rate);
    return true;
}

bool Vst2HostProcess(float** buffers, uint32_t channel_count, uint32_t frames)
{
    if (!g_ready || g_effect == nullptr || g_effect->processReplacing == nullptr ||
        buffers == nullptr || frames == 0 || frames > g_max_frames) {
        return false;
    }

    const uint32_t channels = channel_count < g_channel_count ? channel_count : g_channel_count;
    std::vector<float>& scratch = InputScratch();
    if (scratch.size() < static_cast<size_t>(channels) * g_max_frames) {
        return false;
    }

    float* input[2] = {nullptr, nullptr};
    for (uint32_t channel = 0; channel < channels && channel < 2; ++channel) {
        float* const slot = &scratch[static_cast<size_t>(channel) * g_max_frames];
        std::memcpy(slot, buffers[channel], frames * sizeof(float));
        input[channel] = slot;
    }

    g_effect->processReplacing(g_effect, input, buffers, static_cast<int32_t>(frames));
    return true;
}

uint32_t Vst2HostChannelCount() { return g_ready ? g_channel_count : 0; }

const char* Vst2HostName() { return g_name[0] != '\0' ? g_name : "VST2 plugin"; }

bool Vst2HostOpenEditor()
{
    if (!g_ready || g_effect == nullptr) {
        return false;
    }
    if ((g_effect->flags & kEffFlagsHasEditor) == 0) {
        Log("vst2: \"%s\" has no editor", g_name);
        return false;
    }

    if (g_editor_open) {
        return PluginWindowShow();
    }

    // Ask the plugin how big it wants to be before making the window, so it is right first time.
    unsigned int width = 800;
    unsigned int height = 600;
    ERect* rect = nullptr;
    if (Dispatch(kEffEditGetRect, 0, 0, &rect, 0.0f) != 0 && rect != nullptr) {
        const int rect_width = rect->right - rect->left;
        const int rect_height = rect->bottom - rect->top;
        if (rect_width > 0 && rect_height > 0) {
            width = static_cast<unsigned int>(rect_width);
            height = static_cast<unsigned int>(rect_height);
        }
    }

    const unsigned long window = PluginWindowOpen(width, height, g_name);
    if (window == 0) {
        return false;
    }

    if (Dispatch(kEffEditOpen, 0, 0, reinterpret_cast<void*>(window), 0.0f) == 0) {
        // Some plugins answer 0 and still open. The window is up either way, so this is reported
        // rather than treated as a failure.
        Log("vst2: effEditOpen answered 0 for \"%s\"", g_name);
    }
    PluginWindowFlush();
    g_editor_open = true;
    Log("vst2: editor open for \"%s\" at %ux%u", g_name, width, height);
    return true;
}

void Vst2HostCloseEditor()
{
    if (!g_ready) {
        return;
    }
    PluginWindowHide();
}

void Vst2HostSetLogger(void (*logger)(const char*))
{
    g_logger = logger;
    PluginWindowSetLogger(logger);
}
