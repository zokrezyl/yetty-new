/* extract-assets.c - Extract embedded assets to data and config directories */

#include <yetty/platform/extract-assets.h>
#include <yetty/yconfig.h>
#include <stdlib.h>
#include <string.h>

/* Forward declarations - implemented in platform-paths.c */
const char *yetty_yplatform_get_data_dir(void);
const char *yetty_yplatform_get_config_dir(void);

/* Forward declaration - implemented by incbin-assets.c */
struct yetty_incbin_assets;
struct yetty_incbin_assets *yetty_incbin_assets_create(void);
void yetty_incbin_assets_destroy(struct yetty_incbin_assets *assets);
int yetty_incbin_assets_needs_extraction(struct yetty_incbin_assets *assets, const char *dir);
int yetty_incbin_assets_extract_data_to(struct yetty_incbin_assets *assets, const char *data_dir);
int yetty_incbin_assets_extract_config_to(struct yetty_incbin_assets *assets, const char *config_dir);

struct yetty_ycore_void_result yetty_yplatform_extract_assets(struct yetty_yconfig *config)
{
    const char *data_dir;
    const char *config_dir;
    struct yetty_incbin_assets *assets;
    int needs_extract;

    (void)config;

    data_dir = yetty_yplatform_get_data_dir();
    config_dir = yetty_yplatform_get_config_dir();

    if (!data_dir || !data_dir[0])
        return YETTY_OK_VOID();

    assets = yetty_incbin_assets_create();
    if (!assets)
        return YETTY_OK_VOID(); /* No embedded assets - development build */

    /* Check if data extraction needed */
    needs_extract = yetty_incbin_assets_needs_extraction(assets, data_dir);
    if (needs_extract) {
        if (!yetty_incbin_assets_extract_data_to(assets, data_dir)) {
            yetty_incbin_assets_destroy(assets);
            return YETTY_ERR(yetty_ycore_void, "failed to extract data assets");
        }
    }

    /* Check if config extraction needed */
    if (config_dir && config_dir[0]) {
        needs_extract = yetty_incbin_assets_needs_extraction(assets, config_dir);
        if (needs_extract) {
            if (!yetty_incbin_assets_extract_config_to(assets, config_dir)) {
                yetty_incbin_assets_destroy(assets);
                return YETTY_ERR(yetty_ycore_void, "failed to extract config assets");
            }
        }
    }

    yetty_incbin_assets_destroy(assets);
    return YETTY_OK_VOID();
}
