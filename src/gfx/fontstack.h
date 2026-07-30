// FontStack — the editor's two typefaces, loaded straight out of the plug-in
// bundle with FreeType and wrapped as Cairo font faces.
//
// Michroma is the title face and Roboto the body face, matching the original
// plug-in. Unlike app_server on Haiku, FreeType needs no font *installation*
// step: the .ttf files in Contents/Resources/fonts are opened in place, so the
// sibling port's installFonts() has no counterpart here.
//
// If a face fails to load, face() falls back to a generic Cairo "toy" face of
// the same weight so text still renders (one warning on stderr, then silence).

#pragma once

#include <cairo/cairo.h>

#include <string>

namespace NAMix
{

//------------------------------------------------------------------------
class FontStack
{
public:
    FontStack() = default;
    ~FontStack();

    FontStack(const FontStack &) = delete;
    FontStack &operator=(const FontStack &) = delete;

    // Load both faces from <resourceDir>/fonts. Safe to call once; returns
    // false if either face fell back, having already warned.
    bool load(const std::string &resourceDir);

    cairo_font_face_t *title() const
    {
        return mTitle;
    }
    cairo_font_face_t *body() const
    {
        return mBody;
    }

private:
    // Owns the returned face; sets *outFt to the FT_Face to destroy later.
    cairo_font_face_t *loadFace(const std::string &path, bool bold, void **outFt);

    cairo_font_face_t *mTitle = nullptr;
    cairo_font_face_t *mBody = nullptr;
    void *mTitleFt = nullptr; // FT_Face, destroyed after the cairo face
    void *mBodyFt = nullptr;
    void *mLibrary = nullptr; // FT_Library
};

} // namespace NAMix
