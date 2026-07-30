// ImageCache — the seven raster art layers, loaded from the bundle as PNG.
//
// Cairo reads PNG natively (cairo_image_surface_create_from_png), so nothing
// links libpng directly. The layers are imported at @2x by gui/import_assets.sh
// and drawn scaled down to the 1x layout, which is why Canvas::drawImage picks
// a good-quality filter.
//
// Every load is checked: a missing or corrupt file yields a null surface, one
// warning, and a cached null so the warning does not repeat every frame. Null
// surfaces are silently skipped by the Canvas image calls, so the panel falls
// back to its flat-colour drawing instead of failing.

#pragma once

#include <cairo/cairo.h>

#include <map>
#include <string>

namespace NAMix
{

//------------------------------------------------------------------------
class ImageCache
{
public:
    ImageCache() = default;
    ~ImageCache();

    ImageCache(const ImageCache &) = delete;
    ImageCache &operator=(const ImageCache &) = delete;

    // Layers are loaded from <resourceDir>/img/<name>.png.
    void setResourceDir(const std::string &resourceDir)
    {
        mDir = resourceDir;
    }

    // Returns a surface owned by the cache — never destroy it — or null.
    cairo_surface_t *get(const char *name);

private:
    std::string mDir;
    std::map<std::string, cairo_surface_t *> mCache; // null entry = load failed
};

} // namespace NAMix
