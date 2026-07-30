// Canvas — the drawing surface the editor paints through.
//
// This is a thin, deliberately small wrapper over Cairo that exposes exactly
// the primitives the panel needs, named so the panel code reads the same way
// it does in the sibling Haiku port (which paints through a BView). Keeping
// the surface this narrow is what makes the two implementations comparable
// line for line.
//
// Two conventions are pinned down HERE, once, so no drawing code has to think
// about them:
//
//   * Rect is {x, y, w, h} with EXCLUSIVE edges — a Rect{10, 75, 30, 200}
//     covers x in [10, 40) and y in [75, 275), i.e. exactly 30x200 pixels.
//     This matches the original plug-in's IRECT semantics and the constants in
//     namgeometry.h. (Be's BRect is inclusive on both edges, so a rect built
//     as BRect(x, y, x+w, y+h) there is one pixel wider and taller than the
//     geometry says. Use Rect::fromLTRB() when porting such a construction so
//     the extra pixel does not silently come along.)
//
//   * Arc angles use the KNOB convention: degrees, 0 = straight up, positive =
//     clockwise. strokeArc() converts to Cairo's radians-clockwise-from-east
//     internally.
//
// Colours are 0xRRGGBB (as in namgeometry.h) plus an optional 0..255 alpha.

#pragma once

#include <cairo/cairo.h>

#include <cstdint>
#include <string>

namespace NAMix
{

class FontStack;

//------------------------------------------------------------------------
struct Rect {
    float x = 0, y = 0, w = 0, h = 0;

    constexpr Rect() = default;
    constexpr Rect(float ax, float ay, float aw, float ah) : x(ax), y(ay), w(aw), h(ah)
    {
    }

    // Build from left/top/right/bottom with EXCLUSIVE right/bottom.
    static constexpr Rect fromLTRB(float l, float t, float r, float b)
    {
        return Rect(l, t, r - l, b - t);
    }

    constexpr float left() const
    {
        return x;
    }
    constexpr float top() const
    {
        return y;
    }
    constexpr float right() const
    {
        return x + w;
    }
    constexpr float bottom() const
    {
        return y + h;
    }
    constexpr float centerX() const
    {
        return x + w * 0.5f;
    }
    constexpr float centerY() const
    {
        return y + h * 0.5f;
    }

    constexpr bool contains(float px, float py) const
    {
        return px >= x && px < x + w && py >= y && py < y + h;
    }

    constexpr Rect inset(float d) const
    {
        return Rect(x + d, y + d, w - 2 * d, h - 2 * d);
    }
};

//------------------------------------------------------------------------
// Which of the two bundled faces subsequent text uses. Sizes are set
// separately, mirroring SetFont/SetFontSize.
enum class Font { Body, Title };

//------------------------------------------------------------------------
class Canvas
{
public:
    // Does not take ownership of either argument; both must outlive the Canvas.
    Canvas(cairo_t *cr, const FontStack *fonts, float width, float height);

    Rect bounds() const
    {
        return Rect(0, 0, mWidth, mHeight);
    }
    cairo_t *cr() const
    {
        return mCr;
    }

    //--- state ---------------------------------------------------------
    void setColor(uint32_t rgb, int alpha = 255);
    void setPenSize(float px);

    //--- shapes --------------------------------------------------------
    void fillRect(const Rect &r);
    void fillRoundRect(const Rect &r, float radius);
    void strokeRoundRect(const Rect &r, float radius);
    void strokeLine(float x0, float y0, float x1, float y1);
    void fillEllipse(const Rect &r);
    void fillEllipse(float cx, float cy, float rx, float ry);
    void strokeEllipse(float cx, float cy, float rx, float ry);

    // Knob convention: degrees, 0 = up, positive = clockwise. Sets the colour
    // and pen size itself and restores the pen size afterwards, matching how
    // the panel calls it.
    void strokeArc(float cx, float cy, float radius, double a0Deg, double a1Deg, float pen,
                   uint32_t rgb);

    //--- images --------------------------------------------------------
    // Scale `image` to fill `dest`, alpha-composited. No-op when null, so a
    // failed art load degrades to whatever was drawn underneath.
    void drawImage(cairo_surface_t *image, const Rect &dest);
    // Draw at its own pixel size, centred on (cx, cy).
    void drawImageCentered(cairo_surface_t *image, float cx, float cy);

    //--- text ----------------------------------------------------------
    void setFont(Font f);
    void setFontSize(float px);
    // x, y is the baseline origin of the first glyph (as BView::DrawString).
    void drawString(const char *text, float x, float y);
    float stringWidth(const char *text) const;
    // Truncate with an ellipsis until it fits maxW at the current font.
    std::string clipToWidth(const std::string &s, float maxW) const;

    //--- clipping ------------------------------------------------------
    void pushClip(const Rect &r);
    void popClip();

private:
    void applyFont() const;

    cairo_t *mCr = nullptr;
    const FontStack *mFonts = nullptr;
    float mWidth = 0, mHeight = 0;
    Font mFont = Font::Body;
    float mFontSize = 12.0f;
};

} // namespace NAMix
