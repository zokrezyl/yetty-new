#ifndef YETTY_YUI_WORKSPACE_H
#define YETTY_YUI_WORKSPACE_H

#include <stddef.h>
#include <stdint.h>
#include <yetty/core/result.h>
#include <yetty/core/types.h>
#include <yetty/yui/tile.h>

#ifdef __cplusplus
extern "C" {
#endif

struct yetty_yui_workspace;

/* Result types */
YETTY_RESULT_DECLARE(yetty_yui_workspace_ptr, struct yetty_yui_workspace *);

/* Create/destroy */
struct yetty_yui_workspace_ptr_result yetty_yui_workspace_create(void);

void yetty_yui_workspace_destroy(struct yetty_yui_workspace *ws);

/* Core operations */
struct yetty_core_void_result
yetty_yui_workspace_render(struct yetty_yui_workspace *ws, void *render_pass);

struct yetty_core_void_result
yetty_yui_workspace_resize(struct yetty_yui_workspace *ws, float width,
			   float height);

struct yetty_core_void_result yetty_yui_workspace_run(struct yetty_yui_workspace *ws);

/* Root tile management */
struct yetty_core_void_result
yetty_yui_workspace_set_root(struct yetty_yui_workspace *ws,
			     struct yetty_yui_tile *tile);

struct yetty_yui_tile *
yetty_yui_workspace_root(const struct yetty_yui_workspace *ws);

/* Accessors */
float yetty_yui_workspace_width(const struct yetty_yui_workspace *ws);
float yetty_yui_workspace_height(const struct yetty_yui_workspace *ws);

/* Tree operations */
struct yetty_core_void_result
yetty_yui_workspace_split_pane(struct yetty_yui_workspace *ws,
			       yetty_core_object_id pane_id,
			       enum yetty_yui_orientation orientation);

struct yetty_core_void_result
yetty_yui_workspace_close_tile(struct yetty_yui_workspace *ws,
			       yetty_core_object_id tile_id);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_YUI_WORKSPACE_H */
