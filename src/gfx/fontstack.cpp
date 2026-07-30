// FontStack implementation. See fontstack.h.

#include "fontstack.h"

#include <cairo/cairo-ft.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <cstdio>

namespace NAMix
{

//------------------------------------------------------------------------
FontStack::~FontStack()
{
    // Destroy the cairo faces before the FT_Faces they wrap, and the library
    // last: cairo holds a reference to the FT_Face for as long as the font
    // face lives.
    if (mTitle)
        cairo_font_face_destroy(mTitle);
    if (mBody)
        cairo_font_face_destroy(mBody);
    if (mTitleFt)
        FT_Done_Face(static_cast<FT_Face>(mTitleFt));
    if (mBodyFt)
        FT_Done_Face(static_cast<FT_Face>(mBodyFt));
    if (mLibrary)
        FT_Done_FreeType(static_cast<FT_Library>(mLibrary));
}

//------------------------------------------------------------------------
cairo_font_face_t *FontStack::loadFace(const std::string &path, bool bold, void **outFt)
{
    *outFt = nullptr;

    FT_Face face = nullptr;
    if (mLibrary) {
        const FT_Error err = FT_New_Face(static_cast<FT_Library>(mLibrary), path.c_str(), 0, &face);
        if (err != 0)
            face = nullptr;
    }

    if (face) {
        cairo_font_face_t *cf = cairo_ft_font_face_create_for_ft_face(face, 0);
        if (cf && cairo_font_face_status(cf) == CAIRO_STATUS_SUCCESS) {
            *outFt = face;
            return cf;
        }
        if (cf)
            cairo_font_face_destroy(cf);
        FT_Done_Face(face);
    }

    // Fallback: a generic toy face, so the editor still shows readable text.
    fprintf(stderr, "NAMix: could not load font %s (using a system fallback)\n", path.c_str());
    return cairo_toy_font_face_create("sans-serif", CAIRO_FONT_SLANT_NORMAL,
                                      bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
}

//------------------------------------------------------------------------
bool FontStack::load(const std::string &resourceDir)
{
    FT_Library lib = nullptr;
    if (FT_Init_FreeType(&lib) == 0)
        mLibrary = lib;
    else
        fprintf(stderr, "NAMix: FreeType failed to initialise (using system fallbacks)\n");

    const std::string dir = resourceDir + "/fonts/";
    mTitle = loadFace(dir + "Michroma-Regular.ttf", true, &mTitleFt);
    mBody = loadFace(dir + "Roboto-Regular.ttf", false, &mBodyFt);

    return mTitleFt != nullptr && mBodyFt != nullptr;
}

} // namespace NAMix
