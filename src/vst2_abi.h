// The VST2 ABI, written out rather than included.
//
// Steinberg withdrew the VST2 SDK, so there is no header to depend on. The layout below is the
// published AEffect struct, which is fixed for every VST2 plugin ever shipped: the fields are read
// by binaries we do not control, so the order, the sizes and the padding are all load-bearing.
// Nothing here may be reordered or "tidied".
#ifndef FXBRIDGE_VST2_ABI_H
#define FXBRIDGE_VST2_ABI_H

#include <cstdint>

extern "C" {

struct AEffect;

// The plugin calls this to ask the host things. Opcode list below.
using AudioMasterCallback = intptr_t (*)(AEffect* effect, int32_t opcode, int32_t index,
                                         intptr_t value, void* ptr, float opt);

using AEffectDispatcher = intptr_t (*)(AEffect* effect, int32_t opcode, int32_t index,
                                       intptr_t value, void* ptr, float opt);
using AEffectProcess = void (*)(AEffect* effect, float** inputs, float** outputs,
                                int32_t sample_frames);
using AEffectSetParameter = void (*)(AEffect* effect, int32_t index, float value);
using AEffectGetParameter = float (*)(AEffect* effect, int32_t index);

struct AEffect {
    int32_t magic;                 // 'VstP', below
    AEffectDispatcher dispatcher;
    AEffectProcess process;        // superseded by processReplacing; still present in the layout
    AEffectSetParameter setParameter;
    AEffectGetParameter getParameter;
    int32_t numPrograms;
    int32_t numParams;
    int32_t numInputs;
    int32_t numOutputs;
    int32_t flags;
    intptr_t resvd1;
    intptr_t resvd2;
    int32_t initialDelay;          // latency in samples
    int32_t realQualities;         // unused since VST 2.4
    int32_t offQualities;          // unused since VST 2.4
    float ioRatio;                 // unused since VST 2.4
    void* object;
    void* user;                    // the host's to use; we keep our instance here
    int32_t uniqueID;
    int32_t version;
    AEffectProcess processReplacing;
    void (*processDoubleReplacing)(AEffect*, double**, double**, int32_t);
    char future[56];
};

// 'VstP' - the first field of every VST2 plugin, and the cheapest way to know a library really is
// one before calling anything through it.
constexpr int32_t kEffectMagic = 0x56737450;

// Flags.
constexpr int32_t kEffFlagsHasEditor = 1 << 0;
constexpr int32_t kEffFlagsCanReplacing = 1 << 4;
constexpr int32_t kEffFlagsIsSynth = 1 << 8;

// Opcodes the host sends to the plugin. Only what a host must send to run an effect.
constexpr int32_t kEffOpen = 0;
constexpr int32_t kEffClose = 1;
constexpr int32_t kEffSetProgram = 2;
constexpr int32_t kEffGetParamName = 8;
constexpr int32_t kEffSetSampleRate = 10;
constexpr int32_t kEffSetBlockSize = 11;
constexpr int32_t kEffMainsChanged = 12;   // value 1 = resume, 0 = suspend
constexpr int32_t kEffEditGetRect = 13;
constexpr int32_t kEffEditOpen = 14;       // ptr = the native parent window handle
constexpr int32_t kEffEditClose = 15;
constexpr int32_t kEffEditIdle = 19;
constexpr int32_t kEffGetEffectName = 45;
constexpr int32_t kEffGetVendorString = 47;
constexpr int32_t kEffGetProductString = 48;
constexpr int32_t kEffCanDo = 51;
constexpr int32_t kEffGetVstVersion = 58;

// Opcodes the plugin sends to the host.
constexpr int32_t kAudioMasterVersion = 1;
constexpr int32_t kAudioMasterCurrentId = 2;
constexpr int32_t kAudioMasterIdle = 3;
constexpr int32_t kAudioMasterGetTime = 7;
constexpr int32_t kAudioMasterSizeWindow = 15;
constexpr int32_t kAudioMasterGetSampleRate = 16;
constexpr int32_t kAudioMasterGetBlockSize = 17;
constexpr int32_t kAudioMasterGetCurrentProcessLevel = 23;
constexpr int32_t kAudioMasterGetVendorString = 32;
constexpr int32_t kAudioMasterGetProductString = 33;
constexpr int32_t kAudioMasterGetVendorVersion = 34;
constexpr int32_t kAudioMasterCanDo = 37;
constexpr int32_t kAudioMasterUpdateDisplay = 42;
constexpr int32_t kAudioMasterBeginEdit = 43;
constexpr int32_t kAudioMasterEndEdit = 44;

// The rectangle effEditGetRect answers with.
struct ERect {
    int16_t top;
    int16_t left;
    int16_t bottom;
    int16_t right;
};

// Every VST2 library exports one of these. "main" is the older spelling and plenty of Linux
// plugins still use only that one.
using VstPluginMain = AEffect* (*)(AudioMasterCallback host);

}  // extern "C"

#endif
