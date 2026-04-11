#ifndef YETTY_CONFIG_H
#define YETTY_CONFIG_H

#include <stddef.h>
#include <stdint.h>
#include <yetty/core/result.h>

#ifdef __cplusplus
extern "C" {
#endif

struct yetty_config;

/* Platform-specific paths */
struct yetty_platform_paths {
    const char *shaders_dir;
    const char *fonts_dir;
    const char *runtime_dir;
    const char *bin_dir;
};

/* Result type */
YETTY_RESULT_DECLARE(yetty_config, struct yetty_config *);

/* Config ops */
struct yetty_config_ops {
    void (*destroy)(struct yetty_config *self);

    /* Get string value by slash path */
    const char *(*get_string)(const struct yetty_config *self, const char *path,
                              const char *default_value);
    /* Get int value by slash path */
    int (*get_int)(const struct yetty_config *self, const char *path, int default_value);
    /* Get bool value by slash path */
    int (*get_bool)(const struct yetty_config *self, const char *path, int default_value);

    /* Check if key exists */
    int (*has)(const struct yetty_config *self, const char *path);

    /* Set string value */
    struct yetty_core_void_result (*set_string)(struct yetty_config *self,
                                                 const char *path, const char *value);

    /* Legacy accessors */
    int (*use_damage_tracking)(const struct yetty_config *self);
    int (*show_fps)(const struct yetty_config *self);
    int (*debug_damage_rects)(const struct yetty_config *self);
    uint32_t (*scrollback_lines)(const struct yetty_config *self);
    const char *(*font_family)(const struct yetty_config *self);
};

/* Config base */
struct yetty_config {
    const struct yetty_config_ops *ops;
};

/* Common config keys */
#define YETTY_CONFIG_KEY_PLUGINS_PATH "plugins/path"
#define YETTY_CONFIG_KEY_RENDERING_DAMAGE_TRACKING "rendering/damage-tracking"
#define YETTY_CONFIG_KEY_RENDERING_SHOW_FPS "rendering/show-fps"
#define YETTY_CONFIG_KEY_SCROLLBACK_LINES "scrollback/lines"
#define YETTY_CONFIG_KEY_DEBUG_DAMAGE_RECTS "debug/damage-rects"
#define YETTY_CONFIG_KEY_FONT_FAMILY "font/family"
#define YETTY_CONFIG_KEY_RPC_SOCKET_PATH "rpc/socket-path"

/* Create config */
struct yetty_config_result yetty_config_create(int argc, char *argv[],
                                                const struct yetty_platform_paths *paths);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_CONFIG_H */
