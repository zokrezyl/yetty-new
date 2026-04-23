/* iOS platform paths - app container directories */

#import <Foundation/Foundation.h>
#include <stdio.h>

static char cache_dir_buf[512];
static char config_dir_buf[512];
static char bundle_dir_buf[512];

const char *yetty_yplatform_get_bundle_dir(void)
{
    NSString *bundlePath = [[NSBundle mainBundle] bundlePath];
    if (bundlePath) {
        snprintf(bundle_dir_buf, sizeof(bundle_dir_buf), "%s", [bundlePath UTF8String]);
        return bundle_dir_buf;
    }
    return ".";
}

const char *yetty_yplatform_get_cache_dir(void)
{
    NSArray *paths = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES);
    if (paths.count > 0) {
        snprintf(cache_dir_buf, sizeof(cache_dir_buf), "%s/yetty", [paths[0] UTF8String]);
        return cache_dir_buf;
    }
    return "/tmp/yetty";
}

const char *yetty_yplatform_get_runtime_dir(void)
{
    return yetty_yplatform_get_cache_dir();
}

const char *yetty_yplatform_get_config_dir(void)
{
    NSArray *paths = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES);
    if (paths.count > 0) {
        snprintf(config_dir_buf, sizeof(config_dir_buf), "%s/yetty", [paths[0] UTF8String]);
        return config_dir_buf;
    }
    return "/tmp/yetty";
}

const char *yetty_yplatform_get_assets_dir(void)
{
    /* iOS assets are in the app bundle */
    return yetty_yplatform_get_bundle_dir();
}
