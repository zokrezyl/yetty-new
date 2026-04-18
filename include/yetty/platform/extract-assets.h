#ifndef YETTY_PLATFORM_EXTRACT_ASSETS_H
#define YETTY_PLATFORM_EXTRACT_ASSETS_H

#include <yetty/ycore/result.h>

#ifdef __cplusplus
extern "C" {
#endif

struct yetty_config;

/* Extract embedded assets to cache directory */
struct yetty_core_void_result yetty_platform_extract_assets(struct yetty_config *config);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_PLATFORM_EXTRACT_ASSETS_H */
