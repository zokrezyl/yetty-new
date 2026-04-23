/* config.c - Configuration management using libyaml */

#include <yetty/yconfig.h>
#include <yetty/ycore/types.h>
#include <yaml.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <yetty/yplatform/compat.h>

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
    struct yetty_yconfig base;
    struct config_node *root;
    struct yetty_yplatform_paths paths;
    char shaders_dir[512];
    char fonts_dir[512];
    char runtime_dir[512];
    char bin_dir[512];
};

/* Sub-config view - lightweight wrapper pointing to a sub-node */
#define MAX_SUBCONFIGS 64
struct config_subnode {
    struct yetty_yconfig base;
    struct config_node *node;
};

static struct config_subnode g_subconfigs[MAX_SUBCONFIGS];
static int g_subconfig_count = 0;

/* Forward declarations */
static void config_destroy(struct yetty_yconfig *self);
static const char *config_get_string(const struct yetty_yconfig *self, const char *path, const char *default_value);
static int config_get_int(const struct yetty_yconfig *self, const char *path, int default_value);
static int config_get_bool(const struct yetty_yconfig *self, const char *path, int default_value);
static int config_has(const struct yetty_yconfig *self, const char *path);
static struct yetty_ycore_void_result config_set_string(struct yetty_yconfig *self, const char *path, const char *value);
static int config_use_damage_tracking(const struct yetty_yconfig *self);
static int config_show_fps(const struct yetty_yconfig *self);
static int config_debug_damage_rects(const struct yetty_yconfig *self);
static uint32_t config_scrollback_lines(const struct yetty_yconfig *self);
static const char *config_font_family(const struct yetty_yconfig *self);

static struct yetty_yconfig *config_get_node(const struct yetty_yconfig *self,
                                            const char *path);

static const struct yetty_yconfig_ops config_ops = {
    .destroy = config_destroy,
    .get_string = config_get_string,
    .get_int = config_get_int,
    .get_bool = config_get_bool,
    .has = config_has,
    .set_string = config_set_string,
    .get_node = config_get_node,
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

static void config_destroy(struct yetty_yconfig *self)
{
    struct config_impl *impl = container_of(self, struct config_impl, base);
    node_destroy(impl->root);
    free(impl);
}

static const char *config_get_string(const struct yetty_yconfig *self, const char *path, const char *default_value)
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

static int config_get_int(const struct yetty_yconfig *self, const char *path, int default_value)
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

static int config_get_bool(const struct yetty_yconfig *self, const char *path, int default_value)
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

static int config_has(const struct yetty_yconfig *self, const char *path)
{
    struct config_impl *impl = container_of(self, struct config_impl, base);
    char key[MAX_KEY_LEN] = {0};

    struct config_node *parent = navigate_path(impl->root, path, key);
    if (!parent)
        return 0;

    return node_find_child(parent, key) != NULL;
}

static struct yetty_ycore_void_result config_set_string(struct yetty_yconfig *self, const char *path, const char *value)
{
    struct config_impl *impl = container_of(self, struct config_impl, base);
    char key[MAX_KEY_LEN] = {0};

    struct config_node *parent = navigate_or_create(impl->root, path, key);
    if (!parent)
        return YETTY_ERR(yetty_ycore_void, "failed to create config path");

    node_set_value(parent, key, value);
    return YETTY_OK_VOID();
}

static int config_use_damage_tracking(const struct yetty_yconfig *self)
{
    return config_get_bool(self, YETTY_YCONFIG_KEY_RENDERING_DAMAGE_TRACKING, 1);
}

static int config_show_fps(const struct yetty_yconfig *self)
{
    return config_get_bool(self, YETTY_YCONFIG_KEY_RENDERING_SHOW_FPS, 1);
}

static int config_debug_damage_rects(const struct yetty_yconfig *self)
{
    return config_get_bool(self, YETTY_YCONFIG_KEY_DEBUG_DAMAGE_RECTS, 0);
}

static uint32_t config_scrollback_lines(const struct yetty_yconfig *self)
{
    return (uint32_t)config_get_int(self, YETTY_YCONFIG_KEY_SCROLLBACK_LINES, 10000);
}

static const char *config_font_family(const struct yetty_yconfig *self)
{
    return config_get_string(self, YETTY_YCONFIG_KEY_FONT_FAMILY, "default");
}

/* Forward declaration */
static const struct yetty_yconfig_ops subnode_ops;

/* Subnode ops - same as config ops but uses subnode's node as root */
static void subnode_destroy(struct yetty_yconfig *self)
{
    /* No-op: subnodes don't own their data */
    (void)self;
}

static const char *subnode_get_string(const struct yetty_yconfig *self,
                                      const char *path,
                                      const char *default_value)
{
    struct config_subnode *sub = (struct config_subnode *)self;
    char key[MAX_KEY_LEN] = {0};

    struct config_node *parent = navigate_path(sub->node, path, key);
    if (!parent)
        return default_value;

    struct config_node *node = node_find_child(parent, key);
    if (!node || !node->value[0])
        return default_value;

    return node->value;
}

static int subnode_get_int(const struct yetty_yconfig *self, const char *path,
                           int default_value)
{
    struct config_subnode *sub = (struct config_subnode *)self;
    char key[MAX_KEY_LEN] = {0};

    struct config_node *parent = navigate_path(sub->node, path, key);
    if (!parent)
        return default_value;

    struct config_node *node = node_find_child(parent, key);
    if (!node || !node->is_int)
        return default_value;

    return node->int_value;
}

static int subnode_get_bool(const struct yetty_yconfig *self, const char *path,
                            int default_value)
{
    struct config_subnode *sub = (struct config_subnode *)self;
    char key[MAX_KEY_LEN] = {0};

    struct config_node *parent = navigate_path(sub->node, path, key);
    if (!parent)
        return default_value;

    struct config_node *node = node_find_child(parent, key);
    if (!node || !node->is_bool)
        return default_value;

    return node->bool_value;
}

static int subnode_has(const struct yetty_yconfig *self, const char *path)
{
    struct config_subnode *sub = (struct config_subnode *)self;
    char key[MAX_KEY_LEN] = {0};

    struct config_node *parent = navigate_path(sub->node, path, key);
    if (!parent)
        return 0;

    return node_find_child(parent, key) != NULL;
}

static struct yetty_ycore_void_result subnode_set_string(struct yetty_yconfig *self,
                                                        const char *path,
                                                        const char *value)
{
    (void)self;
    (void)path;
    (void)value;
    return YETTY_ERR(yetty_ycore_void, "cannot set on subnode");
}

static struct yetty_yconfig *subnode_get_node(const struct yetty_yconfig *self,
                                             const char *path);

static const struct yetty_yconfig_ops subnode_ops = {
    .destroy = subnode_destroy,
    .get_string = subnode_get_string,
    .get_int = subnode_get_int,
    .get_bool = subnode_get_bool,
    .has = subnode_has,
    .set_string = subnode_set_string,
    .get_node = subnode_get_node,
    .use_damage_tracking = NULL,
    .show_fps = NULL,
    .debug_damage_rects = NULL,
    .scrollback_lines = NULL,
    .font_family = NULL,
};

static struct yetty_yconfig *create_subconfig(struct config_node *node)
{
    if (!node || g_subconfig_count >= MAX_SUBCONFIGS)
        return NULL;

    struct config_subnode *sub = &g_subconfigs[g_subconfig_count++];
    sub->base.ops = &subnode_ops;
    sub->node = node;
    return &sub->base;
}

static struct yetty_yconfig *config_get_node(const struct yetty_yconfig *self,
                                            const char *path)
{
    struct config_impl *impl = container_of(self, struct config_impl, base);
    char key[MAX_KEY_LEN] = {0};

    struct config_node *parent = navigate_path(impl->root, path, key);
    if (!parent)
        return NULL;

    struct config_node *node = node_find_child(parent, key);
    return create_subconfig(node);
}

static struct yetty_yconfig *subnode_get_node(const struct yetty_yconfig *self,
                                             const char *path)
{
    struct config_subnode *sub = (struct config_subnode *)self;
    char key[MAX_KEY_LEN] = {0};

    struct config_node *parent = navigate_path(sub->node, path, key);
    if (!parent)
        return NULL;

    struct config_node *node = node_find_child(parent, key);
    return create_subconfig(node);
}


/* Store platform paths */

static void store_platform_paths(struct config_impl *impl, const struct yetty_yplatform_paths *paths)
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

/* Find argument value by short or long name, returns NULL if not found */

static const char *find_arg(int argc, char *argv[], const char *short_name, const char *long_name)
{
    for (int i = 1; i < argc; i++) {
        if (short_name && strcmp(argv[i], short_name) == 0 && i + 1 < argc)
            return argv[i + 1];
        if (long_name && strcmp(argv[i], long_name) == 0 && i + 1 < argc)
            return argv[i + 1];
    }
    return NULL;
}

/* Set config value from command line argument */

static void parse_arg_to_config(struct config_impl *impl, int argc, char *argv[],
                                const char *short_name, const char *long_name,
                                const char *config_path)
{
    const char *value = find_arg(argc, argv, short_name, long_name);
    if (value) {
        char key[MAX_KEY_LEN];
        struct config_node *parent = navigate_or_create(impl->root, config_path, key);
        if (parent)
            node_set_value(parent, key, value);
    }
}

/* Parse command line for --temu and --qemu flags */

static void parse_vm_args(struct config_impl *impl, int argc, char *argv[])
{
    char key[MAX_KEY_LEN];
    struct config_node *parent;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--temu") == 0) {
            parent = navigate_or_create(impl->root, YETTY_YCONFIG_KEY_TEMU, key);
            if (parent)
                node_set_value(parent, key, "true");
        } else if (strcmp(argv[i], "--qemu") == 0) {
            parent = navigate_or_create(impl->root, YETTY_YCONFIG_KEY_QEMU, key);
            if (parent)
                node_set_value(parent, key, "true");
        }
    }
}

/* Parse command line for --ssh flag
 *
 * Accepts optional "user@host[:port]" target that follows the flag. */
static int parse_ssh_target(struct config_impl *impl, const char *target)
{
    const char *at, *colon;
    char key[MAX_KEY_LEN];
    char user[MAX_VALUE_LEN];
    char host[MAX_VALUE_LEN];
    char port[32];
    size_t user_len, host_len;
    struct config_node *parent;

    at = strchr(target, '@');
    if (!at || at == target)
        return 0;

    user_len = (size_t)(at - target);
    if (user_len >= sizeof(user))
        return 0;
    memcpy(user, target, user_len);
    user[user_len] = '\0';

    colon = strchr(at + 1, ':');
    if (colon) {
        host_len = (size_t)(colon - (at + 1));
        snprintf(port, sizeof(port), "%s", colon + 1);
    } else {
        host_len = strlen(at + 1);
        port[0] = '\0';
    }
    if (host_len == 0 || host_len >= sizeof(host))
        return 0;
    memcpy(host, at + 1, host_len);
    host[host_len] = '\0';

    parent = navigate_or_create(impl->root, "ssh/username", key);
    if (parent) node_set_value(parent, key, user);

    parent = navigate_or_create(impl->root, "ssh/host", key);
    if (parent) node_set_value(parent, key, host);

    if (port[0]) {
        parent = navigate_or_create(impl->root, "ssh/port", key);
        if (parent) node_set_value(parent, key, port);
    }
    return 1;
}

static void parse_ssh_args(struct config_impl *impl, int argc, char *argv[])
{
    char key[MAX_KEY_LEN];
    struct config_node *parent;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ssh") != 0)
            continue;

        /* Optional "user@host[:port]" follows if it isn't another flag */
        if (i + 1 < argc && argv[i + 1][0] != '-' &&
            parse_ssh_target(impl, argv[i + 1])) {
            i++;
        }
        parent = navigate_or_create(impl->root, YETTY_YCONFIG_KEY_SSH, key);
        if (parent)
            node_set_value(parent, key, "true");
    }
}

/* Public create function */

struct yetty_yconfig_result yetty_yconfig_create(int argc, char *argv[],
                                                const struct yetty_yplatform_paths *paths)
{
    struct config_impl *impl = calloc(1, sizeof(struct config_impl));
    if (!impl)
        return YETTY_ERR(yetty_yconfig, "failed to allocate config");

    impl->base.ops = &config_ops;
    impl->root = node_create(NULL);
    if (!impl->root) {
        free(impl);
        return YETTY_ERR(yetty_yconfig, "failed to allocate config root");
    }

    try_load_config_file(impl, argc, argv);
    store_platform_paths(impl, paths);
    parse_arg_to_config(impl, argc, argv, "-e", NULL, "shell/command");
    parse_arg_to_config(impl, argc, argv, NULL, "--rpc-host", YETTY_YCONFIG_KEY_RPC_HOST);
    parse_arg_to_config(impl, argc, argv, "-r", "--rpc-port", YETTY_YCONFIG_KEY_RPC_PORT);
    parse_vm_args(impl, argc, argv);
    parse_ssh_args(impl, argc, argv);

    return YETTY_OK(yetty_yconfig, &impl->base);
}
