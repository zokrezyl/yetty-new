/* config.c - Configuration management using libyaml */

#include <yetty/config.h>
#include <yaml.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

#define MAX_KEY_LEN 64
#define MAX_VALUE_LEN 512
#define MAX_CHILDREN 64

/* Config node - tree structure */
struct config_node {
    char key[MAX_KEY_LEN];
    char value[MAX_VALUE_LEN];
    int is_int;
    int int_value;
    int is_bool;
    int bool_value;
    struct config_node *children[MAX_CHILDREN];
    int child_count;
};

/* Config implementation */
struct config_impl {
    struct yetty_config base;
    struct config_node *root;
    struct yetty_platform_paths paths;
    char shaders_dir[512];
    char fonts_dir[512];
    char runtime_dir[512];
    char bin_dir[512];
};

/* Forward declarations */
static void config_destroy(struct yetty_config *self);
static const char *config_get_string(const struct yetty_config *self, const char *path, const char *default_value);
static int config_get_int(const struct yetty_config *self, const char *path, int default_value);
static int config_get_bool(const struct yetty_config *self, const char *path, int default_value);
static int config_has(const struct yetty_config *self, const char *path);
static struct yetty_core_void_result config_set_string(struct yetty_config *self, const char *path, const char *value);
static int config_use_damage_tracking(const struct yetty_config *self);
static int config_show_fps(const struct yetty_config *self);
static int config_debug_damage_rects(const struct yetty_config *self);
static uint32_t config_scrollback_lines(const struct yetty_config *self);
static const char *config_font_family(const struct yetty_config *self);

static const struct yetty_config_ops config_ops = {
    .destroy = config_destroy,
    .get_string = config_get_string,
    .get_int = config_get_int,
    .get_bool = config_get_bool,
    .has = config_has,
    .set_string = config_set_string,
    .use_damage_tracking = config_use_damage_tracking,
    .show_fps = config_show_fps,
    .debug_damage_rects = config_debug_damage_rects,
    .scrollback_lines = config_scrollback_lines,
    .font_family = config_font_family,
};

/* Node management */

static struct config_node *node_create(const char *key)
{
    struct config_node *node = calloc(1, sizeof(struct config_node));
    if (!node)
        return NULL;

    if (key)
        strncpy(node->key, key, MAX_KEY_LEN - 1);

    return node;
}

static void node_destroy(struct config_node *node)
{
    if (!node)
        return;

    for (int i = 0; i < node->child_count; i++)
        node_destroy(node->children[i]);

    free(node);
}

static struct config_node *node_find_child(struct config_node *node, const char *key)
{
    for (int i = 0; i < node->child_count; i++) {
        if (strcmp(node->children[i]->key, key) == 0)
            return node->children[i];
    }
    return NULL;
}

static struct config_node *node_get_or_create_child(struct config_node *node, const char *key)
{
    struct config_node *child = node_find_child(node, key);
    if (child)
        return child;

    if (node->child_count >= MAX_CHILDREN)
        return NULL;

    child = node_create(key);
    if (!child)
        return NULL;

    node->children[node->child_count++] = child;
    return child;
}

/* Navigate to node by slash-path, returns leaf key in out_key */
static struct config_node *navigate_path(struct config_node *root, const char *path, char *out_key)
{
    char buf[512];
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    struct config_node *current = root;
    char *token = strtok(buf, "/");
    char *last_token = NULL;

    while (token) {
        char *next = strtok(NULL, "/");
        if (!next) {
            /* This is the leaf key */
            if (out_key)
                strncpy(out_key, token, MAX_KEY_LEN - 1);
            return current;
        }

        current = node_find_child(current, token);
        if (!current)
            return NULL;

        token = next;
    }

    return current;
}

/* Navigate and create intermediate nodes */
static struct config_node *navigate_or_create(struct config_node *root, const char *path, char *out_key)
{
    char buf[512];
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    /* Split path and find all tokens first */
    char *tokens[32];
    int token_count = 0;

    char *p = buf;
    while (*p && token_count < 32) {
        while (*p == '/')
            p++;
        if (!*p)
            break;

        tokens[token_count++] = p;

        while (*p && *p != '/')
            p++;
        if (*p)
            *p++ = '\0';
    }

    if (token_count == 0)
        return NULL;

    /* Navigate to parent, creating as needed */
    struct config_node *current = root;
    for (int i = 0; i < token_count - 1; i++) {
        current = node_get_or_create_child(current, tokens[i]);
        if (!current)
            return NULL;
    }

    /* Return leaf key */
    if (out_key)
        strncpy(out_key, tokens[token_count - 1], MAX_KEY_LEN - 1);

    return current;
}

/* Set value on a node */
static void node_set_value(struct config_node *parent, const char *key, const char *value)
{
    struct config_node *node = node_get_or_create_child(parent, key);
    if (!node)
        return;

    strncpy(node->value, value, MAX_VALUE_LEN - 1);

    /* Try to parse as bool */
    if (strcasecmp(value, "true") == 0 || strcasecmp(value, "yes") == 0 || strcmp(value, "1") == 0) {
        node->is_bool = 1;
        node->bool_value = 1;
    } else if (strcasecmp(value, "false") == 0 || strcasecmp(value, "no") == 0 || strcmp(value, "0") == 0) {
        node->is_bool = 1;
        node->bool_value = 0;
    }

    /* Try to parse as int */
    char *endptr;
    long val = strtol(value, &endptr, 10);
    if (*endptr == '\0' && endptr != value) {
        node->is_int = 1;
        node->int_value = (int)val;
    }
}

/* YAML loading */

static void load_yaml_mapping(yaml_parser_t *parser, struct config_node *node);

static void load_yaml_value(yaml_parser_t *parser, struct config_node *parent, const char *key)
{
    yaml_event_t event;

    if (!yaml_parser_parse(parser, &event))
        return;

    if (event.type == YAML_SCALAR_EVENT) {
        node_set_value(parent, key, (const char *)event.data.scalar.value);
        yaml_event_delete(&event);
    } else if (event.type == YAML_MAPPING_START_EVENT) {
        yaml_event_delete(&event);
        struct config_node *child = node_get_or_create_child(parent, key);
        if (child)
            load_yaml_mapping(parser, child);
    } else if (event.type == YAML_SEQUENCE_START_EVENT) {
        /* Skip sequences for now */
        yaml_event_delete(&event);
        int depth = 1;
        while (depth > 0 && yaml_parser_parse(parser, &event)) {
            if (event.type == YAML_SEQUENCE_START_EVENT)
                depth++;
            else if (event.type == YAML_SEQUENCE_END_EVENT)
                depth--;
            yaml_event_delete(&event);
        }
    } else {
        yaml_event_delete(&event);
    }
}

static void load_yaml_mapping(yaml_parser_t *parser, struct config_node *node)
{
    yaml_event_t event;
    char key[MAX_KEY_LEN] = {0};

    while (yaml_parser_parse(parser, &event)) {
        if (event.type == YAML_MAPPING_END_EVENT) {
            yaml_event_delete(&event);
            break;
        }

        if (event.type == YAML_SCALAR_EVENT) {
            strncpy(key, (const char *)event.data.scalar.value, MAX_KEY_LEN - 1);
            yaml_event_delete(&event);
            load_yaml_value(parser, node, key);
        } else {
            yaml_event_delete(&event);
        }
    }
}

static int load_yaml_file(struct config_node *root, const char *path)
{
    FILE *file = fopen(path, "r");
    if (!file)
        return 0;

    yaml_parser_t parser;
    if (!yaml_parser_initialize(&parser)) {
        fclose(file);
        return 0;
    }

    yaml_parser_set_input_file(&parser, file);

    yaml_event_t event;
    while (yaml_parser_parse(&parser, &event)) {
        if (event.type == YAML_MAPPING_START_EVENT) {
            yaml_event_delete(&event);
            load_yaml_mapping(&parser, root);
            break;
        }
        yaml_event_delete(&event);
        if (event.type == YAML_STREAM_END_EVENT)
            break;
    }

    yaml_parser_delete(&parser);
    fclose(file);
    return 1;
}

/* Config ops implementation */

static void config_destroy(struct yetty_config *self)
{
    struct config_impl *impl = container_of(self, struct config_impl, base);
    node_destroy(impl->root);
    free(impl);
}

static const char *config_get_string(const struct yetty_config *self, const char *path, const char *default_value)
{
    struct config_impl *impl = container_of(self, struct config_impl, base);
    char key[MAX_KEY_LEN] = {0};

    struct config_node *parent = navigate_path(impl->root, path, key);
    if (!parent)
        return default_value;

    struct config_node *node = node_find_child(parent, key);
    if (!node || !node->value[0])
        return default_value;

    return node->value;
}

static int config_get_int(const struct yetty_config *self, const char *path, int default_value)
{
    struct config_impl *impl = container_of(self, struct config_impl, base);
    char key[MAX_KEY_LEN] = {0};

    struct config_node *parent = navigate_path(impl->root, path, key);
    if (!parent)
        return default_value;

    struct config_node *node = node_find_child(parent, key);
    if (!node || !node->is_int)
        return default_value;

    return node->int_value;
}

static int config_get_bool(const struct yetty_config *self, const char *path, int default_value)
{
    struct config_impl *impl = container_of(self, struct config_impl, base);
    char key[MAX_KEY_LEN] = {0};

    struct config_node *parent = navigate_path(impl->root, path, key);
    if (!parent)
        return default_value;

    struct config_node *node = node_find_child(parent, key);
    if (!node || !node->is_bool)
        return default_value;

    return node->bool_value;
}

static int config_has(const struct yetty_config *self, const char *path)
{
    struct config_impl *impl = container_of(self, struct config_impl, base);
    char key[MAX_KEY_LEN] = {0};

    struct config_node *parent = navigate_path(impl->root, path, key);
    if (!parent)
        return 0;

    return node_find_child(parent, key) != NULL;
}

static struct yetty_core_void_result config_set_string(struct yetty_config *self, const char *path, const char *value)
{
    struct config_impl *impl = container_of(self, struct config_impl, base);
    char key[MAX_KEY_LEN] = {0};

    struct config_node *parent = navigate_or_create(impl->root, path, key);
    if (!parent)
        return YETTY_ERR(yetty_core_void, "failed to create config path");

    node_set_value(parent, key, value);
    return YETTY_OK_VOID();
}

static int config_use_damage_tracking(const struct yetty_config *self)
{
    return config_get_bool(self, YETTY_CONFIG_KEY_RENDERING_DAMAGE_TRACKING, 1);
}

static int config_show_fps(const struct yetty_config *self)
{
    return config_get_bool(self, YETTY_CONFIG_KEY_RENDERING_SHOW_FPS, 1);
}

static int config_debug_damage_rects(const struct yetty_config *self)
{
    return config_get_bool(self, YETTY_CONFIG_KEY_DEBUG_DAMAGE_RECTS, 0);
}

static uint32_t config_scrollback_lines(const struct yetty_config *self)
{
    return (uint32_t)config_get_int(self, YETTY_CONFIG_KEY_SCROLLBACK_LINES, 10000);
}

static const char *config_font_family(const struct yetty_config *self)
{
    return config_get_string(self, YETTY_CONFIG_KEY_FONT_FAMILY, "default");
}


/* Store platform paths */

static void store_platform_paths(struct config_impl *impl, const struct yetty_platform_paths *paths)
{
    if (!paths)
        return;

    if (paths->shaders_dir) {
        strncpy(impl->shaders_dir, paths->shaders_dir, sizeof(impl->shaders_dir) - 1);
        char key[MAX_KEY_LEN];
        struct config_node *parent = navigate_or_create(impl->root, "paths/shaders", key);
        if (parent)
            node_set_value(parent, key, paths->shaders_dir);
    }

    if (paths->fonts_dir) {
        strncpy(impl->fonts_dir, paths->fonts_dir, sizeof(impl->fonts_dir) - 1);
        char key[MAX_KEY_LEN];
        struct config_node *parent = navigate_or_create(impl->root, "paths/fonts", key);
        if (parent)
            node_set_value(parent, key, paths->fonts_dir);
    }

    if (paths->runtime_dir) {
        strncpy(impl->runtime_dir, paths->runtime_dir, sizeof(impl->runtime_dir) - 1);
        char key[MAX_KEY_LEN];
        struct config_node *parent = navigate_or_create(impl->root, "paths/runtime", key);
        if (parent)
            node_set_value(parent, key, paths->runtime_dir);
    }

    if (paths->bin_dir) {
        strncpy(impl->bin_dir, paths->bin_dir, sizeof(impl->bin_dir) - 1);
        char key[MAX_KEY_LEN];
        struct config_node *parent = navigate_or_create(impl->root, "paths/bin", key);
        if (parent)
            node_set_value(parent, key, paths->bin_dir);
    }
}

/* Try to find and load config file */

static void try_load_config_file(struct config_impl *impl, int argc, char *argv[])
{
    /* Check for -c or --config argument */
    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) && i + 1 < argc) {
            load_yaml_file(impl->root, argv[i + 1]);
            return;
        }
        if (strncmp(argv[i], "-c=", 3) == 0) {
            load_yaml_file(impl->root, argv[i] + 3);
            return;
        }
        if (strncmp(argv[i], "--config=", 9) == 0) {
            load_yaml_file(impl->root, argv[i] + 9);
            return;
        }
    }

    /* Try XDG config path */
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char path[512];

    if (xdg) {
        snprintf(path, sizeof(path), "%s/yetty/config.yaml", xdg);
        if (load_yaml_file(impl->root, path))
            return;
    }

    if (home) {
        snprintf(path, sizeof(path), "%s/.config/yetty/config.yaml", home);
        load_yaml_file(impl->root, path);
    }
}

/* Parse command line for -e (execute) */

static void parse_execute_arg(struct config_impl *impl, int argc, char *argv[])
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) {
            char key[MAX_KEY_LEN];
            struct config_node *parent = navigate_or_create(impl->root, "shell/command", key);
            if (parent)
                node_set_value(parent, key, argv[i + 1]);
            return;
        }
    }
}

/* Public create function */

struct yetty_config_result yetty_config_create(int argc, char *argv[],
                                                const struct yetty_platform_paths *paths)
{
    struct config_impl *impl = calloc(1, sizeof(struct config_impl));
    if (!impl)
        return YETTY_ERR(yetty_config, "failed to allocate config");

    impl->base.ops = &config_ops;
    impl->root = node_create(NULL);
    if (!impl->root) {
        free(impl);
        return YETTY_ERR(yetty_config, "failed to allocate config root");
    }

    try_load_config_file(impl, argc, argv);
    store_platform_paths(impl, paths);
    parse_execute_arg(impl, argc, argv);

    return YETTY_OK(yetty_config, &impl->base);
}
