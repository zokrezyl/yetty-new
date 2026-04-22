/* extract-assets.c - Extract embedded assets to data and config directories */

#include <yetty/platform/extract-assets.h>
#include <yetty/yconfig.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Forward declarations - implemented in platform-paths.c */
const char *yetty_platform_get_data_dir(void);
const char *yetty_platform_get_config_dir(void);

/* Forward declaration - implemented by incbin-assets.c */
struct yetty_incbin_assets;
struct yetty_incbin_assets *yetty_incbin_assets_create(void);
void yetty_incbin_assets_destroy(struct yetty_incbin_assets *assets);
int yetty_incbin_assets_needs_extraction(struct yetty_incbin_assets *assets, const char *dir);
int yetty_incbin_assets_extract_data_to(struct yetty_incbin_assets *assets, const char *data_dir);
int yetty_incbin_assets_extract_config_to(struct yetty_incbin_assets *assets, const char *config_dir);
int yetty_incbin_assets_extract_tinyemu_to(struct yetty_incbin_assets *assets, const char *data_dir);
int yetty_incbin_assets_has_tinyemu(struct yetty_incbin_assets *assets);

struct yetty_core_void_result yetty_platform_extract_assets(struct yetty_config *config)
{
    const char *data_dir;
    const char *config_dir;
    struct yetty_incbin_assets *assets;
    int needs_extract;

    (void)config;

    data_dir = yetty_platform_get_data_dir();
    config_dir = yetty_platform_get_config_dir();

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
            return YETTY_ERR(yetty_core_void, "failed to extract data assets");
        }
    }

    /* Check if config extraction needed */
    if (config_dir && config_dir[0]) {
        needs_extract = yetty_incbin_assets_needs_extraction(assets, config_dir);
        if (needs_extract) {
            if (!yetty_incbin_assets_extract_config_to(assets, config_dir)) {
                yetty_incbin_assets_destroy(assets);
                return YETTY_ERR(yetty_core_void, "failed to extract config assets");
            }
        }
    }

    /* Extract tinyemu if embedded and config file doesn't exist */
    if (yetty_incbin_assets_has_tinyemu(assets)) {
        char tinyemu_cfg[512];
        snprintf(tinyemu_cfg, sizeof(tinyemu_cfg), "%s/tinyemu/root-riscv64.cfg", data_dir);
        if (access(tinyemu_cfg, F_OK) != 0) {
            if (!yetty_incbin_assets_extract_tinyemu_to(assets, data_dir)) {
                yetty_incbin_assets_destroy(assets);
                return YETTY_ERR(yetty_core_void, "failed to extract tinyemu assets");
            }
        }
    }

    yetty_incbin_assets_destroy(assets);
    return YETTY_OK_VOID();
}
