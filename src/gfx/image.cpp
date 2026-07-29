// ImageCache implementation. See image.h.

#include "image.h"

#include <cstdio>

namespace NAMix
{

//------------------------------------------------------------------------
ImageCache::~ImageCache()
{
    for (auto &entry : mCache)
        if (entry.second)
            cairo_surface_destroy(entry.second);
}

//------------------------------------------------------------------------
cairo_surface_t *ImageCache::get(const char *name)
{
    const std::string key(name);
    auto it = mCache.find(key);
    if (it != mCache.end())
        return it->second; // may be null: a previous failure, already warned

    const std::string path = mDir + "/img/" + key + ".png";
    cairo_surface_t *surface = cairo_image_surface_create_from_png(path.c_str());
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "NAMix: missing art %s (flat fallback)\n", path.c_str());
        cairo_surface_destroy(surface);
        surface = nullptr;
    }

    mCache[key] = surface;
    return surface;
}

} // namespace NAMix
