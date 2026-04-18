#ifndef YETTY_FONT_MS_MSDF_FONT_H
#define YETTY_FONT_MS_MSDF_FONT_H

#include <yetty/yfont/ms-font.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Create monospace MSDF font from .cdb file at requested font size */
struct yetty_font_ms_font_result
yetty_font_ms_msdf_font_create(const char *cdb_path, const char *shader_path,
                               float font_size);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_FONT_MS_MSDF_FONT_H */
