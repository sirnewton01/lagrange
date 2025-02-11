/* Copyright 2020 Jaakko Keränen <jaakko.keranen@iki.fi>

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. */

#include "text.h"
#include "color.h"
#include "metrics.h"
#include "embedded.h"
#include "window.h"
#include "app.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "../stb_truetype.h"

#include <the_Foundation/array.h>
#include <the_Foundation/file.h>
#include <the_Foundation/fileinfo.h>
#include <the_Foundation/hash.h>
#include <the_Foundation/math.h>
#include <the_Foundation/stringlist.h>
#include <the_Foundation/regexp.h>
#include <the_Foundation/path.h>
#include <the_Foundation/ptrset.h>
#include <the_Foundation/vec2.h>

#include <SDL_surface.h>
#include <SDL_hints.h>
#include <SDL_version.h>
#include <stdarg.h>

#if defined (LAGRANGE_ENABLE_HARFBUZZ)
#   include <hb.h>
#endif

#if defined (LAGRANGE_ENABLE_FRIBIDI)
#   include <fribidi/fribidi.h>
#endif

#if SDL_VERSION_ATLEAST(2, 0, 10)
#   define LAGRANGE_RASTER_DEPTH    8
#   define LAGRANGE_RASTER_FORMAT   SDL_PIXELFORMAT_INDEX8
#else
#   define LAGRANGE_RASTER_DEPTH    32
#   define LAGRANGE_RASTER_FORMAT   SDL_PIXELFORMAT_RGBA8888
#endif

iDeclareType(Font)
iDeclareType(Glyph)
iDeclareTypeConstructionArgs(Glyph, iChar ch)

static const float contentScale_Text_ = 1.3f;

int gap_Text;                           /* cf. gap_UI in metrics.h */
int enableHalfPixelGlyphs_Text = iTrue; /* debug setting */
int enableKerning_Text         = iTrue; /* looking up kern pairs is slow */

enum iGlyphFlag {
    rasterized0_GlyphFlag = iBit(1),    /* zero offset */
    rasterized1_GlyphFlag = iBit(2),    /* half-pixel offset */
};

struct Impl_Glyph {
    iHashNode node;
    int flags;
    iFont *font; /* may come from symbols/emoji */
    iRect rect[2]; /* zero and half pixel offset */
    iInt2 d[2];
    float advance; /* scaled */
};

void init_Glyph(iGlyph *d, uint32_t glyphIndex) {
    d->node.key   = glyphIndex;
    d->flags      = 0;
    d->font       = NULL;
    d->rect[0]    = zero_Rect();
    d->rect[1]    = zero_Rect();
    d->advance    = 0.0f;
}

void deinit_Glyph(iGlyph *d) {
    iUnused(d);
}

static uint32_t index_Glyph_(const iGlyph *d) {
    return d->node.key;
}

iLocalDef iBool isRasterized_Glyph_(const iGlyph *d, int hoff) {
    return (d->flags & (rasterized0_GlyphFlag << hoff)) != 0;
}

iLocalDef iBool isFullyRasterized_Glyph_(const iGlyph *d) {
    return (d->flags & (rasterized0_GlyphFlag | rasterized1_GlyphFlag)) ==
           (rasterized0_GlyphFlag | rasterized1_GlyphFlag);
}

iLocalDef void setRasterized_Glyph_(iGlyph *d, int hoff) {
    d->flags |= rasterized0_GlyphFlag << hoff;
}

iDefineTypeConstructionArgs(Glyph, (iChar ch), ch)

/*-----------------------------------------------------------------------------------------------*/

struct Impl_Font {
    iBlock *       data;
    enum iTextFont family;
    stbtt_fontinfo font;
    float          xScale, yScale;
    int            vertOffset; /* offset due to scaling */
    int            height;
    int            baseline;
    iHash          glyphs; /* key is glyph index in the font */ /* TODO: does not need to be a Hash */
    iBool          isMonospaced;
    enum iFontSize sizeId;  /* used to look up different fonts of matching size */
    uint32_t       indexTable[128 - 32]; /* quick ASCII lookup */
#if defined (LAGRANGE_ENABLE_HARFBUZZ)
    hb_blob_t *    hbBlob; /* raw TrueType data */
    hb_face_t *    hbFace;
    hb_font_t *    hbMainFont;
    hb_font_t *    hbFont; /* may be a sub-font with customized font metrics */
#endif
};

static iFont *font_Text_(enum iFontId id);

#if 0
static hb_position_t hbGlyphHKernForNunito_(hb_font_t *font, void *fontData,
                                            hb_codepoint_t firstGlyph, hb_codepoint_t secondGlyph,
                                            void *userData) {
    return 100;
}
#endif

static void init_Font(iFont *d, const iBlock *data, int height, float scale,
                      enum iFontSize sizeId, iBool isMonospaced) {
    init_Hash(&d->glyphs);
    d->data = NULL;
    d->family = undefined_TextFont;
    /* Note: We only use `family` currently for applying a kerning fix to Nunito. */
    if (data == &fontNunitoRegular_Embedded ||
        data == &fontNunitoBold_Embedded ||
        data == &fontNunitoExtraBold_Embedded ||
        //data == &fontNunitoLightItalic_Embedded ||
        data == &fontNunitoExtraLight_Embedded) {
        d->family = nunito_TextFont;
    }
    else if (//data == &fontScheherazadeNewRegular_Embedded) {
             data == &fontNotoSansArabicUIRegular_Embedded) {
        d->family = arabic_TextFont;
    }
    else if (data == &fontNotoSansSymbolsRegular_Embedded ||
             data == &fontNotoSansSymbols2Regular_Embedded ||
             data == &fontNotoEmojiRegular_Embedded ||
             data == &fontSmolEmojiRegular_Embedded) {
        d->family = emojiAndSymbols_TextFont;
    }
    d->isMonospaced = isMonospaced;
    d->height = height;
    iZap(d->font);
    stbtt_InitFont(&d->font, constData_Block(data), 0);
    int ascent, descent;
    stbtt_GetFontVMetrics(&d->font, &ascent, &descent, NULL);
    d->xScale = d->yScale = stbtt_ScaleForPixelHeight(&d->font, height) * scale;
    if (d->isMonospaced) {
        /* It is important that monospaced fonts align 1:1 with the pixel grid so that
           box-drawing characters don't have partially occupied edge pixels, leading to seams
           between adjacent glyphs. */
        int adv;
        stbtt_GetCodepointHMetrics(&d->font, 'M', &adv, NULL);
        const float advance = (float) adv * d->xScale;
        if (advance > 4) { /* not too tiny */
            d->xScale *= floorf(advance) / advance;
        }
    }
    d->baseline   = ascent * d->yScale;
    d->vertOffset = height * (1.0f - scale) / 2;
    /* Custom tweaks. */
    if (data == &fontNotoSansSymbolsRegular_Embedded) {
        d->vertOffset *= 1.2f; 
    }
    else if (data == &fontNotoSansSymbols2Regular_Embedded) {
        d->vertOffset /= 2;
    }
    else if (data == &fontNotoEmojiRegular_Embedded) {
        //d->vertOffset -= height / 30;
    }
    d->sizeId = sizeId;
    memset(d->indexTable, 0xff, sizeof(d->indexTable));
#if defined(LAGRANGE_ENABLE_HARFBUZZ)
    /* HarfBuzz will read the font data. */ {
        d->hbBlob = hb_blob_create(constData_Block(data), size_Block(data),
                                   HB_MEMORY_MODE_READONLY, NULL, NULL);
        d->hbFace = hb_face_create(d->hbBlob, 0);
        d->hbMainFont = hb_font_create(d->hbFace);
#if 0
        /* TODO: The custom kerning function doesn't get called?
           Maybe HarfBuzz needs FreeType to do kerning? */
        if (d->family == nunito_TextFont) {
            /* Customize the kerning of Nunito. */
            d->hbFont = hb_font_create_sub_font(d->hbMainFont);
            hb_font_funcs_t *ffs = hb_font_funcs_create();
            hb_font_funcs_set_glyph_h_kerning_func(ffs, hbGlyphHKernForNunito_, d, NULL);
            hb_font_set_funcs(d->hbFont, ffs, NULL, NULL);
            hb_font_funcs_destroy(ffs);
        }
        else
#endif
        {
            d->hbFont = hb_font_reference(d->hbMainFont);
        }
    }
#endif
}

static void clearGlyphs_Font_(iFont *d) {
    iForEach(Hash, i, &d->glyphs) {
        delete_Glyph((iGlyph *) i.value);
    }
    clear_Hash(&d->glyphs);
}

static void deinit_Font(iFont *d) {
#if defined(LAGRANGE_ENABLE_HARFBUZZ)
    /* HarfBuzz objects. */ {
        hb_font_destroy(d->hbFont);
        hb_font_destroy(d->hbMainFont);
        hb_face_destroy(d->hbFace);
        hb_blob_destroy(d->hbBlob);
    }
#endif
    clearGlyphs_Font_(d);
    deinit_Hash(&d->glyphs);
    delete_Block(d->data);
}

static uint32_t glyphIndex_Font_(iFont *d, iChar ch) {
    /* TODO: Add a small cache of ~5 most recently found indices. */
    const size_t entry = ch - 32;
    if (entry < iElemCount(d->indexTable)) {
        if (d->indexTable[entry] == ~0u) {
            d->indexTable[entry] = stbtt_FindGlyphIndex(&d->font, ch);
        }
        return d->indexTable[entry];
    }
    return stbtt_FindGlyphIndex(&d->font, ch);
}

/*----------------------------------------------------------------------------------------------*/

iDeclareType(Text)
iDeclareType(CacheRow)

struct Impl_CacheRow {
    int   height;
    iInt2 pos;
};

struct Impl_Text {
    enum iTextFont contentFont;
    enum iTextFont headingFont;
    float          contentFontSize;
    iFont          fonts[max_FontId];
    SDL_Renderer * render;
    SDL_Texture *  cache;
    iInt2          cacheSize;
    int            cacheRowAllocStep;
    int            cacheBottom;
    iArray         cacheRows;
    SDL_Palette *  grayscale;
    iRegExp *      ansiEscape;
};

static iText   text_;
static iBlock *userFont_;

static void initFonts_Text_(iText *d) {
    const float   textSize      = fontSize_UI * d->contentFontSize;
    const float   monoSize      = textSize * 0.71f;
    const float   smallMonoSize = monoSize * 0.8f;
    const iBlock *regularFont   = &fontNunitoRegular_Embedded;
    const iBlock *boldFont      = &fontNunitoBold_Embedded;
    const iBlock *italicFont    = &fontNunitoLightItalic_Embedded;
    const iBlock *h12Font       = &fontNunitoExtraBold_Embedded;
    const iBlock *h3Font        = &fontNunitoRegular_Embedded;
    const iBlock *lightFont     = &fontNunitoExtraLight_Embedded;
    float         scaling       = 1.0f; /* glyph scaling (<=1.0), for increasing line spacing */
    float         italicScaling = 1.0f;
    float         lightScaling  = 1.0f;
    float         h123Scaling   = 1.0f; /* glyph scaling (<=1.0), for increasing line spacing */
    if (d->contentFont == firaSans_TextFont) {
        regularFont = &fontFiraSansRegular_Embedded;
        boldFont    = &fontFiraSansSemiBold_Embedded;
        lightFont   = &fontFiraSansLight_Embedded;
        italicFont  = &fontFiraSansItalic_Embedded;
        scaling     = italicScaling = lightScaling = 0.85f;
    }
    else if (d->contentFont == tinos_TextFont) {
        regularFont = &fontTinosRegular_Embedded;
        boldFont    = &fontTinosBold_Embedded;
        lightFont   = &fontLiterataExtraLightopsz18_Embedded;
        italicFont  = &fontTinosItalic_Embedded;
        scaling      = italicScaling = 0.85f;
    }
    else if (d->contentFont == literata_TextFont) {
        regularFont = &fontLiterataRegularopsz14_Embedded;
        boldFont    = &fontLiterataBoldopsz36_Embedded;
        italicFont  = &fontLiterataLightItalicopsz10_Embedded;
        lightFont   = &fontLiterataExtraLightopsz18_Embedded;
    }
    else if (d->contentFont == sourceSans3_TextFont) {
        regularFont = &fontSourceSans3Regular_Embedded;
        boldFont    = &fontSourceSans3Semibold_Embedded;
        italicFont  = &fontSourceSans3It_Embedded;
        lightFont   = &fontSourceSans3ExtraLight_Embedded;
    }
    else if (d->contentFont == iosevka_TextFont) {
        regularFont = &fontIosevkaTermExtended_Embedded;
        boldFont    = &fontIosevkaTermExtended_Embedded;
        italicFont  = &fontIosevkaTermExtended_Embedded;
        lightFont   = &fontIosevkaTermExtended_Embedded;
        scaling     = italicScaling = lightScaling = 0.866f;
    }
    if (d->headingFont == firaSans_TextFont) {
        h12Font     = &fontFiraSansBold_Embedded;
        h3Font      = &fontFiraSansRegular_Embedded;
        h123Scaling = 0.85f;
    }
    else if (d->headingFont == tinos_TextFont) {
        h12Font = &fontTinosBold_Embedded;
        h3Font  = &fontTinosRegular_Embedded;
        h123Scaling = 0.85f;
    }
    else if (d->headingFont == literata_TextFont) {
        h12Font = &fontLiterataBoldopsz36_Embedded;
        h3Font  = &fontLiterataRegularopsz14_Embedded;
    }
    else if (d->headingFont == sourceSans3_TextFont) {
        h12Font = &fontSourceSans3Bold_Embedded;
        h3Font = &fontSourceSans3Regular_Embedded;
    }
    else if (d->headingFont == iosevka_TextFont) {
        h12Font = &fontIosevkaTermExtended_Embedded;
        h3Font  = &fontIosevkaTermExtended_Embedded;
    }
#if defined (iPlatformAppleMobile)
    const float uiSize = fontSize_UI * 1.1f;
#else
    const float uiSize = fontSize_UI;
#endif
    const struct {
        const iBlock *ttf;
        int size;
        float scaling;
        enum iFontSize sizeId;
        /* UI sizes: 1.0, 1.125, 1.333, 1.666 */
        /* Content sizes: smallmono, mono, 1.0, 1.2, 1.333, 1.666, 2.0 */
    } fontData[max_FontId] = {
        /* UI fonts: normal weight */
        { &fontSourceSans3Regular_Embedded, uiSize,               1.0f, uiNormal_FontSize },
        { &fontSourceSans3Regular_Embedded, uiSize * 1.125f,      1.0f, uiMedium_FontSize },
        { &fontSourceSans3Regular_Embedded, uiSize * 1.333f,      1.0f, uiBig_FontSize },
        { &fontSourceSans3Regular_Embedded, uiSize * 1.666f,      1.0f, uiLarge_FontSize },
        { &fontSourceSans3Semibold_Embedded, uiSize * 0.8f,       1.0f, uiNormal_FontSize },
        /* UI fonts: bold weight */
        { &fontSourceSans3Bold_Embedded,    uiSize,               1.0f, uiNormal_FontSize },
        { &fontSourceSans3Bold_Embedded,    uiSize * 1.125f,      1.0f, uiMedium_FontSize },
        { &fontSourceSans3Bold_Embedded,    uiSize * 1.333f,      1.0f, uiBig_FontSize },
        { &fontSourceSans3Bold_Embedded,    uiSize * 1.666f,      1.0f, uiLarge_FontSize },
        /* content fonts */
        { regularFont,                        textSize,             scaling,      contentRegular_FontSize },
        { boldFont,                           textSize,             scaling,      contentRegular_FontSize },
        { italicFont,                         textSize,             italicScaling,contentRegular_FontSize },
        { regularFont,                        textSize * 1.200f,    scaling,      contentMedium_FontSize },
        { h3Font,                             textSize * 1.333f,    h123Scaling,  contentBig_FontSize },
        { h12Font,                            textSize * 1.666f,    h123Scaling,  contentLarge_FontSize },
        { lightFont,                          textSize * 1.666f,    lightScaling, contentLarge_FontSize },
        { h12Font,                            textSize * 2.000f,    h123Scaling,  contentHuge_FontSize },
        { &fontIosevkaTermExtended_Embedded,  smallMonoSize,        1.0f,         contentMonoSmall_FontSize },
        { &fontIosevkaTermExtended_Embedded,  monoSize,             1.0f,         contentMono_FontSize },
        /* extra content fonts */
        { &fontSourceSans3Regular_Embedded,   textSize,             scaling, contentRegular_FontSize },
        { &fontSourceSans3Regular_Embedded,   textSize * 0.80f,     scaling, contentRegular_FontSize },
        /* symbols and scripts */
#define DEFINE_FONT_SET(data, glyphScale) \
        { (data), uiSize,            glyphScale, uiNormal_FontSize }, \
        { (data), uiSize * 1.125f,   glyphScale, uiMedium_FontSize }, \
        { (data), uiSize * 1.333f,   glyphScale, uiBig_FontSize }, \
        { (data), uiSize * 1.666f,   glyphScale, uiLarge_FontSize }, \
        { (data), textSize,          glyphScale, contentRegular_FontSize }, \
        { (data), textSize * 1.200f, glyphScale, contentMedium_FontSize }, \
        { (data), textSize * 1.333f, glyphScale, contentBig_FontSize }, \
        { (data), textSize * 1.666f, glyphScale, contentLarge_FontSize }, \
        { (data), textSize * 2.000f, glyphScale, contentHuge_FontSize }, \
        { (data), smallMonoSize,     glyphScale, contentMonoSmall_FontSize }, \
        { (data), monoSize,          glyphScale, contentMono_FontSize }
        DEFINE_FONT_SET(userFont_ ? userFont_ : &fontIosevkaTermExtended_Embedded, 1.0f),
        DEFINE_FONT_SET(&fontIosevkaTermExtended_Embedded, 0.866f),
        DEFINE_FONT_SET(&fontNotoSansSymbolsRegular_Embedded, 2.0f),
        DEFINE_FONT_SET(&fontNotoSansSymbols2Regular_Embedded, 1.45f),
        DEFINE_FONT_SET(&fontSmolEmojiRegular_Embedded, 1.0f),
        DEFINE_FONT_SET(&fontNotoEmojiRegular_Embedded, 1.10f),
        DEFINE_FONT_SET(&fontNotoSansJPRegular_Embedded, 1.0f),
        DEFINE_FONT_SET(&fontNotoSansSCRegular_Embedded, 1.0f),
        DEFINE_FONT_SET(&fontNanumGothicRegular_Embedded, 1.0f), /* TODO: should use Noto Sans here, too */
        DEFINE_FONT_SET(&fontNotoSansArabicUIRegular_Embedded, 1.0f),
//        DEFINE_FONT_SET(&fontScheherazadeNewRegular_Embedded, 1.0f),
    };
    iForIndices(i, fontData) {
        iFont *font = &d->fonts[i];
        init_Font(font,
                  fontData[i].ttf,
                  fontData[i].size,
                  fontData[i].scaling,
                  fontData[i].sizeId,
                  fontData[i].ttf == &fontIosevkaTermExtended_Embedded);
    }
    gap_Text = iRound(gap_UI * d->contentFontSize);
}

static void deinitFonts_Text_(iText *d) {
    iForIndices(i, d->fonts) {
        deinit_Font(&d->fonts[i]);
    }
}

static int maxGlyphHeight_Text_(const iText *d) {
    return 2 * d->contentFontSize * fontSize_UI;
}

static void initCache_Text_(iText *d) {
    init_Array(&d->cacheRows, sizeof(iCacheRow));
    const int textSize = d->contentFontSize * fontSize_UI;
    iAssert(textSize > 0);
    const iInt2 cacheDims = init_I2(16, 40);
    d->cacheSize = mul_I2(cacheDims, init1_I2(iMax(textSize, fontSize_UI)));
    SDL_RendererInfo renderInfo;
    SDL_GetRendererInfo(d->render, &renderInfo);
    if (renderInfo.max_texture_height > 0 && d->cacheSize.y > renderInfo.max_texture_height) {
        d->cacheSize.y = renderInfo.max_texture_height;
        d->cacheSize.x = renderInfo.max_texture_width;
    }
    d->cacheRowAllocStep = iMax(2, textSize / 6);
    /* Allocate initial (empty) rows. These will be assigned actual locations in the cache
       once at least one glyph is stored. */
    for (int h = d->cacheRowAllocStep;
         h <= 2.5 * textSize + d->cacheRowAllocStep;
         h += d->cacheRowAllocStep) {
        pushBack_Array(&d->cacheRows, &(iCacheRow){ .height = 0 });
    }
    d->cacheBottom = 0;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    d->cache = SDL_CreateTexture(d->render,
                                 SDL_PIXELFORMAT_RGBA4444,
                                 SDL_TEXTUREACCESS_STATIC | SDL_TEXTUREACCESS_TARGET,
                                 d->cacheSize.x,
                                 d->cacheSize.y);
    SDL_SetTextureBlendMode(d->cache, SDL_BLENDMODE_BLEND);
}

static void deinitCache_Text_(iText *d) {
    deinit_Array(&d->cacheRows);
    SDL_DestroyTexture(d->cache);
}

void loadUserFonts_Text(void) {
    if (userFont_) {
        delete_Block(userFont_);
        userFont_ = NULL;
    }
    /* Load the system font. */
    const iPrefs *prefs = prefs_App();
    if (!isEmpty_String(&prefs->symbolFontPath)) {
        iFile *f = new_File(&prefs->symbolFontPath);
        if (open_File(f, readOnly_FileMode)) {
            userFont_ = readAll_File(f);
        }
        else {
            fprintf(stderr, "[Text] failed to open: %s\n", cstr_String(&prefs->symbolFontPath));
        }
        iRelease(f);
    }
}

void init_Text(SDL_Renderer *render) {
    iText *d = &text_;
    loadUserFonts_Text();
    d->contentFont     = nunito_TextFont;
    d->headingFont     = nunito_TextFont;
    d->contentFontSize = contentScale_Text_;
    d->ansiEscape      = new_RegExp("[[()]([0-9;AB]*)m", 0);
    d->render          = render;
    /* A grayscale palette for rasterized glyphs. */ {
        SDL_Color colors[256];
        for (int i = 0; i < 256; ++i) {
            colors[i] = (SDL_Color){ 255, 255, 255, i };
        }
        d->grayscale = SDL_AllocPalette(256);
        SDL_SetPaletteColors(d->grayscale, colors, 0, 256);
    }
    initCache_Text_(d);
    initFonts_Text_(d);
}

void deinit_Text(void) {
    iText *d = &text_;
    SDL_FreePalette(d->grayscale);
    deinitFonts_Text_(d);
    deinitCache_Text_(d);
    d->render = NULL;
    iRelease(d->ansiEscape);
}

void setOpacity_Text(float opacity) {
    SDL_SetTextureAlphaMod(text_.cache, iClamp(opacity, 0.0f, 1.0f) * 255 + 0.5f);
}

void setContentFont_Text(enum iTextFont font) {
    if (text_.contentFont != font) {
        text_.contentFont = font;
        resetFonts_Text();
    }
}

void setHeadingFont_Text(enum iTextFont font) {
    if (text_.headingFont != font) {
        text_.headingFont = font;
        resetFonts_Text();
    }
}

void setContentFontSize_Text(float fontSizeFactor) {
    fontSizeFactor *= contentScale_Text_;
    iAssert(fontSizeFactor > 0);
    if (iAbs(text_.contentFontSize - fontSizeFactor) > 0.001f) {
        text_.contentFontSize = fontSizeFactor;
        resetFonts_Text();
    }
}

static void resetCache_Text_(iText *d) {
    deinitCache_Text_(d);
    for (int i = 0; i < max_FontId; i++) {
        clearGlyphs_Font_(&d->fonts[i]);
    }
    initCache_Text_(d);
}

void resetFonts_Text(void) {
    iText *d = &text_;
    deinitFonts_Text_(d);
    deinitCache_Text_(d);
    initCache_Text_(d);
    initFonts_Text_(d);
}

iLocalDef iFont *font_Text_(enum iFontId id) {
    return &text_.fonts[id & mask_FontId];
}

static SDL_Surface *rasterizeGlyph_Font_(const iFont *d, uint32_t glyphIndex, float xShift) {
    int w, h;
    uint8_t *bmp = stbtt_GetGlyphBitmapSubpixel(
        &d->font, d->xScale, d->yScale, xShift, 0.0f, glyphIndex, &w, &h, 0, 0);
    SDL_Surface *surface8 =
        SDL_CreateRGBSurfaceWithFormatFrom(bmp, w, h, 8, w, SDL_PIXELFORMAT_INDEX8);
    SDL_SetSurfaceBlendMode(surface8, SDL_BLENDMODE_NONE);
    SDL_SetSurfacePalette(surface8, text_.grayscale);
#if LAGRANGE_RASTER_DEPTH != 8
    /* Convert to the cache format. */
    SDL_Surface *surf = SDL_ConvertSurfaceFormat(surface8, LAGRANGE_RASTER_FORMAT, 0);
    SDL_SetSurfaceBlendMode(surf, SDL_BLENDMODE_NONE);
    free(bmp);
    SDL_FreeSurface(surface8);
    return surf;
#else
    return surface8;
#endif
}

iLocalDef iCacheRow *cacheRow_Text_(iText *d, int height) {
    return at_Array(&d->cacheRows, (height - 1) / d->cacheRowAllocStep);
}

static iInt2 assignCachePos_Text_(iText *d, iInt2 size) {
    iCacheRow *cur = cacheRow_Text_(d, size.y);
    if (cur->height == 0) {
        /* Begin a new row height. */
        cur->height = (1 + (size.y - 1) / d->cacheRowAllocStep) * d->cacheRowAllocStep;
        cur->pos.y = d->cacheBottom;
        d->cacheBottom = cur->pos.y + cur->height;
    }
    iAssert(cur->height >= size.y);
    /* TODO: Automatically enlarge the cache if running out of space?
       Maybe make it paged, but beware of texture swapping too often inside a text string. */
    if (cur->pos.x + size.x > d->cacheSize.x) {
        /* Does not fit on this row, advance to a new location in the cache. */
        cur->pos.y = d->cacheBottom;
        cur->pos.x = 0;
        d->cacheBottom += cur->height;
        iAssert(d->cacheBottom <= d->cacheSize.y);
    }
    const iInt2 assigned = cur->pos;
    cur->pos.x += size.x;
    return assigned;
}

static void allocate_Font_(iFont *d, iGlyph *glyph, int hoff) {
    iRect *glRect = &glyph->rect[hoff];
    int    x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBoxSubpixel(
        &d->font, index_Glyph_(glyph), d->xScale, d->yScale, hoff * 0.5f, 0.0f, &x0, &y0, &x1, &y1);
    glRect->size = init_I2(x1 - x0, y1 - y0);
    /* Determine placement in the glyph cache texture, advancing in rows. */
    glRect->pos    = assignCachePos_Text_(&text_, glRect->size);
    glyph->d[hoff] = init_I2(x0, y0);
    glyph->d[hoff].y += d->vertOffset;
    if (hoff == 0) { /* hoff==1 uses same metrics as `glyph` */
        int adv;
        stbtt_GetGlyphHMetrics(&d->font, index_Glyph_(glyph), &adv, NULL);
        glyph->advance = d->xScale * adv;
    }
}

iLocalDef iFont *characterFont_Font_(iFont *d, iChar ch, uint32_t *glyphIndex) {
    if (isVariationSelector_Char(ch)) {
        return d;
    }
    /* Smol Emoji overrides all other fonts. */
    if (ch != 0x20) {
        iFont *smol = font_Text_(smolEmoji_FontId + d->sizeId);
        if (smol != d && (*glyphIndex = glyphIndex_Font_(smol, ch)) != 0) {
            return smol;
        }
    }
    /* Manual exceptions. */ {
        if (ch >= 0x2190 && ch <= 0x2193 /* arrows */) {
            d = font_Text_(iosevka_FontId + d->sizeId);
            *glyphIndex = glyphIndex_Font_(d, ch);
            return d;
        }
    }
    if ((*glyphIndex = glyphIndex_Font_(d, ch)) != 0) {
        return d;
    }
    const int fallbacks[] = {
        notoEmoji_FontId,
        symbols2_FontId,
        symbols_FontId
    };
    /* First fallback is Smol Emoji. */
    if (ch != 0x20) {
        iForIndices(i, fallbacks) {
            iFont *fallback = font_Text_(fallbacks[i] + d->sizeId);
            if (fallback != d && (*glyphIndex = glyphIndex_Font_(fallback, ch)) != 0) {
                return fallback;
            }
        }
    }
    /* Try Simplified Chinese. */
    if (ch >= 0x2e80) {
        iFont *sc = font_Text_(chineseSimplified_FontId + d->sizeId);
        if (sc != d && (*glyphIndex = glyphIndex_Font_(sc, ch)) != 0) {
            return sc;
        }
    }
    /* Could be Korean. */
    if (ch >= 0x3000) {
        iFont *korean = font_Text_(korean_FontId + d->sizeId);
        if (korean != d && (*glyphIndex = glyphIndex_Font_(korean, ch)) != 0) {
            return korean;
        }
    }
    /* Japanese perhaps? */
    if (ch > 0x3040) {
        iFont *japanese = font_Text_(japanese_FontId + d->sizeId);
        if (japanese != d && (*glyphIndex = glyphIndex_Font_(japanese, ch)) != 0) {
            return japanese;
        }
    }
    /* Maybe Arabic. */
    if (ch >= 0x600) {
        iFont *arabic = font_Text_(arabic_FontId + d->sizeId);
        if (arabic != d && (*glyphIndex = glyphIndex_Font_(arabic, ch)) != 0) {
            return arabic;
        }
    }
#if defined (iPlatformApple)
    /* White up arrow is used for the Shift key on macOS. Symbola's glyph is not a great
       match to the other text, so use the UI font instead. */
    if ((ch == 0x2318 || ch == 0x21e7) && d == font_Text_(regular_FontId)) {
        *glyphIndex = glyphIndex_Font_(d = font_Text_(defaultContentRegular_FontId), ch);
        return d;
    }
#endif
    /* User's symbols font. */ {
        iFont *sys = font_Text_(userSymbols_FontId + d->sizeId);
        if (sys != d && (*glyphIndex = glyphIndex_Font_(sys, ch)) != 0) {
            return sys;
        }
    }
    /* Final fallback. */
    iFont *font = font_Text_(iosevka_FontId + d->sizeId);
    if (d != font) {
        *glyphIndex = glyphIndex_Font_(font, ch);
    }
    if (!*glyphIndex) {
        fprintf(stderr, "failed to find %08x (%lc)\n", ch, (int)ch); fflush(stderr);
    }
    return d;
}

static iGlyph *glyphByIndex_Font_(iFont *d, uint32_t glyphIndex) {
    iGlyph* glyph = NULL;
    void *  node = value_Hash(&d->glyphs, glyphIndex);
    if (node) {
        glyph = node;
    }
    else {
        /* If the cache is running out of space, clear it and we'll recache what's needed currently. */
        if (text_.cacheBottom > text_.cacheSize.y - maxGlyphHeight_Text_(&text_)) {
#if !defined (NDEBUG)
            printf("[Text] glyph cache is full, clearing!\n"); fflush(stdout);
#endif
            resetCache_Text_(&text_);
        }
        glyph       = new_Glyph(glyphIndex);
        glyph->font = d;
        /* New glyphs are always allocated at least. This reserves a position in the cache
           and updates the glyph metrics. */
        allocate_Font_(d, glyph, 0);
        allocate_Font_(d, glyph, 1);
        insert_Hash(&d->glyphs, &glyph->node);
    }
    return glyph;
}

static iGlyph *glyph_Font_(iFont *d, iChar ch) {
    /* The glyph may actually come from a different font; look up the right font. */
    uint32_t glyphIndex = 0;
    iFont *font = characterFont_Font_(d, ch, &glyphIndex);
    return glyphByIndex_Font_(font, glyphIndex);
}

static iChar nextChar_(const char **chPos, const char *end) {
    if (*chPos == end) {
        return 0;
    }
    iChar ch;
    int len = decodeBytes_MultibyteChar(*chPos, end, &ch);
    if (len <= 0) {
        (*chPos)++; /* skip it */
        return 0;
    }
    (*chPos) += len;
    return ch;
}

static iBool isControl_Char_(iChar c) {
    return isDefaultIgnorable_Char(c) || isVariationSelector_Char(c) || isFitzpatrickType_Char(c);
}

/*----------------------------------------------------------------------------------------------*/

iDeclareType(AttributedRun)

struct Impl_AttributedRun {
    iRangei   logical; /* UTF-32 codepoint indices in the logical-order text */
    iFont *   font;
    iColor    fgColor;
    struct {
        uint8_t isLineBreak : 1;
        uint8_t isRTL       : 1;
        uint8_t isArabic    : 1; /* Arabic script detected */
    } flags;
};

iDeclareType(AttributedText)
iDeclareTypeConstructionArgs(AttributedText, iRangecc text, size_t maxLen, iFont *font,
                             iColor fgColor, int baseDir, iChar overrideChar)

struct Impl_AttributedText {
    iRangecc source; /* original source text */
    size_t   maxLen;
    iFont *  font;
    iColor   fgColor;
    iArray   runs;
    iArray   logical;         /* UTF-32 text in logical order (mixed directions; matches source) */
    iArray   visual;          /* UTF-32 text in visual order (LTR) */
    iArray   logicalToVisual; /* map visual index to logical index */
    iArray   visualToLogical;
    iArray   logicalToSourceOffset; /* map logical character to an UTF-8 offset in the source text */
    char *   bidiLevels;
    iBool    isBaseRTL;
};

iDefineTypeConstructionArgs(AttributedText,
                            (iRangecc text, size_t maxLen, iFont *font, iColor fgColor,
                             int baseDir, iChar overrideChar),
                            text, maxLen, font, fgColor, baseDir, overrideChar)

static const char *sourcePtr_AttributedText_(const iAttributedText *d, int logicalPos) {
    const int *logToSource = constData_Array(&d->logicalToSourceOffset);
    return d->source.start + logToSource[logicalPos];
}

static iRangecc sourceRange_AttributedText_(const iAttributedText *d, iRangei logical) {
    const int *logToSource = constData_Array(&d->logicalToSourceOffset);
    iRangecc range = {
        d->source.start + logToSource[logical.start],
        d->source.start + logToSource[logical.end]
    };
    iAssert(range.start <= range.end);
    return range;
}

#if 0
static iBool isAllSpace_AttributedText_(const iAttributedText *d, iRangei range) {
    const iChar *logicalText = constData_Array(&d->logical);
    for (size_t i = range.start; i < range.end; i++) {
        if (logicalText[i] != 0x20) {
            return iFalse;
        }
    }
    return iTrue;
}
#endif

static void finishRun_AttributedText_(iAttributedText *d, iAttributedRun *run, int endAt) {
    iAttributedRun finishedRun = *run;
    iAssert(endAt >= 0 && endAt <= size_Array(&d->logical));
    finishedRun.logical.end = endAt;
    if (!isEmpty_Range(&finishedRun.logical)) {
        pushBack_Array(&d->runs, &finishedRun);
        run->flags.isLineBreak = iFalse;
        run->flags.isArabic    = iFalse;
    }
    run->logical.start = endAt;
}

static enum iFontId fontId_Text_(const iFont *font) {
    return (enum iFontId) (font - text_.fonts);
}

static void prepare_AttributedText_(iAttributedText *d, int overrideBaseDir, iChar overrideChar) {
    iAssert(isEmpty_Array(&d->runs));
    size_t length = 0;
    /* Prepare the UTF-32 logical string. */ {
        for (const char *ch = d->source.start; ch < d->source.end; ) {
            iChar u32;
            int len = decodeBytes_MultibyteChar(ch, d->source.end, &u32);
            if (len <= 0) break;
            if (overrideChar) {
                u32 = overrideChar;
            }
            pushBack_Array(&d->logical, &u32);
            length++;
            if (length == d->maxLen) {
                /* TODO: Check the combining class; only count base characters here. */
                break;
            }
            /* Remember the byte offset to each character. We will need this to communicate
               back the wrapped UTF-8 ranges. */
            pushBack_Array(&d->logicalToSourceOffset, &(int){ ch - d->source.start });
            ch += len;
        }
#if defined (LAGRANGE_ENABLE_FRIBIDI)
        /* Use FriBidi to reorder the codepoints. */
        resize_Array(&d->visual, length);
        resize_Array(&d->logicalToVisual, length);
        resize_Array(&d->visualToLogical, length);
        d->bidiLevels = length ? malloc(length) : NULL;
        FriBidiParType baseDir = (FriBidiParType) FRIBIDI_TYPE_ON;
        fribidi_log2vis(constData_Array(&d->logical),
                        length,
                        &baseDir,
                        data_Array(&d->visual),
                        data_Array(&d->logicalToVisual),
                        data_Array(&d->visualToLogical),
                        (FriBidiLevel *) d->bidiLevels);
        d->isBaseRTL = (overrideBaseDir == 0 ? FRIBIDI_IS_RTL(baseDir) : (overrideBaseDir < 0));
#else
        /* 1:1 mapping. */
        setCopy_Array(&d->visual, &d->logical);
        resize_Array(&d->logicalToVisual, length);
        for (size_t i = 0; i < length; i++) {
            set_Array(&d->logicalToVisual, i, &(int){ i });
        }
        setCopy_Array(&d->visualToLogical, &d->logicalToVisual);
        d->isBaseRTL = iFalse;
#endif
    }
    /* The mapping needs to include the terminating NULL position. */ {
        pushBack_Array(&d->logicalToSourceOffset, &(int){ d->source.end - d->source.start });
        pushBack_Array(&d->logicalToVisual, &(int){ length });
        pushBack_Array(&d->visualToLogical, &(int){ length });
    }
    iAttributedRun run         = { .logical = { 0, length },
                                   .font    = d->font,
                                   .fgColor = d->fgColor };
    const int *    logToSource = constData_Array(&d->logicalToSourceOffset);
    const int *    logToVis    = constData_Array(&d->logicalToVisual);
    const iChar *  logicalText = constData_Array(&d->logical);
    iBool          isRTL       = d->isBaseRTL;
    int            numNonSpace = 0;
    for (int pos = 0; pos < length; pos++) {
        const iChar ch = logicalText[pos];
#if defined (LAGRANGE_ENABLE_FRIBIDI)
        if (d->bidiLevels) {
            const char lev = d->bidiLevels[pos];
            const iBool isNeutral = FRIBIDI_IS_NEUTRAL(lev);
            if (!isNeutral) {
                iBool rtl = FRIBIDI_IS_RTL(lev) != 0;
                if (rtl != isRTL) {
                    /* Direction changes; must end the current run. */
    //                printf("dir change at %zu: %lc U+%04X\n", pos, ch, ch);
                    finishRun_AttributedText_(d, &run, pos);
                    isRTL = rtl;
                }
            }
        }
#else
        const iBool isNeutral = iTrue;
#endif
        run.flags.isRTL = isRTL;
        if (ch == 0x1b) { /* ANSI escape. */
            pos++;
            const char *srcPos = d->source.start + logToSource[pos];
            /* Do a regexp match in the source text. */
            iRegExpMatch m;
            init_RegExpMatch(&m);
            if (match_RegExp(text_.ansiEscape, srcPos, d->source.end - srcPos, &m)) {
                finishRun_AttributedText_(d, &run, pos - 1);
                run.fgColor = ansiForeground_Color(capturedRange_RegExpMatch(&m, 1),
                                                   tmParagraph_ColorId);
                pos += length_Rangecc(capturedRange_RegExpMatch(&m, 0));
                iAssert(logToSource[pos] == end_RegExpMatch(&m) - d->source.start);
                /* The run continues after the escape sequence. */
                run.logical.start = pos--; /* loop increments `pos` */
                continue;
            }
        }
        if (ch == '\v') {
            finishRun_AttributedText_(d, &run, pos);
            /* An internal color escape. */
            iChar esc = logicalText[++pos];
            int colorNum = none_ColorId; /* default color */
            if (esc == '\v') { /* Extended range. */
                esc = logicalText[++pos] + asciiExtended_ColorEscape;
                colorNum = esc - asciiBase_ColorEscape;
            }
            else if (esc != 0x24) { /* ASCII Cancel */
                colorNum = esc - asciiBase_ColorEscape;
            }
            run.logical.start = pos + 1;
            run.fgColor = (colorNum >= 0 ? get_Color(colorNum) : d->fgColor);
            continue;
        }
        if (ch == '\n') {
            finishRun_AttributedText_(d, &run, pos);
            /* A separate run for the newline. */
            run.logical.start = pos;
            run.flags.isLineBreak = iTrue;
            finishRun_AttributedText_(d, &run, pos + 1);
            continue;
        }
        if (isControl_Char_(ch)) {
            continue;
        }
        if (ch == 0x20) {
            if (run.font->family == emojiAndSymbols_TextFont) {
                finishRun_AttributedText_(d, &run, pos);
                run.font = d->font; /* never use space from the symbols font, it's too wide */
            }
            continue;
        }
        iFont *currentFont = d->font;
        if (run.font->family == arabic_TextFont && isPunct_Char(ch)) {
            currentFont = run.font; /* remain as Arabic for whitespace */
        }
        const iGlyph *glyph = glyph_Font_(currentFont, ch);
        if (index_Glyph_(glyph) && glyph->font != run.font) {
            /* A different font is being used for this character. */
            finishRun_AttributedText_(d, &run, pos);
            run.font = glyph->font;
#if 0
            printf("changing font to %d at pos %u (%lc) U+%04X\n", fontId_Text_(run.font), pos, (int)logicalText[pos],
                   (int)logicalText[pos]);
#endif
        }
#if defined (LAGRANGE_ENABLE_FRIBIDI)
        if (fribidi_get_bidi_type(ch) == FRIBIDI_TYPE_AL) {
            run.flags.isArabic = iTrue; /* Arabic letter */
        }
#endif
    }
    if (!isEmpty_Range(&run.logical)) {
        pushBack_Array(&d->runs, &run);
    }
#if 0
    printf("[AttributedText] %zu runs:\n", size_Array(&d->runs));
    iConstForEach(Array, i, &d->runs) {
        const iAttributedRun *run = i.value;
        printf("  %zu %s log:%d...%d vis:%d...%d {%s}\n", index_ArrayConstIterator(&i),
               run->flags.isRTL ? "<-" : "->",
               run->logical.start, run->logical.end - 1,
               logToVis[run->logical.start], logToVis[run->logical.end - 1],
               cstr_Rangecc(sourceRange_AttributedText_(d, run->logical)));
    }
#endif
}

void init_AttributedText(iAttributedText *d, iRangecc text, size_t maxLen, iFont *font, iColor fgColor,
                         int baseDir, iChar overrideChar) {
    d->source  = text;
    d->maxLen  = maxLen ? maxLen : iInvalidSize;
    d->font    = font;
    d->fgColor = fgColor;
    init_Array(&d->runs, sizeof(iAttributedRun));
    init_Array(&d->logical, sizeof(iChar));
    init_Array(&d->visual, sizeof(iChar));
    init_Array(&d->logicalToVisual, sizeof(int));
    init_Array(&d->visualToLogical, sizeof(int));
    init_Array(&d->logicalToSourceOffset, sizeof(int));
    d->bidiLevels = NULL;
    d->isBaseRTL = iFalse;
    prepare_AttributedText_(d, baseDir, overrideChar);
}

void deinit_AttributedText(iAttributedText *d) {
    free(d->bidiLevels);
    deinit_Array(&d->logicalToSourceOffset);
    deinit_Array(&d->logicalToVisual);
    deinit_Array(&d->visualToLogical);
    deinit_Array(&d->visual);
    deinit_Array(&d->logical);
    deinit_Array(&d->runs);
}

/*----------------------------------------------------------------------------------------------*/

iDeclareType(RasterGlyph)

struct Impl_RasterGlyph {
    iGlyph *glyph;
    int     hoff;
    iRect   rect;
};

static void cacheGlyphs_Font_(iFont *d, const iArray *glyphIndices) {
    /* TODO: Make this an object so it can be used sequentially without reallocating buffers. */
    SDL_Surface *buf     = NULL;
    const iInt2  bufSize = init_I2(iMin(512, d->height * iMin(2 * size_Array(glyphIndices), 20)),
                                   d->height * 4 / 3);
    int          bufX    = 0;
    iArray *     rasters = NULL;
    SDL_Texture *oldTarget = NULL;
    iBool        isTargetChanged = iFalse;
    iAssert(isExposed_Window(get_Window()));
    /* We'll flush the buffered rasters periodically until everything is cached. */
    size_t index = 0;
    while (index < size_Array(glyphIndices)) {
        for (; index < size_Array(glyphIndices); index++) {
            const uint32_t glyphIndex = constValue_Array(glyphIndices, index, uint32_t);
            const int lastCacheBottom = text_.cacheBottom;
            iGlyph *glyph = glyphByIndex_Font_(d, glyphIndex);
            if (text_.cacheBottom < lastCacheBottom) {
                /* The cache was reset due to running out of space. We need to restart from
                   the beginning! */
                bufX = 0;
                if (rasters) {
                    clear_Array(rasters);
                }
                index = 0;
                break;
            }
            if (!isFullyRasterized_Glyph_(glyph)) {
                /* Need to cache this. */
                if (buf == NULL) {
                    rasters = new_Array(sizeof(iRasterGlyph));
                    buf     = SDL_CreateRGBSurfaceWithFormat(
                                0, bufSize.x, bufSize.y,
                                LAGRANGE_RASTER_DEPTH,
                                LAGRANGE_RASTER_FORMAT);
                    SDL_SetSurfaceBlendMode(buf, SDL_BLENDMODE_NONE);
                    SDL_SetSurfacePalette(buf, text_.grayscale);
                }
                SDL_Surface *surfaces[2] = {
                    !isRasterized_Glyph_(glyph, 0) ?
                            rasterizeGlyph_Font_(glyph->font, index_Glyph_(glyph), 0) : NULL,
                    !isRasterized_Glyph_(glyph, 1) ?
                            rasterizeGlyph_Font_(glyph->font, index_Glyph_(glyph), 0.5f) : NULL
                };
                iBool outOfSpace = iFalse;
                iForIndices(i, surfaces) {
                    if (surfaces[i]) {
                        const int w = surfaces[i]->w;
                        const int h = surfaces[i]->h;
                        if (bufX + w <= bufSize.x) {
                            SDL_BlitSurface(surfaces[i],
                                            NULL,
                                            buf,
                                            &(SDL_Rect){ bufX, 0, w, h });
                            pushBack_Array(rasters,
                                           &(iRasterGlyph){ glyph, i, init_Rect(bufX, 0, w, h) });
                            bufX += w;
                        }
                        else {
                            outOfSpace = iTrue;
                            break;
                        }
                    }
                }
                iForIndices(i, surfaces) { /* cleanup */
                    if (surfaces[i]) {
                        if (surfaces[i]->flags & SDL_PREALLOC) {
                            free(surfaces[i]->pixels);
                        }
                        SDL_FreeSurface(surfaces[i]);
                    }
                }
                if (outOfSpace) {
                    /* Redo this glyph. `index` does not get incremented. */
                    break;
                }
            }
        }
        /* Finished or the buffer is full, copy the glyphs to the cache texture. */
        if (!isEmpty_Array(rasters)) {
            SDL_Texture *bufTex = SDL_CreateTextureFromSurface(text_.render, buf);
            SDL_SetTextureBlendMode(bufTex, SDL_BLENDMODE_NONE);
            if (!isTargetChanged) {
                isTargetChanged = iTrue;
                oldTarget = SDL_GetRenderTarget(text_.render);
                SDL_SetRenderTarget(text_.render, text_.cache);
            }
//            printf("copying %zu rasters from %p\n", size_Array(rasters), bufTex); fflush(stdout);
            iConstForEach(Array, i, rasters) {
                const iRasterGlyph *rg = i.value;
//                iAssert(isEqual_I2(rg->rect.size, rg->glyph->rect[rg->hoff].size));
                const iRect *glRect = &rg->glyph->rect[rg->hoff];
                SDL_RenderCopy(text_.render,
                               bufTex,
                               (const SDL_Rect *) &rg->rect,
                               (const SDL_Rect *) glRect);
                setRasterized_Glyph_(rg->glyph, rg->hoff);
//                printf(" - %u (hoff %d)\n", index_Glyph_(rg->glyph), rg->hoff);
            }
            SDL_DestroyTexture(bufTex);
            /* Resume with an empty buffer. */
            clear_Array(rasters);
            bufX = 0;
        }
    }
    if (rasters) {
        delete_Array(rasters);
    }
    if (buf) {
        SDL_FreeSurface(buf);
    }
    if (isTargetChanged) {
        SDL_SetRenderTarget(text_.render, oldTarget);
    }
}

static void cacheSingleGlyph_Font_(iFont *d, uint32_t glyphIndex) {
    iArray indices;
    init_Array(&indices, sizeof(uint32_t));
    pushBack_Array(&indices, &glyphIndex);
    cacheGlyphs_Font_(d, &indices);
    deinit_Array(&indices);
}

static void cacheTextGlyphs_Font_(iFont *d, const iRangecc text) {
    iArray glyphIndices;
    init_Array(&glyphIndices, sizeof(uint32_t));
    iAttributedText attrText;
    init_AttributedText(&attrText, text, 0, d, (iColor){}, 0, 0);
    /* We use AttributedText here so the font lookup matches the behavior during text drawing --
       glyphs may be selected from a font that's different than `d`. */
    const iChar *logicalText = constData_Array(&attrText.logical);
    iConstForEach(Array, i, &attrText.runs) {
        const iAttributedRun *run = i.value;
        if (run->flags.isLineBreak) {
            continue;
        }
        for (int pos = run->logical.start; pos < run->logical.end; pos++) {
            const iChar ch = logicalText[pos];
            if (!isSpace_Char(ch) && !isControl_Char_(ch)) {
                /* TODO: Use `run->font`; the glyph may be selected from a different font. */
                const uint32_t glyphIndex = glyphIndex_Font_(d, ch);
                if (glyphIndex) {
                    pushBack_Array(&glyphIndices, &glyphIndex);
                }
            }
        }
    }
    deinit_AttributedText(&attrText);
    /* TODO: Cache glyphs from ALL the fonts we encountered above. */
    cacheGlyphs_Font_(d, &glyphIndices);
    deinit_Array(&glyphIndices);
}

enum iRunMode {
    measure_RunMode                 = 0,
    draw_RunMode                    = 1,
    modeMask_RunMode                = 0x00ff,
    flagsMask_RunMode               = 0xff00,
//    noWrapFlag_RunMode              = iBit(9),
    visualFlag_RunMode              = iBit(10), /* actual visible bounding box of the glyph,
                                                   e.g., for icons */
    permanentColorFlag_RunMode      = iBit(11),
    alwaysVariableWidthFlag_RunMode = iBit(12),
    fillBackground_RunMode          = iBit(13),
//    stopAtNewline_RunMode           = iBit(14), /* don't advance past \n, consider it a wrap pos */
};

iDeclareType(RunArgs)

struct Impl_RunArgs {
    enum iRunMode mode;
    iRangecc      text;
    size_t        maxLen; /* max characters to process */
    iInt2         pos;
    iWrapText *   wrap;
//    int           xposLimit;        /* hard limit for wrapping */
//    int           xposLayoutBound;  /* visible bound for layout purposes; does not affect wrapping */
    int           color;
    int           baseDir;
    /* TODO: Cleanup using TextMetrics
       Use TextMetrics output pointer instead of return value & cursorAdvance_out. */
    iInt2 *       cursorAdvance_out;
//    const char ** continueFrom_out;
    int *         runAdvance_out;
};

static iBool notify_WrapText_(iWrapText *d, const char *ending, int origin, int advance, iBool isBaseRTL) {
    if (d && d->wrapFunc && d->wrapRange_.start) {
        /* `wrapRange_` uses logical indices. */
        const char *end   = ending ? ending : d->wrapRange_.end;
        iRangecc    range = { d->wrapRange_.start, end };
        iAssert(range.start <= range.end);
        const iBool result = d->wrapFunc(d, range, origin, advance, isBaseRTL);
        if (result) {
            d->wrapRange_.start = end;
        }
        else {
            d->wrapRange_ = iNullRange;
        }
        return result;
    }
    return iTrue;
}

float horizKern_Font_(iFont *d, uint32_t glyph1, uint32_t glyph2) {
#if defined (LAGRANGE_ENABLE_KERNING)
    if (!enableKerning_Text || d->family != nunito_TextFont) {
        return 0.0f;
    }
    if (glyph1 && glyph2) {
        /* These indices will be quickly found from the lookup table. */
        const uint32_t gi_h = glyphIndex_Font_(d, 'h');
        const uint32_t gi_i = glyphIndex_Font_(d, 'i');
        int kern = 0;
        /* Nunito needs some kerning fixes. */
        if (glyph1 == glyphIndex_Font_(d, 'W') && (glyph2 == gi_h || glyph2 == gi_i)) {
            kern = -60;
        }
        else if (glyph1 == glyphIndex_Font_(d, 'T') && glyph2 == gi_h) {
            kern = -25;
        }
        else if (glyph1 == glyphIndex_Font_(d, 'V') && glyph2 == gi_i) {
            kern = -40;
        }
        return d->xScale * kern;
    }
#endif
    return 0.0f;
}

#if defined (LAGRANGE_ENABLE_HARFBUZZ)

iDeclareType(GlyphBuffer)

struct Impl_GlyphBuffer {
    hb_buffer_t *        hb;
    iFont *              font;
    const iChar *        logicalText;
    hb_glyph_info_t *    glyphInfo;
    hb_glyph_position_t *glyphPos;
    unsigned int         glyphCount;
};

static void init_GlyphBuffer_(iGlyphBuffer *d, iFont *font, const iChar *logicalText) {
    d->hb          = hb_buffer_create();
    d->font        = font;
    d->logicalText = logicalText;
    d->glyphInfo   = NULL;
    d->glyphPos    = NULL;
    d->glyphCount  = 0;
}

static void deinit_GlyphBuffer_(iGlyphBuffer *d) {
    hb_buffer_destroy(d->hb);
}

static void shape_GlyphBuffer_(iGlyphBuffer *d) {
    if (!d->glyphInfo) {
        hb_shape(d->font->hbFont, d->hb, NULL, 0);
        d->glyphInfo = hb_buffer_get_glyph_infos(d->hb, &d->glyphCount);
        d->glyphPos  = hb_buffer_get_glyph_positions(d->hb, &d->glyphCount);
    }
}

static float advance_GlyphBuffer_(const iGlyphBuffer *d, iRangei wrapPosRange) {
    float x = 0.0f;
    for (unsigned int i = 0; i < d->glyphCount; i++) {
        const int logPos = d->glyphInfo[i].cluster;
        if (logPos < wrapPosRange.start || logPos >= wrapPosRange.end) {
            continue;
        }
        x += d->font->xScale * d->glyphPos[i].x_advance;
        if (i + 1 < d->glyphCount) {
            x += horizKern_Font_(d->font,
                                 d->glyphInfo[i].codepoint,
                                 d->glyphInfo[i + 1].codepoint);
        }
    }
    return x;
}

static void evenMonospaceAdvances_GlyphBuffer_(iGlyphBuffer *d, iFont *baseFont) {
    shape_GlyphBuffer_(d);
    const float monoAdvance = glyph_Font_(baseFont, 'M')->advance;
    for (unsigned int i = 0; i < d->glyphCount; ++i) {
        const hb_glyph_info_t *info = d->glyphInfo + i;
        if (d->glyphPos[i].x_advance > 0 && d->font != baseFont) {
            const iChar ch = d->logicalText[info->cluster];
            if (isPictograph_Char(ch) || isEmoji_Char(ch)) {
                const float dw = d->font->xScale * d->glyphPos[i].x_advance - monoAdvance;
                d->glyphPos[i].x_offset  -= dw / 2 / d->font->xScale - 1;
                d->glyphPos[i].x_advance -= dw     / d->font->xScale - 1;
            }
        }
    }
}

static iRect run_Font_(iFont *d, const iRunArgs *args) {
    const int    mode       = args->mode;
    const iInt2  orig       = args->pos;
    iRect        bounds     = { orig, init_I2(0, d->height) };
    float        xCursor    = 0.0f;
    float        yCursor    = 0.0f;
    float        xCursorMax = 0.0f;
    const iBool  isMonospaced = d->isMonospaced;
    iWrapText *wrap = args->wrap;
    iAssert(args->text.end >= args->text.start);
    /* Split the text into a number of attributed runs that specify exactly which
       font is used and other attributes such as color. (HarfBuzz shaping is done
       with one specific font.) */
    iAttributedText attrText;
    init_AttributedText(&attrText, args->text, args->maxLen, d, get_Color(args->color),
                        args->baseDir, wrap ? wrap->overrideChar : 0);
    if (wrap) {
        wrap->baseDir = attrText.isBaseRTL ? -1 : +1;
        /* TODO: Duplicated args? */
        iAssert(equalRange_Rangecc(wrap->text, args->text));
        /* Initialize the wrap range. */
        wrap->wrapRange_        = args->text;
        wrap->hitAdvance_out    = zero_I2();
        wrap->hitChar_out       = NULL;
        wrap->hitGlyphNormX_out = 0.0f;
    }
    const iChar *logicalText = constData_Array(&attrText.logical);
    const iChar *visualText  = constData_Array(&attrText.visual);
    const int *  logToVis    = constData_Array(&attrText.logicalToVisual);
    const int *  visToLog    = constData_Array(&attrText.visualToLogical);
    const size_t runCount    = size_Array(&attrText.runs);
    iArray buffers;
    init_Array(&buffers, sizeof(iGlyphBuffer));
    resize_Array(&buffers, runCount);
    /* Prepare the HarfBuzz buffers. They will be lazily shaped when needed. */
    iConstForEach(Array, i, &attrText.runs) {
        const iAttributedRun *run = i.value;
        iGlyphBuffer *buf = at_Array(&buffers, index_ArrayConstIterator(&i));
        init_GlyphBuffer_(buf, run->font, logicalText);
        /* Insert the text in visual order (LTR) in the HarfBuzz buffer for shaping.
           First we need to map the logical run to the corresponding visual run. */
        int v[2] = { logToVis[run->logical.start], logToVis[run->logical.end - 1] };
        if (v[0] > v[1]) {
            iSwap(int, v[0], v[1]); /* always LTR */
        }
        for (int vis = v[0]; vis <= v[1]; vis++) {
            hb_buffer_add(buf->hb, visualText[vis], visToLog[vis]);
        }
        hb_buffer_set_content_type(buf->hb, HB_BUFFER_CONTENT_TYPE_UNICODE);
        hb_buffer_set_direction(buf->hb, HB_DIRECTION_LTR); /* visual */
        if (run->flags.isArabic) {
            hb_buffer_set_script(buf->hb, HB_SCRIPT_ARABIC);
        }
    }
    if (isMonospaced) {
        /* Fit borrowed glyphs into the expected monospacing. */
        for (size_t runIndex = 0; runIndex < runCount; runIndex++) {
            evenMonospaceAdvances_GlyphBuffer_(at_Array(&buffers, runIndex), d);
        }
    }
    iBool        willAbortDueToWrap = iFalse;
    const size_t textLen            = size_Array(&attrText.logical);
    iRanges      wrapRuns           = { 0, runCount };
    iRangei      wrapPosRange       = { 0, textLen };
    int          wrapResumePos      = textLen;  /* logical position where next line resumes */
    size_t       wrapResumeRunIndex = runCount; /* index of run where next line resumes */
    const int    layoutBound        = (wrap ? wrap->maxWidth : 0);
    iBool        isFirst            = iTrue;
    const iBool  checkHitPoint      = wrap && !isEqual_I2(wrap->hitPoint, zero_I2());
    const iBool  checkHitChar       = wrap && wrap->hitChar;
    while (!isEmpty_Range(&wrapRuns)) {
        if (isFirst) {
            isFirst = iFalse;
        }
        else {
            xCursor = 0;
            yCursor += d->height;
        }
        float wrapAdvance = 0.0f;
        /* First we need to figure out how much text fits on the current line. */
        if (wrap && (wrap->maxWidth > 0 || checkHitPoint)) {
            const iBool isHitPointOnThisLine = (checkHitPoint && wrap->hitPoint.y >= orig.y + yCursor &&
                                                wrap->hitPoint.y < orig.y + yCursor + d->height);
            float breakAdvance = -1.0f;
            iAssert(wrapPosRange.end == textLen);
            /* Determine ends of wrapRuns and wrapVisRange. */
            for (size_t runIndex = wrapRuns.start; runIndex < wrapRuns.end; runIndex++) {
                const iAttributedRun *run = at_Array(&attrText.runs, runIndex);
                if (run->flags.isLineBreak) {
                    if (checkHitChar) {
                        if (wrap->hitChar == sourcePtr_AttributedText_(&attrText, run->logical.start)) {
                            wrap->hitAdvance_out = init_I2(wrapAdvance, yCursor);
                        }
                    }
                    wrapPosRange.end   = run->logical.start;
                    wrapResumePos      = run->logical.end;
                    wrapRuns.end       = runIndex;
                    wrapResumeRunIndex = runIndex + 1;
                    //yCursor += d->height;
                    break;
                }
                wrapResumeRunIndex = runCount;
                wrapResumePos      = textLen;
                iGlyphBuffer *buf = at_Array(&buffers, runIndex);
                iAssert(run->font == buf->font);
                shape_GlyphBuffer_(buf);
                int safeBreakPos = -1;
                iChar prevCh = 0;
                for (unsigned int ir = 0; ir < buf->glyphCount; ir++) {
                    const int i = (run->flags.isRTL ? buf->glyphCount - ir - 1 : ir);
                    const hb_glyph_info_t *info    = &buf->glyphInfo[i];
                    const hb_codepoint_t   glyphId = info->codepoint;
                    const int              logPos  = info->cluster;
                    if (logPos < wrapPosRange.start || logPos >= wrapPosRange.end) {
                        continue;
                    }
                    if (checkHitChar) {
                        if (wrap->hitChar == sourcePtr_AttributedText_(&attrText, logPos)) {
                            wrap->hitAdvance_out = init_I2(wrapAdvance, yCursor);
                        }
                    }
                    /* Check if the hit point is on the left side of this line. */
                    if (isHitPointOnThisLine && !wrap->hitChar_out && wrap->hitPoint.x < orig.x) {
                        wrap->hitChar_out = sourcePtr_AttributedText_(&attrText, logPos);
                        wrap->hitGlyphNormX_out = 0.0f;
                    }
                    const iGlyph *glyph      = glyphByIndex_Font_(run->font, glyphId);
                    const int     glyphFlags = hb_glyph_info_get_glyph_flags(info);
                    const float   xOffset    = run->font->xScale * buf->glyphPos[i].x_offset;
                    const float   xAdvance   = run->font->xScale * buf->glyphPos[i].x_advance;
                    iAssert(xAdvance >= 0);
                    if (args->wrap->mode == word_WrapTextMode) {
                        /* When word wrapping, only consider certain places breakable. */
                        const iChar ch = logicalText[logPos];
                        if ((ch >= 128 || !ispunct(ch)) && (prevCh == '-' || prevCh == '/')) {
                            safeBreakPos = logPos;
                            breakAdvance = wrapAdvance;
    //                        isSoftHyphenBreak = iFalse;
                        }
                        else if (isSpace_Char(ch)) {
                            safeBreakPos = logPos;
                            breakAdvance = wrapAdvance;
    //                        isSoftHyphenBreak = iFalse;
                        }
                        prevCh = ch;
                    }
                    else {
                        //if (~glyphFlags & HB_GLYPH_FLAG_UNSAFE_TO_BREAK) {
                            safeBreakPos = logPos;
                            breakAdvance = wrapAdvance;
                        //}
                    }
                    if (isHitPointOnThisLine) {
                        if (wrap->hitPoint.x >= orig.x + wrapAdvance &&
                            wrap->hitPoint.x < orig.x + wrapAdvance + xAdvance) {
                            wrap->hitChar_out = sourcePtr_AttributedText_(&attrText, logPos);
                            wrap->hitGlyphNormX_out = (wrap->hitPoint.x - wrapAdvance) / xAdvance;
                        }
                    }
                    /* Out of room? */
                    if (wrap->maxWidth > 0 &&
                        wrapAdvance + xOffset + glyph->d[0].x + glyph->rect[0].size.x >
                        args->wrap->maxWidth) {
                        if (safeBreakPos >= 0) {
                            wrapPosRange.end = safeBreakPos;
                        }
                        else {
                            if (args->wrap->mode == word_WrapTextMode && run->logical.start > wrapPosRange.start) {
                                /* Don't have a word break position, so the whole run needs
                                   to be cut. */
                                wrapPosRange.end = run->logical.start;
                                wrapResumePos = run->logical.start;
                                wrapRuns.end = runIndex + 1;
                                wrapResumeRunIndex = runIndex;
                                break;
                            }
                            wrapPosRange.end = logPos;
                            breakAdvance     = wrapAdvance;
                        }
                        wrapResumePos      = wrapPosRange.end;
                        if (args->wrap->mode != anyCharacter_WrapTextMode) {
                            while (wrapResumePos < textLen && isSpace_Char(logicalText[wrapResumePos])) {
                                wrapResumePos++; /* skip space */
                            }
                        }
                        wrapRuns.end       = runIndex + 1; /* still includes this run */
                        wrapResumeRunIndex = runIndex;     /* ...but continue from the same one */
                        wrapAdvance        = breakAdvance;
                        break;
                    }
                    wrapAdvance += xAdvance;
                    /* Additional kerning tweak. It would be better to use HarfBuzz font callbacks,
                       but they don't seem to get called? */
                    if (i + 1 < buf->glyphCount) {
                        wrapAdvance += horizKern_Font_(buf->font,
                                                       glyphId,
                                                       buf->glyphInfo[i + 1].codepoint);
                    }
                }
            }
            if (isHitPointOnThisLine && wrap->hitPoint.x >= orig.x + wrapAdvance) {
                /* On the right side. */
                if (wrapResumePos == textLen) {
                    wrap->hitChar_out = sourcePtr_AttributedText_(&attrText, wrapResumePos);
                }
                else {
                    const char *hit = sourcePtr_AttributedText_(&attrText, iMax(0, wrapResumePos - 1));
                    while (hit > args->text.start) {
                        if (!isSpace_Char(hit[-1])) break;
                        hit--;
                    }
                    wrap->hitChar_out = hit;
                }
                wrap->hitGlyphNormX_out = 0.0f;
            }
        }
        else {
            /* Not wrapped so everything fits! Calculate total advance without wrapping. */
            for (size_t i = wrapRuns.start; i < wrapRuns.end; i++) {
                wrapAdvance += advance_GlyphBuffer_(at_Array(&buffers, i), wrapPosRange);
            }
        }
        iArray runOrder;
        init_Array(&runOrder, sizeof(size_t));
        /* Reorder the run indices according to text direction. */ {
            size_t oppositeInsertIndex = iInvalidPos;
            for (size_t runIndex = wrapRuns.start; runIndex < wrapRuns.end; runIndex++) {
                const iAttributedRun *run = at_Array(&attrText.runs, runIndex);
                if (!attrText.isBaseRTL) { /* left-to-right */
                    if (run->flags.isRTL) {
                        if (oppositeInsertIndex == iInvalidPos) {
                            oppositeInsertIndex = size_Array(&runOrder);
                        }
                        insert_Array(&runOrder, oppositeInsertIndex, &runIndex);
                    }
                    else {
                        pushBack_Array(&runOrder, &runIndex);
                        oppositeInsertIndex = iInvalidPos;
                    }
                }
                else { /* right-to-left */
                    if (!run->flags.isRTL) {
                        if (oppositeInsertIndex == iInvalidPos) {
                            oppositeInsertIndex = 0;
                        }
                        insert_Array(&runOrder, oppositeInsertIndex++, &runIndex);
                    }
                    else {
                        pushFront_Array(&runOrder, &runIndex);
                        oppositeInsertIndex = iInvalidPos;
                    }
                }
            }
        }
#if 0
        printf("Run order: ");
        iConstForEach(Array, ro, &runOrder) {
            const size_t *idx = ro.value;
            printf("%zu {%s}\n", *idx,
                   cstr_Rangecc(sourceRange_AttributedText_(&attrText,                                                                   ((const iAttributedRun *) at_Array(&attrText.runs, *idx))->logical)));
        }
        printf("\n");
#endif
        iAssert(size_Array(&runOrder) == size_Range(&wrapRuns));
        /* Alignment. */
        int origin = 0;
        iBool isRightAligned = attrText.isBaseRTL;
        if (isRightAligned) {
            if (layoutBound > 0) {
                origin = layoutBound - wrapAdvance;
            }
//            else if (args->xposLayoutBound > 0) {
//                iAssert(mode & draw_RunMode);
////                origin = args->xposLayoutBound - orig.x - wrapAdvance * 2;
//            }
        }
        /* Make a callback for each wrapped line. */
        if (wrap && wrap->wrapFunc &&
            !notify_WrapText_(args->wrap,
                              sourcePtr_AttributedText_(&attrText, wrapResumePos),
                              origin,
                              iRound(wrapAdvance),
                              attrText.isBaseRTL)) {
            willAbortDueToWrap = iTrue;
        }
        xCursor = origin;
        /* We have determined a possible wrap position and alignment for the work runs,
           so now we can process the glyphs. */
        for (size_t logRunIndex = 0; logRunIndex < size_Array(&runOrder); logRunIndex++) {
            const size_t runIndex = constValue_Array(&runOrder, logRunIndex, size_t);
            const iAttributedRun *run = at_Array(&attrText.runs, runIndex);
            if (run->flags.isLineBreak) {
                xCursor = 0.0f;
                yCursor += d->height;
                continue;
            }
            iGlyphBuffer *buf = at_Array(&buffers, runIndex);
            shape_GlyphBuffer_(buf);
            iAssert(run->font == buf->font);
            /* Process all the glyphs. */
            for (unsigned int i = 0; i < buf->glyphCount; i++) {
                const hb_glyph_info_t *info    = &buf->glyphInfo[i];
                const hb_codepoint_t   glyphId = info->codepoint;
                const int              logPos  = info->cluster;
                if (logPos < wrapPosRange.start || logPos >= wrapPosRange.end) {
                    /* Already handled this part of the run. */
                    continue;
                }
                const float xOffset  = run->font->xScale * buf->glyphPos[i].x_offset;
                const float yOffset  = run->font->yScale * buf->glyphPos[i].y_offset;
                const float xAdvance = run->font->xScale * buf->glyphPos[i].x_advance;
                const float yAdvance = run->font->yScale * buf->glyphPos[i].y_advance;
                const iGlyph *glyph = glyphByIndex_Font_(run->font, glyphId);
                const float xf = xCursor + xOffset;
                const int hoff = enableHalfPixelGlyphs_Text ? (xf - ((int) xf) > 0.5f ? 1 : 0) : 0;
                /* Output position for the glyph. */
                SDL_Rect dst = { orig.x + xCursor + xOffset + glyph->d[hoff].x,
                                 orig.y + yCursor - yOffset + glyph->font->baseline + glyph->d[hoff].y,
                                 glyph->rect[hoff].size.x,
                                 glyph->rect[hoff].size.y };
                if (mode & visualFlag_RunMode) {
                    if (isEmpty_Rect(bounds)) {
                        bounds = init_Rect(dst.x, dst.y, dst.w, dst.h);
                    }
                    else {
                        bounds = union_Rect(bounds, init_Rect(dst.x, dst.y, dst.w, dst.h));
                    }
                }
                else {
                    bounds.size.x = iMax(bounds.size.x, dst.x + dst.w - orig.x);
                    bounds.size.y = iMax(bounds.size.y, yCursor + glyph->font->height);
                }
                if (mode & draw_RunMode && logicalText[logPos] > 0x20) {
                    /* Draw the glyph. */
                    if (!isRasterized_Glyph_(glyph, hoff)) {
                        cacheSingleGlyph_Font_(run->font, glyphId); /* may cause cache reset */
                        glyph = glyphByIndex_Font_(run->font, glyphId);
                        iAssert(isRasterized_Glyph_(glyph, hoff));
                    }
                    if (~mode & permanentColorFlag_RunMode) {
                        const iColor clr = run->fgColor;
                        SDL_SetTextureColorMod(text_.cache, clr.r, clr.g, clr.b);
                        if (args->mode & fillBackground_RunMode) {
                            SDL_SetRenderDrawColor(text_.render, clr.r, clr.g, clr.b, 0);
                        }
                    }
                    SDL_Rect src;
                    memcpy(&src, &glyph->rect[hoff], sizeof(SDL_Rect));
                    if (args->mode & fillBackground_RunMode) {
                        /* Alpha blending looks much better if the RGB components don't change in
                           the partially transparent pixels. */
                        /* TODO: Backgrounds of all glyphs should be cleared before drawing anything else. */
                        SDL_RenderFillRect(text_.render, &dst);
                    }
                    SDL_RenderCopy(text_.render, text_.cache, &src, &dst);
#if 0
                    /* Show spaces and direction. */
                    if (logicalText[logPos] == 0x20) {
                        const iColor debug = get_Color(run->flags.isRTL ? yellow_ColorId : red_ColorId);
                        SDL_SetRenderDrawColor(text_.render, debug.r, debug.g, debug.b, 255);
                        dst.w = xAdvance;
                        dst.h = d->height / 2;
                        dst.y -= d->height / 2;
                        SDL_RenderFillRect(text_.render, &dst);
                    }
#endif
                }
                xCursor += xAdvance;
                yCursor += yAdvance;
                /* Additional kerning tweak. It would be better to use HarfBuzz font callbacks,
                   but they don't seem to get called? */
                if (i + 1 < buf->glyphCount) {
                    xCursor += horizKern_Font_(run->font, glyphId, buf->glyphInfo[i + 1].codepoint);
                }
                xCursorMax = iMax(xCursorMax, xCursor);
            }
        }
        deinit_Array(&runOrder);
        if (willAbortDueToWrap) {
            break;
        }
        wrapRuns.start     = wrapResumeRunIndex;
        wrapRuns.end       = runCount;
        wrapPosRange.start = wrapResumePos;
        wrapPosRange.end   = textLen;
    }
    if (checkHitChar && wrap->hitChar == args->text.end) {
        wrap->hitAdvance_out = init_I2(xCursor, yCursor);
    }
    if (endsWith_Rangecc(args->text, "\n")) {
        /* FIXME: This is a kludge, the wrap loop should handle this case, too. */
        /* The last wrap is an empty newline wrap. */
        xCursor = 0;
        yCursor += d->height;
    }
    if (args->cursorAdvance_out) {
        *args->cursorAdvance_out = init_I2(xCursor, yCursor);
    }
    if (args->runAdvance_out) {
        *args->runAdvance_out = xCursorMax;
    }
    iForEach(Array, b, &buffers) {
        deinit_GlyphBuffer_(b.value);
    }
    deinit_AttributedText(&attrText);
    return bounds;
}

#else /* !defined (LAGRANGE_ENABLE_HARFBUZZ) */

/* The fallback method: an incomplete solution for simple scripts. */
#   define run_Font_    runSimple_Font_
#   include "text_simple.c"

#endif /* defined (LAGRANGE_ENABLE_HARFBUZZ) */

int lineHeight_Text(int fontId) {
    return font_Text_(fontId)->height;
}

iTextMetrics measureRange_Text(int fontId, iRangecc text) {
    if (isEmpty_Range(&text)) {
        return (iTextMetrics){ init_Rect(0, 0, 0, lineHeight_Text(fontId)), zero_I2() };
    }
    iTextMetrics tm;
    tm.bounds = run_Font_(font_Text_(fontId), &(iRunArgs){
        .mode = measure_RunMode,
        .text = text,
        .cursorAdvance_out = &tm.advance
    });
    return tm;
}

iRect visualBounds_Text(int fontId, iRangecc text) {
    return run_Font_(font_Text_(fontId),
                     &(iRunArgs){
                         .mode = measure_RunMode | visualFlag_RunMode,
                         .text = text,
                     });
}

void cache_Text(int fontId, iRangecc text) {
    cacheTextGlyphs_Font_(font_Text_(fontId), text);
}

static int runFlagsFromId_(enum iFontId fontId) {
    int runFlags = 0;
    if (fontId & alwaysVariableFlag_FontId) {
        runFlags |= alwaysVariableWidthFlag_RunMode;
    }
    return runFlags;
}

static iBool cbAdvanceOneLine_(iWrapText *d, iRangecc range, int origin, int advance,
                               iBool isBaseRTL) {
    iUnused(origin, advance, isBaseRTL);
    *((const char **) d->context) = range.end;
    return iFalse; /* just one line */
}

iInt2 tryAdvance_Text(int fontId, iRangecc text, int width, const char **endPos) {
    iWrapText wrap = { .mode     = word_WrapTextMode,
                       .text     = text,
                       .maxWidth = width,
                       .wrapFunc = cbAdvanceOneLine_,
                       .context  = endPos };
    /* The return value is expected to be the horizontal/vertical bounds. */
    return measure_WrapText(&wrap, fontId).bounds.size;
}

iInt2 tryAdvanceNoWrap_Text(int fontId, iRangecc text, int width, const char **endPos) {
    /* "NoWrap" means words aren't wrapped; the line is broken at nearest character. */
    iWrapText wrap = { .mode     = anyCharacter_WrapTextMode,
                       .text     = text,
                       .maxWidth = width,
                       .wrapFunc = cbAdvanceOneLine_,
                       .context  = endPos };
    iTextMetrics tm = measure_WrapText(&wrap, fontId);
    return init_I2(maxWidth_TextMetrics(tm), tm.bounds.size.y);
}

iTextMetrics measureN_Text(int fontId, const char *text, size_t n) {
    if (n == 0) {
        return (iTextMetrics){ init_Rect(0, 0, 0, lineHeight_Text(fontId)),
                               zero_I2() };
    }
    iTextMetrics tm;
    tm.bounds = run_Font_(font_Text_(fontId),
              &(iRunArgs){ .mode              = measure_RunMode | runFlagsFromId_(fontId),
                           .text              = range_CStr(text),
                           .maxLen            = n,
                           .cursorAdvance_out = &tm.advance });
    return tm;
}

static void drawBoundedN_Text_(int fontId, iInt2 pos, int xposBound, int color, iRangecc text, size_t maxLen) {
    iText *      d    = &text_;
    iFont *      font = font_Text_(fontId);
    const iColor clr  = get_Color(color & mask_ColorId);
    SDL_SetTextureColorMod(d->cache, clr.r, clr.g, clr.b);
    run_Font_(font,
              &(iRunArgs){ .mode = draw_RunMode |
                                   (color & permanent_ColorId ? permanentColorFlag_RunMode : 0) |
                                   (color & fillBackground_ColorId ? fillBackground_RunMode : 0) |
                                   runFlagsFromId_(fontId),
                           .text            = text,
                           .maxLen          = maxLen,                           
                           .pos             = pos,
//                           .xposLayoutBound = xposBound,
                           .color           = color & mask_ColorId,
                           .baseDir         = xposBound ? iSign(xposBound - pos.x) : 0 });
}

static void drawBounded_Text_(int fontId, iInt2 pos, int xposBound, int color, iRangecc text) {
    drawBoundedN_Text_(fontId, pos, xposBound, color, text, 0);
}

static void draw_Text_(int fontId, iInt2 pos, int color, iRangecc text) {
    drawBounded_Text_(fontId, pos, 0, color, text);
}

void drawAlign_Text(int fontId, iInt2 pos, int color, enum iAlignment align, const char *text, ...) {
    iBlock chars;
    init_Block(&chars, 0); {
        va_list args;
        va_start(args, text);
        vprintf_Block(&chars, text, args);
        va_end(args);
    }
    if (align == center_Alignment) {
        pos.x -= measure_Text(fontId, cstr_Block(&chars)).bounds.size.x / 2;
    }
    else if (align == right_Alignment) {
        pos.x -= measure_Text(fontId, cstr_Block(&chars)).bounds.size.x;
    }
    draw_Text_(fontId, pos, color, range_Block(&chars));
    deinit_Block(&chars);
}

void draw_Text(int fontId, iInt2 pos, int color, const char *text, ...) {
    iBlock chars;
    init_Block(&chars, 0); {
        va_list args;
        va_start(args, text);
        vprintf_Block(&chars, text, args);
        va_end(args);
    }
    draw_Text_(fontId, pos, color, range_Block(&chars));
    deinit_Block(&chars);
}

void drawString_Text(int fontId, iInt2 pos, int color, const iString *text) {
    draw_Text_(fontId, pos, color, range_String(text));
}

void drawRange_Text(int fontId, iInt2 pos, int color, iRangecc text) {
    draw_Text_(fontId, pos, color, text);
}

void drawRangeN_Text(int fontId, iInt2 pos, int color, iRangecc text, size_t maxChars) {
    drawBoundedN_Text_(fontId, pos, 0, color, text, maxChars);
}

void drawOutline_Text(int fontId, iInt2 pos, int outlineColor, int fillColor, iRangecc text) {
    for (int off = 0; off < 4; ++off) {
        drawRange_Text(fontId,
                       add_I2(pos, init_I2(off % 2 == 0 ? -1 : 1, off / 2 == 0 ? -1 : 1)),
                       outlineColor,
                       text);
    }
    if (fillColor != none_ColorId) {
        drawRange_Text(fontId, pos, fillColor, text);
    }
}

iTextMetrics measureWrapRange_Text(int fontId, int maxWidth, iRangecc text) {
    iWrapText wrap = { .text = text, .maxWidth = maxWidth, .mode = word_WrapTextMode };
    return measure_WrapText(&wrap, fontId);
}

void drawBoundRange_Text(int fontId, iInt2 pos, int boundWidth, int color, iRangecc text) {
    /* This function is used together with text that has already been wrapped, so we'll know
       the bound width but don't have to re-wrap the text. */
    drawBounded_Text_(fontId, pos, pos.x + boundWidth, color, text);
}

int drawWrapRange_Text(int fontId, iInt2 pos, int maxWidth, int color, iRangecc text) {
    const char *endp;
    while (!isEmpty_Range(&text)) {
        const iInt2 adv = tryAdvance_Text(fontId, text, maxWidth, &endp);
        drawRange_Text(fontId, pos, color, (iRangecc){ text.start, endp });
        text.start = endp;
        pos.y += iMax(adv.y, lineHeight_Text(fontId));
    }
    return pos.y;
}

void drawCentered_Text(int fontId, iRect rect, iBool alignVisual, int color, const char *format, ...) {
    iBlock chars;
    init_Block(&chars, 0); {
        va_list args;
        va_start(args, format);
        vprintf_Block(&chars, format, args);
        va_end(args);
    }
    drawCenteredRange_Text(fontId, rect, alignVisual, color, range_Block(&chars));
    deinit_Block(&chars);
}

void drawCenteredOutline_Text(int fontId, iRect rect, iBool alignVisual, int outlineColor,
                              int fillColor, const char *format, ...) {
    iBlock chars;
    init_Block(&chars, 0); {
        va_list args;
        va_start(args, format);
        vprintf_Block(&chars, format, args);
        va_end(args);
    }
    if (outlineColor != none_ColorId) {
        for (int off = 0; off < 4; ++off) {
            drawCenteredRange_Text(
                fontId,
                moved_Rect(rect, init_I2(off % 2 == 0 ? -1 : 1, off / 2 == 0 ? -1 : 1)),
                alignVisual,
                outlineColor,
                range_Block(&chars));
        }
    }
    if (fillColor != none_ColorId) {
        drawCenteredRange_Text(fontId, rect, alignVisual, fillColor, range_Block(&chars));
    }
    deinit_Block(&chars);
}

void drawCenteredRange_Text(int fontId, iRect rect, iBool alignVisual, int color, iRangecc text) {
    iRect textBounds = alignVisual ? visualBounds_Text(fontId, text)
                                   : measureRange_Text(fontId, text).bounds;
    textBounds.pos = sub_I2(mid_Rect(rect), mid_Rect(textBounds));
    textBounds.pos.x = iMax(textBounds.pos.x, left_Rect(rect)); /* keep left edge visible */
    draw_Text_(fontId, textBounds.pos, color, text);
}

iTextMetrics measure_WrapText(iWrapText *d, int fontId) {
    iTextMetrics tm;
    tm.bounds = run_Font_(font_Text_(fontId),
                          &(iRunArgs){ .mode = measure_RunMode | runFlagsFromId_(fontId),
                                       .text = d->text,
                                       .wrap = d,
                                       .cursorAdvance_out = &tm.advance });
    return tm;
}

iTextMetrics draw_WrapText(iWrapText *d, int fontId, iInt2 pos, int color) {
    iTextMetrics tm;
#if !defined (LAGRANGE_ENABLE_HARFBUZZ)
    /* In simple mode, each line must be wrapped first so we can break at the right points
       and do wrap notifications before drawing. */
    iRangecc text = d->text;
    iZap(tm);
    d->wrapRange_ = (iRangecc){ d->text.start, d->text.start };
    const iInt2 orig = pos;
    while (!isEmpty_Range(&text)) {
        const char *endPos;
        const int width = d->mode == word_WrapTextMode
                              ? tryAdvance_Text(fontId, text, d->maxWidth, &endPos).x
                              : tryAdvanceNoWrap_Text(fontId, text, d->maxWidth, &endPos).x;
        notify_WrapText_(d, endPos, 0, width, iFalse);
        drawRange_Text(fontId, pos, color, (iRangecc){ text.start, endPos });
        text.start = endPos;
        pos.y += lineHeight_Text(fontId);
        tm.bounds.size.x = iMax(tm.bounds.size.x, width);
        tm.bounds.size.y = pos.y - orig.y;
    }
    tm.advance = sub_I2(pos, orig);
#else
    tm.bounds = run_Font_(font_Text_(fontId),
              &(iRunArgs){ .mode  = draw_RunMode | runFlagsFromId_(fontId) |
                                    (color & permanent_ColorId ? permanentColorFlag_RunMode : 0) |
                                    (color & fillBackground_ColorId ? fillBackground_RunMode : 0),
                           .text  = d->text,
                           .pos   = pos,
                           .wrap  = d,
                           .color = color & mask_ColorId,
                           .cursorAdvance_out = &tm.advance,
    });
#endif
    return tm;
}

SDL_Texture *glyphCache_Text(void) {
    return text_.cache;
}

static void freeBitmap_(void *ptr) {
    stbtt_FreeBitmap(ptr, NULL);
}

iString *renderBlockChars_Text(const iBlock *fontData, int height, enum iTextBlockMode mode,
                               const iString *text) {
    iBeginCollect();
    stbtt_fontinfo font;
    iZap(font);
    stbtt_InitFont(&font, constData_Block(fontData), 0);
    int ascent;
    stbtt_GetFontVMetrics(&font, &ascent, NULL, NULL);
    iDeclareType(CharBuf);
    struct Impl_CharBuf {
        uint8_t *pixels;
        iInt2 size;
        int dy;
        int advance;
    };
    iArray *    chars     = collectNew_Array(sizeof(iCharBuf));
    int         pxRatio   = (mode == quadrants_TextBlockMode ? 2 : 1);
    int         pxHeight  = height * pxRatio;
    const float scale     = stbtt_ScaleForPixelHeight(&font, pxHeight);
    const float xScale    = scale * 2; /* character aspect ratio */
    const int   baseline  = ascent * scale;
    int         width     = 0;
    size_t      strRemain = length_String(text);
    iConstForEach(String, i, text) {
        if (!strRemain) break;
        if (isVariationSelector_Char(i.value) || isDefaultIgnorable_Char(i.value)) {
            strRemain--;
            continue;
        }
        iCharBuf buf = { .size = zero_I2() };
        buf.pixels = stbtt_GetCodepointBitmap(
            &font, xScale, scale, i.value, &buf.size.x, &buf.size.y, 0, &buf.dy);
        stbtt_GetCodepointHMetrics(&font, i.value, &buf.advance, NULL);
        buf.advance *= xScale;
        if (!isSpace_Char(i.value)) {
            if (mode == quadrants_TextBlockMode) {
                buf.advance = (buf.size.x - 1) / 2 * 2 + 2;
            }
            else {
                buf.advance = buf.size.x + 1;
            }
        }
        pushBack_Array(chars, &buf);
        collect_Garbage(buf.pixels, freeBitmap_);
        width += buf.advance;
        strRemain--;
    }
    const size_t len = (mode == quadrants_TextBlockMode ? height * ((width + 1) / 2 + 1)
                                                        : (height * (width + 1)));
    iChar *outBuf = iCollectMem(malloc(sizeof(iChar) * len));
    for (size_t i = 0; i < len; ++i) {
        outBuf[i] = 0x20;
    }
    iChar *outPos = outBuf;
    for (int y = 0; y < pxHeight; y += pxRatio) {
        const iCharBuf *ch = constData_Array(chars);
        int lx = 0;
        for (int x = 0; x < width; x += pxRatio, lx += pxRatio) {
            if (lx >= ch->advance) {
                ch++;
                lx = 0;
            }
            const int ly = y - baseline - ch->dy;
            if (mode == quadrants_TextBlockMode) {
                #define checkPixel_(offx, offy) \
                    (lx + offx < ch->size.x && ly + offy < ch->size.y && ly + offy >= 0 ? \
                        ch->pixels[(lx + offx) + (ly + offy) * ch->size.x] > 155 \
                        : iFalse)
                const int mask = (checkPixel_(0, 0) ? 1 : 0) |
                                 (checkPixel_(1, 0) ? 2 : 0) |
                                 (checkPixel_(0, 1) ? 4 : 0) |
                                 (checkPixel_(1, 1) ? 8 : 0);
                #undef checkPixel_
                static const iChar blocks[16] = { 0x0020, 0x2598, 0x259D, 0x2580, 0x2596, 0x258C,
                                                  0x259E, 0x259B, 0x2597, 0x259A, 0x2590, 0x259C,
                                                  0x2584, 0x2599, 0x259F, 0x2588 };
                *outPos++ = blocks[mask];
            }
            else {
                static const iChar shades[5] = { 0x0020, 0x2591, 0x2592, 0x2593, 0x2588 };
                *outPos++ = shades[lx < ch->size.x && ly < ch->size.y && ly >= 0 ?
                                   ch->pixels[lx + ly * ch->size.x] * 5 / 256 : 0];
            }
        }
        *outPos++ = '\n';
    }
    /* We could compose the lines separately, but we'd still need to convert them to Strings
       individually to trim them. */
    iStringList *lines = split_String(collect_String(newUnicodeN_String(outBuf, len)), "\n");
    while (!isEmpty_StringList(lines) &&
           isEmpty_String(collect_String(trimmed_String(at_StringList(lines, 0))))) {
        popFront_StringList(lines);
    }
    while (!isEmpty_StringList(lines) && isEmpty_String(collect_String(trimmed_String(
                                             at_StringList(lines, size_StringList(lines) - 1))))) {
        popBack_StringList(lines);
    }
    iEndCollect();
    return joinCStr_StringList(iClob(lines), "\n");
}

/*-----------------------------------------------------------------------------------------------*/

iDefineTypeConstructionArgs(TextBuf, (iWrapText *wrapText, int font, int color), wrapText, font, color)

void init_TextBuf(iTextBuf *d, iWrapText *wrapText, int font, int color) {
    SDL_Renderer *render = text_.render;
    d->size = measure_WrapText(wrapText, font).bounds.size;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    if (d->size.x * d->size.y) {
        d->texture = SDL_CreateTexture(render,
                                       SDL_PIXELFORMAT_RGBA4444,
                                       SDL_TEXTUREACCESS_STATIC | SDL_TEXTUREACCESS_TARGET,
                                       d->size.x,
                                       d->size.y);
    }
    else {
        d->texture = NULL;
    }
    if (d->texture) {
        SDL_Texture *oldTarget = SDL_GetRenderTarget(render);
        SDL_SetRenderTarget(render, d->texture);
        SDL_SetRenderDrawBlendMode(render, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(render, 255, 255, 255, 0);
        SDL_RenderClear(render);
        SDL_SetTextureBlendMode(text_.cache, SDL_BLENDMODE_NONE); /* blended when TextBuf is drawn */
        draw_WrapText(wrapText, font, zero_I2(), color | fillBackground_ColorId);
        SDL_SetTextureBlendMode(text_.cache, SDL_BLENDMODE_BLEND);
        SDL_SetRenderTarget(render, oldTarget);
        SDL_SetTextureBlendMode(d->texture, SDL_BLENDMODE_BLEND);
    }
}

void deinit_TextBuf(iTextBuf *d) {
    SDL_DestroyTexture(d->texture);
}

iTextBuf *newRange_TextBuf(int font, int color, iRangecc text) {
    return new_TextBuf(&(iWrapText){ .text = text }, font, color);
}

void draw_TextBuf(const iTextBuf *d, iInt2 pos, int color) {
    const iColor clr = get_Color(color);
    SDL_SetTextureColorMod(d->texture, clr.r, clr.g, clr.b);
    SDL_RenderCopy(text_.render,
                   d->texture,
                   &(SDL_Rect){ 0, 0, d->size.x, d->size.y },
                   &(SDL_Rect){ pos.x, pos.y, d->size.x, d->size.y });
}
