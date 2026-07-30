// panelrender — render the editor offline to a PNG.
//
// This is the graphics stack's test rig: it exercises the same Canvas, font,
// PNG and SVG code the plug-in editor uses, but with no X11, no host and no
// VST3 objects, so the panel's geometry and art can be checked before any
// windowing work exists and diffed against the original plug-in's own panel.
//
// Usage: panelrender <output.png> [resource-dir]
// The resource directory defaults to the usual runtime resolution
// (NAMIX_RESOURCE_DIR, the bundle layout, or an executable-relative dir).

#include "gfx/canvas.h"
#include "gfx/fontstack.h"
#include "gfx/image.h"
#include "gfx/svg.h"
#include "namgeometry.h"
#include "platform/respath.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace NAMix;

namespace
{

// The ten icons, by their upstream basenames, with the on-screen height each
// is drawn at. Sizes mirror the sibling port's icon set so the sheet below is
// directly comparable with it.
struct IconSpec {
    const char *name;
    int height;
};
constexpr IconSpec kIcons[] = {
    {"Gear", 26},      {"File", 18},      {"Cross", 16}, {"SlimmableIcon", 20}, {"IRIconOn", 22},
    {"IRIconOff", 22}, {"ModelIcon", 22}, {"Globe", 18}, {"ArrowLeft", 14},     {"ArrowRight", 14},
};
constexpr int kIconCount = static_cast<int>(sizeof(kIcons) / sizeof(kIcons[0]));

// A knob body, exactly as the panel draws it: face art, dim track arc, azure
// value arc, and the pointer notch. Reproduced here (rather than shared) so
// this rig stays independent of the editor's own class.
void drawKnobBody(Canvas &c, ImageCache &images, double norm, int cx, int cy, int r)
{
    const Rect face(cx - r, cy - r, 2.0f * r, 2.0f * r);
    if (cairo_surface_t *knob = images.get("KnobBackground")) {
        c.drawImage(knob, face);
    } else {
        c.setColor(0x28282E);
        c.fillEllipse(face);
    }

    const double a0 = -geo::kKnobSweepDeg / 2.0;
    const double aVal = a0 + norm * geo::kKnobSweepDeg;
    const float arcR = r * static_cast<float>(geo::kKnobArcFrac);
    c.strokeArc(cx, cy, arcR, a0, -a0, 3.0f, 0x2E2B34);
    if (norm > 0.001)
        c.strokeArc(cx, cy, arcR, a0, aVal, 3.0f, geo::kAzure);

    const double ang = aVal * 3.14159265358979323846 / 180.0;
    const float s = static_cast<float>(std::sin(ang)), co = static_cast<float>(std::cos(ang));
    const float lo = r * 0.45f, hi = r * static_cast<float>(geo::kKnobPointerFrac);
    c.setColor(geo::kAzure);
    c.setPenSize(3.0f);
    c.strokeLine(cx + lo * s, cy - lo * co, cx + hi * s, cy - hi * co);
    c.setPenSize(1.0f);
}

void drawMeter(Canvas &c, ImageCache &images, const geo::MeterRect &m, float level,
               const char *label)
{
    const Rect rect(m.x, m.y, m.w, m.h);
    if (cairo_surface_t *bg = images.get("MeterBackground")) {
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
        c.setColor(0x000000);
        for (float y = trackY + 2.0f; y < trackY + trackH; y += 2.0f)
            c.fillRect(Rect(trackX, y, trackW, 1.0f));
        c.setColor(0x6A9FF0);
        c.fillRect(Rect(trackX, fillTop, trackW, 1.0f));
    }

    c.setColor(geo::kDimColor);
    c.setFontSize(10);
    c.drawString(label, m.x + (m.w - c.stringWidth(label)) / 2.0f, m.y + m.h + 14);
}

} // namespace

//------------------------------------------------------------------------
int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <output.png> [resource-dir]\n", argv[0]);
        return 2;
    }
    const char *outPath = argv[1];
    const std::string res = (argc > 2) ? std::string(argv[2]) : resourceDir();
    if (res.empty()) {
        fprintf(stderr, "panelrender: no resource directory; pass one explicitly\n");
        return 1;
    }
    printf("panelrender: resources = %s\n", res.c_str());

    FontStack fonts;
    const bool fontsOk = fonts.load(res);
    ImageCache images;
    images.setResourceDir(res);
    SvgCache svgs;
    svgs.setResourceDir(res);

    cairo_surface_t *surface =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, geo::kWinW, geo::kWinH);
    cairo_t *cr = cairo_create(surface);
    Canvas c(cr, &fonts, geo::kWinW, geo::kWinH);

    // --- backdrop + line overlay ---
    if (cairo_surface_t *bg = images.get("Background")) {
        c.drawImage(bg, c.bounds());
    } else {
        c.setColor(geo::kBgColor);
        c.fillRect(c.bounds());
    }
    if (cairo_surface_t *lines = images.get("Lines"))
        c.drawImage(lines, c.bounds());

    // --- title, in the title face ---
    c.setFont(Font::Title);
    c.setFontSize(22);
    c.setColor(geo::kTextColor);
    {
        const char *title = "NAMix";
        c.drawString(title, geo::kTitleX + (geo::kTitleW - c.stringWidth(title)) / 2.0f,
                     geo::kTitleY + 34);
    }
    c.setFont(Font::Body);

    // --- the six knobs, swept across their range so every arc angle shows ---
    for (int i = 0; i < geo::kKnobCount; ++i) {
        const geo::KnobSpec &k = geo::kKnobs[i];
        const double norm = static_cast<double>(i) / (geo::kKnobCount - 1);
        drawKnobBody(c, images, norm, k.cx, k.cy, k.r);

        c.setColor(geo::kTextColor);
        c.setFontSize(12);
        c.drawString(k.label, k.cx - c.stringWidth(k.label) / 2.0f, k.cy - geo::kKnobLabelDY);

        char val[32];
        snprintf(val, sizeof(val), "%.1f%s%s", norm * 10.0, k.unit ? " " : "",
                 k.unit ? k.unit : "");
        c.setColor(geo::kDimColor);
        c.setFontSize(11);
        c.drawString(val, k.cx - c.stringWidth(val) / 2.0f, k.cy + geo::kKnobValueDY);
    }

    // --- the two toggles, one on and one off ---
    for (int i = 0; i < geo::kToggleCount; ++i) {
        const geo::ToggleSpec &t = geo::kToggles[i];
        const bool on = (i == 0);
        const Rect pill(t.cx - geo::kToggleW / 2.0f, t.cy - geo::kToggleH / 2.0f, geo::kToggleW,
                        geo::kToggleH);
        c.setColor(on ? geo::kAzure : 0x2A2730);
        c.fillRoundRect(pill, geo::kToggleH / 2.0f);

        const float hs = 18.0f;
        const float hx = on ? pill.right() - hs - 2.0f : pill.left() + 2.0f;
        const Rect handle(hx, t.cy - hs / 2.0f, hs, hs);
        if (cairo_surface_t *sw = images.get("SlideSwitchHandle")) {
            c.drawImage(sw, handle);
        } else {
            c.setColor(geo::kTextColor);
            c.fillEllipse(handle);
        }

        c.setColor(geo::kTextColor);
        c.setFontSize(11);
        c.drawString(t.label, t.cx - c.stringWidth(t.label) / 2.0f,
                     t.cy + geo::kToggleH / 2.0f + 15);
    }

    // --- the two file rows, one loaded and one empty ---
    const geo::FileRow *rows[2] = {&geo::kModelRow, &geo::kIrRow};
    for (int i = 0; i < 2; ++i) {
        const geo::FileRow &row = *rows[i];
        const bool model = (i == 0);
        const bool loaded = model;
        const Rect rect(row.x, row.y, row.w, row.h);
        if (cairo_surface_t *fb = images.get("FileBackground")) {
            c.drawImage(fb, rect);
        } else {
            c.setColor(0x14161B);
            c.fillRoundRect(rect, 6.0f);
        }

        const float cy = row.y + row.h / 2.0f;
        c.drawImageCentered(
            svgs.getByHeight(model ? "ModelIcon" : (loaded ? "IRIconOn" : "IRIconOff"), 22),
            geo::kRowIconCX, cy);
        c.drawImageCentered(svgs.getByHeight("File", 18), row.x + 16, cy);
        c.drawImageCentered(svgs.getByHeight("ArrowLeft", 14), row.x + 40, cy);
        c.drawImageCentered(svgs.getByHeight("ArrowRight", 14), row.x + 60, cy);

        c.setFontSize(12);
        const std::string text =
            loaded ? std::string("A Very Long Example Capture Name.nam") : row.placeholder;
        c.setColor(loaded ? geo::kTextColor : geo::kDimColor);
        const float midL = row.x + 78, midR = row.x + row.w - 30;
        const std::string disp = c.clipToWidth(text, midR - midL);
        c.drawString(disp.c_str(), midL + (midR - midL - c.stringWidth(disp.c_str())) / 2.0f,
                     cy + 4);

        c.drawImageCentered(svgs.getByHeight(loaded ? "Cross" : "Globe", loaded ? 16 : 18),
                            row.x + row.w - 16, cy);
    }

    // --- meters at two different levels ---
    drawMeter(c, images, geo::kInputMeter, 0.62f, "IN");
    drawMeter(c, images, geo::kOutputMeter, 0.35f, "OUT");

    // --- gear + slimmable icon, at their layout positions ---
    c.drawImageCentered(svgs.getByHeight("Gear", 26), geo::kGearCX, geo::kGearCY);
    c.drawImageCentered(svgs.getByHeight("SlimmableIcon", geo::kSlimIconH),
                        geo::kSlimIconX + geo::kSlimIconW / 2.0f,
                        geo::kSlimIconY + geo::kSlimIconH / 2.0f);

    cairo_destroy(cr);

    const cairo_status_t status = cairo_surface_write_to_png(surface, outPath);
    cairo_surface_destroy(surface);
    if (status != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "panelrender: could not write %s: %s\n", outPath,
                cairo_status_to_string(status));
        return 1;
    }

    // Report what actually resolved, so a silent fallback cannot pass as a
    // clean run: every asset the panel needs must load.
    int missing = 0;
    const char *layers[] = {"Background",          "Lines",           "KnobBackground",
                            "FileBackground",      "MeterBackground", "SlideSwitchHandle",
                            "InputLevelBackground"};
    for (const char *l : layers)
        if (!images.get(l))
            ++missing;
    for (int i = 0; i < kIconCount; ++i)
        if (!svgs.getByHeight(kIcons[i].name, kIcons[i].height))
            ++missing;
    if (!fontsOk)
        ++missing;

    printf("panelrender: wrote %s (%dx%d)\n", outPath, geo::kWinW, geo::kWinH);
    if (missing) {
        fprintf(stderr, "panelrender: %d asset(s) failed to load\n", missing);
        return 1;
    }
    printf("panelrender: all 17 art assets and both fonts loaded\n");
    return 0;
}
