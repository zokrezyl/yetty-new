#ifndef YETTY_FONT_MS_RASTER_FONT_H
#define YETTY_FONT_MS_RASTER_FONT_H

#include <yetty/yfont/ms-font.h>

#ifdef __cplusplus
extern "C" {
#endif

struct yetty_config;

/* Create monospace raster font from config */
struct yetty_font_ms_font_result yetty_font_ms_raster_font_create(
    struct yetty_config *config,
    float cell_width,
    float cell_height);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_FONT_MS_RASTER_FONT_H */
