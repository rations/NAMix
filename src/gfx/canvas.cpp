// Canvas implementation. See canvas.h for the rect and angle conventions.

#include "canvas.h"
#include "fontstack.h"

#include <cmath>

namespace NAMix
{

namespace
{

constexpr double kPi = 3.14159265358979323846;

// Append a rounded-rect path. Cairo has no primitive for it; four arcs joined
// by the straight edges is the standard construction.
void roundRectPath(cairo_t *cr, const Rect &r, float radius)
{
    const double maxR = std::min(r.w, r.h) * 0.5;
    const double rad = std::min(static_cast<double>(radius), maxR);
    if (rad <= 0.0) {
        cairo_rectangle(cr, r.x, r.y, r.w, r.h);
        return;
    }
    const double l = r.left(), t = r.top(), rt = r.right(), b = r.bottom();
    cairo_new_sub_path(cr);
    cairo_arc(cr, rt - rad, t + rad, rad, -0.5 * kPi, 0.0); // top-right
    cairo_arc(cr, rt - rad, b - rad, rad, 0.0, 0.5 * kPi);  // bottom-right
    cairo_arc(cr, l + rad, b - rad, rad, 0.5 * kPi, kPi);   // bottom-left
    cairo_arc(cr, l + rad, t + rad, rad, kPi, 1.5 * kPi);   // top-left
    cairo_close_path(cr);
}

} // namespace

//------------------------------------------------------------------------
Canvas::Canvas(cairo_t *cr, const FontStack *fonts, float width, float height)
    : mCr(cr), mFonts(fonts), mWidth(width), mHeight(height)
{
    cairo_set_line_cap(mCr, CAIRO_LINE_CAP_BUTT);
    cairo_set_line_join(mCr, CAIRO_LINE_JOIN_MITER);
    applyFont();
}

//------------------------------------------------------------------------
void Canvas::setColor(uint32_t rgb, int alpha)
{
    cairo_set_source_rgba(mCr, ((rgb >> 16) & 0xff) / 255.0, ((rgb >> 8) & 0xff) / 255.0,
                          (rgb & 0xff) / 255.0, alpha / 255.0);
}

void Canvas::setPenSize(float px)
{
    cairo_set_line_width(mCr, px);
}

//------------------------------------------------------------------------
void Canvas::fillRect(const Rect &r)
{
    cairo_rectangle(mCr, r.x, r.y, r.w, r.h);
    cairo_fill(mCr);
}

void Canvas::fillRoundRect(const Rect &r, float radius)
{
    roundRectPath(mCr, r, radius);
    cairo_fill(mCr);
}

void Canvas::strokeRoundRect(const Rect &r, float radius)
{
    // Half-pixel offset so a 1px stroke lands ON the boundary rather than
    // straddling two pixel rows.
    roundRectPath(mCr, Rect(r.x + 0.5f, r.y + 0.5f, r.w - 1.0f, r.h - 1.0f), radius);
    cairo_stroke(mCr);
}

void Canvas::strokeLine(float x0, float y0, float x1, float y1)
{
    cairo_move_to(mCr, x0, y0);
    cairo_line_to(mCr, x1, y1);
    cairo_stroke(mCr);
}

void Canvas::fillEllipse(const Rect &r)
{
    fillEllipse(r.centerX(), r.centerY(), r.w * 0.5f, r.h * 0.5f);
}

void Canvas::fillEllipse(float cx, float cy, float rx, float ry)
{
    if (rx <= 0.0f || ry <= 0.0f)
        return;
    cairo_save(mCr);
    cairo_translate(mCr, cx, cy);
    cairo_scale(mCr, rx, ry);
    cairo_arc(mCr, 0.0, 0.0, 1.0, 0.0, 2.0 * kPi);
    cairo_restore(mCr); // restore before filling: keeps the pen circular
    cairo_fill(mCr);
}

void Canvas::strokeEllipse(float cx, float cy, float rx, float ry)
{
    if (rx <= 0.0f || ry <= 0.0f)
        return;
    cairo_save(mCr);
    cairo_translate(mCr, cx, cy);
    cairo_scale(mCr, rx, ry);
    cairo_arc(mCr, 0.0, 0.0, 1.0, 0.0, 2.0 * kPi);
    cairo_restore(mCr);
    cairo_stroke(mCr);
}

//------------------------------------------------------------------------
// Knob convention -> Cairo. Knob angles are degrees with 0 straight up and
// positive clockwise; Cairo's cairo_arc takes radians measured clockwise from
// east in a y-down space. An angle A therefore maps to (A - 90) degrees, and
// because both conventions run clockwise the sweep direction is preserved —
// cairo_arc always goes in increasing-angle (clockwise) order, which is what
// a0 < a1 means here too.
void Canvas::strokeArc(float cx, float cy, float radius, double a0Deg, double a1Deg, float pen,
                       uint32_t rgb)
{
    if (radius <= 0.0f)
        return;
    setColor(rgb);
    setPenSize(pen);
    const double a0 = (a0Deg - 90.0) * kPi / 180.0;
    const double a1 = (a1Deg - 90.0) * kPi / 180.0;
    cairo_new_sub_path(mCr);
    if (a1 >= a0)
        cairo_arc(mCr, cx, cy, radius, a0, a1);
    else
        cairo_arc_negative(mCr, cx, cy, radius, a0, a1);
    cairo_stroke(mCr);
    setPenSize(1.0f);
}

//------------------------------------------------------------------------
void Canvas::drawImage(cairo_surface_t *image, const Rect &dest)
{
    if (!image || dest.w <= 0.0f || dest.h <= 0.0f)
        return;
    const int iw = cairo_image_surface_get_width(image);
    const int ih = cairo_image_surface_get_height(image);
    if (iw <= 0 || ih <= 0)
        return;

    cairo_save(mCr);
    cairo_translate(mCr, dest.x, dest.y);
    cairo_scale(mCr, dest.w / static_cast<double>(iw), dest.h / static_cast<double>(ih));
    cairo_set_source_surface(mCr, image, 0.0, 0.0);
    // GOOD filtering: the raster layers are imported at @2x and drawn down to
    // 1x, so the downscale quality is visible on every panel edge.
    cairo_pattern_set_filter(cairo_get_source(mCr), CAIRO_FILTER_GOOD);
    cairo_rectangle(mCr, 0.0, 0.0, iw, ih);
    cairo_fill(mCr);
    cairo_restore(mCr);
}

void Canvas::drawImageCentered(cairo_surface_t *image, float cx, float cy)
{
    if (!image)
        return;
    const float w = static_cast<float>(cairo_image_surface_get_width(image));
    const float h = static_cast<float>(cairo_image_surface_get_height(image));
    drawImage(image, Rect(cx - w * 0.5f, cy - h * 0.5f, w, h));
}

//------------------------------------------------------------------------
void Canvas::applyFont() const
{
    if (mFonts) {
        cairo_font_face_t *face = (mFont == Font::Title) ? mFonts->title() : mFonts->body();
        if (face)
            cairo_set_font_face(mCr, face);
    }
    cairo_set_font_size(mCr, mFontSize);
}

void Canvas::setFont(Font f)
{
    mFont = f;
    applyFont();
}

void Canvas::setFontSize(float px)
{
    mFontSize = px;
    cairo_set_font_size(mCr, px);
}

void Canvas::drawString(const char *text, float x, float y)
{
    if (!text || !*text)
        return;
    cairo_move_to(mCr, x, y);
    cairo_show_text(mCr, text);
    cairo_new_path(mCr); // show_text leaves the current point set
}

float Canvas::stringWidth(const char *text) const
{
    if (!text || !*text)
        return 0.0f;
    cairo_text_extents_t ext;
    cairo_text_extents(mCr, text, &ext);
    // x_advance, not width: this is where the next glyph would start, which is
    // what centring and truncation need.
    return static_cast<float>(ext.x_advance);
}

std::string Canvas::clipToWidth(const std::string &s, float maxW) const
{
    if (stringWidth(s.c_str()) <= maxW)
        return s;

    // Trim whole UTF-8 characters off the end until the string plus an
    // ellipsis fits. Cutting mid-sequence would emit a replacement glyph.
    std::string cut = s;
    while (!cut.empty()) {
        // Step back over one UTF-8 character.
        size_t n = cut.size() - 1;
        while (n > 0 && (static_cast<unsigned char>(cut[n]) & 0xC0) == 0x80)
            --n;
        cut.resize(n);
        if (cut.empty())
            break;
        const std::string candidate = cut + "\xE2\x80\xA6"; // U+2026 HORIZONTAL ELLIPSIS
        if (stringWidth(candidate.c_str()) <= maxW)
            return candidate;
    }
    return std::string();
}

//------------------------------------------------------------------------
void Canvas::pushClip(const Rect &r)
{
    cairo_save(mCr);
    cairo_rectangle(mCr, r.x, r.y, r.w, r.h);
    cairo_clip(mCr);
}

void Canvas::popClip()
{
    cairo_restore(mCr);
    // cairo_restore also rolls back the font selection made before the save.
    applyFont();
}

} // namespace NAMix
