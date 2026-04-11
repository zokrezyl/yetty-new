/* extract-assets.c - Extract embedded assets to cache directory */

#include <yetty/platform/extract-assets.h>
#include <yetty/config.h>
#include <stdlib.h>
#include <string.h>

/* Forward declaration - implemented by incbin-assets.c */
struct yetty_incbin_assets;
struct yetty_incbin_assets *yetty_incbin_assets_create(void);
void yetty_incbin_assets_destroy(struct yetty_incbin_assets *assets);
int yetty_incbin_assets_needs_extraction(struct yetty_incbin_assets *assets, const char *cache_dir);
int yetty_incbin_assets_extract_to(struct yetty_incbin_assets *assets, const char *cache_dir);

struct yetty_core_void_result yetty_platform_extract_assets(struct yetty_config *config)
{
    const char *shaders_path;
    char cache_dir[512];
    char *last_slash;
    struct yetty_incbin_assets *assets;
    int needs_extract;

    if (!config)
        return YETTY_OK_VOID();

    shaders_path = config->ops->get_string(config, "paths/shaders", "");
    if (!shaders_path || !shaders_path[0])
        return YETTY_OK_VOID();

    /* Get parent directory of shaders path */
    strncpy(cache_dir, shaders_path, sizeof(cache_dir) - 1);
    cache_dir[sizeof(cache_dir) - 1] = '\0';
    last_slash = strrchr(cache_dir, '/');
    if (last_slash)
        *last_slash = '\0';
    else
        return YETTY_OK_VOID();

    assets = yetty_incbin_assets_create();
    if (!assets)
        return YETTY_OK_VOID(); /* No embedded assets - development build */

    needs_extract = yetty_incbin_assets_needs_extraction(assets, cache_dir);
    if (!needs_extract) {
        yetty_incbin_assets_destroy(assets);
        return YETTY_OK_VOID();
    }

    if (!yetty_incbin_assets_extract_to(assets, cache_dir)) {
        yetty_incbin_assets_destroy(assets);
        return YETTY_ERR(yetty_core_void, "failed to extract assets");
    }

    yetty_incbin_assets_destroy(assets);
    return YETTY_OK_VOID();
}
