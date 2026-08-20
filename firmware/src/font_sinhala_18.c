/*******************************************************************************
 * Size: 18 px
 * Bpp: 4
 * Opts: --font NotoSansSinhala[wdth,wght].ttf -r 0x0D9E --size 18 --format lvgl
 *   --bpp 4 --no-compress -o font_sinhala_18.c --lv-include lvgl.h
 *
 * Single-glyph fallback font: just U+0D9E "ඞ" (Sinhala letter, not covered by
 * DejaVu Sans Mono / font_mono_18). Wired in via lv_font_t.fallback rather
 * than merged into font_mono_18 itself, so the DejaVu regen recipe in
 * docs/fonts.md stays untouched. LVGL 9 hand-patched per that doc: dropped
 * the LVGL_VERSION_MAJOR</8 branches, added release_glyph/kerning/
 * static_bitmap/fallback/user_data — see font_mono_18.c for the same shape.
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl.h"
#endif

#ifndef FONT_SINHALA_18
#define FONT_SINHALA_18 1
#endif

#if FONT_SINHALA_18

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+0D9E "ඞ" */
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x5, 0xad, 0xff, 0xfd, 0x92, 0x0, 0x0,
    0x3, 0xee, 0xa6, 0x43, 0x57, 0xdf, 0x80, 0x0,
    0x0, 0x70, 0x0, 0x0, 0x0, 0x6, 0xf8, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x6f, 0x30,
    0x5, 0xbe, 0xff, 0xd8, 0x0, 0x0, 0xd, 0xa0,
    0x4f, 0x6c, 0xb4, 0x6e, 0xa0, 0x0, 0x8, 0xf0,
    0x8d, 0x9, 0xb0, 0x8, 0xd0, 0x0, 0x3, 0xf2,
    0x2e, 0xff, 0x54, 0x9f, 0x70, 0x0, 0x2, 0xf3,
    0x0, 0x29, 0xee, 0x93, 0x0, 0x0, 0x1, 0xf4,
    0x0, 0xdc, 0x40, 0x0, 0x0, 0x0, 0x1, 0xf3,
    0x7, 0xe0, 0x0, 0x1, 0x10, 0x0, 0x3, 0xf2,
    0x9, 0xb0, 0x0, 0xa, 0xa0, 0x0, 0x6, 0xf0,
    0x8, 0xd0, 0x0, 0xe, 0xe0, 0x0, 0xd, 0xa0,
    0x2, 0xfb, 0x55, 0xbd, 0xdb, 0x56, 0xce, 0x20,
    0x0, 0x3b, 0xef, 0xa1, 0x1b, 0xfe, 0xa2, 0x0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 257, .box_w = 16, .box_h = 16, .ofs_x = 0, .ofs_y = 0}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/

/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 3486, .range_length = 1, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    }
};

/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

static const lv_font_fmt_txt_dsc_t font_dsc = {
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 4,
    .kern_classes = 0,
    .bitmap_format = 0
};

/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
const lv_font_t font_sinhala_18 = {
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .release_glyph = NULL,
    .line_height = 16,          /*The maximum line height required by the font*/
    .base_line = 0,             /*Baseline measured from the bottom of the line*/
    .subpx = LV_FONT_SUBPX_NONE,
    .kerning = 0,
    .static_bitmap = 0,
    .underline_position = -2,
    .underline_thickness = 1,
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
    .fallback = NULL,
    .user_data = NULL,
};

#endif /*#if FONT_SINHALA_18*/
