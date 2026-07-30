// SvgCache — runtime SVG rasterisation for the ten NAM icons.
//
// The icons ship as the original plug-in's own .svg files, copied verbatim,
// and are rasterised here by NanoSVG — the same rasteriser the original
// renders them with — so glyph shapes match by construction and stay
// resolution-independent. That is the whole reason for not baking them into
// fixed-size PNGs.
//
// Each icon is parsed once and rasterised on demand at whatever pixel size the
// layout asks for; results are cached per (icon, width, height). A failed
// parse or rasterisation yields a null surface, which every Canvas image call
// treats as "draw nothing" — so a missing or broken icon degrades instead of
// crashing (one warning per icon, then silence).
//
// NanoSVG writes NON-premultiplied RGBA; Cairo's ARGB32 wants premultiplied
// BGRA in native byte order. The conversion happens on upload here, once, so
// no drawing code has to think about it — without it the icons show bright
// fringing wherever they are antialiased against the dark panel.

#pragma once

#include <cairo/cairo.h>

#include <map>
#include <string>
#include <utility>

struct NSVGimage;
struct NSVGrasterizer;

namespace NAMix
{

//------------------------------------------------------------------------
class SvgCache
{
public:
    SvgCache() = default;
    ~SvgCache();

    SvgCache(const SvgCache &) = delete;
    SvgCache &operator=(const SvgCache &) = delete;

    // Icons are loaded from <resourceDir>/img/<name>.svg.
    void setResourceDir(const std::string &resourceDir)
    {
        mDir = resourceDir;
    }

    // Rasterise `name` (upstream basename, no extension) to w x h pixels.
    // Returns a surface owned by the cache — never destroy it — or null.
    cairo_surface_t *get(const char *name, int w, int h);

    // Rasterise at the icon's own aspect ratio, scaled to the given height.
    cairo_surface_t *getByHeight(const char *name, int h);

    // Same, driven by width instead. Wide icons (ModelIcon is 121x36) must be
    // sized this way: giving them the row height the near-square icons use
    // makes them several times too wide and they collide with their
    // neighbours.
    cairo_surface_t *getByWidth(const char *name, int w);

private:
    struct Key {
        std::string name;
        int w, h;
        bool operator<(const Key &o) const
        {
            return std::tie(name, w, h) < std::tie(o.name, o.w, o.h);
        }
    };

    // Parsed document, kept so repeated size requests do not re-parse.
    NSVGimage *document(const char *name);

    std::string mDir;
    NSVGrasterizer *mRasterizer = nullptr;
    std::map<std::string, NSVGimage *> mDocs; // null entry = load already failed
    std::map<Key, cairo_surface_t *> mCache;
};

} // namespace NAMix
