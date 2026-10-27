// NAMix native editor implementation.
//
// The amp-head panel of the original Neural Amp Modeler, composited from the
// plug-in's own MIT-licensed art (loaded from the bundle's
// Contents/Resources/img): a textured backdrop and line overlay, six knobs
// (face bitmap + code-drawn pointer and value arc), two toggle switches, a
// model row and an IR row (loaded through the controller's INamFileLoader),
// and input/output level meters fed by the hidden meter parameters. The ten
// icons are the original SVGs, rasterised at the size they are drawn at.
//
// Every art load is checked with a flat-colour fallback, so a missing
// Resources directory degrades rather than crashes.
//
// Threading: everything here runs on the host's run-loop thread (the
// kPlatformTypeX11EmbedWindowID contract); the controller is called on the
// same thread, so value reads/writes and ParamChanged() need no locking.

#include "namview.h"
#include "namcontroller.h"
#include "namids.h"
#include "platform/respath.h"

#include "pluginterfaces/base/keycodes.h"
#include "pluginterfaces/base/ustring.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <strings.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>
#include <vector>

using namespace Steinberg;

namespace NAMix
{

namespace fs = std::filesystem;

namespace
{
// Drag and wheel gearing, from the original plug-in's knobs (iPlug2's
// IKnobControlBase). A drag covers the full range in DEFAULT_GEARING (4.0)
// heights of the knob's drag bounds; the face diameter stands in for those
// bounds here, which upstream derives from its own label/knob/value layout.
// Holding Shift or Ctrl makes a drag ten times finer and a wheel notch a
// tenth of its usual 1% (IsFineControl: "mod.C || mod.S").
constexpr float kDragGearing = 4.0f;
constexpr float kFineDragFactor = 10.0f;
constexpr double kWheelStep = 0.01, kFineWheelStep = 0.001;
// The dBu box has no wheel in the original (it is a text field there), so its
// step is NAMix's own: whole decibels, or tenths with Shift or Ctrl.
constexpr double kCalWheelDb = 1.0, kCalFineWheelDb = 0.1;
// Long enough for "-100.0", short enough that no typed number can overflow.
constexpr size_t kEditMaxChars = 8;
constexpr double kPi = 3.14159265358979323846;

bool isFine(unsigned mods)
{
    return (mods & (X11PlugView::kPointerShift | X11PlugView::kPointerControl)) != 0;
}

// On-screen heights the SVG icons are rasterised at.
constexpr int kGearH = 26;
constexpr int kLoadH = 18;
constexpr int kClearH = 16;
constexpr int kGlobeH = 18;
constexpr int kArrowH = 14;
constexpr int kRowIconH = 22;
// ModelIcon is a wide wordmark (121x36) rather than a near-square glyph like
// the IR icons (63x62), so it is sized by width. Driving it by kRowIconH
// instead would make it ~74px wide and it would run into the folder icon.
constexpr int kModelIconW = 42;

const char *baseName(const std::string &path)
{
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path.c_str() : path.c_str() + slash + 1;
}
} // namespace

//------------------------------------------------------------------------
// static_cast, not reinterpret_cast: NamController inherits from both
// EditController and INamFileLoader, so converting to the base may need a
// pointer adjustment that only a static_cast performs.
NamEditorView::NamEditorView(NamController *controller)
    : X11PlugView(static_cast<Vst::EditController *>(controller)), mController(controller)
{
    mUiScale = geo::uiScale();
    ViewRect size(0, 0, static_cast<int>(std::lround(geo::kWinW * mUiScale)),
                  static_cast<int>(std::lround(geo::kWinH * mUiScale)));
    setRect(size);
}

//------------------------------------------------------------------------
// Art and fonts are loaded here rather than in the constructor: createView()
// must stay harmless in headless hosts, which create and destroy views without
// ever attaching them.
void NamEditorView::onAttached()
{
    if (mResourcesLoaded)
        return;
    const std::string &res = resourceDir();
    mFonts.load(res);
    mImages.setResourceDir(res);
    mSvgs.setResourceDir(res);
    mResourcesLoaded = true;
}

// Fit the 3:2 logical canvas into the current host allocation without
// distorting the panel. Any extra area becomes a clean black letterbox.
void NamEditorView::onResize(int width, int height)
{
    if (width <= 0 || height <= 0)
        return;
    mUiScale = std::min(static_cast<float>(width) / geo::kWinW,
                        static_cast<float>(height) / geo::kWinH);
    mUiOffsetX = (width - geo::kWinW * mUiScale) * 0.5f;
    mUiOffsetY = (height - geo::kWinH * mUiScale) * 0.5f;
}

// The window is going: drop an open field without applying it, and hand the
// keyboard back while the window it was given to still exists. A drag cannot
// outlive the editor either, so its edit gesture is closed for the host.
void NamEditorView::onRemoved()
{
    cancelTextEdit();
    if (mDragParam && mController)
        mController->endEdit(mDragParam);
    mDragParam = 0;
}

//------------------------------------------------------------------------
double NamEditorView::paramValue(Vst::ParamID id) const
{
    return mController ? mController->getParamNormalized(id) : 0.0;
}

std::string NamEditorView::paramText(Vst::ParamID id) const
{
    if (!mController)
        return std::string();
    Vst::String128 str = {0};
    if (mController->getParamStringByValue(id, mController->getParamNormalized(id), str) !=
        kResultOk)
        return std::string();
    char buf[128] = "";
    UString(str, 128).toAscii(buf, sizeof(buf));
    return std::string(buf);
}

bool NamEditorView::filePath(bool model, std::string &out) const
{
    if (!mController)
        return false;
    char buf[4096] = "";
    const tresult r = model ? mController->getModelFile(buf, sizeof(buf))
                            : mController->getIrFile(buf, sizeof(buf));
    if (r != kResultOk || buf[0] == 0)
        return false;
    out.assign(buf);
    return true;
}

bool NamEditorView::hitCircle(float x, float y, float cx, float cy, float r)
{
    const float dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= r * r;
}

//------------------------------------------------------------------------
// Drawing
//------------------------------------------------------------------------
void NamEditorView::onDraw(cairo_t *cr)
{
    cairo_translate(cr, mUiOffsetX, mUiOffsetY);
    cairo_scale(cr, mUiScale, mUiScale);
    Canvas c(cr, &mFonts, geo::kWinW, geo::kWinH);
    compose(c);
}

void NamEditorView::compose(Canvas &c)
{
    c.setFont(Font::Body);
    if (cairo_surface_t *bg = mImages.get("Background")) {
        c.drawImage(bg, c.bounds());
    } else {
        c.setColor(geo::kBgColor);
        c.fillRect(c.bounds());
    }
    if (cairo_surface_t *lines = mImages.get("Lines"))
        c.drawImage(lines, c.bounds());

    drawTitle(c);
    for (int i = 0; i < geo::kKnobCount; ++i)
        drawKnob(c, geo::kKnobs[i]);
    for (int i = 0; i < geo::kToggleCount; ++i)
        drawToggle(c, geo::kToggles[i]);
    drawFileRow(c, geo::kModelRow, true);
    drawFileRow(c, geo::kIrRow, false);
    drawMeter(c, geo::kInputMeter, mInDisp, "IN");
    drawMeter(c, geo::kOutputMeter, mOutDisp, "OUT");
    drawGear(c);
    if (slimAvailable())
        drawSlimIcon(c);

    if (mSettingsOpen)
        drawSettings(c);
    if (mSlimOpen)
        drawSlimOverlay(c);
    if (mBrowser.isOpen())
        mBrowser.draw(c);
}

//------------------------------------------------------------------------
void NamEditorView::drawTitle(Canvas &c)
{
    c.setFont(Font::Title);
    c.setFontSize(22);
    c.setColor(geo::kTextColor);
    const char *title = "NAMix";
    c.drawString(title, geo::kTitleX + (geo::kTitleW - c.stringWidth(title)) / 2.0f,
                 geo::kTitleY + 34);
    c.setFont(Font::Body);
}

//------------------------------------------------------------------------
// Face art + dim track arc + azure value arc + pointer notch.
void NamEditorView::drawKnobBody(Canvas &c, double norm, int cx, int cy, int r)
{
    const Rect face(cx - r, cy - r, 2.0f * r, 2.0f * r);
    if (cairo_surface_t *knob = mImages.get("KnobBackground")) {
        c.drawImage(knob, face);
    } else {
        c.setColor(0x28282E);
        c.fillEllipse(face);
    }

    const double a0 = -geo::kKnobSweepDeg / 2.0; // min (down-left)
    const double aVal = a0 + norm * geo::kKnobSweepDeg;
    const float arcR = r * static_cast<float>(geo::kKnobArcFrac);
    c.strokeArc(cx, cy, arcR, a0, -a0, 3.0f, 0x2E2B34); // full track
    if (norm > 0.001)
        c.strokeArc(cx, cy, arcR, a0, aVal, 3.0f, geo::kAzure); // value

    // Pointer notch on the face at the value angle.
    const double ang = aVal * kPi / 180.0;
    const float s = static_cast<float>(std::sin(ang)), co = static_cast<float>(std::cos(ang));
    const float lo = r * 0.45f, hi = r * static_cast<float>(geo::kKnobPointerFrac);
    c.setColor(geo::kAzure);
    c.setPenSize(3.0f);
    c.strokeLine(cx + lo * s, cy - lo * co, cx + hi * s, cy - hi * co);
    c.setPenSize(1.0f);
}

void NamEditorView::drawKnob(Canvas &c, const geo::KnobSpec &k)
{
    const double norm = paramValue(k.id);
    drawKnobBody(c, norm, k.cx, k.cy, k.r);

    c.setColor(geo::kTextColor);
    c.setFontSize(12);
    c.drawString(k.label, k.cx - c.stringWidth(k.label) / 2.0f, k.cy - geo::kKnobLabelDY);

    if (editing(k.id)) {
        drawEditField(c, knobValueRect(k), 11, k.cy + geo::kKnobValueDY);
        return;
    }
    const std::string val = paramText(k.id);
    if (!val.empty()) {
        const std::string disp = k.unit ? (val + " " + k.unit) : val;
        c.setColor(geo::kDimColor);
        c.setFontSize(11);
        c.drawString(disp.c_str(), k.cx - c.stringWidth(disp.c_str()) / 2.0f,
                     k.cy + geo::kKnobValueDY);
    }
}

//------------------------------------------------------------------------
// The open text field: a dark box with an azure edge, the text centred on the
// readout's own baseline so nothing moves as the field opens, and either the
// whole text highlighted (just opened) or a caret.
void NamEditorView::drawEditField(Canvas &c, const Rect &r, float fontSize, float baseline)
{
    c.setColor(0x000000, 200);
    c.fillRoundRect(r, 3.0f);
    c.setColor(geo::kAzure);
    c.setPenSize(1.0f);
    c.strokeRoundRect(r, 3.0f);

    c.setFontSize(fontSize);
    const float textW = c.stringWidth(mEditText.c_str());
    const float x0 = r.left() + (r.w - textW) / 2.0f;
    if (mEditSelectAll && !mEditText.empty()) {
        c.setColor(geo::kAzure, 110);
        c.fillRect(Rect(x0 - 1.0f, r.top() + 3.0f, textW + 2.0f, r.h - 6.0f));
    }
    c.setColor(geo::kTextColor);
    c.drawString(mEditText.c_str(), x0, baseline);
    if (!mEditSelectAll) {
        const std::string head = mEditText.substr(0, mEditCaret);
        const float cx = x0 + c.stringWidth(head.c_str());
        c.strokeLine(cx, r.top() + 3.0f, cx, r.bottom() - 3.0f);
    }
}

//------------------------------------------------------------------------
void NamEditorView::drawToggle(Canvas &c, const geo::ToggleSpec &t)
{
    const bool on = paramValue(t.id) > 0.5;
    const Rect pill(t.cx - geo::kToggleW / 2.0f, t.cy - geo::kToggleH / 2.0f, geo::kToggleW,
                    geo::kToggleH);
    c.setColor(on ? geo::kAzure : 0x2A2730);
    c.fillRoundRect(pill, geo::kToggleH / 2.0f);

    const float hs = 18.0f;
    const float hx = on ? pill.right() - hs - 2.0f : pill.left() + 2.0f;
    const Rect handle(hx, t.cy - hs / 2.0f, hs, hs);
    if (cairo_surface_t *sw = mImages.get("SlideSwitchHandle")) {
        c.drawImage(sw, handle);
    } else {
        c.setColor(geo::kTextColor);
        c.fillEllipse(handle);
    }

    c.setColor(geo::kTextColor);
    c.setFontSize(11);
    c.drawString(t.label, t.cx - c.stringWidth(t.label) / 2.0f, t.cy + geo::kToggleH / 2.0f + 15);
}

//------------------------------------------------------------------------
void NamEditorView::drawFileRow(Canvas &c, const geo::FileRow &row, bool model)
{
    const Rect rect(row.x, row.y, row.w, row.h);
    if (cairo_surface_t *fb = mImages.get("FileBackground")) {
        c.drawImage(fb, rect);
    } else {
        c.setColor(0x14161B);
        c.fillRoundRect(rect, 6.0f);
    }

    std::string path;
    const bool loaded = filePath(model, path);
    const float cy = row.y + row.h / 2.0f;

    // Status icon to the left of the pill: amp icon (model) / IR on-off.
    if (model)
        c.drawImageCentered(mSvgs.getByWidth("ModelIcon", kModelIconW), geo::kRowIconCX, cy);
    else
        c.drawImageCentered(mSvgs.getByHeight(loaded ? "IRIconOn" : "IRIconOff", kRowIconH),
                            geo::kRowIconCX, cy);

    // Load (folder) + prev/next arrows at the pill's left.
    c.drawImageCentered(mSvgs.getByHeight("File", kLoadH), row.x + 16, cy);
    c.drawImageCentered(mSvgs.getByHeight("ArrowLeft", kArrowH), row.x + 40, cy);
    c.drawImageCentered(mSvgs.getByHeight("ArrowRight", kArrowH), row.x + 60, cy);

    // File name / placeholder, centred in the middle region (truncated).
    c.setFontSize(12);
    c.setColor(loaded ? geo::kTextColor : geo::kDimColor);
    const float midL = row.x + 78, midR = row.x + row.w - 30;
    const std::string text = loaded ? std::string(baseName(path)) : std::string(row.placeholder);
    const std::string disp = c.clipToWidth(text, midR - midL);
    c.drawString(disp.c_str(), midL + (midR - midL - c.stringWidth(disp.c_str())) / 2.0f, cy + 4);

    // Clear (loaded) or globe "Get" (empty) at the pill's right.
    c.drawImageCentered(mSvgs.getByHeight(loaded ? "Cross" : "Globe", loaded ? kClearH : kGlobeH),
                        row.x + row.w - 16, cy);
}

//------------------------------------------------------------------------
// Replicates the original NAMMeterControl rendering: the fill track is a 10 px
// column centred in the meter, inset by (0.1*H - 10) top and bottom; a black
// grid every 2 px gives the segmented look, and a bright tick sits at the top
// of the fill. No separate peak indicator (the original draws none).
void NamEditorView::drawMeter(Canvas &c, const geo::MeterRect &m, float level, const char *label)
{
    const Rect rect(m.x, m.y, m.w, m.h);
    if (cairo_surface_t *bg = mImages.get("MeterBackground")) {
        c.drawImage(bg, rect);
    } else {
        c.setColor(0x101216);
        c.fillRect(rect);
    }

    const float trackW = 10.0f;
    const float trackX = m.x + (m.w - trackW) / 2.0f;
    const float topMargin = 0.1f * m.h - 10.0f;
    const float trackY = m.y + topMargin;
    const float trackH = m.h - 2.0f * topMargin;

    const float lv = level < 0 ? 0 : (level > 1 ? 1 : level);
    if (lv > 0.0f) {
        const float h = trackH * lv;
        const float fillTop = trackY + trackH - h;
        c.setColor(geo::kAzure);
        c.fillRect(Rect(trackX, fillTop, trackW, trackY + trackH - fillTop));
        // Black segmented grid over the track.
        c.setColor(0x000000);
        for (float y = trackY + 2.0f; y < trackY + trackH; y += 2.0f)
            c.fillRect(Rect(trackX, y, trackW, 1.0f));
        // Bright tick at the top of the fill.
        c.setColor(0x6A9FF0);
        c.fillRect(Rect(trackX, fillTop, trackW, 1.0f));
    }

    c.setColor(geo::kDimColor);
    c.setFontSize(10);
    c.drawString(label, m.x + (m.w - c.stringWidth(label)) / 2.0f, m.y + m.h + 14);
}

//------------------------------------------------------------------------
void NamEditorView::drawGear(Canvas &c)
{
    c.drawImageCentered(mSvgs.getByHeight("Gear", kGearH), geo::kGearCX, geo::kGearCY);
}

void NamEditorView::drawSlimIcon(Canvas &c)
{
    c.drawImageCentered(mSvgs.getByHeight("SlimmableIcon", geo::kSlimIconH),
                        geo::kSlimIconX + geo::kSlimIconW / 2.0f,
                        geo::kSlimIconY + geo::kSlimIconH / 2.0f);
}

void NamEditorView::dimPanel(Canvas &c)
{
    c.setColor(0x000000, 170);
    c.fillRect(c.bounds());
}

//------------------------------------------------------------------------
Rect NamEditorView::settingsCloseBox()
{
    return Rect::fromLTRB(560, 50, 578, 68);
}
Rect NamEditorView::calValueRect()
{
    return Rect::fromLTRB(120, 192, 210, 218);
}
Rect NamEditorView::calibrateToggleRect()
{
    return Rect(143, 232, geo::kToggleW, geo::kToggleH);
}
Rect NamEditorView::outputModeRow(int i)
{
    const float y = 214 + i * 26;
    return Rect(312, y, 220, 22);
}
// 64 px wide, inside the 83 px knob pitch; 18 px tall around the readout's
// baseline, which keeps it clear of the face above (cy + r) and of the toggle
// pills below (cy + 74).
Rect NamEditorView::knobValueRect(const geo::KnobSpec &k)
{
    return Rect(k.cx - 32.0f, k.cy + geo::kKnobValueDY - 13.0f, 64.0f, 18.0f);
}

bool NamEditorView::editFieldRect(Rect &out) const
{
    if (mEditParam == 0)
        return false;
    if (mEditParam == kInputCalibrationLevelId) {
        out = calValueRect();
        return true;
    }
    for (int i = 0; i < geo::kKnobCount; ++i) {
        if (geo::kKnobs[i].id == mEditParam) {
            out = knobValueRect(geo::kKnobs[i]);
            return true;
        }
    }
    return false;
}

//------------------------------------------------------------------------
// Full-window settings overlay (matches the original layout): title + close at
// top, input calibration on the left, output mode on the right, and a
// model-info / credits band along the bottom.
void NamEditorView::drawSettings(Canvas &c)
{
    c.setColor(0x161318);
    c.fillRect(c.bounds());
    c.setColor(0x2E2B34);
    c.setPenSize(1.0f);
    c.strokeRoundRect(Rect::fromLTRB(8, 8, geo::kWinW - 8, geo::kWinH - 8), 10.0f);

    // Title + close.
    c.setColor(geo::kTextColor);
    c.setFont(Font::Title);
    c.setFontSize(24);
    c.drawString("SETTINGS", (geo::kWinW - c.stringWidth("SETTINGS")) / 2.0f, 68);
    c.setFont(Font::Body);

    const Rect close = settingsCloseBox();
    c.setColor(geo::kDimColor);
    c.setPenSize(2.0f);
    c.strokeLine(close.left(), close.top(), close.right(), close.bottom());
    c.strokeLine(close.left(), close.bottom(), close.right(), close.top());
    c.setPenSize(1.0f);

    // --- Input calibration (left) ---
    const Rect val = calValueRect();
    if (cairo_surface_t *ib = mImages.get("InputLevelBackground"))
        c.drawImage(ib, val);
    // Both halves of this block are gated together on the capture's
    // "input_level_dbu", matching the original plug-in: it derives one
    // disableInputCalibrationControls flag from HasInputLevel() and applies it
    // to the Calibrate Input toggle and the level box in the same breath.
    if (editing(kInputCalibrationLevelId)) {
        drawEditField(c, val, 13, val.top() + 18);
    } else {
        const std::string calText = paramText(kInputCalibrationLevelId);
        c.setFontSize(13);
        c.setColor(calibrateInputAvailable() ? geo::kTextColor : 0x5A5760);
        c.drawString(calText.c_str(), val.left() + (val.w - c.stringWidth(calText.c_str())) / 2.0f,
                     val.top() + 18);
    }

    const Rect tog = calibrateToggleRect();
    const bool calOn = paramValue(kCalibrateInputId) > 0.5;
    c.setColor(calibrateInputAvailable() && calOn ? geo::kAzure : 0x2A2730);
    c.fillRoundRect(tog, geo::kToggleH / 2.0f);
    const float hs = 18.0f;
    const float hx = calOn ? tog.right() - hs - 2.0f : tog.left() + 2.0f;
    const Rect handle(hx, tog.top() + 2.0f, hs, hs);
    if (cairo_surface_t *sw = mImages.get("SlideSwitchHandle"))
        c.drawImage(sw, handle);

    c.setFontSize(12);
    c.setColor(calibrateInputAvailable() ? geo::kTextColor : 0x5A5760);
    c.drawString("Calibrate Input", val.left() + (val.w - c.stringWidth("Calibrate Input")) / 2.0f,
                 tog.bottom() + 16);

    // --- Output mode (right) ---
    c.setFontSize(13);
    c.setColor(geo::kTextColor);
    c.drawString("Output Mode", 340, 200);
    const char *modes[3] = {"Raw", "Normalized", "Calibrated"};
    const int cur = static_cast<int>(paramValue(kOutputModeId) * 2.0 + 0.5);
    c.setFontSize(12);
    // The modes are annotated, never disabled. A mode whose metadata is absent
    // is a no-op in the DSP -- applyDsp leaves the gain at unity, which is what
    // Raw does -- so refusing the click buys nothing and takes a working mode
    // away from any capture whose metadata we read differently than the user
    // expects. The original plug-in reaches the same place from the other
    // direction: its OutputModeControl only ever rewrites the two labels, and
    // both of its attempts to call SetDisabled on the radio group are commented
    // out, one of them with "FIXME -- need to disable only the normalized
    // model". Say what is missing, then let the user choose.
    for (int i = 0; i < 3; ++i) {
        const Rect row = outputModeRow(i);
        const float dotX = row.left() + 8, dotY = row.top() + 11;
        c.setColor(geo::kAzure);
        c.strokeEllipse(dotX, dotY, 6, 6);
        if (i == cur)
            c.fillEllipse(dotX, dotY, 3, 3);
        c.setColor(geo::kTextColor);
        std::string label(modes[i]);
        if (!outputModeSupported(i))
            label += " [Not supported by model]";
        c.drawString(label.c_str(), row.left() + 22, row.top() + 16);
    }

    // --- Bottom band: separator + model info (left) + credits (right) ---
    c.setColor(0x2A2730);
    c.strokeLine(24, 306, geo::kWinW - 24, 306);
    c.setFontSize(11);
    c.setColor(geo::kDimColor);
    std::string model;
    const bool haveModel = filePath(true, model);
    c.drawString("Model information:", 28, 326);
    c.drawString(haveModel ? baseName(model) : "(no model loaded)", 28, 342);

    c.drawString("NAMix — Neural Amp Modeler for Linux", 300, 326);
    c.drawString("© 2026 rations · MIT License", 300, 342);
    c.drawString("Based on NeuralAmpModelerPlugin by Steven Atkinson", 300, 358);
    c.setColor(geo::kAzure);
    c.drawString("www.neuralampmodeler.com", 300, 374);
}

//------------------------------------------------------------------------
void NamEditorView::drawSlimOverlay(Canvas &c)
{
    dimPanel(c);
    drawKnobBody(c, paramValue(kSlimId), geo::kSlimKnobCX, geo::kSlimKnobCY, geo::kSlimKnobR);
    c.setColor(geo::kTextColor);
    c.setFont(Font::Title);
    c.setFontSize(15);
    c.drawString("Slim", geo::kSlimKnobCX - c.stringWidth("Slim") / 2.0f,
                 geo::kSlimKnobCY - geo::kSlimKnobR - 12);
    c.setFont(Font::Body);
    c.setFontSize(11);
    c.setColor(geo::kDimColor);
    const char *hint = "click outside to close";
    c.drawString(hint, geo::kSlimKnobCX - c.stringWidth(hint) / 2.0f,
                 geo::kSlimKnobCY + geo::kSlimKnobR + 26);
}

//------------------------------------------------------------------------
// Interaction
//------------------------------------------------------------------------
void NamEditorView::editParam(Vst::ParamID id, double norm)
{
    if (!mController)
        return;
    mController->beginEdit(id);
    mController->setParamNormalized(id, norm);
    mController->performEdit(id, norm);
    mController->endEdit(id);
}

void NamEditorView::nudgeParam(Vst::ParamID id, double delta)
{
    double norm = paramValue(id) + delta;
    norm = norm < 0.0 ? 0.0 : (norm > 1.0 ? 1.0 : norm);
    editParam(id, norm);
    invalidate();
}

// Double-click returns a control to its parameter's own default, as the
// original's knobs do (IControl::OnMouseDblClick -> SetValueToDefault).
void NamEditorView::resetParam(Vst::ParamID id)
{
    if (!mController)
        return;
    if (const Vst::Parameter *p = mController->getParameterObject(id))
        editParam(id, p->getInfo().defaultNormalizedValue);
    invalidate();
}

void NamEditorView::startDrag(Vst::ParamID id, float y, float range)
{
    if (!mController)
        return;
    mDragParam = id;
    mDragLastY = y;
    mDragRange = range > 1.0f ? range : 1.0f;
    mDragNorm = paramValue(id);
    mController->beginEdit(id);
    // No explicit pointer grab is needed: X delivers an implicit grab to the
    // window that saw the ButtonPress for as long as the button is held, so
    // motion outside the editor still arrives here.
}

//------------------------------------------------------------------------
void NamEditorView::onMouseDown(int x, int y, int button, unsigned /*mods*/, bool doubleClick)
{
    if (button != 1) // left button only, as on the original
        return;
    const float fx = (static_cast<float>(x) - mUiOffsetX) / mUiScale;
    const float fy = (static_cast<float>(y) - mUiOffsetY) / mUiScale;
    mMouseX = fx;
    mMouseY = fy;

    // An open field keeps a click inside it -- and takes the keyboard back,
    // in case the host took it away -- while a click anywhere else commits it
    // and then goes on to do whatever it was aimed at.
    Rect field;
    if (editFieldRect(field)) {
        if (field.contains(fx, fy)) {
            setKeyboardFocus(true);
            return;
        }
        commitTextEdit();
    }

    // The file browser captures input while open.
    if (mBrowser.isOpen()) {
        const FileBrowser::Result r = mBrowser.handleClick(fx, fy);
        if (r == FileBrowser::Result::Chosen && mController) {
            if (mBrowserForModel)
                mController->setModelFile(mBrowser.chosenPath().c_str());
            else
                mController->setIrFile(mBrowser.chosenPath().c_str());
        }
        invalidate();
        return;
    }

    // Slim overlay captures input while open.
    if (mSlimOpen) {
        if (hitCircle(fx, fy, geo::kSlimKnobCX, geo::kSlimKnobCY, geo::kSlimKnobR)) {
            if (doubleClick)
                resetParam(kSlimId);
            else
                startDrag(kSlimId, fy, kDragGearing * 2.0f * geo::kSlimKnobR);
        } else {
            mSlimOpen = false; // click outside dismisses
        }
        invalidate();
        return;
    }
    // Settings overlay captures input while open.
    if (mSettingsOpen) {
        handleSettingsClick(fx, fy);
        return;
    }

    // Gear -> open settings.
    if (hitCircle(fx, fy, geo::kGearCX, geo::kGearCY, geo::kGearR + 4)) {
        mSettingsOpen = true;
        invalidate();
        return;
    }
    // Slim icon (slimmable models only) -> open slim overlay.
    if (slimAvailable()) {
        const Rect si(geo::kSlimIconX, geo::kSlimIconY, geo::kSlimIconW, geo::kSlimIconH);
        if (si.contains(fx, fy)) {
            mSlimOpen = true;
            invalidate();
            return;
        }
    }

    // Knobs: a vertical drag on the face, a double-click on it to reset, and
    // a click on the readout below it to type a value -- the three ways the
    // original's IVKnobControl offers.
    for (int i = 0; i < geo::kKnobCount; ++i) {
        const geo::KnobSpec &k = geo::kKnobs[i];
        if (hitCircle(fx, fy, k.cx, k.cy, k.r)) {
            if (doubleClick)
                resetParam(k.id);
            else
                startDrag(k.id, fy, kDragGearing * 2.0f * k.r);
            return;
        }
        if (knobValueRect(k).contains(fx, fy)) {
            beginTextEdit(k.id);
            return;
        }
    }
    // Toggles.
    for (int i = 0; i < geo::kToggleCount; ++i) {
        const geo::ToggleSpec &t = geo::kToggles[i];
        const Rect pill(t.cx - geo::kToggleW / 2.0f, t.cy - geo::kToggleH / 2.0f, geo::kToggleW,
                        geo::kToggleH);
        if (pill.contains(fx, fy)) {
            editParam(t.id, paramValue(t.id) > 0.5 ? 0.0 : 1.0);
            invalidate();
            return;
        }
    }
    // File rows.
    if (handleFileRowClick(geo::kModelRow, fx, fy, true))
        return;
    handleFileRowClick(geo::kIrRow, fx, fy, false);
}

//------------------------------------------------------------------------
// The drag accumulates from the last position rather than measuring from the
// first, as the original's does, so pressing or releasing Shift mid-drag
// changes the gearing from that point on instead of making the knob jump.
// The accumulator is clamped too, so overshooting an end costs no travel on
// the way back.
void NamEditorView::onMouseMove(int x, int y, unsigned mods)
{
    mMouseX = (static_cast<float>(x) - mUiOffsetX) / mUiScale;
    mMouseY = (static_cast<float>(y) - mUiOffsetY) / mUiScale;
    if (!mDragParam || !mController)
        return;
    const float fy = (static_cast<float>(y) - mUiOffsetY) / mUiScale;
    const float range = mDragRange * (isFine(mods) ? kFineDragFactor : 1.0f);
    mDragNorm += static_cast<double>(mDragLastY - fy) / range;
    mDragNorm = mDragNorm < 0.0 ? 0.0 : (mDragNorm > 1.0 ? 1.0 : mDragNorm);
    mDragLastY = fy;
    mController->setParamNormalized(mDragParam, mDragNorm);
    mController->performEdit(mDragParam, mDragNorm);
    invalidate();
}

void NamEditorView::onMouseUp(int /*x*/, int /*y*/, int button)
{
    if (button != 1)
        return;
    if (mDragParam && mController) {
        mController->endEdit(mDragParam);
        mDragParam = 0;
    }
}

//------------------------------------------------------------------------
// Wheel adjusts whatever is under the cursor (wheel up = increase).
void NamEditorView::onMouseWheel(int x, int y, int delta, unsigned mods)
{
    const float fx = (static_cast<float>(x) - mUiOffsetX) / mUiScale;
    const float fy = (static_cast<float>(y) - mUiOffsetY) / mUiScale;
    const bool fine = isFine(mods);
    const double step = delta * (fine ? kFineWheelStep : kWheelStep);

    if (mBrowser.isOpen()) {
        if (mBrowser.handleWheel(delta))
            invalidate();
        return;
    }
    if (mSlimOpen) {
        if (hitCircle(fx, fy, geo::kSlimKnobCX, geo::kSlimKnobCY, geo::kSlimKnobR))
            nudgeParam(kSlimId, step);
        return;
    }
    if (mSettingsOpen) {
        if (calibrateInputAvailable() && calValueRect().contains(fx, fy) &&
            !editing(kInputCalibrationLevelId)) {
            const double db = delta * (fine ? kCalFineWheelDb : kCalWheelDb);
            nudgeParam(kInputCalibrationLevelId, db / (ranges::kCalMax - ranges::kCalMin));
        }
        return;
    }
    for (int i = 0; i < geo::kKnobCount; ++i) {
        const geo::KnobSpec &k = geo::kKnobs[i];
        if (hitCircle(fx, fy, k.cx, k.cy, k.r)) {
            nudgeParam(k.id, step);
            return;
        }
    }
}

//------------------------------------------------------------------------
void NamEditorView::handleSettingsClick(float x, float y)
{
    if (settingsCloseBox().inset(-6.0f).contains(x, y)) {
        mSettingsOpen = false;
        invalidate();
        return;
    }
    // Output mode (index 0/1/2 -> normalized 0/0.5/1). Every mode is
    // selectable; the ones the capture has no metadata for say so in their
    // label and behave as Raw. See drawSettings for why none of them is
    // refused.
    for (int i = 0; i < 3; ++i) {
        if (outputModeRow(i).contains(x, y)) {
            editParam(kOutputModeId, i * 0.5);
            invalidate();
            return;
        }
    }
    // The toggle and the level are gated together -- see drawSettings.
    if (calibrateInputAvailable()) {
        if (calibrateToggleRect().contains(x, y)) {
            editParam(kCalibrateInputId, paramValue(kCalibrateInputId) > 0.5 ? 0.0 : 1.0);
            invalidate();
            return;
        }
        // Typed, as the original's InputLevelControl is: an interface level
        // is read off a spec sheet and entered, not found by feel.
        if (calValueRect().contains(x, y))
            beginTextEdit(kInputCalibrationLevelId);
    }
}

//------------------------------------------------------------------------
// Typed values
//------------------------------------------------------------------------
void NamEditorView::beginTextEdit(Vst::ParamID id)
{
    if (!mController || id == 0 || editing(id))
        return;
    commitTextEdit();
    mEditParam = id;
    // Seeded with the value as the readout shows it, without the unit, and
    // selected, so typing replaces it and the arrow keys keep it to adjust.
    mEditText = paramText(id);
    if (mEditText.size() > kEditMaxChars)
        mEditText.resize(kEditMaxChars);
    mEditCaret = mEditText.size();
    mEditSelectAll = true;
    // Held for exactly as long as the field is open: every exit from it runs
    // through commitTextEdit() or cancelTextEdit(), which hand it back.
    setKeyboardFocus(true);
    invalidate();
}

void NamEditorView::commitTextEdit()
{
    if (mEditParam == 0)
        return;
    const Vst::ParamID id = mEditParam;
    std::string text = mEditText;
    cancelTextEdit();
    if (!mController)
        return;

    // Parsed here rather than by Parameter::fromString, whose sscanf("%lf")
    // follows the host's LC_NUMERIC: under a locale that writes 2,5 it would
    // read "2.5" as 2. std::from_chars ignores the locale, and both decimal
    // marks are accepted so either habit works. from_chars takes no leading
    // '+', so one is dropped first.
    std::replace(text.begin(), text.end(), ',', '.');
    const char *begin = text.c_str();
    if (*begin == '+')
        ++begin;
    double plain = 0.0;
    const std::from_chars_result r = std::from_chars(begin, text.c_str() + text.size(), plain);
    // Text that is not a number -- an emptied field, a lone "-" -- is a user
    // part-way through an edit, not a request for the minimum, so the
    // parameter keeps its value.
    if (r.ec != std::errc() || r.ptr == begin)
        return;
    Vst::Parameter *p = mController->getParameterObject(id);
    if (!p)
        return;
    // Clamped rather than refused: the readout redraws at the limit, which
    // says what was set. No isfinite() check is needed (and under this
    // target's -ffast-math it would fold away): only [0-9.,+-] can be typed,
    // at most kEditMaxChars of them, so the value is always finite.
    const double lo = p->toPlain(0.0), hi = p->toPlain(1.0);
    plain = std::clamp(plain, std::min(lo, hi), std::max(lo, hi));
    editParam(id, p->toNormalized(plain));
    invalidate();
}

void NamEditorView::cancelTextEdit()
{
    if (mEditParam == 0)
        return;
    mEditParam = 0;
    mEditText.clear();
    mEditCaret = 0;
    mEditSelectAll = false;
    setKeyboardFocus(false);
    invalidate();
}

// One key. Returns whether it was consumed, which is the whole contract: the
// SDK warns that answering kResultTrue for a key that was not handled blocks
// the host's own key commands, and in a DAW the space bar is one of those.
bool NamEditorView::handleTextKey(char16 key, int16 keyCode, int16 modifiers)
{
    if (mEditParam == 0)
        return false;

    switch (keyCode) {
        case KEY_RETURN:
        case KEY_ENTER:
        case KEY_TAB:
            commitTextEdit();
            return true;
        case KEY_ESCAPE:
            cancelTextEdit();
            return true;
        case KEY_BACK:
            if (mEditSelectAll) {
                mEditSelectAll = false;
                mEditText.clear();
                mEditCaret = 0;
            } else if (mEditCaret > 0) {
                mEditText.erase(--mEditCaret, 1);
            }
            invalidate();
            return true;
        case KEY_DELETE:
            if (mEditSelectAll) {
                mEditSelectAll = false;
                mEditText.clear();
                mEditCaret = 0;
            } else if (mEditCaret < mEditText.size()) {
                mEditText.erase(mEditCaret, 1);
            }
            invalidate();
            return true;
        // Moving the caret says "edit this rather than replace it": the
        // selection drops, the text stays, and the caret lands at the end the
        // arrow points at.
        case KEY_LEFT:
            if (mEditSelectAll)
                mEditCaret = 0;
            else if (mEditCaret > 0)
                --mEditCaret;
            mEditSelectAll = false;
            invalidate();
            return true;
        case KEY_RIGHT:
            if (mEditSelectAll)
                mEditCaret = mEditText.size();
            else if (mEditCaret < mEditText.size())
                ++mEditCaret;
            mEditSelectAll = false;
            invalidate();
            return true;
        case KEY_HOME:
            mEditSelectAll = false;
            mEditCaret = 0;
            invalidate();
            return true;
        case KEY_END:
            mEditSelectAll = false;
            mEditCaret = mEditText.size();
            invalidate();
            return true;
        default:
            break;
    }

    // Shift is part of typing (it is set for '+' on most layouts); only the
    // command modifiers are refused, so Ctrl-S still reaches the host.
    const int16 kCommandMods = static_cast<int16>(kAlternateKey | kCommandKey | kControlKey);
    if ((modifiers & kCommandMods) != 0)
        return false;

    char16 ch = key;
    // For a host that fills in only the virtual code: keycodes.h encodes the
    // digits as '0' + VKEY_FIRST_ASCII, and VirtualKeyCodeToChar undoes it.
    if ((ch < 0x20 || ch > 0x7E) && keyCode >= VKEY_FIRST_ASCII)
        ch = static_cast<char16>(VirtualKeyCodeToChar(static_cast<uint8>(keyCode)));
    if (ch < 0x20 || ch > 0x7E)
        return false;
    // Consumed either way from here: the field has the keyboard, so a
    // character it will not take must still not reach the host's commands.
    const bool numeric =
        (ch >= '0' && ch <= '9') || ch == '.' || ch == ',' || ch == '-' || ch == '+';
    if (!numeric)
        return true;
    if (mEditSelectAll) {
        mEditSelectAll = false;
        mEditText.clear();
        mEditCaret = 0;
    }
    if (mEditText.size() >= kEditMaxChars)
        return true;
    mEditText.insert(mEditCaret, 1, static_cast<char>(ch));
    ++mEditCaret;
    invalidate();
    return true;
}

// The SDK's route: a host that passes keys to the view lands here, and one
// that does not reaches handleTextKey through onKeyDownNative instead. Only
// one of them can carry a given key -- while the editor holds the X focus the
// host never sees it -- so a key is never handled twice.
tresult PLUGIN_API NamEditorView::onKeyDown(char16 key, int16 keyCode, int16 modifiers)
{
    return handleTextKey(key, keyCode, modifiers) ? kResultTrue : kResultFalse;
}

bool NamEditorView::onKeyDownNative(char16 key, int16 keyCode, int16 modifiers)
{
    return handleTextKey(key, keyCode, modifiers);
}

//------------------------------------------------------------------------
bool NamEditorView::handleFileRowClick(const geo::FileRow &row, float x, float y, bool model)
{
    const Rect rect(row.x, row.y, row.w, row.h);
    if (!rect.contains(x, y))
        return false;

    std::string path;
    const bool loaded = filePath(model, path);

    const Rect prevBox = Rect::fromLTRB(row.x + 30, row.y + 3, row.x + 50, row.y + row.h - 3);
    const Rect nextBox = Rect::fromLTRB(row.x + 50, row.y + 3, row.x + 70, row.y + row.h - 3);
    const Rect rightBox =
        Rect::fromLTRB(row.x + row.w - 28, row.y + 3, row.x + row.w - 4, row.y + row.h - 3);

    if (prevBox.contains(x, y)) {
        cycleFile(model, -1);
        return true;
    }
    if (nextBox.contains(x, y)) {
        cycleFile(model, +1);
        return true;
    }
    if (rightBox.contains(x, y)) {
        if (loaded) { // clear (x)
            if (mController) {
                if (model)
                    mController->setModelFile("");
                else
                    mController->setIrFile("");
            }
            invalidate();
        } else { // globe -> the model/IR download page
            openUrl("https://www.neuralampmodeler.com");
        }
        return true;
    }

    // Anywhere else on the row opens the browser.
    openBrowser(model);
    return true;
}

//------------------------------------------------------------------------
void NamEditorView::openBrowser(bool model)
{
    std::string current;
    filePath(model, current);
    mBrowserForModel = model;
    mBrowser.open(current, model ? "nam" : "wav",
                  model ? "Select a model" : "Select an impulse response");
    invalidate();
}

//------------------------------------------------------------------------
// Cycle to the previous/next same-extension file in the loaded file's
// directory (alphabetical). No-op when nothing is loaded.
void NamEditorView::cycleFile(bool model, int dir)
{
    std::string current;
    if (!filePath(model, current))
        return;

    std::error_code ec;
    const fs::path curPath(current);
    const fs::path parent = curPath.parent_path();
    if (parent.empty())
        return;
    const char *ext = model ? "nam" : "wav";

    std::vector<std::string> files;
    fs::directory_iterator it(parent, fs::directory_options::skip_permission_denied, ec);
    if (ec)
        return;
    for (const fs::directory_entry &entry : it) {
        const std::string name = entry.path().filename().string();
        const size_t dot = name.find_last_of('.');
        if (dot != std::string::npos && strcasecmp(name.c_str() + dot + 1, ext) == 0)
            files.push_back(entry.path().string());
    }
    if (files.size() < 2)
        return;
    std::sort(files.begin(), files.end());

    int idx = -1;
    for (size_t i = 0; i < files.size(); ++i)
        if (files[i] == curPath.string()) {
            idx = static_cast<int>(i);
            break;
        }
    if (idx < 0)
        return;

    const int n = static_cast<int>(files.size());
    const std::string &next = files[static_cast<size_t>(((idx + dir) % n + n) % n)];
    if (mController) {
        if (model)
            mController->setModelFile(next.c_str());
        else
            mController->setIrFile(next.c_str());
    }
    invalidate();
}

//------------------------------------------------------------------------
// Hand the URL to the desktop's default handler. fork+exec rather than
// system(): no shell is involved, so the URL cannot be interpreted, and the
// double fork means no zombie is left for the host to reap.
void NamEditorView::openUrl(const char *url)
{
    const pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            execlp("xdg-open", "xdg-open", url, static_cast<char *>(nullptr));
            _exit(127);
        }
        _exit(0);
    } else if (pid > 0) {
        int status = 0;
        waitpid(pid, &status, 0);
    }
}

//------------------------------------------------------------------------
// Controller callbacks (host run-loop thread)
//------------------------------------------------------------------------
void NamEditorView::ParamChanged(Vst::ParamID id, Vst::ParamValue value)
{
    if (id == kInputMeterId) {
        if (static_cast<float>(value) > mInDisp)
            mInDisp = static_cast<float>(value); // fast attack; the tick releases
        return;
    }
    if (id == kOutputMeterId) {
        if (static_cast<float>(value) > mOutDisp)
            mOutDisp = static_cast<float>(value);
        return;
    }
    invalidate();
}

// What the loaded capture states about its own levels. The caps are read from
// the controller on every call rather than copied here, so a panel opened over
// a capture that was loaded before it describes that capture and not the
// defaults. The two output predicates only choose a label; only
// calibrateInputAvailable() takes controls away, and it takes both of them.
bool NamEditorView::normalizedSupported() const
{
    if (!mController)
        return true;
    const ModelCaps &caps = mController->modelCaps();
    return !caps.loaded || caps.hasLoudness;
}

bool NamEditorView::calibratedSupported() const
{
    if (!mController)
        return true;
    const ModelCaps &caps = mController->modelCaps();
    return !caps.loaded || caps.hasOutputLevel;
}

bool NamEditorView::calibrateInputAvailable() const
{
    if (!mController)
        return true;
    const ModelCaps &caps = mController->modelCaps();
    return !caps.loaded || caps.hasInputLevel;
}

bool NamEditorView::outputModeSupported(int mode) const
{
    if (mode == 0)
        return true; // Raw compensates for nothing, so nothing can be missing
    return mode == 1 ? normalizedSupported() : calibratedSupported();
}

// Slim is not gated the way the output controls are: it is an icon that opens
// an overlay, not a control to grey, and a capture that is not slimmable has
// no size to choose. So this asks whether the loaded model IS slimmable, and
// an empty slot answers no.
bool NamEditorView::slimAvailable() const
{
    return mController && mController->modelCaps().slimmable;
}

//------------------------------------------------------------------------
void NamEditorView::ModelCapsChanged()
{
    // A load can take the Slim control away underneath an open overlay: load a
    // non-slimmable capture while it is up and the icon that opened it is
    // gone. Shut it, as the original plug-in hides its own overlay when the
    // model it belongs to goes away. Nothing is lost by closing — the knob
    // publishes each edit as it is made.
    if (mSlimOpen && !slimAvailable())
        mSlimOpen = false;
    // Likewise the dBu box, which a capture without "input_level_dbu"
    // disables: a value typed for it must not land after it has gone.
    if (editing(kInputCalibrationLevelId) && !calibrateInputAvailable())
        cancelTextEdit();
    invalidate();
}

//------------------------------------------------------------------------
// ~15 dB/s release at 30 Hz, matching the original meter.
void NamEditorView::onTick()
{
    mInDisp *= 0.944f;
    mOutDisp *= 0.944f;
    if (mInDisp > 0.002f || mOutDisp > 0.002f)
        invalidate();
}

} // namespace NAMix
