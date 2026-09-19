// The LV2 build's shared vocabulary: the URIs, the port table, and the two
// wrapper-level messages that exist only because LV2 has no setComponentState.
//
// WHAT THE LV2 BUILD IS. It is not a second plug-in. NAMix.so instantiates the
// SAME NamProcessor a VST3 host does and drives it through IAudioProcessor;
// NAMix_ui.so instantiates the SAME NamController and NamEditorView. Everything
// in this directory is the adapter between LV2's callbacks and the VST3 objects
// underneath — a host, in other words, of exactly the shape standalone/ already
// is, minus JACK and plus LV2.
//
// That is a deliberate choice over porting the DSP and the panel a second time.
// A parallel LV2 processor would be a second copy of the chain order, the
// output-mode logic and the state format, and every claim this plug-in carries
// would then be a claim about one of the two copies. This way there is one DSP
// path, one editor, and one state format.
//
// THE PORT TABLE. The control ports are the plug-in's own parameters minus the
// feedback block at 200.., which is processor -> editor and becomes control
// OUTPUT ports instead. The list below is checked against the controller's own
// declarations by tools/namix_ttlgen.cpp, which refuses to write a TTL if the
// two disagree — so they cannot drift even though only one of them is a list.

#pragma once

#include "namids.h"
#include "version.h"

#include <cmath>
#include <cstdint>

namespace NAMix
{
namespace lv2
{

// The plug-in's identity. The UI is a fragment of it, which keeps one
// repository URL answering for both.
inline constexpr const char *kPluginUri = "https://github.com/rations/NAMix";
inline constexpr const char *kUiUri = "https://github.com/rations/NAMix#ui";

// --- what a host puts on the screen ---------------------------------------
//
// THE VERSION IS NOT COSMETIC. lv2core.ttl's own documentation is explicit:
// "Releases of plugins and extensions MUST be explicitly versioned", and "an
// odd minor or micro version, OR MINOR VERSION ZERO, indicates that the
// resource is a development version... Where feasible, hosts SHOULD NOT expose
// such plugins to users by default." A bundle that declares neither is read as
// 0.0, which is precisely that case — so it would be asking to be hidden.
//
// LV2 has no major version, only these two, so the mapping from a three-part
// project version is a decision rather than an arithmetic. It is made once here
// and CHECKED: the asserts below refuse to build rather than let a version be
// carried across that would claim something untrue.
//
// THE MAPPING IS DOUBLING, AND THE PROJECT'S OWN VERSION IS NOT CONSTRAINED BY
// IT. Using the project's numbers directly would quietly make LV2's convention
// into a rule about the product: 0.7.0 would not build, because 7 is odd. That
// is backwards. A project numbers its releases; LV2 numbers a BUNDLE, and what
// its numbers describe is port-table compatibility and release status, not a
// product. So the project version is whatever the project says, and the
// bundle's is derived from it by doubling: even by construction, monotonically
// increasing with the project's own numbering, and needing no second list for
// anyone to keep in step.
inline constexpr int kLv2MinorVersion = 2 * SUB_VERSION_INT;
inline constexpr int kLv2MicroVersion = 2 * RELEASE_NUMBER_INT;
static_assert(MAJOR_VERSION_INT == 0,
              "LV2 has no major version. Decide how the project's MAJOR maps onto "
              "lv2:minorVersion before releasing 1.0, and keep lv2:minorVersion monotonically "
              "increasing across releases - a host loads only the highest it can see.");
static_assert(kLv2MinorVersion != 0,
              "lv2:minorVersion 0 declares a pre-release plug-in that hosts are told not to show "
              "by default. Give this release an even, non-zero minor version.");
static_assert(kLv2MinorVersion % 2 == 0 && kLv2MicroVersion % 2 == 0,
              "LV2 reads an odd lv2:minorVersion or lv2:microVersion as a development build that "
              "hosts SHOULD NOT expose by default. The doubling above makes that impossible, so "
              "this fires only if the mapping itself was changed - which is a decision to make "
              "deliberately, not a number to adjust.");

// The VST3 files the plug-in under stringSubCategory ("Fx|Distortion"). LV2's
// taxonomy is a class rather than a string, and this is that category's name in
// it (verified against lv2core.ttl, where lv2:DistortionPlugin is "A plugin
// that adds distortion to its input"). The pair is written together so that
// changing one without the other is visible; namix_lv2check reads the class
// back out of the installed bundle and compares it with this.
inline constexpr const char *kLv2PluginClass = "lv2:DistortionPlugin";
inline constexpr const char *kLv2PluginClassUri = "http://lv2plug.in/ns/lv2core#DistortionPlugin";

// --- the wrapper's own atom vocabulary ------------------------------------
//
// One object type carries every VST3 IMessage in both directions. It is a
// TUNNEL rather than a set of lv2:Parameters reached by patch:Set, and that is
// a decision with a reason on each side.
//
// Against patch: the message types are entirely internal traffic between two
// halves of one plug-in that share a source tree, so the only thing idiomatic
// patch parameters would add is HOST introspection — a generic UI able to set
// the model path with no editor open. An lv2:Parameter URI is a permanent
// commitment to a name, and nothing has asked for it here yet. Adding them
// later is purely additive and breaks nothing.
//
// For the tunnel: it is generic over whatever the two halves put in a message,
// so a fifth message type costs nothing and cannot be forgotten on one side.
// Every attribute the sender wrote is carried, in order, with its type.
inline constexpr const char *kAtomMessageUri = "https://github.com/rations/NAMix#message";
// The message's VST3 id string, e.g. "NAMLoadModel".
inline constexpr const char *kAtomMessageIdUri = "https://github.com/rations/NAMix#msgId";
// A Tuple of [key (String), type tag (Int), value] triples. A Tuple rather than
// an Object because VST3 attribute keys are arbitrary strings rather than URIs,
// and inventing a URI per attribute would put back the drift the tunnel exists
// to remove.
inline constexpr const char *kAtomMessageBodyUri = "https://github.com/rations/NAMix#msgBody";

// --- the two messages that exist only in this build ------------------------
//
// A VST3 host calls IEditController::setComponentState so the editor's mirror
// of the model and IR paths agrees with the processor's. LV2 has no such call:
// the UI and the DSP are separate shared objects that a host may not even load
// into the same process. So the UI asks, and the DSP wrapper answers out of the
// processor's own getState — the same blob, read by the same reader, so there
// is no second state format and nothing to keep in step.
//
// Neither id ever reaches NamProcessor::notify: they are served by the wrapper,
// which holds the processor pointer and needs nothing from it that
// IAudioProcessor does not already offer.
inline constexpr const char *kMsgLv2RequestState = "NAMixLv2RequestState";
inline constexpr const char *kMsgLv2State = "NAMixLv2State";
inline constexpr const char *kLv2StateAttr = "blob";

// --- ports -----------------------------------------------------------------

enum PortIndex : std::uint32_t {
    kPortAudioIn = 0,
    kPortAudioOutL = 1,
    kPortAudioOutR = 2,
    // The UI's messages to the DSP.
    kPortAtomIn = 3,
    // The DSP's replies to the UI.
    kPortAtomOut = 4,
    kPortControlFirst = 5,
};

// The control inputs, in the controller's own declaration order so that a
// reader can hold this list and src/namcontroller.cpp's initialize() side by
// side.
//
// NOT hand-counted and not trusted: tools/namix_ttlgen.cpp instantiates the
// controller and asserts that this list is exactly the set of parameters the
// controller declares outside the feedback block. A parameter added to the
// plug-in and forgotten here fails the TTL build rather than becoming a control
// no LV2 host can reach.
inline constexpr Steinberg::Vst::ParamID kControlIds[] = {
    kBypassId,
    kInputGainId,
    kOutputGainId,
    kNoiseGateThresholdId,

    kBassId,
    kMiddleId,
    kTrebleId,

    kToneStackOnId,
    kNoiseGateOnId,

    kOutputModeId,
    kSlimId,
    kCalibrateInputId,
    kInputCalibrationLevelId,
};
inline constexpr int kControlInCount =
    static_cast<int>(sizeof(kControlIds) / sizeof(kControlIds[0]));

// The feedback block, as control OUTPUT ports. Every one of these needs a
// ui:portNotification block in the TTL or it never reaches the UI: the UI
// extension says a host calls port_event() for control port INPUTS by default,
// so without those declarations both meters are permanently dead while the
// plug-in builds, loads and otherwise works perfectly.
inline constexpr Steinberg::Vst::ParamID kFeedbackIds[] = {
    kInputMeterId,
    kOutputMeterId,
};
inline constexpr int kFeedbackCount =
    static_cast<int>(sizeof(kFeedbackIds) / sizeof(kFeedbackIds[0]));

inline constexpr std::uint32_t kPortFeedbackFirst = kPortControlFirst + kControlInCount;
// lv2:reportsLatency. The VST3 build reports latency through
// getLatencySamples(); LV2's way of saying the same thing is a control output
// port carrying the lv2:latency designation, so the figure is the same figure
// by construction — the wrapper copies it straight out of the processor.
inline constexpr std::uint32_t kPortLatency = kPortFeedbackFirst + kFeedbackCount;
inline constexpr std::uint32_t kPortCount = kPortLatency + 1;

// The ParamID a control-input port carries. Out of range returns 0, which is
// not a ParamID this plug-in uses.
inline constexpr Steinberg::Vst::ParamID controlPortParam(int index)
{
    if (index < 0 || index >= kControlInCount)
        return 0;
    return kControlIds[index];
}

// --- what a control port carries -------------------------------------------
//
// PLAIN values, not normalized ones: an LV2 host draws its generic control from
// the port's own lv2:minimum / lv2:maximum and prints the number it holds, so a
// port carrying 0.63 would put "0.63" where the panel says "+10.4 dB". The VST3
// build's automation lane says the real figure and this one has to as well.
//
// That means the wrapper needs each parameter's range, and this is the table
// that gives it. Every bound below is written as the SAME constant the
// controller declares the parameter from, so there is no second copy of a
// number — only a second statement of WHICH range each id uses. That statement
// is checked: tools/namix_ttlgen.cpp instantiates the controller and compares
// every row here against Parameter::toPlain at 0 and 1, its stepCount and its
// default, and refuses to write a TTL when they disagree.
struct ControlSpec {
    Steinberg::Vst::ParamID id;
    double min;
    double max;
    // VST3's stepCount: 0 for a continuous control, N for one with N+1 discrete
    // values. A toggle is 1, the three-way output mode is 2.
    int steps;
    double def; // plain
};

inline constexpr ControlSpec kControlSpecs[kControlInCount] = {
    {kBypassId, 0.0, 1.0, 1, 0.0},
    {kInputGainId, ranges::kGainMin, ranges::kGainMax, 0, ranges::kGainDefault},
    {kOutputGainId, ranges::kGainMin, ranges::kGainMax, 0, ranges::kGainDefault},
    {kNoiseGateThresholdId, ranges::kNgMin, ranges::kNgMax, 0, ranges::kNgDefault},

    {kBassId, ranges::kToneMin, ranges::kToneMax, 0, ranges::kToneDefault},
    {kMiddleId, ranges::kToneMin, ranges::kToneMax, 0, ranges::kToneDefault},
    {kTrebleId, ranges::kToneMin, ranges::kToneMax, 0, ranges::kToneDefault},

    {kToneStackOnId, 0.0, 1.0, 1, 1.0},
    {kNoiseGateOnId, 0.0, 1.0, 1, 1.0},

    // Index 1 of Raw / Normalized / Calibrated. The default is stated here as a
    // plain index and in namcontroller.cpp as a normalized 0.5; they are the
    // same number and namix_ttlgen is what says so.
    {kOutputModeId, 0.0, kOutputModeCount - 1, kOutputModeCount - 1, 1.0},
    {kSlimId, 0.0, 1.0, 0, 0.0},
    {kCalibrateInputId, 0.0, 1.0, 1, 0.0},
    {kInputCalibrationLevelId, ranges::kCalMin, ranges::kCalMax, 0, ranges::kCalDefault},
};

inline constexpr ControlSpec controlSpec(int port)
{
    if (port < 0 || port >= kControlInCount)
        return ControlSpec{0, 0.0, 1.0, 0, 0.0};
    return kControlSpecs[port];
}

// Plain -> normalized, which is what every VST3 parameter queue carries.
// Clamped rather than trusted: a control port is host input and a host may
// write anything into it.
inline double controlNorm(const ControlSpec &spec, double plain)
{
    const double span = spec.max - spec.min;
    if (span <= 0.0)
        return 0.0;
    double n = (plain - spec.min) / span;
    return n < 0.0 ? 0.0 : (n > 1.0 ? 1.0 : n);
}

// Normalized -> plain, the inverse, with a stepped control snapped to a whole
// step so a host that reads the port back sees the value it would have set.
inline double controlPlain(const ControlSpec &spec, double norm)
{
    norm = norm < 0.0 ? 0.0 : (norm > 1.0 ? 1.0 : norm);
    const double plain = spec.min + norm * (spec.max - spec.min);
    if (spec.steps <= 0)
        return plain;
    const double step = (spec.max - spec.min) / static_cast<double>(spec.steps);
    if (step <= 0.0)
        return spec.min;
    return spec.min + std::floor((plain - spec.min) / step + 0.5) * step;
}

// --- state -----------------------------------------------------------------
//
// The VST3 state blob is stored whole, under one key, and it is the authority
// for everything: the thirteen controls, the output section, and the model and
// IR paths. There is no second state format, so a project saved by either build
// is read by the same reader that already handles three state versions.
//
// state:mapPath is layered ON TOP of that rather than replacing it, because it
// is the one thing the VST3 format cannot do: a host that moves or archives a
// session can relocate the files a plug-in references, but only for paths the
// plug-in handed it through abstract_path(). So each file path is stored twice
// — once abstracted, which is what survives a move, and once raw, which is what
// the blob itself contains.
//
// The raw copy is not redundant. At restore the wrapper has to answer one
// question — did the files move? — and the blob is the only other place the
// answer lives. Parsing it here to find out would mean a second reader for a
// format that already has one, which is exactly the drift this file keeps
// warning about; storing the raw path costs a few dozen bytes and answers it
// outright. When abstract and raw resolve to the same file, restore is the blob
// alone. When they differ, the wrapper re-sends the load for the relocated
// path.
inline constexpr const char *kStateBlobUri = "https://github.com/rations/NAMix#state";
// <prefix>Abstract / <prefix>Raw per slot, with the slot's own name appended.
inline constexpr const char *kStatePathAbstractPrefix =
    "https://github.com/rations/NAMix#pathAbstract";
inline constexpr const char *kStatePathRawPrefix = "https://github.com/rations/NAMix#pathRaw";

// The path slots, in one order shared by save and restore — the same two paths,
// in the same order, that NamProcessor::getState writes at the end of the blob.
enum PathSlot : int {
    kPathSlotModel = 0,
    kPathSlotIr = 1,
    kPathSlotCount = 2,
};
inline constexpr const char *kPathSlotName[kPathSlotCount] = {"Model", "Ir"};

} // namespace lv2
} // namespace NAMix
