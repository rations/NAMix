// NAMix editor geometry. The canvas size and palette are mirrored from
// gui/geometry.sh (the art pipeline's source of truth); the control rects are
// derived from the original Neural Amp Modeler layout (its 600x400 panel:
// window pad -20, content pad -10, a six-cell knob grid at y[105,225], two
// toggles under knobs 2 and 4, 200x30 model/IR rows near the bottom, and
// 30px-wide vertical meters on the outer edges). Keep the canvas/palette in
// sync with geometry.sh.

#pragma once

#include "namids.h"

#include "pluginterfaces/vst/vsttypes.h"

namespace NAMix
{
namespace geo
{

// Editor canvas (geometry.sh: WIN_W/WIN_H).
constexpr int kWinW = 600;
constexpr int kWinH = 400;

// Palette (geometry.sh; from the original Colors.h). 0xRRGGBB.
constexpr uint32_t kBgColor = 0x1D1A1F;   // raisin-black backdrop
constexpr uint32_t kAzure = 0x5085E8;     // theme accent
constexpr uint32_t kTextColor = 0xF2F2F2; // near-white labels
constexpr uint32_t kDimColor = 0xA2B2BF;  // empty / disabled text
constexpr uint32_t kPeakColor = 0xFF3B30; // meter peak marker

// Title band.
constexpr int kTitleX = 30, kTitleY = 30, kTitleW = 540, kTitleH = 50;

// --- Knobs (6), from the original knob grid ---
struct KnobSpec {
    Steinberg::Vst::ParamID id;
    int cx, cy, r; // face center and radius
    const char *label;
    const char *unit; // appended to the value readout (nullptr = none)
};
constexpr int kKnobCount = 6;
constexpr KnobSpec kKnobs[kKnobCount] = {
    {kInputGainId, 92, 165, 37, "Input", "dB"},               //
    {kNoiseGateThresholdId, 175, 165, 37, "Threshold", "dB"}, //
    {kBassId, 258, 165, 37, "Bass", nullptr},                 //
    {kMiddleId, 342, 165, 37, "Middle", nullptr},             //
    {kTrebleId, 425, 165, 37, "Treble", nullptr},             //
    {kOutputGainId, 508, 165, 37, "Output", "dB"},            //
};
// Knob value arc + pointer: ~270-degree sweep with a dead zone at the bottom.
// Angles in degrees, 0 = straight up, positive = clockwise (see namview.cpp).
constexpr double kKnobSweepDeg = 270.0;
constexpr double kKnobPointerFrac = 0.82; // pointer tip / radius
constexpr double kKnobArcFrac = 0.92;     // value-arc radius / knob radius
constexpr int kKnobLabelDY = 52;          // label baseline above face center
constexpr int kKnobValueDY = 54;          // value baseline below face center

// --- Toggle switches (2) ---
struct ToggleSpec {
    Steinberg::Vst::ParamID id;
    int cx, cy; // pill center
    const char *label;
};
constexpr int kToggleW = 44, kToggleH = 22;
constexpr int kToggleCount = 2;
constexpr ToggleSpec kToggles[kToggleCount] = {
    {kNoiseGateOnId, 175, 250, "Noise Gate"},
    {kToneStackOnId, 342, 250, "EQ"},
};

// --- File rows (model / IR) ---
struct FileRow {
    int x, y, w, h;
    const char *placeholder;
    const char *ext; // file-panel filter (no dot)
};
// Rows are 400 px wide, centered in the content area (x[100,500]) — the exact
// original geometry (fileWidth 200 -> 2*200 centered), matching the Linux port.
constexpr FileRow kModelRow = {100, 309, 400, 30, "Select model...", "nam"};
constexpr FileRow kIrRow = {100, 347, 400, 30, "Select IR...", "wav"};

// --- Level meters (input / output) ---
struct MeterRect {
    Steinberg::Vst::ParamID id;
    int x, y, w, h;
};
constexpr MeterRect kInputMeter = {kInputMeterId, 10, 75, 30, 200};
constexpr MeterRect kOutputMeter = {kOutputMeterId, 560, 75, 30, 200};

// --- Gear (settings) button, top-right ---
constexpr int kGearCX = 556, kGearCY = 46, kGearR = 13;

// --- Settings overlay (gear) ---
constexpr int kSettingsX = 120, kSettingsY = 66, kSettingsW = 360, kSettingsH = 268;

// --- Slim: icon in the 520 gap by the model row (slimmable only) + overlay ---
// Centered vertically on the model row (y 309..339 -> icon 314..334).
constexpr int kSlimIconX = 520, kSlimIconY = 314, kSlimIconW = 40, kSlimIconH = 20;
constexpr int kSlimKnobCX = 300, kSlimKnobCY = 186, kSlimKnobR = 42;

// --- Status icons to the left of the file rows (model amp icon / IR on-off) ---
constexpr int kRowIconCX = 74; // center x of the left-of-row status icon

} // namespace geo
} // namespace NAMix
