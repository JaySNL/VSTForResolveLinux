# Third-party components

This project links two third-party header sets. Neither is modified, and both are pulled in as
git submodules rather than vendored.

## CLAP — `third_party/clap`

The CLever Audio Plug-in API headers, from <https://github.com/free-audio/clap>.
Licensed **MIT**. Used to host `.clap` plugins.

## travesty (part of DPF) — `third_party/dpf`

DPF's clean-room C description of the VST3 ABI, from <https://github.com/DISTRHO/DPF>, at
`distrho/src/travesty`. Licensed **ISC**. Used to host `.vst3` plugins.

travesty is used *instead of* Steinberg's VST3 SDK on purpose. It is a plain-C, independently
written description of the interfaces, which keeps this project free of the SDK entirely.

## A note on VST2

`src/vst2_abi.h` is a clean-room description of the VST2 ABI written for this project. Steinberg
withdrew the VST2 SDK from distribution and stopped issuing licences in October 2018, so no VST2
SDK header is used, included or redistributed here. This is the same approach LinVst and yabridge
take.

## Not included

Nothing from Blackmagic Design is copied, redistributed or modified on disk. This project loads
into Resolve as a plugin through Resolve's own `BMDPlugins.Path` mechanism and patches structures
in its own process memory at run time.
