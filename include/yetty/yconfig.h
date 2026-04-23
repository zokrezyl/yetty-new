#ifndef YETTY_YCONFIG_H
#define YETTY_YCONFIG_H

#include <stddef.h>
#include <stdint.h>
#include <yetty/ycore/result.h>

#ifdef __cplusplus
extern "C" {
#endif

struct yetty_yconfig;

/* Platform-specific paths */
struct yetty_yplatform_paths {
    const char *shaders_dir;
    const char *fonts_dir;
    const char *runtime_dir;
    const char *bin_dir;
};

/* Result type */
YETTY_YRESULT_DECLARE(yetty_yconfig, struct yetty_yconfig *);

/* Config ops */
struct yetty_yconfig_ops {
    void (*destroy)(struct yetty_yconfig *self);

    /* Get string value by slash path */
    const char *(*get_string)(const struct yetty_yconfig *self, const char *path,
                              const char *default_value);
    /* Get int value by slash path */
    int (*get_int)(const struct yetty_yconfig *self, const char *path, int default_value);
    /* Get bool value by slash path */
    int (*get_bool)(const struct yetty_yconfig *self, const char *path, int default_value);

    /* Check if key exists */
    int (*has)(const struct yetty_yconfig *self, const char *path);

    /* Set string value */
    struct yetty_ycore_void_result (*set_string)(struct yetty_yconfig *self,
                                                 const char *path, const char *value);

    /* Get sub-config at path (returns NULL if not found, no ownership transfer) */
    struct yetty_yconfig *(*get_node)(const struct yetty_yconfig *self,
                                     const char *path);

    /* Legacy accessors */
    int (*use_damage_tracking)(const struct yetty_yconfig *self);
    int (*show_fps)(const struct yetty_yconfig *self);
    int (*debug_damage_rects)(const struct yetty_yconfig *self);
    uint32_t (*scrollback_lines)(const struct yetty_yconfig *self);
    const char *(*font_family)(const struct yetty_yconfig *self);
};

/* Config base */
struct yetty_yconfig {
    const struct yetty_yconfig_ops *ops;
};

/* Common config keys */
#define YETTY_YCONFIG_KEY_PLUGINS_PATH "plugins/path"
#define YETTY_YCONFIG_KEY_RENDERING_DAMAGE_TRACKING "rendering/damage-tracking"
#define YETTY_YCONFIG_KEY_RENDERING_SHOW_FPS "rendering/show-fps"
#define YETTY_YCONFIG_KEY_SCROLLBACK_LINES "scrollback/lines"
#define YETTY_YCONFIG_KEY_DEBUG_DAMAGE_RECTS "debug/damage-rects"
#define YETTY_YCONFIG_KEY_FONT_FAMILY "font/family"
#define YETTY_YCONFIG_KEY_TERMINAL_FONT_RENDER_METHOD "terminal/text-layer/font/render-method"
#define YETTY_YCONFIG_KEY_RPC_HOST "rpc/host"
#define YETTY_YCONFIG_KEY_RPC_PORT "rpc/port"
#define YETTY_YCONFIG_KEY_RPC_SOCKET_PATH "rpc/socket-path"
#define YETTY_YCONFIG_KEY_VIRTUAL "virtual"

/* Create config */
struct yetty_yconfig_result yetty_yconfig_create(int argc, char *argv[],
                                                const struct yetty_yplatform_paths *paths);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_YCONFIG_H */
