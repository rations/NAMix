// NAMix — Neural Amp Modeler for Linux, a raw VST3 plug-in (no framework).
//
// Based on NeuralAmpModelerPlugin by Steven Atkinson (MIT licence); the DSP
// core (NeuralAmpModelerCore, AudioDSPTools) is reused directly. This plug-in
// is written against the VST3 SDK only — no plug-in framework.
//
// The class UIDs and the parameter/message identities below are deliberately
// identical to the Haiku sibling port's: the two are the same plug-in on two
// operating systems, and they write byte-compatible state, so a project saved
// with one opens with the other.

#pragma once

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/vsttypes.h"

namespace NAMix
{

// Parameter IDs. Never change these after a release — projects embed them.
enum ParamIDs : Steinberg::Vst::ParamID {
    kBypassId = 100,
    kInputGainId = 101,             // -40 .. +40 dB, default 0
    kOutputGainId = 102,            // -40 .. +40 dB, default 0
    kNoiseGateThresholdId = 103,    // -100 .. 0 dB, default -80
    kBassId = 104,                  // 0 .. 10, default 5
    kMiddleId = 105,                // 0 .. 10, default 5
    kTrebleId = 106,                // 0 .. 10, default 5
    kToneStackOnId = 107,           // toggle, default on
    kNoiseGateOnId = 108,           // toggle, default on
    kOutputModeId = 109,            // Raw / Normalized / Calibrated, default Normalized
    kSlimId = 110,                  // 0 .. 1, default 0 — slimmable (A2) models only
    kCalibrateInputId = 111,        // toggle, default off — models with input level only
    kInputCalibrationLevelId = 112, // -60 .. +60 dBu, default 12

    // Hidden, read-only meter parameters (processor -> editor via output
    // parameter changes; never automated, never persisted). Values are the
    // per-block peak level mapped to 0 .. 1 over the meter dB range below.
    kInputMeterId = 200,
    kOutputMeterId = 201,
};

// Plain-value ranges shared by the processor (denormalization) and the
// controller (RangeParameter setup). Keep the two sides in sync via these.
namespace ranges
{
inline constexpr double kGainMin = -40.0, kGainMax = 40.0, kGainDefault = 0.0;
inline constexpr double kNgMin = -100.0, kNgMax = 0.0, kNgDefault = -80.0;
inline constexpr double kToneMin = 0.0, kToneMax = 10.0, kToneDefault = 5.0;
// Input calibration level (dBu) — range and default match the original plug-in.
inline constexpr double kCalMin = -60.0, kCalMax = 60.0, kCalDefault = 12.0;
// Level-meter display range (dB): a linear peak is mapped to 0 .. 1 across
// this window before travelling to the editor. Matches the original meter.
inline constexpr double kMeterMinDb = -70.0, kMeterMaxDb = 0.0;
} // namespace ranges

// How many entries kOutputModeId has: Raw, Normalized, Calibrated. Named
// because a stepped parameter's step count is one less than its entry count,
// and the LV2 port table has to state both.
inline constexpr int kOutputModeCount = 3;

// Message IDs for controller -> processor file loading (IConnectionPoint).
// Attribute "path" carries a UTF-8 byte string (setBinary); empty = clear.
inline constexpr const char *kMsgLoadModel = "NAMLoadModel";
inline constexpr const char *kMsgLoadIr = "NAMLoadIR";
inline constexpr const char *kMsgPathAttr = "path";

// Slim travels controller -> processor as a message too (attribute "slim",
// setFloat): SetSlimmableSize rebuilds part of the network, which is
// thread-safe but NOT RT-safe, so it must run on the message thread — the
// RT parameter queue only carries the value for state bookkeeping.
inline constexpr const char *kMsgSetSlim = "NAMSetSlim";
inline constexpr const char *kSlimAttr = "slim";

// Model capabilities travel processor -> controller after every model load
// or clear (int attributes, 0/1), so the editor can disable or retitle the
// controls that the current capture does not support.
inline constexpr const char *kMsgModelCaps = "NAMModelCaps";
inline constexpr const char *kCapsLoadedAttr = "modelLoaded";
inline constexpr const char *kCapsSlimmableAttr = "slimmable";
inline constexpr const char *kCapsLoudnessAttr = "hasLoudness";
inline constexpr const char *kCapsInLevelAttr = "hasInputLevel";
inline constexpr const char *kCapsOutLevelAttr = "hasOutputLevel";

// What the caps message carries. The three metadata flags are independent
// predicates over different JSON keys, and each gates a different control:
// "loudness" gates Normalized, "output_level_dbu" gates Calibrated, and
// "input_level_dbu" gates the Calibrate Input toggle. Merging any two of
// them disables a control over metadata it never needed. `loaded` is false
// for the model-cleared path, where the other flags say nothing at all:
// a capture that is not loaded has no metadata to be missing.
struct ModelCaps {
    bool loaded = false;
    bool slimmable = false;
    bool hasLoudness = false;
    bool hasInputLevel = false;
    bool hasOutputLevel = false;
};

static DECLARE_UID(NamProcessorUID, 0x80781530, 0x12284EB4, 0x89676AE5, 0x52A4FB2B);
static DECLARE_UID(NamControllerUID, 0xFD4220E5, 0xACFD437A, 0x9318389D, 0x1DED6791);

} // namespace NAMix
