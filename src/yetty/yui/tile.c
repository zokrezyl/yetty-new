#include <yetty/yui/tile.h>
#include <yetty/yui/view.h>
#include <stdlib.h>
#include <stdatomic.h>

/*=============================================================================
 * Object ID generation
 *===========================================================================*/

static atomic_uint_fast64_t g_next_id = 1;

static yetty_core_object_id next_object_id(void)
{
	return atomic_fetch_add(&g_next_id, 1);
}

/*=============================================================================
 * Internal tile structures
 *===========================================================================*/

struct yetty_yui_tile {
	const struct yetty_yui_tile_ops *ops;
	yetty_core_object_id id;
	enum yetty_yui_tile_type type;
	struct yetty_yui_rect bounds;
	struct yetty_yui_tile *parent;
};

struct yetty_yui_tile_ops {
	void (*destroy)(struct yetty_yui_tile *self);
	struct yetty_core_void_result (*render)(struct yetty_yui_tile *self,
						void *render_pass);
	struct yetty_core_void_result (*set_bounds)(struct yetty_yui_tile *self,
						    struct yetty_yui_rect bounds);
	struct yetty_core_void_result (*run)(struct yetty_yui_tile *self);
};

struct yetty_yui_split {
	struct yetty_yui_tile base;
	enum yetty_yui_orientation orientation;
	float ratio;
	struct yetty_yui_tile *first;
	struct yetty_yui_tile *second;
};

struct yetty_yui_pane {
	struct yetty_yui_tile base;
	struct yetty_yui_view **views;
	size_t view_count;
	size_t view_capacity;
	int focused;
};

/*=============================================================================
 * Split implementation
 *===========================================================================*/

static void split_destroy(struct yetty_yui_tile *self)
{
	struct yetty_yui_split *split = (struct yetty_yui_split *)self;

	if (split->first)
		yetty_yui_tile_destroy(split->first);
	if (split->second)
		yetty_yui_tile_destroy(split->second);

	free(split);
}

static struct yetty_core_void_result split_render(struct yetty_yui_tile *self,
						  void *render_pass)
{
	struct yetty_yui_split *split = (struct yetty_yui_split *)self;
	struct yetty_core_void_result res;

	if (split->first) {
		res = yetty_yui_tile_render(split->first, render_pass);
		if (YETTY_IS_ERR(res))
			return res;
	}
	if (split->second) {
		res = yetty_yui_tile_render(split->second, render_pass);
		if (YETTY_IS_ERR(res))
			return res;
	}

	return YETTY_OK_VOID();
}

static struct yetty_core_void_result split_set_bounds(struct yetty_yui_tile *self,
						      struct yetty_yui_rect bounds)
{
	struct yetty_yui_split *split = (struct yetty_yui_split *)self;
	struct yetty_yui_rect first_bounds, second_bounds;

	split->base.bounds = bounds;

	if (split->orientation == YETTY_YUI_HORIZONTAL) {
		float first_h = bounds.h * split->ratio;

		first_bounds = (struct yetty_yui_rect){
		    bounds.x, bounds.y, bounds.w, first_h};
		second_bounds = (struct yetty_yui_rect){
		    bounds.x, bounds.y + first_h, bounds.w, bounds.h - first_h};
	} else {
		float first_w = bounds.w * split->ratio;

		first_bounds = (struct yetty_yui_rect){
		    bounds.x, bounds.y, first_w, bounds.h};
		second_bounds = (struct yetty_yui_rect){
		    bounds.x + first_w, bounds.y, bounds.w - first_w, bounds.h};
	}

	if (split->first)
		yetty_yui_tile_set_bounds(split->first, first_bounds);
	if (split->second)
		yetty_yui_tile_set_bounds(split->second, second_bounds);

	return YETTY_OK_VOID();
}

static struct yetty_core_void_result split_run(struct yetty_yui_tile *self)
{
	struct yetty_yui_split *split = (struct yetty_yui_split *)self;
	struct yetty_core_void_result res;

	if (split->first) {
		res = yetty_yui_tile_run(split->first);
		if (YETTY_IS_ERR(res))
			return res;
	}
	if (split->second) {
		res = yetty_yui_tile_run(split->second);
		if (YETTY_IS_ERR(res))
			return res;
	}

	return YETTY_OK_VOID();
}

static const struct yetty_yui_tile_ops split_ops = {
    .destroy = split_destroy,
    .render = split_render,
    .set_bounds = split_set_bounds,
    .run = split_run,
};

struct yetty_yui_tile_ptr_result
yetty_yui_split_create(enum yetty_yui_orientation orientation)
{
	struct yetty_yui_split *split;

	split = calloc(1, sizeof(struct yetty_yui_split));
	if (!split)
		return YETTY_ERR(yetty_yui_tile_ptr, "allocation failed");

	split->base.ops = &split_ops;
	split->base.id = next_object_id();
	split->base.type = YETTY_YUI_TILE_SPLIT;
	split->orientation = orientation;
	split->ratio = 0.5f;

	return YETTY_OK(yetty_yui_tile_ptr, &split->base);
}

/*=============================================================================
 * Pane implementation
 *===========================================================================*/

static void pane_destroy(struct yetty_yui_tile *self)
{
	struct yetty_yui_pane *pane = (struct yetty_yui_pane *)self;

	for (size_t i = 0; i < pane->view_count; i++) {
		if (pane->views[i])
			yetty_yui_view_destroy(pane->views[i]);
	}

	free(pane->views);
	free(pane);
}

static struct yetty_core_void_result pane_render(struct yetty_yui_tile *self,
						 void *render_pass)
{
	struct yetty_yui_pane *pane = (struct yetty_yui_pane *)self;

	if (pane->view_count > 0 && pane->views[pane->view_count - 1])
		return yetty_yui_view_render(pane->views[pane->view_count - 1],
					     render_pass);

	return YETTY_OK_VOID();
}

static struct yetty_core_void_result pane_set_bounds(struct yetty_yui_tile *self,
						     struct yetty_yui_rect bounds)
{
	struct yetty_yui_pane *pane = (struct yetty_yui_pane *)self;

	pane->base.bounds = bounds;

	/* Propagate to active view */
	if (pane->view_count > 0 && pane->views[pane->view_count - 1])
		yetty_yui_view_set_bounds(pane->views[pane->view_count - 1],
					  bounds);

	return YETTY_OK_VOID();
}

static struct yetty_core_void_result pane_run(struct yetty_yui_tile *self)
{
	struct yetty_yui_pane *pane = (struct yetty_yui_pane *)self;
	struct yetty_core_void_result res;

	for (size_t i = 0; i < pane->view_count; i++) {
		if (pane->views[i]) {
			res = yetty_yui_view_run(pane->views[i]);
			if (YETTY_IS_ERR(res))
				return res;
		}
	}

	return YETTY_OK_VOID();
}

static const struct yetty_yui_tile_ops pane_ops = {
    .destroy = pane_destroy,
    .render = pane_render,
    .set_bounds = pane_set_bounds,
    .run = pane_run,
};

struct yetty_yui_tile_ptr_result yetty_yui_pane_create(void)
{
	struct yetty_yui_pane *pane;

	pane = calloc(1, sizeof(struct yetty_yui_pane));
	if (!pane)
		return YETTY_ERR(yetty_yui_tile_ptr, "allocation failed");

	pane->base.ops = &pane_ops;
	pane->base.id = next_object_id();
	pane->base.type = YETTY_YUI_TILE_PANE;

	return YETTY_OK(yetty_yui_tile_ptr, &pane->base);
}

/*=============================================================================
 * Tile public API
 *===========================================================================*/

void yetty_yui_tile_destroy(struct yetty_yui_tile *tile)
{
	if (!tile)
		return;
	if (tile->ops && tile->ops->destroy)
		tile->ops->destroy(tile);
}

struct yetty_core_void_result yetty_yui_tile_render(struct yetty_yui_tile *tile,
						    void *render_pass)
{
	if (!tile)
		return YETTY_ERR(yetty_core_void, "tile is NULL");
	if (!tile->ops || !tile->ops->render)
		return YETTY_ERR(yetty_core_void, "render not implemented");
	return tile->ops->render(tile, render_pass);
}

struct yetty_core_void_result
yetty_yui_tile_set_bounds(struct yetty_yui_tile *tile,
			  struct yetty_yui_rect bounds)
{
	if (!tile)
		return YETTY_ERR(yetty_core_void, "tile is NULL");
	if (!tile->ops || !tile->ops->set_bounds)
		return YETTY_ERR(yetty_core_void, "set_bounds not implemented");
	return tile->ops->set_bounds(tile, bounds);
}

struct yetty_core_void_result yetty_yui_tile_run(struct yetty_yui_tile *tile)
{
	if (!tile)
		return YETTY_ERR(yetty_core_void, "tile is NULL");
	if (!tile->ops || !tile->ops->run)
		return YETTY_OK_VOID();
	return tile->ops->run(tile);
}

yetty_core_object_id yetty_yui_tile_id(const struct yetty_yui_tile *tile)
{
	return tile ? tile->id : YETTY_CORE_OBJECT_ID_NONE;
}

enum yetty_yui_tile_type yetty_yui_tile_type(const struct yetty_yui_tile *tile)
{
	return tile ? tile->type : YETTY_YUI_TILE_PANE;
}

struct yetty_yui_rect yetty_yui_tile_bounds(const struct yetty_yui_tile *tile)
{
	if (!tile)
		return (struct yetty_yui_rect){0, 0, 0, 0};
	return tile->bounds;
}

/*=============================================================================
 * Split public API
 *===========================================================================*/

struct yetty_core_void_result
yetty_yui_split_set_first(struct yetty_yui_tile *tile,
			  struct yetty_yui_tile *child)
{
	struct yetty_yui_split *split;

	if (!tile || tile->type != YETTY_YUI_TILE_SPLIT)
		return YETTY_ERR(yetty_core_void, "not a split");

	split = (struct yetty_yui_split *)tile;
	split->first = child;
	if (child)
		child->parent = tile;

	return YETTY_OK_VOID();
}

struct yetty_core_void_result
yetty_yui_split_set_second(struct yetty_yui_tile *tile,
			   struct yetty_yui_tile *child)
{
	struct yetty_yui_split *split;

	if (!tile || tile->type != YETTY_YUI_TILE_SPLIT)
		return YETTY_ERR(yetty_core_void, "not a split");

	split = (struct yetty_yui_split *)tile;
	split->second = child;
	if (child)
		child->parent = tile;

	return YETTY_OK_VOID();
}

struct yetty_core_void_result
yetty_yui_split_set_ratio(struct yetty_yui_tile *tile, float ratio)
{
	struct yetty_yui_split *split;

	if (!tile || tile->type != YETTY_YUI_TILE_SPLIT)
		return YETTY_ERR(yetty_core_void, "not a split");
	if (ratio < 0.0f || ratio > 1.0f)
		return YETTY_ERR(yetty_core_void, "ratio must be 0-1");

	split = (struct yetty_yui_split *)tile;
	split->ratio = ratio;

	return YETTY_OK_VOID();
}

struct yetty_yui_tile *yetty_yui_split_first(const struct yetty_yui_tile *tile)
{
	if (!tile || tile->type != YETTY_YUI_TILE_SPLIT)
		return NULL;
	return ((struct yetty_yui_split *)tile)->first;
}

struct yetty_yui_tile *yetty_yui_split_second(const struct yetty_yui_tile *tile)
{
	if (!tile || tile->type != YETTY_YUI_TILE_SPLIT)
		return NULL;
	return ((struct yetty_yui_split *)tile)->second;
}

float yetty_yui_split_ratio(const struct yetty_yui_tile *tile)
{
	if (!tile || tile->type != YETTY_YUI_TILE_SPLIT)
		return 0.5f;
	return ((struct yetty_yui_split *)tile)->ratio;
}

enum yetty_yui_orientation
yetty_yui_split_orientation(const struct yetty_yui_tile *tile)
{
	if (!tile || tile->type != YETTY_YUI_TILE_SPLIT)
		return YETTY_YUI_HORIZONTAL;
	return ((struct yetty_yui_split *)tile)->orientation;
}

/*=============================================================================
 * Pane public API
 *===========================================================================*/

struct yetty_core_void_result
yetty_yui_pane_push_view(struct yetty_yui_tile *tile,
			 struct yetty_yui_view *view)
{
	struct yetty_yui_pane *pane;

	if (!tile || tile->type != YETTY_YUI_TILE_PANE)
		return YETTY_ERR(yetty_core_void, "not a pane");

	pane = (struct yetty_yui_pane *)tile;

	if (pane->view_count >= pane->view_capacity) {
		size_t new_cap = pane->view_capacity ? pane->view_capacity * 2 : 4;
		struct yetty_yui_view **new_views;

		new_views = realloc(pane->views,
				    new_cap * sizeof(struct yetty_yui_view *));
		if (!new_views)
			return YETTY_ERR(yetty_core_void, "allocation failed");

		pane->views = new_views;
		pane->view_capacity = new_cap;
	}

	pane->views[pane->view_count++] = view;

	/* Set bounds on new view */
	if (view)
		yetty_yui_view_set_bounds(view, pane->base.bounds);

	return YETTY_OK_VOID();
}

struct yetty_core_void_result
yetty_yui_pane_pop_view(struct yetty_yui_tile *tile)
{
	struct yetty_yui_pane *pane;

	if (!tile || tile->type != YETTY_YUI_TILE_PANE)
		return YETTY_ERR(yetty_core_void, "not a pane");

	pane = (struct yetty_yui_pane *)tile;

	if (pane->view_count == 0)
		return YETTY_ERR(yetty_core_void, "no views to pop");

	pane->view_count--;

	return YETTY_OK_VOID();
}

struct yetty_yui_view *yetty_yui_pane_active_view(const struct yetty_yui_tile *tile)
{
	const struct yetty_yui_pane *pane;

	if (!tile || tile->type != YETTY_YUI_TILE_PANE)
		return NULL;

	pane = (const struct yetty_yui_pane *)tile;
	if (pane->view_count == 0)
		return NULL;

	return pane->views[pane->view_count - 1];
}

size_t yetty_yui_pane_view_count(const struct yetty_yui_tile *tile)
{
	if (!tile || tile->type != YETTY_YUI_TILE_PANE)
		return 0;
	return ((const struct yetty_yui_pane *)tile)->view_count;
}

int yetty_yui_pane_has_view(const struct yetty_yui_tile *tile,
			    yetty_core_object_id view_id)
{
	const struct yetty_yui_pane *pane;

	if (!tile || tile->type != YETTY_YUI_TILE_PANE)
		return 0;

	pane = (const struct yetty_yui_pane *)tile;
	for (size_t i = 0; i < pane->view_count; i++) {
		if (pane->views[i] && yetty_yui_view_id(pane->views[i]) == view_id)
			return 1;
	}

	return 0;
}

int yetty_yui_pane_focused(const struct yetty_yui_tile *tile)
{
	if (!tile || tile->type != YETTY_YUI_TILE_PANE)
		return 0;
	return ((const struct yetty_yui_pane *)tile)->focused;
}

void yetty_yui_pane_set_focused(struct yetty_yui_tile *tile, int focused)
{
	if (!tile || tile->type != YETTY_YUI_TILE_PANE)
		return;
	((struct yetty_yui_pane *)tile)->focused = focused;
}

/*=============================================================================
 * Tree helpers
 *===========================================================================*/

struct yetty_yui_tile *yetty_yui_tile_find_by_id(struct yetty_yui_tile *root,
						 yetty_core_object_id id)
{
	struct yetty_yui_tile *found;

	if (!root)
		return NULL;
	if (root->id == id)
		return root;

	if (root->type == YETTY_YUI_TILE_SPLIT) {
		struct yetty_yui_split *split = (struct yetty_yui_split *)root;

		found = yetty_yui_tile_find_by_id(split->first, id);
		if (found)
			return found;
		return yetty_yui_tile_find_by_id(split->second, id);
	}

	return NULL;
}

struct yetty_yui_tile *
yetty_yui_tile_find_parent_split(struct yetty_yui_tile *root,
				 yetty_core_object_id target_id)
{
	struct yetty_yui_split *split;
	struct yetty_yui_tile *found;

	if (!root || root->type != YETTY_YUI_TILE_SPLIT)
		return NULL;

	split = (struct yetty_yui_split *)root;

	if ((split->first && split->first->id == target_id) ||
	    (split->second && split->second->id == target_id))
		return root;

	found = yetty_yui_tile_find_parent_split(split->first, target_id);
	if (found)
		return found;

	return yetty_yui_tile_find_parent_split(split->second, target_id);
}

struct yetty_yui_tile *
yetty_yui_tile_find_focused_pane(struct yetty_yui_tile *root)
{
	struct yetty_yui_split *split;
	struct yetty_yui_tile *found;

	if (!root)
		return NULL;

	if (root->type == YETTY_YUI_TILE_PANE) {
		if (((struct yetty_yui_pane *)root)->focused)
			return root;
		return NULL;
	}

	split = (struct yetty_yui_split *)root;
	found = yetty_yui_tile_find_focused_pane(split->first);
	if (found)
		return found;

	return yetty_yui_tile_find_focused_pane(split->second);
}
