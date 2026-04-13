#ifndef YETTY_FONT_RASTER_FONT_H
#define YETTY_FONT_RASTER_FONT_H

#include <yetty/yfont/font.h>

#ifdef __cplusplus
extern "C" {
#endif

struct yetty_config;

/* Create raster font from config — reads paths/fonts, font/family, loads basic latin */
struct yetty_font_font_result yetty_font_raster_font_create(
    struct yetty_config *config,
    float cell_width,
    float cell_height);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_FONT_RASTER_FONT_H */
