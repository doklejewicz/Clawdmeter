# Recompiling fonts

The `firmware/src/font_*.c` files are pre-compiled LVGL bitmap fonts.

```bash
npm install -g lv_font_conv
```

Generate each one (one at a time — `lv_font_conv` doesn't like loop-driven
invocations) with `--no-compress` (required for LVGL 9):

```bash
# Tiempos Text (titles, 56px)
lv_font_conv --font assets/TiemposText-400-Regular.otf -r 0x20-0x7E \
  --size 56 --format lvgl --bpp 4 --no-compress \
  -o firmware/src/font_tiempos_56.c --lv-include "lvgl.h"

# Styrene B (large numbers 48, panel labels 28, small text 24, minimal 20)
for size in 48 28 24 20; do
  lv_font_conv --font assets/StyreneB-Regular.otf -r 0x20-0x7E \
    --size $size --format lvgl --bpp 4 --no-compress \
    -o firmware/src/font_styrene_${size}.c --lv-include "lvgl.h"
done

# DejaVu Sans Mono (32px, with spinner Unicode chars)
lv_font_conv --font assets/DejaVuSansMono.ttf \
  -r 0x20-0x7E,0xB7,0x2026,0x2722,0x2733,0x2736,0x273B,0x273D \
  --size 32 --format lvgl --bpp 4 --no-compress \
  -o firmware/src/font_mono_32.c --lv-include "lvgl.h"
```

**Important:** `lv_font_conv` v1.5.3 outputs LVGL 8 format. Each generated
file must be patched for LVGL 9 compatibility:

1. Remove `#if LVGL_VERSION_MAJOR >= 8` guards around `font_dsc` and the font struct
2. Remove the `.cache` field from `font_dsc`
3. Add `.release_glyph = NULL`, `.kerning = 0`, `.static_bitmap = 0` to the font struct
4. Add `.fallback = NULL`, `.user_data = NULL` to the font struct

Without these patches, fonts compile but render as invisible.

## One-off glyphs via `.fallback` (e.g. font_sinhala_18.c)

The bitmap fonts above only bake in an explicit `-r` codepoint range — nothing
outside it renders (blank/tofu), and mixing glyphs from two source fonts into
one `lv_font_conv` run isn't supported. For a single extra character that the
main font's source doesn't cover (e.g. `font_mono_18`'s DejaVu Sans Mono has
no Sinhala glyphs), it's cheaper to generate a tiny standalone font with just
that codepoint and chain it on via LVGL's built-in `lv_font_t.fallback` field
— `lv_font_get_glyph_dsc()` (`lv_font.c`) walks the `fallback` chain
automatically, no per-label wiring needed:

```bash
lv_font_conv --font NotoSansSinhala.ttf -r 0x0D9E \
  --size 18 --format lvgl --bpp 4 --no-compress \
  -o firmware/src/font_sinhala_18.c --lv-include "lvgl.h"
```

Apply the same 4 LVGL 9 patches above, then **hand-edit the *primary* font's
`.c` file** (`font_mono_18.c` in this case) to point at it — this second edit
isn't part of the regen recipe and isn't scripted, so redo it if
`font_mono_18` ever gets regenerated:

```c
extern const lv_font_t font_sinhala_18;   // near the top, after the FONT_MONO_18 guard
...
.fallback = &font_sinhala_18,             // was NULL, in the font_mono_18 struct
```

Only `font_mono_18` (the CYD's small-screen tier) got this treatment — large
boards' `font_mono_32` still shows tofu for the same glyph until it gets the
same patch.
