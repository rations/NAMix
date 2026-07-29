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

#include "pluginterfaces/base/ustring.h"

#include <algorithm>
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
constexpr float kKnobDragRange = 200.0f; // pixels of vertical drag for full range
constexpr double kPi = 3.14159265358979323846;

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
    ViewRect size(0, 0, geo::kWinW, geo::kWinH);
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
    if (mSlimmable)
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
    const std::string calText = paramText(kInputCalibrationLevelId);
    c.setFontSize(13);
    c.setColor(mHasInputLevel ? geo::kTextColor : 0x5A5760);
    c.drawString(calText.c_str(), val.left() + (val.w - c.stringWidth(calText.c_str())) / 2.0f,
                 val.top() + 18);

    const Rect tog = calibrateToggleRect();
    const bool calOn = paramValue(kCalibrateInputId) > 0.5;
    c.setColor(!mHasInputLevel ? 0x2A2730 : (calOn ? geo::kAzure : 0x2A2730));
    c.fillRoundRect(tog, geo::kToggleH / 2.0f);
    const float hs = 18.0f;
    const float hx = calOn ? tog.right() - hs - 2.0f : tog.left() + 2.0f;
    const Rect handle(hx, tog.top() + 2.0f, hs, hs);
    if (cairo_surface_t *sw = mImages.get("SlideSwitchHandle"))
        c.drawImage(sw, handle);

    c.setFontSize(12);
    c.setColor(mHasInputLevel ? geo::kTextColor : 0x5A5760);
    c.drawString("Calibrate Input", val.left() + (val.w - c.stringWidth("Calibrate Input")) / 2.0f,
                 tog.bottom() + 16);

    // --- Output mode (right) ---
    c.setFontSize(13);
    c.setColor(geo::kTextColor);
    c.drawString("Output Mode", 340, 200);
    const char *modes[3] = {"Raw", "Normalized", "Calibrated"};
    const int cur = static_cast<int>(paramValue(kOutputModeId) * 2.0 + 0.5);
    c.setFontSize(12);
    for (int i = 0; i < 3; ++i) {
        const Rect row = outputModeRow(i);
        const bool gated = (i > 0 && !mHasOutputLevel);
        const float dotX = row.left() + 8, dotY = row.top() + 11;
        c.setColor(gated ? 0x4A4750 : geo::kAzure);
        c.strokeEllipse(dotX, dotY, 6, 6);
        if (i == cur) {
            c.setColor(geo::kAzure);
            c.fillEllipse(dotX, dotY, 3, 3);
        }
        c.setColor(gated ? 0x5A5760 : geo::kTextColor);
        std::string label(modes[i]);
        if (gated)
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

void NamEditorView::startDrag(Vst::ParamID id, float y)
{
    if (!mController)
        return;
    mDragParam = id;
    mDragStartY = y;
    mDragStartNorm = paramValue(id);
    mController->beginEdit(id);
    // No explicit pointer grab is needed: X delivers an implicit grab to the
    // window that saw the ButtonPress for as long as the button is held, so
    // motion outside the editor still arrives here.
}

//------------------------------------------------------------------------
void NamEditorView::onMouseDown(int x, int y, int button)
{
    if (button != 1) // left button only, as on the original
        return;
    const float fx = static_cast<float>(x), fy = static_cast<float>(y);
    mMouseX = fx;
    mMouseY = fy;

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
        if (hitCircle(fx, fy, geo::kSlimKnobCX, geo::kSlimKnobCY, geo::kSlimKnobR))
            startDrag(kSlimId, fy);
        else
            mSlimOpen = false; // click outside dismisses
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
    if (mSlimmable) {
        const Rect si(geo::kSlimIconX, geo::kSlimIconY, geo::kSlimIconW, geo::kSlimIconH);
        if (si.contains(fx, fy)) {
            mSlimOpen = true;
            invalidate();
            return;
        }
    }

    // Knobs: start a vertical drag.
    for (int i = 0; i < geo::kKnobCount; ++i) {
        const geo::KnobSpec &k = geo::kKnobs[i];
        if (hitCircle(fx, fy, k.cx, k.cy, k.r)) {
            startDrag(k.id, fy);
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
void NamEditorView::onMouseMove(int x, int y)
{
    mMouseX = static_cast<float>(x);
    mMouseY = static_cast<float>(y);
    if (!mDragParam || !mController)
        return;
    double norm = mDragStartNorm + static_cast<double>(mDragStartY - y) / kKnobDragRange;
    norm = norm < 0.0 ? 0.0 : (norm > 1.0 ? 1.0 : norm);
    mController->setParamNormalized(mDragParam, norm);
    mController->performEdit(mDragParam, norm);
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
void NamEditorView::onMouseWheel(int x, int y, int delta)
{
    const float fx = static_cast<float>(x), fy = static_cast<float>(y);
    const double step = delta * 0.05;

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
        if (mHasInputLevel && calValueRect().contains(fx, fy))
            nudgeParam(kInputCalibrationLevelId, step);
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
    // Output mode (index 0/1/2 -> normalized 0/0.5/1). Normalized and
    // Calibrated need model output-level metadata.
    for (int i = 0; i < 3; ++i) {
        if (outputModeRow(i).contains(x, y)) {
            if (i > 0 && !mHasOutputLevel)
                return; // gated
            editParam(kOutputModeId, i * 0.5);
            invalidate();
            return;
        }
    }
    if (mHasInputLevel) {
        if (calibrateToggleRect().contains(x, y)) {
            editParam(kCalibrateInputId, paramValue(kCalibrateInputId) > 0.5 ? 0.0 : 1.0);
            invalidate();
            return;
        }
        if (calValueRect().contains(x, y))
            startDrag(kInputCalibrationLevelId, y); // drag to edit dBu
    }
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

void NamEditorView::ModelCapsChanged(bool slimmable, bool hasInputLevel, bool hasOutputLevel)
{
    mSlimmable = slimmable;
    mHasInputLevel = hasInputLevel;
    mHasOutputLevel = hasOutputLevel;
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
