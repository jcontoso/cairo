/* -*- Mode: c; tab-width: 8; c-basic-offset: 4; indent-tabs-mode: t; -*- */
/* Cairo - a vector graphics library with display and print output
 *
 * Copyright © 2010 Mozilla Foundation
 *
 * This library is free software; you can redistribute it and/or
 * modify it either under the terms of the GNU Lesser General Public
 * License version 2.1 as published by the Free Software Foundation
 * (the "LGPL") or, at your option, under the terms of the Mozilla
 * Public License Version 1.1 (the "MPL"). If you do not alter this
 * notice, a recipient may use your version of this file under either
 * the MPL or the LGPL.
 *
 * You should have received a copy of the LGPL along with this library
 * in the file COPYING-LGPL-2.1; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 * You should have received a copy of the MPL along with this library
 * in the file COPYING-MPL-1.1
 *
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY
 * OF ANY KIND, either express or implied. See the LGPL or the MPL for
 * the specific language governing rights and limitations.
 *
 * The Original Code is the cairo graphics library.
 *
 * The Initial Developer of the Original Code is the Mozilla Foundation
 *
 * Contributor(s):
 *	Bas Schouten <bschouten@mozilla.com>
 */

#include "cairoint.h"

#include "cairo-win32-private.h"
#include "cairo-pattern-private.h"
#include "cairo-surface-private.h"
#include "cairo-image-surface-private.h"
#include "cairo-clip-private.h"
#include "cairo-win32-refptr.hpp"
#include "cairo-dwrite-private.hpp"
#include "cairo-truetype-subset-private.h"
#include "cairo-scaled-font-subsets-private.h"
#include <float.h>

#include <wincodec.h>

typedef HRESULT (WINAPI*D2D1CreateFactoryFunc)(
    D2D1_FACTORY_TYPE factoryType,
    REFIID iid,
    CONST D2D1_FACTORY_OPTIONS *pFactoryOptions,
    void **factory
);

#define CAIRO_INT_STATUS_SUCCESS (cairo_int_status_t)CAIRO_STATUS_SUCCESS

// Forward declarations
cairo_int_status_t
_dwrite_draw_glyphs_to_gdi_surface_d2d(cairo_win32_surface_t *surface,
				       DWRITE_MATRIX *transform,
				       DWRITE_GLYPH_RUN *run,
				       COLORREF color,
				       const RECT &area);

static cairo_int_status_t
_dwrite_draw_glyphs_to_gdi_surface_gdi(cairo_win32_surface_t *surface,
				       DWRITE_MATRIX *transform,
				       DWRITE_GLYPH_RUN *run,
				       COLORREF color,
				       cairo_dwrite_scaled_font_t *scaled_font,
				       const RECT &area);

/**
 * _cairo_dwrite_error:
 * @hr HRESULT code
 * @context: context string to display along with the error
 *
 * Helper function to print a human readable form a HRESULT.
 *
 * Return value: A cairo status code for the error code
 **/
static cairo_int_status_t
_cairo_dwrite_error (HRESULT hr, const char *context)
{
    void *lpMsgBuf;

    if (!FormatMessageW (FORMAT_MESSAGE_ALLOCATE_BUFFER |
			 FORMAT_MESSAGE_FROM_SYSTEM,
			 NULL,
			 hr,
			 MAKELANGID (LANG_NEUTRAL, SUBLANG_DEFAULT),
			 (LPWSTR) &lpMsgBuf,
			 0, NULL)) {
	fprintf (stderr, "%s: Unknown DWrite error HRESULT=0x%08lx\n", context, (unsigned long)hr);
    } else {
	fprintf (stderr, "%s: %S\n", context, (wchar_t *)lpMsgBuf);
	LocalFree (lpMsgBuf);
    }
    fflush (stderr);

    return (cairo_int_status_t)_cairo_error (CAIRO_STATUS_DWRITE_ERROR);
}

class D2DFactory
{
public:
    static RefPtr<ID2D1Factory> Instance()
    {
	if (!mFactoryInstance) {
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
	    D2D1CreateFactoryFunc createD2DFactory = (D2D1CreateFactoryFunc)
		GetProcAddress(LoadLibraryW(L"d2d1.dll"), "D2D1CreateFactory");
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
	    if (createD2DFactory) {
		D2D1_FACTORY_OPTIONS options;
		options.debugLevel = D2D1_DEBUG_LEVEL_NONE;
		createD2DFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
				 __uuidof(ID2D1Factory),
				 &options,
				 (void**)&mFactoryInstance);
	    }
	}
	return mFactoryInstance;
    }

    static RefPtr<IDWriteFactory4> Instance4()
    {
	if (!mFactoryInstance4) {
	    if (Instance()) {
		Instance()->QueryInterface(&mFactoryInstance4);
	    }
	}
	return mFactoryInstance4;
    }

    static RefPtr<ID2D1DCRenderTarget> RenderTarget()
    {
	if (!mRenderTarget) {
	    if (!Instance()) {
		return NULL;
	    }
	    // Create a DC render target.
	    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
		D2D1_RENDER_TARGET_TYPE_DEFAULT,
		D2D1::PixelFormat(
		    DXGI_FORMAT_B8G8R8A8_UNORM,
		    D2D1_ALPHA_MODE_PREMULTIPLIED),
		0,
		0,
		D2D1_RENDER_TARGET_USAGE_NONE,
		D2D1_FEATURE_LEVEL_DEFAULT
		);

	    Instance()->CreateDCRenderTarget(&props, &mRenderTarget);
	}
	return mRenderTarget;
    }

private:
    static RefPtr<ID2D1Factory> mFactoryInstance;
    static RefPtr<IDWriteFactory4> mFactoryInstance4;
    static RefPtr<ID2D1DCRenderTarget> mRenderTarget;
};

class WICImagingFactory
{
public:
    static RefPtr<IWICImagingFactory> Instance()
    {
	if (!mFactoryInstance) {
	    CoInitialize(NULL);
	    CoCreateInstance(CLSID_WICImagingFactory,
			     NULL,
			     CLSCTX_INPROC_SERVER,
			     IID_PPV_ARGS(&mFactoryInstance));
	}
	return mFactoryInstance;
    }
private:
    static RefPtr<IWICImagingFactory> mFactoryInstance;
};


RefPtr<IDWriteFactory> DWriteFactory::mFactoryInstance;
RefPtr<IDWriteFactory4> DWriteFactory::mFactoryInstance4;

RefPtr<IWICImagingFactory> WICImagingFactory::mFactoryInstance;
RefPtr<IDWriteFontCollection> DWriteFactory::mSystemCollection;
RefPtr<IDWriteRenderingParams> DWriteFactory::mDefaultRenderingParams;
RefPtr<IDWriteRenderingParams> DWriteFactory::mCustomClearTypeRenderingParams;
RefPtr<IDWriteRenderingParams> DWriteFactory::mForceGDIClassicRenderingParams;
FLOAT DWriteFactory::mGamma = -1.0;
FLOAT DWriteFactory::mEnhancedContrast = -1.0;
FLOAT DWriteFactory::mClearTypeLevel = -1.0;
int DWriteFactory::mPixelGeometry = -1;
int DWriteFactory::mRenderingMode = -1;

RefPtr<ID2D1Factory> D2DFactory::mFactoryInstance;
RefPtr<ID2D1DCRenderTarget> D2DFactory::mRenderTarget;

/* Functions #cairo_font_face_backend_t */
static cairo_status_t
_cairo_dwrite_font_face_create_for_toy (cairo_toy_font_face_t   *toy_face,
					cairo_font_face_t      **font_face);
static cairo_bool_t
_cairo_dwrite_font_face_destroy (void *font_face);

static cairo_status_t
_cairo_dwrite_font_face_scaled_font_create (void			*abstract_face,
					    const cairo_matrix_t	*font_matrix,
					    const cairo_matrix_t	*ctm,
					    const cairo_font_options_t *options,
					    cairo_scaled_font_t **font);

const cairo_font_face_backend_t _cairo_dwrite_font_face_backend = {
    CAIRO_FONT_TYPE_DWRITE,
    _cairo_dwrite_font_face_create_for_toy,
    _cairo_dwrite_font_face_destroy,
    _cairo_dwrite_font_face_scaled_font_create
};

/* Functions #cairo_scaled_font_backend_t */

static void _cairo_dwrite_scaled_font_fini(void *scaled_font);

static cairo_warn cairo_int_status_t
_cairo_dwrite_scaled_glyph_init(void			     *scaled_font,
				cairo_scaled_glyph_t	     *scaled_glyph,
				cairo_scaled_glyph_info_t     info,
				const cairo_color_t          *foreground_color);

static cairo_int_status_t
_cairo_dwrite_load_truetype_table(void		       *scaled_font,
				  unsigned long         tag,
				  long                  offset,
				  unsigned char        *buffer,
				  unsigned long        *length);

static unsigned long
_cairo_dwrite_ucs4_to_index(void		       *scaled_font,
			    uint32_t                    ucs4);

static cairo_int_status_t
_cairo_dwrite_is_synthetic(void                       *scaled_font,
			   cairo_bool_t               *is_synthetic);

static cairo_bool_t
_cairo_dwrite_has_color_glyphs(void *scaled_font);

static const cairo_scaled_font_backend_t _cairo_dwrite_scaled_font_backend = {
    CAIRO_FONT_TYPE_DWRITE,
    _cairo_dwrite_scaled_font_fini,
    _cairo_dwrite_scaled_glyph_init,
    NULL, /* text_to_glyphs */
    _cairo_dwrite_ucs4_to_index,
    _cairo_dwrite_load_truetype_table,
    NULL, /* index_to_ucs4 */
    _cairo_dwrite_is_synthetic,
    NULL, /* index_to_glyph_name */
    NULL, /* load_type1_data */
    _cairo_dwrite_has_color_glyphs
};

/* Helper conversion functions */


/**
 * _cairo_dwrite_matrix_from_matrix:
 * Get a DirectWrite matrix from a cairo matrix. Note that DirectWrite uses row
 * vectors where cairo uses column vectors. Hence the transposition.
 *
 * \param Cairo matrix
 * \return DirectWrite matrix
 **/
static DWRITE_MATRIX
_cairo_dwrite_matrix_from_matrix(const cairo_matrix_t *matrix)
{
    DWRITE_MATRIX dwmat;
    dwmat.m11 = (FLOAT)matrix->xx;
    dwmat.m12 = (FLOAT)matrix->yx;
    dwmat.m21 = (FLOAT)matrix->xy;
    dwmat.m22 = (FLOAT)matrix->yy;
    dwmat.dx = (FLOAT)matrix->x0;
    dwmat.dy = (FLOAT)matrix->y0;
    return dwmat;
}

/* Helper functions for cairo_dwrite_scaled_glyph_init() */
static cairo_int_status_t
_cairo_dwrite_scaled_font_init_glyph_metrics
    (cairo_dwrite_scaled_font_t *scaled_font, cairo_scaled_glyph_t *scaled_glyph);

static cairo_int_status_t
_cairo_dwrite_scaled_font_init_glyph_color_surface(cairo_dwrite_scaled_font_t *scaled_font,
						   cairo_scaled_glyph_t	      *scaled_glyph,
						   const cairo_color_t        *foreground_color);

static cairo_int_status_t
_cairo_dwrite_scaled_font_init_glyph_surface
    (cairo_dwrite_scaled_font_t *scaled_font, cairo_scaled_glyph_t *scaled_glyph);

static cairo_int_status_t
_cairo_dwrite_scaled_font_init_glyph_path
    (cairo_dwrite_scaled_font_t *scaled_font, cairo_scaled_glyph_t *scaled_glyph);

/* implement the font backend interface */

static cairo_status_t
_cairo_dwrite_font_face_create_for_toy (cairo_toy_font_face_t   *toy_face,
					cairo_font_face_t      **font_face)
{
    WCHAR *face_name;
    int face_name_len;

    if (!DWriteFactory::Instance()) {
	return (cairo_status_t)CAIRO_INT_STATUS_UNSUPPORTED;
    }

    face_name_len = MultiByteToWideChar(CP_UTF8, 0, toy_face->family, -1, NULL, 0);
    face_name = new WCHAR[face_name_len];
    MultiByteToWideChar(CP_UTF8, 0, toy_face->family, -1, face_name, face_name_len);

    RefPtr<IDWriteFontFamily> family = DWriteFactory::FindSystemFontFamily(face_name);
    delete face_name;
    if (!family) {
	/* If the family is not found, use the default that should always exist. */
	face_name_len = MultiByteToWideChar(CP_UTF8, 0, CAIRO_FONT_FAMILY_DEFAULT, -1, NULL, 0);
	face_name = new WCHAR[face_name_len];
	MultiByteToWideChar(CP_UTF8, 0, CAIRO_FONT_FAMILY_DEFAULT, -1, face_name, face_name_len);

	family = DWriteFactory::FindSystemFontFamily(face_name);
	delete face_name;
	if (!family) {
	    *font_face = (cairo_font_face_t*)&_cairo_font_face_nil;
	    return (cairo_status_t)CAIRO_INT_STATUS_UNSUPPORTED;
	}
    }

    DWRITE_FONT_WEIGHT weight;
    switch (toy_face->weight) {
    case CAIRO_FONT_WEIGHT_BOLD:
	weight = DWRITE_FONT_WEIGHT_BOLD;
	break;
    case CAIRO_FONT_WEIGHT_NORMAL:
    default:
	weight = DWRITE_FONT_WEIGHT_NORMAL;
	break;
    }

    DWRITE_FONT_STYLE style;
    switch (toy_face->slant) {
    case CAIRO_FONT_SLANT_ITALIC:
	style = DWRITE_FONT_STYLE_ITALIC;
	break;
    case CAIRO_FONT_SLANT_OBLIQUE:
	style = DWRITE_FONT_STYLE_OBLIQUE;
	break;
    case CAIRO_FONT_SLANT_NORMAL:
    default:
	style = DWRITE_FONT_STYLE_NORMAL;
	break;
    }

    RefPtr<IDWriteFont> font;
    HRESULT hr = family->GetFirstMatchingFont(weight, DWRITE_FONT_STRETCH_NORMAL, style, &font);
    if (FAILED(hr))
	return (cairo_status_t)_cairo_dwrite_error (hr, "GetFirstMatchingFont failed");

    RefPtr<IDWriteFontFace> dwriteface;
    hr = font->CreateFontFace(&dwriteface);
    if (FAILED(hr))
	return (cairo_status_t)_cairo_dwrite_error (hr, "CreateFontFace failed");

    *font_face = cairo_dwrite_font_face_create_for_dwrite_fontface(dwriteface);
    return CAIRO_STATUS_SUCCESS;
}

static cairo_bool_t
_cairo_dwrite_font_face_destroy (void *font_face)
{
    cairo_dwrite_font_face_t *dwrite_font_face = static_cast<cairo_dwrite_font_face_t*>(font_face);
    if (dwrite_font_face->dwriteface)
	dwrite_font_face->dwriteface->Release();
    return TRUE;
}

static inline unsigned short
read_short(const char *buf)
{
    return be16_to_cpu(*(unsigned short*)buf);
}

void
_cairo_dwrite_glyph_run_from_glyphs(cairo_glyph_t *glyphs,
				    int num_glyphs,
				    cairo_dwrite_scaled_font_t *scaled_font,
				    AutoDWriteGlyphRun *run,
				    cairo_bool_t *transformed)
{
    run->allocate(num_glyphs);

    UINT16 *indices = const_cast<UINT16*>(run->glyphIndices);
    FLOAT *advances = const_cast<FLOAT*>(run->glyphAdvances);
    DWRITE_GLYPH_OFFSET *offsets = const_cast<DWRITE_GLYPH_OFFSET*>(run->glyphOffsets);

    cairo_dwrite_font_face_t *dwriteff = reinterpret_cast<cairo_dwrite_font_face_t*>(scaled_font->base.font_face);

    run->bidiLevel = 0;
    run->fontFace = dwriteff->dwriteface;
    run->glyphCount = num_glyphs;
    run->isSideways = FALSE;

    if (scaled_font->mat.xy == 0 && scaled_font->mat.yx == 0 &&
	scaled_font->mat.xx == scaled_font->base.font_matrix.xx &&
	scaled_font->mat.yy == scaled_font->base.font_matrix.yy) {
	// Fast route, don't actually use a transform but just
	// set the correct font size.
	*transformed = 0;

	run->fontEmSize = (FLOAT)scaled_font->base.font_matrix.yy;

	for (int i = 0; i < num_glyphs; i++) {
	    indices[i] = (WORD) glyphs[i].index;

	    offsets[i].ascenderOffset = -(FLOAT)(glyphs[i].y);
	    offsets[i].advanceOffset = (FLOAT)(glyphs[i].x);
	    advances[i] = 0.0;
	}
    } else {
	*transformed = 1;
        // Transforming positions by the inverse matrix, then by the original
        // matrix later may introduce small errors, especially because the
        // D2D matrix is single-precision whereas the cairo one is double.
        // This is a problem when glyph positions were originally at exactly
        // half-pixel locations, which eventually round to whole pixels for
        // GDI rendering - the errors introduced here cause them to round in
        // unpredictable directions, instead of all rounding in a consistent
        // way, leading to poor glyph spacing (bug 675383).
        // To mitigate this, nudge the positions by a tiny amount to try and
        // ensure that after the two transforms, they'll still round in a
        // consistent direction.
        const double EPSILON = 0.0001;
	for (int i = 0; i < num_glyphs; i++) {
	    indices[i] = (WORD) glyphs[i].index;
	    double x = glyphs[i].x + EPSILON;
	    double y = glyphs[i].y;
	    cairo_matrix_transform_point(&scaled_font->mat_inverse, &x, &y);
	    // Since we will multiply by our ctm matrix later for rotation effects
	    // and such, adjust positions by the inverse matrix now. Y-axis is
	    // inverted! Therefor the offset is -y.
	    offsets[i].ascenderOffset = -(FLOAT)y;
	    offsets[i].advanceOffset = (FLOAT)x;
	    advances[i] = 0.0;
	}
	// The font matrix takes care of the scaling if we have a transform,
	// emSize should be 1.
	run->fontEmSize = 1.0f;
    }
}

#define GASP_TAG 0x70736167
#define GASP_DOGRAY 0x2

static cairo_bool_t
do_grayscale(IDWriteFontFace *dwface, unsigned int ppem)
{
    void *tableContext;
    char *tableData;
    UINT32 tableSize;
    BOOL exists;
    dwface->TryGetFontTable(GASP_TAG, (const void**)&tableData, &tableSize, &tableContext, &exists);

    if (exists) {
	if (tableSize < 4) {
	    dwface->ReleaseFontTable(tableContext);
	    return true;
	}
	struct gaspRange {
	    unsigned short maxPPEM; // Stored big-endian
	    unsigned short behavior; // Stored big-endian
	};
	unsigned short numRanges = read_short(tableData + 2);
	if (tableSize < (UINT)4 + numRanges * 4) {
	    dwface->ReleaseFontTable(tableContext);
	    return true;
	}
	gaspRange *ranges = (gaspRange *)(tableData + 4);
	for (int i = 0; i < numRanges; i++) {
	    if (be16_to_cpu(ranges[i].maxPPEM) > ppem) {
		if (!(be16_to_cpu(ranges[i].behavior) & GASP_DOGRAY)) {
		    dwface->ReleaseFontTable(tableContext);
		    return false;
		}
		break;
	    }
	}
	dwface->ReleaseFontTable(tableContext);
    }
    return true;
}

static cairo_status_t
_cairo_dwrite_font_face_scaled_font_create (void			*abstract_face,
					    const cairo_matrix_t	*font_matrix,
					    const cairo_matrix_t	*ctm,
					    const cairo_font_options_t  *options,
					    cairo_scaled_font_t **font)
{
    cairo_status_t status;
    cairo_dwrite_font_face_t *font_face = static_cast<cairo_dwrite_font_face_t*>(abstract_face);

    /* Must do malloc and not C++ new, since Cairo frees this. */
    cairo_dwrite_scaled_font_t *dwrite_font = (cairo_dwrite_scaled_font_t*)_cairo_malloc(
	sizeof(cairo_dwrite_scaled_font_t));
    if (unlikely(dwrite_font == NULL))
	return _cairo_error (CAIRO_STATUS_NO_MEMORY);

    *font = reinterpret_cast<cairo_scaled_font_t*>(dwrite_font);
    status = _cairo_scaled_font_init (&dwrite_font->base,
				      &font_face->base,
				      font_matrix,
				      ctm,
				      options,
				      &_cairo_dwrite_scaled_font_backend);
    if (status) {
	free(dwrite_font);
	return status;
    }

    cairo_font_extents_t extents;

    DWRITE_FONT_METRICS metrics;
    font_face->dwriteface->GetMetrics(&metrics);

    extents.ascent = (FLOAT)metrics.ascent / metrics.designUnitsPerEm;
    extents.descent = (FLOAT)metrics.descent / metrics.designUnitsPerEm;
    extents.height = (FLOAT)(metrics.ascent + metrics.descent + metrics.lineGap) / metrics.designUnitsPerEm;
    extents.max_x_advance = 14.0;
    extents.max_y_advance = 0.0;

    dwrite_font->mat = dwrite_font->base.ctm;
    cairo_matrix_multiply(&dwrite_font->mat, &dwrite_font->mat, font_matrix);
    dwrite_font->mat_inverse = dwrite_font->mat;
    cairo_matrix_invert (&dwrite_font->mat_inverse);

    cairo_antialias_t default_quality = CAIRO_ANTIALIAS_SUBPIXEL;

    dwrite_font->measuring_mode = DWRITE_MEASURING_MODE_NATURAL;

    // The following code detects the system quality at scaled_font creation time,
    // this means that if cleartype settings are changed but the scaled_fonts
    // are re-used, they might not adhere to the new system setting until re-
    // creation.
    switch (cairo_win32_get_system_text_quality()) {
	case CLEARTYPE_QUALITY:
	    default_quality = CAIRO_ANTIALIAS_SUBPIXEL;
	    break;
	case ANTIALIASED_QUALITY:
	    default_quality = CAIRO_ANTIALIAS_GRAY;
	    dwrite_font->measuring_mode = DWRITE_MEASURING_MODE_GDI_CLASSIC;
	    break;
	case DEFAULT_QUALITY:
	    // _get_system_quality() seems to think aliased is default!
	    default_quality = CAIRO_ANTIALIAS_NONE;
	    dwrite_font->measuring_mode = DWRITE_MEASURING_MODE_GDI_CLASSIC;
	    break;
    }

    if (default_quality == CAIRO_ANTIALIAS_GRAY) {
	if (!do_grayscale(font_face->dwriteface, (unsigned int)_cairo_round(font_matrix->yy))) {
	    default_quality = CAIRO_ANTIALIAS_NONE;
	}
    }

    if (options->antialias == CAIRO_ANTIALIAS_DEFAULT) {
	dwrite_font->antialias_mode = default_quality;
    } else {
	dwrite_font->antialias_mode = options->antialias;
    }

    dwrite_font->rendering_mode =
        default_quality == CAIRO_ANTIALIAS_SUBPIXEL ?
            cairo_dwrite_scaled_font_t::TEXT_RENDERING_NORMAL : cairo_dwrite_scaled_font_t::TEXT_RENDERING_NO_CLEARTYPE;

    return _cairo_scaled_font_set_metrics (*font, &extents);
}

/* Implementation #cairo_dwrite_scaled_font_backend_t */
static void
_cairo_dwrite_scaled_font_fini(void *scaled_font)
{
}

static cairo_int_status_t
_cairo_dwrite_scaled_glyph_init(void			     *scaled_font,
				cairo_scaled_glyph_t	     *scaled_glyph,
				cairo_scaled_glyph_info_t     info,
				const cairo_color_t          *foreground_color)
{
    cairo_dwrite_scaled_font_t *scaled_dwrite_font = static_cast<cairo_dwrite_scaled_font_t*>(scaled_font);
    cairo_int_status_t status = CAIRO_INT_STATUS_UNSUPPORTED;

    if ((info & CAIRO_SCALED_GLYPH_INFO_METRICS) != 0) {
	status = _cairo_dwrite_scaled_font_init_glyph_metrics (scaled_dwrite_font, scaled_glyph);
	if (status)
	    return status;
    }

    if (info & CAIRO_SCALED_GLYPH_INFO_COLOR_SURFACE) {
	status = _cairo_dwrite_scaled_font_init_glyph_color_surface (scaled_dwrite_font, scaled_glyph, foreground_color);
	if (status)
	    return status;
    }

    if (info & CAIRO_SCALED_GLYPH_INFO_SURFACE) {
	status = _cairo_dwrite_scaled_font_init_glyph_surface (scaled_dwrite_font, scaled_glyph);
	if (status)
	    return status;
    }

    if ((info & CAIRO_SCALED_GLYPH_INFO_PATH) != 0) {
	status = _cairo_dwrite_scaled_font_init_glyph_path (scaled_dwrite_font, scaled_glyph);
	if (status)
	    return status;
    }

    return CAIRO_INT_STATUS_SUCCESS;
}

static unsigned long
_cairo_dwrite_ucs4_to_index(void			     *scaled_font,
			    uint32_t		      ucs4)
{
    cairo_dwrite_scaled_font_t *dwritesf = static_cast<cairo_dwrite_scaled_font_t*>(scaled_font);
    cairo_dwrite_font_face_t *face = reinterpret_cast<cairo_dwrite_font_face_t*>(dwritesf->base.font_face);

    UINT16 index;
    face->dwriteface->GetGlyphIndicesA(&ucs4, 1, &index);
    return index;
}

/* cairo_dwrite_scaled_glyph_init() helper function bodies */
static cairo_int_status_t
_cairo_dwrite_scaled_font_init_glyph_metrics(cairo_dwrite_scaled_font_t *scaled_font,
					     cairo_scaled_glyph_t *scaled_glyph)
{
    UINT16 charIndex = (UINT16)_cairo_scaled_glyph_index (scaled_glyph);
    cairo_dwrite_font_face_t *font_face = (cairo_dwrite_font_face_t*)scaled_font->base.font_face;
    cairo_text_extents_t extents;

    DWRITE_GLYPH_METRICS metrics;
    DWRITE_FONT_METRICS fontMetrics;
    font_face->dwriteface->GetMetrics(&fontMetrics);
    HRESULT hr = font_face->dwriteface->GetDesignGlyphMetrics(&charIndex, 1, &metrics);
    if (FAILED(hr)) {
	return CAIRO_INT_STATUS_UNSUPPORTED;
    }

    // TODO: Treat swap_xy.
    extents.width = (FLOAT)(metrics.advanceWidth - metrics.leftSideBearing - metrics.rightSideBearing) /
	fontMetrics.designUnitsPerEm;
    extents.height = (FLOAT)(metrics.advanceHeight - metrics.topSideBearing - metrics.bottomSideBearing) /
	fontMetrics.designUnitsPerEm;
    extents.x_advance = (FLOAT)metrics.advanceWidth / fontMetrics.designUnitsPerEm;
    extents.x_bearing = (FLOAT)metrics.leftSideBearing / fontMetrics.designUnitsPerEm;
    extents.y_advance = 0.0;
    extents.y_bearing = (FLOAT)(metrics.topSideBearing - metrics.verticalOriginY) /
	fontMetrics.designUnitsPerEm;

    // We pad the extents here because GetDesignGlyphMetrics returns "ideal" metrics
    // for the glyph outline, without accounting for hinting/gridfitting/antialiasing,
    // and therefore it does not always cover all pixels that will actually be touched.
    if (scaled_font->base.options.antialias != CAIRO_ANTIALIAS_NONE &&
	extents.width > 0 && extents.height > 0) {
	extents.width += scaled_font->mat_inverse.xx * 2;
	extents.x_bearing -= scaled_font->mat_inverse.xx;
    }

    _cairo_scaled_glyph_set_metrics (scaled_glyph,
				     &scaled_font->base,
				     &extents);
    return CAIRO_INT_STATUS_SUCCESS;
}

/*
 * Stack-based helper implementing IDWriteGeometrySink.
 * Used to determine the path of the glyphs.
 */

class GeometryRecorder : public IDWriteGeometrySink
{
public:
    GeometryRecorder(cairo_path_fixed_t *aCairoPath)
	: mCairoPath(aCairoPath) {}

    // IUnknown interface
    IFACEMETHOD(QueryInterface)(IID const& iid, OUT void** ppObject)
    {
	if (iid != __uuidof(IDWriteGeometrySink))
	    return E_NOINTERFACE;

	*ppObject = static_cast<IDWriteGeometrySink*>(this);

	return S_OK;
    }

    IFACEMETHOD_(ULONG, AddRef)()
    {
	return 1;
    }

    IFACEMETHOD_(ULONG, Release)()
    {
	return 1;
    }

    IFACEMETHODIMP_(void) SetFillMode(D2D1_FILL_MODE fillMode)
    {
	return;
    }

    STDMETHODIMP Close()
    {
	return S_OK;
    }

    IFACEMETHODIMP_(void) SetSegmentFlags(D2D1_PATH_SEGMENT vertexFlags)
    {
	return;
    }

    cairo_fixed_t GetFixedX(const D2D1_POINT_2F &point)
    {
	unsigned int control_word;
	_controlfp_s(&control_word, _CW_DEFAULT, MCW_PC);
	return _cairo_fixed_from_double(point.x);
    }

    cairo_fixed_t GetFixedY(const D2D1_POINT_2F &point)
    {
	unsigned int control_word;
	_controlfp_s(&control_word, _CW_DEFAULT, MCW_PC);
	return _cairo_fixed_from_double(point.y);
    }

    IFACEMETHODIMP_(void) BeginFigure(
	D2D1_POINT_2F startPoint,
	D2D1_FIGURE_BEGIN figureBegin)
    {
	mStartPoint = startPoint;
	cairo_status_t status = _cairo_path_fixed_move_to(mCairoPath,
							  GetFixedX(startPoint),
							  GetFixedY(startPoint));
	(void)status; /* squelch warning */
    }

    IFACEMETHODIMP_(void) EndFigure(
	D2D1_FIGURE_END figureEnd)
    {
	if (figureEnd == D2D1_FIGURE_END_CLOSED) {
	    cairo_status_t status = _cairo_path_fixed_line_to(mCairoPath,
							      GetFixedX(mStartPoint),
							      GetFixedY(mStartPoint));
	    (void)status; /* squelch warning */
	}
    }

    IFACEMETHODIMP_(void) AddBeziers(
	const D2D1_BEZIER_SEGMENT *beziers,
	UINT beziersCount)
    {
	for (unsigned int i = 0; i < beziersCount; i++) {
	    cairo_status_t status = _cairo_path_fixed_curve_to(mCairoPath,
							       GetFixedX(beziers[i].point1),
							       GetFixedY(beziers[i].point1),
							       GetFixedX(beziers[i].point2),
							       GetFixedY(beziers[i].point2),
							       GetFixedX(beziers[i].point3),
							       GetFixedY(beziers[i].point3));
	    (void)status; /* squelch warning */
	}
    }

    IFACEMETHODIMP_(void) AddLines(
	const D2D1_POINT_2F *points,
	UINT pointsCount)
    {
	for (unsigned int i = 0; i < pointsCount; i++) {
	    cairo_status_t status = _cairo_path_fixed_line_to(mCairoPath,
							      GetFixedX(points[i]),
							      GetFixedY(points[i]));
	    (void)status; /* squelch warning */
	}
    }

private:
    cairo_path_fixed_t *mCairoPath;
    D2D1_POINT_2F mStartPoint;
};

static cairo_int_status_t
_cairo_dwrite_scaled_font_init_glyph_path(cairo_dwrite_scaled_font_t *scaled_font,
					  cairo_scaled_glyph_t *scaled_glyph)
{
    cairo_int_status_t status;
    cairo_path_fixed_t *path;
    path = _cairo_path_fixed_create();
    GeometryRecorder recorder(path);

    DWRITE_GLYPH_OFFSET offset;
    offset.advanceOffset = 0;
    offset.ascenderOffset = 0;
    UINT16 glyphId = (UINT16)_cairo_scaled_glyph_index(scaled_glyph);
    FLOAT advance = 0.0;
    cairo_dwrite_font_face_t *dwriteff = (cairo_dwrite_font_face_t*)scaled_font->base.font_face;

    /* GetGlyphRunOutline seems to ignore hinting so just use the em size to get the outline
     * to avoid rounding errors when converting to cairo_path_fixed_t.
     */
    DWRITE_FONT_METRICS metrics;
    dwriteff->dwriteface->GetMetrics(&metrics);
    HRESULT hr = dwriteff->dwriteface->GetGlyphRunOutline(metrics.designUnitsPerEm,
							  &glyphId,
							  &advance,
							  &offset,
							  1,
							  FALSE,
							  FALSE,
							  &recorder);
    if (!SUCCEEDED(hr))
	return _cairo_dwrite_error (hr, "GetGlyphRunOutline failed");

    status = (cairo_int_status_t)_cairo_path_fixed_close_path(path);

    /* Now scale the em size down to 1.0 and apply the font matrix and font ctm. */
    cairo_matrix_t mat = scaled_font->base.ctm;
    cairo_matrix_multiply(&mat, &scaled_font->base.font_matrix, &mat);
    cairo_matrix_scale (&mat, 1.0/metrics.designUnitsPerEm, 1.0/metrics.designUnitsPerEm);
    _cairo_path_fixed_transform(path, &mat);

    _cairo_scaled_glyph_set_path (scaled_glyph,
				  &scaled_font->base,
				  path);
    return status;
}

static cairo_int_status_t
_cairo_dwrite_scaled_font_init_glyph_color_surface(cairo_dwrite_scaled_font_t *scaled_font,
						   cairo_scaled_glyph_t	      *scaled_glyph,
						   const cairo_color_t        *foreground_color)
{
    int width, height;
    double x1, y1, x2, y2;
    cairo_glyph_t glyph;
    cairo_bool_t uses_foreground_color = FALSE;

    cairo_dwrite_font_face_t *dwrite_font_face = (cairo_dwrite_font_face_t *)scaled_font->base.font_face;
    if (!dwrite_font_face->have_color) {
	scaled_glyph->color_glyph = FALSE;
	scaled_glyph->color_glyph_set = TRUE;
	return CAIRO_INT_STATUS_UNSUPPORTED;
    }

    x1 = _cairo_fixed_integer_floor (scaled_glyph->bbox.p1.x);
    y1 = _cairo_fixed_integer_floor (scaled_glyph->bbox.p1.y);
    x2 = _cairo_fixed_integer_ceil (scaled_glyph->bbox.p2.x);
    y2 = _cairo_fixed_integer_ceil (scaled_glyph->bbox.p2.y);
    width = (int)(x2 - x1);
    height = (int)(y2 - y1);

    glyph.index = _cairo_scaled_glyph_index (scaled_glyph);
    glyph.x = x1;
    glyph.y = y1;

    DWRITE_GLYPH_RUN run;
    FLOAT advance = 0;
    UINT16 index = (UINT16)glyph.index;
    DWRITE_GLYPH_OFFSET offset;
    double x = -glyph.x;
    double y = -glyph.y;
    DWRITE_MATRIX matrix;
    D2D1_POINT_2F origin = {0, 0};
    RefPtr<IDWriteColorGlyphRunEnumerator1> run_enumerator;
    HRESULT hr;

    /**
     * We transform by the inverse transformation here. This will put our glyph
     * locations in the space in which we draw. Which is later transformed by
     * the transformation matrix that we use. This will transform the
     * glyph positions back to where they were before when drawing, but the
     * glyph shapes will be transformed by the transformation matrix.
     */
    cairo_matrix_transform_point(&scaled_font->mat_inverse, &x, &y);
    offset.advanceOffset = (FLOAT)x;
    /** Y-axis is inverted */
    offset.ascenderOffset = -(FLOAT)y;

    run.fontFace = dwrite_font_face->dwriteface;
    run.fontEmSize = 1;
    run.glyphCount = 1;
    run.glyphIndices = &index;
    run.glyphAdvances = &advance;
    run.glyphOffsets = &offset;
    run.isSideways = FALSE;
    run.bidiLevel = 0;

    matrix = _cairo_dwrite_matrix_from_matrix(&scaled_font->mat);

    /* The list of glyph image formats this renderer is prepared to support. */
    DWRITE_GLYPH_IMAGE_FORMATS supported_formats =
        DWRITE_GLYPH_IMAGE_FORMATS_COLR |
        DWRITE_GLYPH_IMAGE_FORMATS_SVG |
        DWRITE_GLYPH_IMAGE_FORMATS_PNG |
        DWRITE_GLYPH_IMAGE_FORMATS_JPEG |
        DWRITE_GLYPH_IMAGE_FORMATS_TIFF |
        DWRITE_GLYPH_IMAGE_FORMATS_PREMULTIPLIED_B8G8R8A8;

    hr = DWriteFactory::Instance4()->TranslateColorGlyphRun(
	origin,
	&run,
	NULL, /* glyphRunDescription */
	supported_formats,
	DWRITE_MEASURING_MODE_NATURAL,
	&matrix,
	0,
	&run_enumerator);

    if (hr == DWRITE_E_NOCOLOR) {
	/* No color glyphs */
	scaled_glyph->color_glyph = FALSE;
	scaled_glyph->color_glyph_set = TRUE;
	return CAIRO_INT_STATUS_UNSUPPORTED;
    }

    if (FAILED(hr))
	return _cairo_dwrite_error (hr, "TranslateColorGlyphRun failed");

    /* We have a color glyph(s). Use Direct2D to render it to a bitmap */
    if (!WICImagingFactory::Instance() || !D2DFactory::Instance())
	return _cairo_dwrite_error (hr, "Instance failed");

    RefPtr<IWICBitmap> bitmap;
    hr = WICImagingFactory::Instance()->CreateBitmap ((UINT)width,
						      (UINT)height,
						      GUID_WICPixelFormat32bppPBGRA,
						      WICBitmapCacheOnLoad,
						      &bitmap);
    if (FAILED(hr))
	return _cairo_dwrite_error (hr, "CreateBitmap failed");

    D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
	D2D1_RENDER_TARGET_TYPE_DEFAULT,
	D2D1::PixelFormat(
	    DXGI_FORMAT_B8G8R8A8_UNORM,
	    D2D1_ALPHA_MODE_PREMULTIPLIED),
	0,
	0,
	D2D1_RENDER_TARGET_USAGE_NONE,
	D2D1_FEATURE_LEVEL_DEFAULT);

    RefPtr<ID2D1RenderTarget> rt;
    hr = D2DFactory::Instance()->CreateWicBitmapRenderTarget (bitmap, properties, &rt);
    if (FAILED(hr))
	return _cairo_dwrite_error (hr, "CreateWicBitmapRenderTarget failed");

    RefPtr<ID2D1DeviceContext4> dc4;
    hr = rt->QueryInterface(&dc4);
    if (FAILED(hr))
	return _cairo_dwrite_error (hr, "QueryInterface(&dc4) failed");

    RefPtr<ID2D1SolidColorBrush> foreground_color_brush;
    dc4->CreateSolidColorBrush(
	D2D1::ColorF(foreground_color->red,
		     foreground_color->green,
		     foreground_color->blue,
		     foreground_color->alpha), &foreground_color_brush);

    RefPtr<ID2D1SolidColorBrush> color_brush;
    dc4->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &color_brush);

    dc4->SetDpi(96, 96); /* 1 unit = 1 pixel */
    rt->SetTransform(D2D1::Matrix3x2F(matrix.m11,
				      matrix.m12,
				      matrix.m21,
				      matrix.m22,
				      matrix.dx,
				      matrix.dy));

    dc4->BeginDraw();
    dc4->Clear(NULL); /* Transparent black */

    while (true) {
	BOOL have_run;
	hr = run_enumerator->MoveNext(&have_run);
	if (FAILED(hr) || !have_run)
	    break;

	DWRITE_COLOR_GLYPH_RUN1 const* color_run;
	hr = run_enumerator->GetCurrentRun(&color_run);
	if (FAILED(hr))
	    return _cairo_dwrite_error (hr, "GetCurrentRun failed");

	switch (color_run->glyphImageFormat) {
	    case DWRITE_GLYPH_IMAGE_FORMATS_PNG:
	    case DWRITE_GLYPH_IMAGE_FORMATS_JPEG:
	    case DWRITE_GLYPH_IMAGE_FORMATS_TIFF:
	    case DWRITE_GLYPH_IMAGE_FORMATS_PREMULTIPLIED_B8G8R8A8:
		/* Bitmap glyphs */
		dc4->DrawColorBitmapGlyphRun(color_run->glyphImageFormat,
					     origin,
					     &color_run->glyphRun,
					     DWRITE_MEASURING_MODE_NATURAL,
					     D2D1_COLOR_BITMAP_GLYPH_SNAP_OPTION_DEFAULT);
		break;

	    case DWRITE_GLYPH_IMAGE_FORMATS_SVG:
		/* SVG glyphs */
		dc4->DrawSvgGlyphRun(origin,
				     &color_run->glyphRun,
				     foreground_color_brush,
				     nullptr,
				     0,
				     DWRITE_MEASURING_MODE_NATURAL);
		uses_foreground_color = TRUE;
		break;
	    case DWRITE_GLYPH_IMAGE_FORMATS_TRUETYPE:
	    case DWRITE_GLYPH_IMAGE_FORMATS_CFF:
	    case DWRITE_GLYPH_IMAGE_FORMATS_COLR:
		/* Outline glyphs */
		if (color_run->paletteIndex == 0xFFFF) {
		    D2D1_COLOR_F color = foreground_color_brush->GetColor();
		    color_brush->SetColor(&color);
		    uses_foreground_color = TRUE;
		} else {
		    color_brush->SetColor(color_run->runColor);
		}

		dc4->DrawGlyphRun(origin,
				  &color_run->glyphRun,
				  color_run->glyphRunDescription,
				  color_brush,
				  DWRITE_MEASURING_MODE_NATURAL);
	    case DWRITE_GLYPH_IMAGE_FORMATS_NONE:
		break;
	}
    }

    hr = dc4->EndDraw();
    if (FAILED(hr))
	return _cairo_dwrite_error (hr, "EndDraw failed");

    cairo_surface_t *image = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, width, height);
    int stride = cairo_image_surface_get_stride (image);
    WICRect rect = { 0, 0, width, height };
    bitmap->CopyPixels(&rect,
		       stride,
		       height * stride,
		       cairo_image_surface_get_data (image));
    cairo_surface_mark_dirty (image);
    cairo_surface_set_device_offset (image, -x1, -y1);
    _cairo_scaled_glyph_set_color_surface (scaled_glyph,
					   &scaled_font->base,
					   (cairo_image_surface_t *) image,
					   uses_foreground_color);
    scaled_glyph->color_glyph = TRUE;
    scaled_glyph->color_glyph_set = TRUE;

    return CAIRO_INT_STATUS_SUCCESS;
}

/* Helper function adapted from _compute_mask in cairo-win32-font.c */

/* Compute an alpha-mask from a monochrome RGB24 image
 */
static cairo_surface_t *
_compute_a8_mask (cairo_surface_t *surface)
{
    cairo_image_surface_t *glyph;
    cairo_image_surface_t *mask;
    int i, j;

    glyph = (cairo_image_surface_t *)cairo_surface_map_to_image (surface, NULL);
    if (unlikely (glyph->base.status))
        return &glyph->base;

    /* No quality param, just use the non-ClearType path */

    /* Compute an alpha-mask by using the green channel of a (presumed monochrome)
     * RGB24 image.
     */
    mask = (cairo_image_surface_t *)
        cairo_image_surface_create (CAIRO_FORMAT_A8, glyph->width, glyph->height);
    if (likely (mask->base.status == CAIRO_STATUS_SUCCESS)) {
        for (i = 0; i < glyph->height; i++) {
            uint32_t *p = (uint32_t *) (glyph->data + i * glyph->stride);
            uint8_t *q = (uint8_t *) (mask->data + i * mask->stride);

            for (j = 0; j < glyph->width; j++)
                *q++ = 255 - ((*p++ & 0x0000ff00) >> 8);
        }
    }

    cairo_surface_unmap_image (surface, &glyph->base);
    return &mask->base;
}

static cairo_int_status_t
_cairo_dwrite_scaled_font_init_glyph_surface(cairo_dwrite_scaled_font_t *scaled_font,
					     cairo_scaled_glyph_t	*scaled_glyph)
{
    cairo_int_status_t status;
    cairo_glyph_t glyph;
    cairo_win32_surface_t *surface;
    cairo_t *cr;
    cairo_surface_t *image;
    int width, height;
    double x1, y1, x2, y2;

    x1 = _cairo_fixed_integer_floor (scaled_glyph->bbox.p1.x);
    y1 = _cairo_fixed_integer_floor (scaled_glyph->bbox.p1.y);
    x2 = _cairo_fixed_integer_ceil (scaled_glyph->bbox.p2.x);
    y2 = _cairo_fixed_integer_ceil (scaled_glyph->bbox.p2.y);
    width = (int)(x2 - x1);
    height = (int)(y2 - y1);

    glyph.index = _cairo_scaled_glyph_index (scaled_glyph);
    glyph.x = -x1;
    glyph.y = -y1;

    DWRITE_GLYPH_RUN run;
    FLOAT advance = 0;
    UINT16 index = (UINT16)glyph.index;
    DWRITE_GLYPH_OFFSET offset;
    double x = glyph.x;
    double y = glyph.y;
    RECT area;
    DWRITE_MATRIX matrix;

    surface = (cairo_win32_surface_t *)
	cairo_win32_surface_create_with_dib (CAIRO_FORMAT_RGB24, width, height);

    cr = cairo_create (&surface->base);
    cairo_set_source_rgb (cr, 1, 1, 1);
    cairo_paint (cr);
    status = (cairo_int_status_t)cairo_status (cr);
    cairo_destroy(cr);
    if (status)
	goto FAIL;

    /**
     * We transform by the inverse transformation here. This will put our glyph
     * locations in the space in which we draw. Which is later transformed by
     * the transformation matrix that we use. This will transform the
     * glyph positions back to where they were before when drawing, but the
     * glyph shapes will be transformed by the transformation matrix.
     */
    cairo_matrix_transform_point(&scaled_font->mat_inverse, &x, &y);
    offset.advanceOffset = (FLOAT)x;
    /** Y-axis is inverted */
    offset.ascenderOffset = -(FLOAT)y;

    area.top = 0;
    area.bottom = height;
    area.left = 0;
    area.right = width;

    run.glyphCount = 1;
    run.glyphAdvances = &advance;
    run.fontFace = ((cairo_dwrite_font_face_t*)scaled_font->base.font_face)->dwriteface;
    run.fontEmSize = 1.0f;
    run.bidiLevel = 0;
    run.glyphIndices = &index;
    run.isSideways = FALSE;
    run.glyphOffsets = &offset;

    matrix = _cairo_dwrite_matrix_from_matrix(&scaled_font->mat);

    status = _dwrite_draw_glyphs_to_gdi_surface_gdi (surface, &matrix, &run,
            RGB(0,0,0), scaled_font, area);
    if (status)
	goto FAIL;

    GdiFlush();

    image = _compute_a8_mask (&surface->base);
    status = (cairo_int_status_t)image->status;
    if (status)
	goto FAIL;

    cairo_surface_set_device_offset (image, -x1, -y1);
    _cairo_scaled_glyph_set_surface (scaled_glyph,
				     &scaled_font->base,
				     (cairo_image_surface_t *) image);

  FAIL:
    cairo_surface_destroy (&surface->base);

    return status;
}

static cairo_int_status_t
_cairo_dwrite_load_truetype_table(void                 *scaled_font,
				  unsigned long         tag,
				  long                  offset,
				  unsigned char        *buffer,
				  unsigned long        *length)
{
    cairo_dwrite_scaled_font_t *dwritesf = static_cast<cairo_dwrite_scaled_font_t*>(scaled_font);
    cairo_dwrite_font_face_t *face = reinterpret_cast<cairo_dwrite_font_face_t*>(dwritesf->base.font_face);

    const void *data;
    UINT32 size;
    void *tableContext;
    BOOL exists;
    HRESULT hr;
    hr = face->dwriteface->TryGetFontTable (be32_to_cpu (tag),
					    &data,
					    &size,
					    &tableContext,
					    &exists);
    if (FAILED(hr))
	return _cairo_dwrite_error (hr, "TryGetFontTable failed");

    if (!exists) {
	return CAIRO_INT_STATUS_UNSUPPORTED;
    }

    if (buffer && *length && (UINT32)offset < size) {
        size = MIN(size - (UINT32)offset, *length);
        memcpy(buffer, (const char*)data + offset, size);
    }
    *length = size;

    if (tableContext) {
	face->dwriteface->ReleaseFontTable(tableContext);
    }
    return (cairo_int_status_t)CAIRO_STATUS_SUCCESS;
}

static cairo_int_status_t
_cairo_dwrite_is_synthetic(void                      *scaled_font,
			   cairo_bool_t              *is_synthetic)
{
    cairo_dwrite_scaled_font_t *dwritesf = static_cast<cairo_dwrite_scaled_font_t*>(scaled_font);
    cairo_dwrite_font_face_t *face = reinterpret_cast<cairo_dwrite_font_face_t*>(dwritesf->base.font_face);
    HRESULT hr;
    cairo_int_status_t status;

    if (face->dwriteface->GetSimulations() != DWRITE_FONT_SIMULATIONS_NONE) {
	*is_synthetic = FALSE;
	return CAIRO_INT_STATUS_SUCCESS;
    }

    RefPtr<IDWriteFontFace5> fontFace5;
    if (FAILED(face->dwriteface->QueryInterface(&fontFace5))) {
	/* If IDWriteFontFace5 is not available, assume this version of
	 * DirectWrite does not support variations.
	 */
	*is_synthetic = FALSE;
	return CAIRO_INT_STATUS_SUCCESS;
    }

    if (!fontFace5->HasVariations()) {
	*is_synthetic = FALSE;
	return CAIRO_INT_STATUS_SUCCESS;
    }

    RefPtr<IDWriteFontResource> fontResource;
    hr = fontFace5->GetFontResource(&fontResource);
    if (FAILED(hr))
	return _cairo_dwrite_error (hr, "GetFontResource failed");

    UINT32 axis_count = fontResource->GetFontAxisCount();
    DWRITE_FONT_AXIS_VALUE *axis_defaults = new DWRITE_FONT_AXIS_VALUE[axis_count];
    DWRITE_FONT_AXIS_VALUE *axis_values = new DWRITE_FONT_AXIS_VALUE[axis_count];

    hr = fontResource->GetDefaultFontAxisValues(axis_defaults, axis_count);
    if (FAILED(hr)) {
	status = _cairo_dwrite_error (hr, "GetDefaultFontAxisValues failed");
	goto cleanup;
    }

    hr = fontFace5->GetFontAxisValues(axis_values, axis_count);
    if (FAILED(hr)) {
	status = _cairo_dwrite_error (hr, "GetFontAxisValues failed");
	goto cleanup;
    }

    /* The DirectWrite documentation does not state if the tags of the returned
     * defaults and values arrays are in the same order. So assume they are not.
     */
    *is_synthetic = FALSE;
    status = CAIRO_INT_STATUS_SUCCESS;
    for (UINT32 i = 0; i< axis_count; i++) {
	for (UINT32 j = 0; j < axis_count; j++) {
	    if (axis_values[i].axisTag == axis_defaults[j].axisTag) {
		if (axis_values[i].value != axis_defaults[j].value) {
		    *is_synthetic = TRUE;
		    goto cleanup;
		}
		break;
	    }
	}
    }

  cleanup:
    delete[] axis_defaults;
    delete[] axis_values;
    
    return status;
}

static cairo_bool_t
_cairo_dwrite_has_color_glyphs(void *scaled_font)
{
    cairo_dwrite_scaled_font_t *dwritesf = static_cast<cairo_dwrite_scaled_font_t*>(scaled_font);

    return ((cairo_dwrite_font_face_t *)dwritesf->base.font_face)->have_color;
}

cairo_font_face_t*
cairo_dwrite_font_face_create_for_dwrite_fontface_internal(void* dwrite_font_face)
{
    IDWriteFontFace *dwriteface = static_cast<IDWriteFontFace*>(dwrite_font_face);
    // Must do malloc and not C++ new, since Cairo frees this.
    cairo_dwrite_font_face_t *face = (cairo_dwrite_font_face_t *)_cairo_malloc(sizeof(cairo_dwrite_font_face_t));
    if (unlikely (face == NULL)) {
	_cairo_error_throw (CAIRO_STATUS_NO_MEMORY);
	return (cairo_font_face_t*)&_cairo_font_face_nil;
    }

    dwriteface->AddRef();
    face->dwriteface = dwriteface;
    face->have_color = false;

    /* Ensure IDWriteFactory4 is available before enabling color fonts */
    if (DWriteFactory::Instance4()) {
	RefPtr<IDWriteFontFace2> fontFace2;
	if (SUCCEEDED(dwriteface->QueryInterface(&fontFace2))) {
	    if (fontFace2->IsColorFont())
		face->have_color = true;
	}
    }

    cairo_font_face_t *font_face;
    font_face = (cairo_font_face_t*)face;
    _cairo_font_face_init (&((cairo_dwrite_font_face_t*)font_face)->base, &_cairo_dwrite_font_face_backend);

    return font_face;
}

void
cairo_dwrite_scaled_font_set_force_GDI_classic(cairo_scaled_font_t *dwrite_scaled_font, cairo_bool_t force)
{
    cairo_dwrite_scaled_font_t *font = reinterpret_cast<cairo_dwrite_scaled_font_t*>(dwrite_scaled_font);
    if (force && font->rendering_mode == cairo_dwrite_scaled_font_t::TEXT_RENDERING_NORMAL) {
        font->rendering_mode = cairo_dwrite_scaled_font_t::TEXT_RENDERING_GDI_CLASSIC;
    } else if (!force && font->rendering_mode == cairo_dwrite_scaled_font_t::TEXT_RENDERING_GDI_CLASSIC) {
        font->rendering_mode = cairo_dwrite_scaled_font_t::TEXT_RENDERING_NORMAL;
    }
}

cairo_bool_t
cairo_dwrite_scaled_font_get_force_GDI_classic(cairo_scaled_font_t *dwrite_scaled_font)
{
    cairo_dwrite_scaled_font_t *font = reinterpret_cast<cairo_dwrite_scaled_font_t*>(dwrite_scaled_font);
    return font->rendering_mode == cairo_dwrite_scaled_font_t::TEXT_RENDERING_GDI_CLASSIC;
}

void
cairo_dwrite_set_cleartype_params(FLOAT gamma, FLOAT contrast, FLOAT level,
				  int geometry, int mode)
{
    DWriteFactory::SetRenderingParams(gamma, contrast, level, geometry, mode);
}

int
cairo_dwrite_get_cleartype_rendering_mode()
{
    return DWriteFactory::GetClearTypeRenderingMode();
}

static cairo_int_status_t
_dwrite_draw_glyphs_to_gdi_surface_gdi(cairo_win32_surface_t *surface,
				       DWRITE_MATRIX *transform,
				       DWRITE_GLYPH_RUN *run,
				       COLORREF color,
				       cairo_dwrite_scaled_font_t *scaled_font,
				       const RECT &area)
{
    RefPtr<IDWriteGdiInterop> gdiInterop;
    DWriteFactory::Instance()->GetGdiInterop(&gdiInterop);
    RefPtr<IDWriteBitmapRenderTarget> rt;
    HRESULT hr;

    cairo_dwrite_scaled_font_t::TextRenderingState renderingState =
      scaled_font->rendering_mode;

    hr = gdiInterop->CreateBitmapRenderTarget(surface->dc,
					      area.right - area.left,
					      area.bottom - area.top,
					      &rt);

    if (FAILED(hr)) {
	if (hr == E_OUTOFMEMORY) {
	    return (cairo_int_status_t)CAIRO_STATUS_NO_MEMORY;
	} else {
	    return CAIRO_INT_STATUS_UNSUPPORTED;
	}
    }

    if ((renderingState == cairo_dwrite_scaled_font_t::TEXT_RENDERING_NORMAL ||
         renderingState == cairo_dwrite_scaled_font_t::TEXT_RENDERING_GDI_CLASSIC)
        /* && !surface->base.permit_subpixel_antialiasing */ ) {
      renderingState = cairo_dwrite_scaled_font_t::TEXT_RENDERING_NO_CLEARTYPE;
      RefPtr<IDWriteBitmapRenderTarget1> rt1;
      hr = rt->QueryInterface(&rt1);

      if (SUCCEEDED(hr) && rt1) {
        rt1->SetTextAntialiasMode(DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE);
      }
    }

    RefPtr<IDWriteRenderingParams> params =
        DWriteFactory::RenderingParams(renderingState);

    /**
     * We set the number of pixels per DIP to 1.0. This is because we always want
     * to draw in device pixels, and not device independent pixels. On high DPI
     * systems this value will be higher than 1.0 and automatically upscale
     * fonts, we don't want this since we do our own upscaling for various reasons.
     */
    rt->SetPixelsPerDip(1.0);

    if (transform) {
	rt->SetCurrentTransform(transform);
    }
    BitBlt(rt->GetMemoryDC(),
	   0, 0,
	   area.right - area.left, area.bottom - area.top,
	   surface->dc,
	   area.left, area.top,
	   SRCCOPY | NOMIRRORBITMAP);
    DWRITE_MEASURING_MODE measureMode;
    switch (renderingState) {
	case cairo_dwrite_scaled_font_t::TEXT_RENDERING_GDI_CLASSIC:
	case cairo_dwrite_scaled_font_t::TEXT_RENDERING_NO_CLEARTYPE:
	    measureMode = DWRITE_MEASURING_MODE_GDI_CLASSIC;
        break;
    default:
        measureMode = DWRITE_MEASURING_MODE_NATURAL;
        break;
    }
    rt->DrawGlyphRun(0, 0, measureMode, run, params, color);
    BitBlt(surface->dc,
	   area.left, area.top,
	   area.right - area.left, area.bottom - area.top,
	   rt->GetMemoryDC(),
	   0, 0,
	   SRCCOPY | NOMIRRORBITMAP);
    return CAIRO_INT_STATUS_SUCCESS;
}

cairo_int_status_t
_dwrite_draw_glyphs_to_gdi_surface_d2d(cairo_win32_surface_t *surface,
				       DWRITE_MATRIX *transform,
				       DWRITE_GLYPH_RUN *run,
				       COLORREF color,
				       const RECT &area)
{
    HRESULT hr;

    RefPtr<ID2D1DCRenderTarget> rt = D2DFactory::RenderTarget();

    // XXX don't we need to set RenderingParams on this RenderTarget?

    hr = rt->BindDC(surface->dc, &area);
    if (FAILED(hr))
	return CAIRO_INT_STATUS_UNSUPPORTED;

    // D2D uses 0x00RRGGBB not 0x00BBGGRR like COLORREF.
    color = (color & 0xFF) << 16 |
	(color & 0xFF00) |
	(color & 0xFF0000) >> 16;
    RefPtr<ID2D1SolidColorBrush> brush;
    hr = rt->CreateSolidColorBrush(D2D1::ColorF(color, 1.0), &brush);
    if (FAILED(hr))
	return CAIRO_INT_STATUS_UNSUPPORTED;

    if (transform) {
	rt->SetTransform(D2D1::Matrix3x2F(transform->m11,
					  transform->m12,
					  transform->m21,
					  transform->m22,
					  transform->dx,
					  transform->dy));
    }
    rt->BeginDraw();
    rt->DrawGlyphRun(D2D1::Point2F(0, 0), run, brush);
    hr = rt->EndDraw();
    if (transform) {
	rt->SetTransform(D2D1::Matrix3x2F::Identity());
    }
    if (FAILED(hr))
	return CAIRO_INT_STATUS_UNSUPPORTED;

    return CAIRO_INT_STATUS_SUCCESS;
}

/* Surface helper function */
cairo_int_status_t
_cairo_dwrite_show_glyphs_on_surface(void			*surface,
				    cairo_operator_t	 op,
				    const cairo_pattern_t	*source,
				    cairo_glyph_t		*glyphs,
				    int			 num_glyphs,
				    cairo_scaled_font_t	*scaled_font,
				    cairo_clip_t	*clip)
{
    // TODO: Check font & surface for types.
    cairo_dwrite_scaled_font_t *dwritesf = reinterpret_cast<cairo_dwrite_scaled_font_t*>(scaled_font);
    cairo_dwrite_font_face_t *dwriteff = reinterpret_cast<cairo_dwrite_font_face_t*>(scaled_font->font_face);
    cairo_win32_surface_t *dst = reinterpret_cast<cairo_win32_surface_t*>(surface);
    cairo_int_status_t status;
    /* We can only handle dwrite fonts */
    if (cairo_scaled_font_get_type (scaled_font) != CAIRO_FONT_TYPE_DWRITE)
	return CAIRO_INT_STATUS_UNSUPPORTED;

    /* We can only handle opaque solid color sources */
    if (!_cairo_pattern_is_opaque_solid(source))
	return CAIRO_INT_STATUS_UNSUPPORTED;

    /* We can only handle operator SOURCE or OVER with the destination
     * having no alpha */
    if (op != CAIRO_OPERATOR_SOURCE && op != CAIRO_OPERATOR_OVER)
	return CAIRO_INT_STATUS_UNSUPPORTED;

    /* If we have a fallback mask clip set on the dst, we have
     * to go through the fallback path */
    if (!_cairo_surface_is_win32_printing (&dst->base)) {
        if (clip != NULL)
            _cairo_win32_display_surface_set_clip (to_win32_display_surface (dst), clip);
        else
            _cairo_win32_display_surface_unset_clip (to_win32_display_surface (dst));
    }

    /* It is vital that dx values for dxy_buf are calculated from the delta of
     * _logical_ x coordinates (not user x coordinates) or else the sum of all
     * previous dx values may start to diverge from the current glyph's x
     * coordinate due to accumulated rounding error. As a result strings could
     * be painted shorter or longer than expected. */

    AutoDWriteGlyphRun run;
    run.allocate(num_glyphs);

    UINT16 *indices = const_cast<UINT16*>(run.glyphIndices);
    FLOAT *advances = const_cast<FLOAT*>(run.glyphAdvances);
    DWRITE_GLYPH_OFFSET *offsets = const_cast<DWRITE_GLYPH_OFFSET*>(run.glyphOffsets);

    BOOL transform = FALSE;
    /* Needed to calculate bounding box for efficient blitting */
    INT32 smallestX = INT_MAX;
    INT32 largestX = 0;
    INT32 smallestY = INT_MAX;
    INT32 largestY = 0;
    for (int i = 0; i < num_glyphs; i++) {
	if (glyphs[i].x < smallestX) {
	    smallestX = (INT32)glyphs[i].x;
	}
	if (glyphs[i].x > largestX) {
	    largestX = (INT32)glyphs[i].x;
	}
	if (glyphs[i].y < smallestY) {
	    smallestY = (INT32)glyphs[i].y;
	}
	if (glyphs[i].y > largestY) {
	    largestY = (INT32)glyphs[i].y;
	}
    }
    /**
     * Here we try to get a rough estimate of the area that this glyph run will
     * cover on the surface. Since we use GDI interop to draw we will be copying
     * data around the size of the area of the surface that we map. We will want
     * to map an area as small as possible to prevent large surfaces to be
     * copied around. We take the X/Y-size of the font as margin on the left/top
     * twice the X/Y-size of the font as margin on the right/bottom.
     * This should always cover the entire area where the glyphs are.
     */
    RECT fontArea;
    fontArea.left = (INT32)(smallestX - scaled_font->font_matrix.xx);
    fontArea.right = (INT32)(largestX + scaled_font->font_matrix.xx * 2);
    fontArea.top = (INT32)(smallestY - scaled_font->font_matrix.yy);
    fontArea.bottom = (INT32)(largestY + scaled_font->font_matrix.yy * 2);
    if (fontArea.left < 0)
	fontArea.left = 0;
    if (fontArea.top < 0)
	fontArea.top = 0;
    if (fontArea.bottom > dst->extents.height) {
	fontArea.bottom = dst->extents.height;
    }
    if (fontArea.right > dst->extents.width) {
	fontArea.right = dst->extents.width;
    }
    if (fontArea.right <= fontArea.left ||
	fontArea.bottom <= fontArea.top) {
	return CAIRO_INT_STATUS_SUCCESS;
    }
    if (fontArea.right > dst->extents.width) {
	fontArea.right = dst->extents.width;
    }
    if (fontArea.bottom > dst->extents.height) {
	fontArea.bottom = dst->extents.height;
    }

    run.bidiLevel = 0;
    run.fontFace = dwriteff->dwriteface;
    run.isSideways = FALSE;
    if (dwritesf->mat.xy == 0 && dwritesf->mat.yx == 0 &&
	dwritesf->mat.xx == scaled_font->font_matrix.xx &&
	dwritesf->mat.yy == scaled_font->font_matrix.yy) {

	for (int i = 0; i < num_glyphs; i++) {
	    indices[i] = (WORD) glyphs[i].index;
	    // Since we will multiply by our ctm matrix later for rotation effects
	    // and such, adjust positions by the inverse matrix now.
	    offsets[i].ascenderOffset = (FLOAT)(fontArea.top - glyphs[i].y);
	    offsets[i].advanceOffset = (FLOAT)(glyphs[i].x - fontArea.left);
	    advances[i] = 0.0;
	}
	run.fontEmSize = (FLOAT)scaled_font->font_matrix.yy;
    } else {
	transform = TRUE;
        // See comment about EPSILON in _cairo_dwrite_glyph_run_from_glyphs
        const double EPSILON = 0.0001;
	for (int i = 0; i < num_glyphs; i++) {
	    indices[i] = (WORD) glyphs[i].index;
	    double x = glyphs[i].x - fontArea.left + EPSILON;
	    double y = glyphs[i].y - fontArea.top;
	    cairo_matrix_transform_point(&dwritesf->mat_inverse, &x, &y);
	    /**
	     * Since we will multiply by our ctm matrix later for rotation effects
	     * and such, adjust positions by the inverse matrix now. The Y-axis
	     * is inverted so the offset becomes negative.
	     */
	    offsets[i].ascenderOffset = -(FLOAT)y;
	    offsets[i].advanceOffset = (FLOAT)x;
	    advances[i] = 0.0;
	}
	run.fontEmSize = 1.0f;
    }

    cairo_solid_pattern_t *solid_pattern = (cairo_solid_pattern_t *)source;
    COLORREF color = RGB(((int)solid_pattern->color.red_short) >> 8,
		((int)solid_pattern->color.green_short) >> 8,
		((int)solid_pattern->color.blue_short) >> 8);

    DWRITE_MATRIX matrix = _cairo_dwrite_matrix_from_matrix(&dwritesf->mat);

    DWRITE_MATRIX *mat;
    if (transform) {
	mat = &matrix;
    } else {
	mat = NULL;
    }

    RECT area;
    area.left = dst->extents.x;
    area.top = dst->extents.y;
    area.right = area.left + dst->extents.width;
    area.bottom = area.top + dst->extents.height;

#ifdef CAIRO_TRY_D2D_TO_GDI
    status = _dwrite_draw_glyphs_to_gdi_surface_d2d(dst,
						    mat,
						    &run,
						    color,
						    fontArea);

    if (status == (cairo_status_t)CAIRO_INT_STATUS_UNSUPPORTED) {
#endif
	status = _dwrite_draw_glyphs_to_gdi_surface_gdi(dst,
							mat,
							&run,
							color,
							dwritesf,
							fontArea);

#ifdef CAIRO_TRY_D2D_TO_GDI
    }
#endif

    return status;
}

#define ENHANCED_CONTRAST_REGISTRY_KEY \
    HKEY_CURRENT_USER, "Software\\Microsoft\\Avalon.Graphics\\DISPLAY1\\EnhancedContrastLevel"

void
DWriteFactory::CreateRenderingParams()
{
    if (!Instance()) {
	return;
    }

    Instance()->CreateRenderingParams(&mDefaultRenderingParams);

    // For EnhancedContrast, we override the default if the user has not set it
    // in the registry (by using the ClearType Tuner).
    FLOAT contrast;
    if (mEnhancedContrast >= 0.0 && mEnhancedContrast <= 10.0) {
	contrast = mEnhancedContrast;
    } else {
	HKEY hKey;
	if (RegOpenKeyExA(ENHANCED_CONTRAST_REGISTRY_KEY,
			  0, KEY_READ, &hKey) == ERROR_SUCCESS)
	{
	    contrast = mDefaultRenderingParams->GetEnhancedContrast();
	    RegCloseKey(hKey);
	} else {
	    contrast = 1.0;
	}
    }

    // For parameters that have not been explicitly set via the SetRenderingParams API,
    // we copy values from default params (or our overridden value for contrast)
    FLOAT gamma =
        mGamma >= 1.0 && mGamma <= 2.2 ?
            mGamma : mDefaultRenderingParams->GetGamma();
    FLOAT clearTypeLevel =
        mClearTypeLevel >= 0.0 && mClearTypeLevel <= 1.0 ?
            mClearTypeLevel : mDefaultRenderingParams->GetClearTypeLevel();
    DWRITE_PIXEL_GEOMETRY pixelGeometry =
        mPixelGeometry >= DWRITE_PIXEL_GEOMETRY_FLAT && mPixelGeometry <= DWRITE_PIXEL_GEOMETRY_BGR ?
            (DWRITE_PIXEL_GEOMETRY)mPixelGeometry : mDefaultRenderingParams->GetPixelGeometry();
    DWRITE_RENDERING_MODE renderingMode =
        mRenderingMode >= DWRITE_RENDERING_MODE_DEFAULT && mRenderingMode <= DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL_SYMMETRIC ?
            (DWRITE_RENDERING_MODE)mRenderingMode : mDefaultRenderingParams->GetRenderingMode();

    Instance()->CreateCustomRenderingParams(gamma, contrast, clearTypeLevel,
	pixelGeometry, renderingMode,
	&mCustomClearTypeRenderingParams);

    Instance()->CreateCustomRenderingParams(gamma, contrast, clearTypeLevel,
        pixelGeometry, DWRITE_RENDERING_MODE_CLEARTYPE_GDI_CLASSIC,
        &mForceGDIClassicRenderingParams);
}

/* Check if a specific font table in a DWrite font and a scaled font is identical */
static cairo_int_status_t
compare_font_tables (cairo_dwrite_font_face_t *dwface,
		     cairo_scaled_font_t      *scaled_font,
		     unsigned long             tag,
		     cairo_bool_t             *match)
{
    unsigned long size;
    cairo_int_status_t status;
    unsigned char *buffer = NULL;
    const void *dw_data;
    UINT32 dw_size;
    void *dw_tableContext = NULL;
    BOOL dw_exists = FALSE;
    HRESULT hr;

    hr = dwface->dwriteface->TryGetFontTable(be32_to_cpu (tag),
					     &dw_data,
					     &dw_size,
					     &dw_tableContext,
					     &dw_exists);
    if (FAILED(hr))
	return _cairo_dwrite_error (hr, "TryGetFontTable failed");

    if (!dw_exists) {
	*match = FALSE;
	status = CAIRO_INT_STATUS_SUCCESS;
	goto cleanup;
    }

    status = scaled_font->backend->load_truetype_table (scaled_font, tag, 0, NULL, &size);
    if (unlikely(status))
	goto cleanup;

    if (size != dw_size) {
	*match = FALSE;
	status = CAIRO_INT_STATUS_SUCCESS;
	goto cleanup;
    }

    buffer = (unsigned char *) _cairo_malloc (size);
    if (unlikely (buffer == NULL)) {
	status = (cairo_int_status_t) _cairo_error (CAIRO_STATUS_NO_MEMORY);
	goto cleanup;
    }

    status = scaled_font->backend->load_truetype_table (scaled_font, tag, 0, buffer, &size);
    if (unlikely(status))
	goto cleanup;

    *match = memcmp (dw_data, buffer, size) == 0;
    status = CAIRO_INT_STATUS_SUCCESS;

cleanup:
    free (buffer);
    if (dw_tableContext)
	dwface->dwriteface->ReleaseFontTable(dw_tableContext);

    return status;
}

/* Check if a DWrite font and a scaled font areis identical
 *
 * DWrite does not allow accessing the entire font data using tag=0 so we compare
 * two of the font tables:
 * - 'name' table
 * - 'head' table since this contains the checksum for the entire font
 */
static cairo_int_status_t
font_tables_match (cairo_dwrite_font_face_t *dwface,
		   cairo_scaled_font_t      *scaled_font,
		   cairo_bool_t             *match)
{
    cairo_int_status_t status;

    status = compare_font_tables (dwface, scaled_font, TT_TAG_name, match);
    if (unlikely(status))
	return status;

    if (!*match)
	return CAIRO_INT_STATUS_SUCCESS;

    status = compare_font_tables (dwface, scaled_font, TT_TAG_head, match);
    if (unlikely(status))
	return status;

    return CAIRO_INT_STATUS_SUCCESS;
}

/*
 * Helper for _cairo_win32_printing_surface_show_glyphs to create a win32 equivalent
 * of a dwrite scaled_font so that we can print using ExtTextOut instead of drawing
 * paths or blitting glyph bitmaps.
 */
cairo_int_status_t
_cairo_dwrite_scaled_font_create_win32_scaled_font (cairo_scaled_font_t *scaled_font,
                                                    cairo_scaled_font_t **new_font)
{
    if (cairo_scaled_font_get_type (scaled_font) != CAIRO_FONT_TYPE_DWRITE) {
        return CAIRO_INT_STATUS_UNSUPPORTED;
    }

    cairo_font_face_t *face = cairo_scaled_font_get_font_face (scaled_font);
    cairo_dwrite_font_face_t *dwface = reinterpret_cast<cairo_dwrite_font_face_t*>(face);

    RefPtr<IDWriteGdiInterop> gdiInterop;
    DWriteFactory::Instance()->GetGdiInterop(&gdiInterop);
    if (!gdiInterop) {
        return CAIRO_INT_STATUS_UNSUPPORTED;
    }

    LOGFONTW logfont;
    if (FAILED(gdiInterop->ConvertFontFaceToLOGFONT (dwface->dwriteface, &logfont))) {
	return CAIRO_INT_STATUS_UNSUPPORTED;
    }

    /* DWrite must have been using an outline font, so we want GDI to use the same,
     * even if there's also a bitmap face available
     */
    logfont.lfOutPrecision = OUT_OUTLINE_PRECIS;

    cairo_font_face_t *win32_face = cairo_win32_font_face_create_for_logfontw (&logfont);
    if (cairo_font_face_status (win32_face)) {
	cairo_font_face_destroy (win32_face);
        return CAIRO_INT_STATUS_UNSUPPORTED;
    }

    cairo_matrix_t font_matrix;
    cairo_scaled_font_get_font_matrix (scaled_font, &font_matrix);

    cairo_matrix_t ctm;
    cairo_scaled_font_get_ctm (scaled_font, &ctm);

    cairo_font_options_t options;
    cairo_scaled_font_get_font_options (scaled_font, &options);

    cairo_scaled_font_t *font = cairo_scaled_font_create (win32_face,
			                                  &font_matrix,
			                                  &ctm,
			                                  &options);
    cairo_font_face_destroy (win32_face);

    if (cairo_scaled_font_status(font)) {
	cairo_scaled_font_destroy (font);
        return CAIRO_INT_STATUS_UNSUPPORTED;
    }

    cairo_bool_t match;
    cairo_int_status_t status;
    status = font_tables_match (dwface, font, &match);
    if (status) {
	cairo_scaled_font_destroy (font);
	return status;
    }

    /* If the font tables aren't equal, then GDI may have failed to
     * find the right font and substituted a different font.
     */
    if (!match) {
#if 0
	char *ps_name;
	char *font_name;
	status = _cairo_truetype_read_font_name (scaled_font, &ps_name, &font_name);
	printf("dwrite fontname: %s PS name: %s\n", font_name, ps_name);
	free (font_name);
	free (ps_name);
	status = _cairo_truetype_read_font_name (font, &ps_name, &font_name);
	printf("win32  fontname: %s PS name: %s\n", font_name, ps_name);
	free (font_name);
	free (ps_name);
#endif
	cairo_scaled_font_destroy (font);
        return CAIRO_INT_STATUS_UNSUPPORTED;
    }

    *new_font = font;
    return CAIRO_INT_STATUS_SUCCESS;
}
