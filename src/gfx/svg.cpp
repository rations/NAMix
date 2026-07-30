// SvgCache implementation. See svg.h.

#include "svg.h"

#define NANOSVG_IMPLEMENTATION
#define NANOSVG_ALL_COLOR_KEYWORDS
#include "nanosvg/nanosvg.h"

#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvgrast.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <vector>

namespace NAMix
{

namespace
{

// NanoSVG -> Cairo pixel conversion: RGBA (non-premultiplied, byte order R,G,B,A)
// becomes ARGB32 (premultiplied, native-endian uint32 with A in the high byte).
// Doing this wrong is visible as bright halos around every antialiased icon
// edge, so it lives in exactly one place.
void rgbaToPremultipliedArgb(const uint8_t *src, unsigned char *dst, int w, int h, int dstStride)
{
    for (int y = 0; y < h; ++y) {
        uint32_t *out = reinterpret_cast<uint32_t *>(dst + static_cast<std::ptrdiff_t>(y) * dstStride);
        const uint8_t *in = src + static_cast<std::ptrdiff_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            const uint32_t r = in[0], g = in[1], b = in[2], a = in[3];
            // Rounded multiply-by-alpha: (v * a + 127) / 255.
            const uint32_t pr = (r * a + 127u) / 255u;
            const uint32_t pg = (g * a + 127u) / 255u;
            const uint32_t pb = (b * a + 127u) / 255u;
            out[x] = (a << 24) | (pr << 16) | (pg << 8) | pb;
            in += 4;
        }
    }
}

} // namespace

//------------------------------------------------------------------------
SvgCache::~SvgCache()
{
    for (auto &entry : mCache)
        if (entry.second)
            cairo_surface_destroy(entry.second);
    for (auto &entry : mDocs)
        if (entry.second)
            nsvgDelete(entry.second);
    if (mRasterizer)
        nsvgDeleteRasterizer(mRasterizer);
}

//------------------------------------------------------------------------
NSVGimage *SvgCache::document(const char *name)
{
    const std::string key(name);
    auto it = mDocs.find(key);
    if (it != mDocs.end())
        return it->second; // may be null: a previous failure, already warned

    const std::string path = mDir + "/img/" + key + ".svg";
    // nsvgParseFromFile takes the units and the DPI the SVG's own lengths are
    // interpreted in; 96 dpi is the CSS reference the icons are authored to.
    NSVGimage *doc = nsvgParseFromFile(path.c_str(), "px", 96.0f);
    if (!doc || doc->width <= 0.0f || doc->height <= 0.0f) {
        if (doc) {
            nsvgDelete(doc);
            doc = nullptr;
        }
        fprintf(stderr, "NAMix: missing or unreadable icon %s (drawing nothing)\n", path.c_str());
    }
    mDocs[key] = doc;
    return doc;
}

//------------------------------------------------------------------------
cairo_surface_t *SvgCache::get(const char *name, int w, int h)
{
    if (w <= 0 || h <= 0)
        return nullptr;

    const Key key{std::string(name), w, h};
    auto it = mCache.find(key);
    if (it != mCache.end())
        return it->second;

    NSVGimage *doc = document(name);
    if (!doc) {
        mCache[key] = nullptr;
        return nullptr;
    }

    if (!mRasterizer) {
        mRasterizer = nsvgCreateRasterizer();
        if (!mRasterizer) {
            fprintf(stderr, "NAMix: could not create the SVG rasterizer\n");
            mCache[key] = nullptr;
            return nullptr;
        }
    }

    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        mCache[key] = nullptr;
        return nullptr;
    }

    // Rasterise into a scratch RGBA buffer first: nsvgRasterize writes
    // non-premultiplied RGBA, which is not Cairo's in-memory layout.
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);

    // Uniform scale that fits the document into w x h, centred — so a request
    // whose aspect ratio differs from the icon's letterboxes rather than
    // distorting the glyph.
    const float scale =
        std::min(static_cast<float>(w) / doc->width, static_cast<float>(h) / doc->height);
    const float tx = (static_cast<float>(w) - doc->width * scale) * 0.5f;
    const float ty = (static_cast<float>(h) - doc->height * scale) * 0.5f;

    // nsvgRasterize takes one uniform scale and applies tx/ty after scaling
    // (verified against the vendored header), which is exactly the fit above.
    nsvgRasterize(mRasterizer, doc, tx, ty, scale, rgba.data(), w, h, w * 4);

    cairo_surface_flush(surface);
    rgbaToPremultipliedArgb(rgba.data(), cairo_image_surface_get_data(surface), w, h,
                            cairo_image_surface_get_stride(surface));
    cairo_surface_mark_dirty(surface);

    mCache[key] = surface;
    return surface;
}

//------------------------------------------------------------------------
cairo_surface_t *SvgCache::getByHeight(const char *name, int h)
{
    NSVGimage *doc = document(name);
    if (!doc || h <= 0)
        return nullptr;
    const int w = static_cast<int>(std::lround(static_cast<double>(h) * doc->width / doc->height));
    return get(name, std::max(w, 1), h);
}

//------------------------------------------------------------------------
cairo_surface_t *SvgCache::getByWidth(const char *name, int w)
{
    NSVGimage *doc = document(name);
    if (!doc || w <= 0)
        return nullptr;
    const int h = static_cast<int>(std::lround(static_cast<double>(w) * doc->height / doc->width));
    return get(name, w, std::max(h, 1));
}

} // namespace NAMix
