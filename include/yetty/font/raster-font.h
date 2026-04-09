#ifndef YETTY_FONT_RASTER_FONT_H
#define YETTY_FONT_RASTER_FONT_H

#include <yetty/font/font.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Create raster font - returns base font type */
struct yetty_font_font_result yetty_font_raster_font_create(
    const char *fonts_dir,
    const char *font_name,
    float cell_width,
    float cell_height,
    int shared);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_FONT_RASTER_FONT_H */
