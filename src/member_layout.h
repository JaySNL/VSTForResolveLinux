// Where the stock effect keeps the members this bridge reads and writes.
//
// THIS IS THE FIXED-OFFSET BUILD, v0.3.1.1, AND IT IS FOR DAVINCI RESOLVE 21.1.2 ONLY.
//
// v0.3.1 reads these offsets at load, out of the instructions of the Resolve functions that own
// them. Microsoft Defender's machine-learning engine flags that build
// (Trojan:Script/Wacatac.C!ml - a false positive, reported). This build carries the same nine
// numbers as constants instead, measured on 21.1.2.0010, and contains none of the code that
// reads Resolve's instructions.
//
// The cost is the one v0.3.0.1 had: the numbers are right for exactly one Resolve. Between 21.1.0
// and 21.1.2 every one of these moved by eight bytes, and on any Resolve other than 21.1.2 this
// build reads and writes at the wrong place - the panel step writes through one of them. Resolve
// 21.1.0 and earlier: use v0.3.0.1. Anything newer than 21.1.2: use v0.3.1 or later.
#ifndef FXBRIDGE_MEMBER_LAYOUT_H
#define FXBRIDGE_MEMBER_LAYOUT_H

#include <cstddef>

struct MemberLayout {
    // BMDAudioPluginImpl::UpdateChannelCount, from the primary object.
    size_t input_channels = 0;
    size_t output_channels = 0;
    // BMDAudioPluginImpl::Bypass(bool).
    size_t bypass_flag = 0;
    // AudioPlugin::SetDirty(bool), from the AudioPlugin sub-object.
    size_t dirty_flag = 0;
    size_t parent = 0;
    // BMDAudioPlugin<BMDStereoDelay>::GetEffectName.
    size_t name_record = 0;
    // BMDStereoDelay::GenerateUserInterface.
    size_t resource_tree = 0;
    size_t panel_width = 0;
    size_t panel_height = 0;
};

// Fills `out` with the 21.1.2.0010 offsets and logs them. Always succeeds; `handle` is unused.
bool ReadMemberLayout(void* handle, MemberLayout* out, void (*log)(const char*, ...));

#endif
