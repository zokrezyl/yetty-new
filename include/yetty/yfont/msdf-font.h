#ifndef YETTY_FONT_MSDF_FONT_H
#define YETTY_FONT_MSDF_FONT_H

#include <yetty/yfont/font.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Create monospace MSDF font from CDB file path.
 * cdb_path: path to the .cdb file with pre-generated MSDF glyphs
 * pixel_range: SDF pixel range (typically 4.0)
 */
struct yetty_font_font_result
yetty_font_msdf_font_create(const char *cdb_path, float pixel_range);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_FONT_MSDF_FONT_H */
