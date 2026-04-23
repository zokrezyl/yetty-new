/* Linux platform paths - XDG directories */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char cache_dir_buf[512];
static char data_dir_buf[512];
static char runtime_dir_buf[512];
static char config_dir_buf[512];

const char *yetty_yplatform_get_cache_dir(void)
{
    const char *xdg = getenv("XDG_CACHE_HOME");
    if (xdg) {
        snprintf(cache_dir_buf, sizeof(cache_dir_buf), "%s/yetty", xdg);
        return cache_dir_buf;
    }

    const char *home = getenv("HOME");
    if (home) {
        snprintf(cache_dir_buf, sizeof(cache_dir_buf), "%s/.cache/yetty", home);
        return cache_dir_buf;
    }

    return "/tmp/yetty";
}

const char *yetty_yplatform_get_data_dir(void)
{
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg) {
        snprintf(data_dir_buf, sizeof(data_dir_buf), "%s/yetty", xdg);
        return data_dir_buf;
    }

    const char *home = getenv("HOME");
    if (home) {
        snprintf(data_dir_buf, sizeof(data_dir_buf), "%s/.local/share/yetty", home);
        return data_dir_buf;
    }

    return "/tmp/yetty";
}

const char *yetty_yplatform_get_runtime_dir(void)
{
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg) {
        snprintf(runtime_dir_buf, sizeof(runtime_dir_buf), "%s/yetty", xdg);
        return runtime_dir_buf;
    }

    snprintf(runtime_dir_buf, sizeof(runtime_dir_buf), "/tmp/yetty-%d", getuid());
    return runtime_dir_buf;
}

const char *yetty_yplatform_get_config_dir(void)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg) {
        snprintf(config_dir_buf, sizeof(config_dir_buf), "%s/yetty", xdg);
        return config_dir_buf;
    }

    const char *home = getenv("HOME");
    if (home) {
        snprintf(config_dir_buf, sizeof(config_dir_buf), "%s/.config/yetty", home);
        return config_dir_buf;
    }

    return "/tmp/yetty";
}
