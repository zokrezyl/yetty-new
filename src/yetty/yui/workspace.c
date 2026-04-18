#include <yetty/yui/workspace.h>
#include <yetty/yui/tile.h>
#include <stdlib.h>

/*=============================================================================
 * Internal workspace structure
 *===========================================================================*/

struct yetty_yui_workspace {
	struct yetty_yui_tile *root;
	float width;
	float height;
};

/*=============================================================================
 * Create/destroy
 *===========================================================================*/

struct yetty_yui_workspace_ptr_result yetty_yui_workspace_create(void)
{
	struct yetty_yui_workspace *ws;

	ws = calloc(1, sizeof(struct yetty_yui_workspace));
	if (!ws)
		return YETTY_ERR(yetty_yui_workspace_ptr, "allocation failed");

	return YETTY_OK(yetty_yui_workspace_ptr, ws);
}

void yetty_yui_workspace_destroy(struct yetty_yui_workspace *ws)
{
	if (!ws)
		return;

	if (ws->root)
		yetty_yui_tile_destroy(ws->root);

	free(ws);
}

/*=============================================================================
 * Core operations
 *===========================================================================*/

struct yetty_core_void_result
yetty_yui_workspace_render(struct yetty_yui_workspace *ws, void *render_pass)
{
	if (!ws)
		return YETTY_ERR(yetty_core_void, "workspace is NULL");

	if (ws->root)
		return yetty_yui_tile_render(ws->root, render_pass);

	return YETTY_OK_VOID();
}

struct yetty_core_void_result
yetty_yui_workspace_resize(struct yetty_yui_workspace *ws, float width,
			   float height)
{
	struct yetty_yui_rect bounds;

	if (!ws)
		return YETTY_ERR(yetty_core_void, "workspace is NULL");

	ws->width = width;
	ws->height = height;

	if (ws->root) {
		bounds = (struct yetty_yui_rect){0, 0, width, height};
		return yetty_yui_tile_set_bounds(ws->root, bounds);
	}

	return YETTY_OK_VOID();
}

struct yetty_core_void_result
yetty_yui_workspace_run(struct yetty_yui_workspace *ws)
{
	if (!ws)
		return YETTY_ERR(yetty_core_void, "workspace is NULL");

	if (ws->root)
		return yetty_yui_tile_run(ws->root);

	return YETTY_OK_VOID();
}

/*=============================================================================
 * Root tile management
 *===========================================================================*/

struct yetty_core_void_result
yetty_yui_workspace_set_root(struct yetty_yui_workspace *ws,
			     struct yetty_yui_tile *tile)
{
	struct yetty_yui_rect bounds;

	if (!ws)
		return YETTY_ERR(yetty_core_void, "workspace is NULL");

	ws->root = tile;

	if (tile && ws->width > 0 && ws->height > 0) {
		bounds = (struct yetty_yui_rect){0, 0, ws->width, ws->height};
		return yetty_yui_tile_set_bounds(tile, bounds);
	}

	return YETTY_OK_VOID();
}

struct yetty_yui_tile *
yetty_yui_workspace_root(const struct yetty_yui_workspace *ws)
{
	return ws ? ws->root : NULL;
}

float yetty_yui_workspace_width(const struct yetty_yui_workspace *ws)
{
	return ws ? ws->width : 0;
}

float yetty_yui_workspace_height(const struct yetty_yui_workspace *ws)
{
	return ws ? ws->height : 0;
}

/*=============================================================================
 * Tree operations
 *===========================================================================*/

struct yetty_core_void_result
yetty_yui_workspace_split_pane(struct yetty_yui_workspace *ws,
			       yetty_core_object_id pane_id,
			       enum yetty_yui_orientation orientation)
{
	struct yetty_yui_tile *target;
	struct yetty_yui_tile *parent_split;
	struct yetty_yui_tile_ptr_result split_res;
	struct yetty_yui_tile_ptr_result new_pane_res;
	struct yetty_yui_tile *split;
	struct yetty_yui_tile *new_pane;
	struct yetty_core_void_result res;

	if (!ws)
		return YETTY_ERR(yetty_core_void, "workspace is NULL");
	if (!ws->root)
		return YETTY_ERR(yetty_core_void, "no root tile");

	/* Find target pane */
	target = yetty_yui_tile_find_by_id(ws->root, pane_id);
	if (!target)
		return YETTY_ERR(yetty_core_void, "pane not found");

	/* Create new split */
	split_res = yetty_yui_split_create(orientation);
	if (YETTY_IS_ERR(split_res))
		return YETTY_ERR(yetty_core_void, split_res.error.msg);
	split = split_res.value;

	/* Create new pane */
	new_pane_res = yetty_yui_pane_create();
	if (YETTY_IS_ERR(new_pane_res)) {
		yetty_yui_tile_destroy(split);
		return YETTY_ERR(yetty_core_void, new_pane_res.error.msg);
	}
	new_pane = new_pane_res.value;

	/* Set up split children */
	res = yetty_yui_split_set_first(split, target);
	if (YETTY_IS_ERR(res)) {
		yetty_yui_tile_destroy(split);
		yetty_yui_tile_destroy(new_pane);
		return res;
	}

	res = yetty_yui_split_set_second(split, new_pane);
	if (YETTY_IS_ERR(res)) {
		yetty_yui_tile_destroy(split);
		yetty_yui_tile_destroy(new_pane);
		return res;
	}

	/* Replace target in tree */
	if (ws->root == target) {
		/* Target was root */
		return yetty_yui_workspace_set_root(ws, split);
	}

	/* Find parent and replace */
	parent_split = yetty_yui_tile_find_parent_split(ws->root, pane_id);
	if (!parent_split) {
		yetty_yui_tile_destroy(split);
		return YETTY_ERR(yetty_core_void, "parent split not found");
	}

	if (yetty_yui_split_first(parent_split) == target) {
		res = yetty_yui_split_set_first(parent_split, split);
	} else {
		res = yetty_yui_split_set_second(parent_split, split);
	}

	if (YETTY_IS_ERR(res))
		return res;

	/* Re-layout */
	if (ws->width > 0 && ws->height > 0) {
		struct yetty_yui_rect bounds = {0, 0, ws->width, ws->height};
		return yetty_yui_tile_set_bounds(ws->root, bounds);
	}

	return YETTY_OK_VOID();
}

struct yetty_core_void_result
yetty_yui_workspace_close_tile(struct yetty_yui_workspace *ws,
			       yetty_core_object_id tile_id)
{
	struct yetty_yui_tile *parent_split;
	struct yetty_yui_tile *sibling;
	struct yetty_yui_tile *grandparent;
	struct yetty_core_void_result res;

	if (!ws)
		return YETTY_ERR(yetty_core_void, "workspace is NULL");
	if (!ws->root)
		return YETTY_ERR(yetty_core_void, "no root tile");

	/* Closing root? */
	if (yetty_yui_tile_id(ws->root) == tile_id) {
		yetty_yui_tile_destroy(ws->root);
		ws->root = NULL;
		return YETTY_OK_VOID();
	}

	/* Find parent split */
	parent_split = yetty_yui_tile_find_parent_split(ws->root, tile_id);
	if (!parent_split)
		return YETTY_ERR(yetty_core_void, "parent split not found");

	/* Determine sibling */
	if (yetty_yui_tile_id(yetty_yui_split_first(parent_split)) == tile_id) {
		sibling = yetty_yui_split_second(parent_split);
	} else {
		sibling = yetty_yui_split_first(parent_split);
	}

	/* Parent split is root? Promote sibling to root */
	if (ws->root == parent_split) {
		/* Clear parent's children to prevent double-free */
		yetty_yui_split_set_first(parent_split, NULL);
		yetty_yui_split_set_second(parent_split, NULL);
		yetty_yui_tile_destroy(parent_split);

		ws->root = sibling;
		if (sibling && ws->width > 0 && ws->height > 0) {
			struct yetty_yui_rect bounds = {0, 0, ws->width, ws->height};
			return yetty_yui_tile_set_bounds(sibling, bounds);
		}
		return YETTY_OK_VOID();
	}

	/* Find grandparent and replace parent_split with sibling */
	grandparent = yetty_yui_tile_find_parent_split(
	    ws->root, yetty_yui_tile_id(parent_split));
	if (!grandparent)
		return YETTY_ERR(yetty_core_void, "grandparent not found");

	/* Clear parent's children to prevent double-free */
	yetty_yui_split_set_first(parent_split, NULL);
	yetty_yui_split_set_second(parent_split, NULL);

	if (yetty_yui_split_first(grandparent) == parent_split) {
		res = yetty_yui_split_set_first(grandparent, sibling);
	} else {
		res = yetty_yui_split_set_second(grandparent, sibling);
	}

	yetty_yui_tile_destroy(parent_split);

	if (YETTY_IS_ERR(res))
		return res;

	/* Re-layout */
	if (ws->width > 0 && ws->height > 0) {
		struct yetty_yui_rect bounds = {0, 0, ws->width, ws->height};
		return yetty_yui_tile_set_bounds(ws->root, bounds);
	}

	return YETTY_OK_VOID();
}
