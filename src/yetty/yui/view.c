#include <yetty/yui/view.h>
#include <stdlib.h>
#include <stdatomic.h>

/*=============================================================================
 * Object ID generation
 *===========================================================================*/

static atomic_uint_fast64_t g_next_view_id = 1;

static yetty_core_object_id next_view_id(void)
{
	return atomic_fetch_add(&g_next_view_id, 1);
}

/*=============================================================================
 * ID generation for subclasses
 *===========================================================================*/

yetty_core_object_id yetty_yui_view_next_id(void)
{
	return next_view_id();
}

/*=============================================================================
 * View public API
 *===========================================================================*/

void yetty_yui_view_destroy(struct yetty_yui_view *view)
{
	if (!view)
		return;
	if (view->ops && view->ops->destroy)
		view->ops->destroy(view);
}

struct yetty_core_void_result yetty_yui_view_render(struct yetty_yui_view *view,
						    void *render_pass)
{
	if (!view)
		return YETTY_ERR(yetty_core_void, "view is NULL");
	if (!view->ops || !view->ops->render)
		return YETTY_ERR(yetty_core_void, "render not implemented");
	return view->ops->render(view, render_pass);
}

struct yetty_core_void_result yetty_yui_view_run(struct yetty_yui_view *view)
{
	if (!view)
		return YETTY_ERR(yetty_core_void, "view is NULL");
	if (!view->ops || !view->ops->run)
		return YETTY_OK_VOID();
	return view->ops->run(view);
}

void yetty_yui_view_set_bounds(struct yetty_yui_view *view,
			       struct yetty_yui_rect bounds)
{
	if (!view)
		return;
	view->bounds = bounds;
	if (view->ops && view->ops->set_bounds)
		view->ops->set_bounds(view, bounds);
}

yetty_core_object_id yetty_yui_view_id(const struct yetty_yui_view *view)
{
	return view ? view->id : YETTY_CORE_OBJECT_ID_NONE;
}

struct yetty_yui_rect yetty_yui_view_bounds(const struct yetty_yui_view *view)
{
	if (!view)
		return (struct yetty_yui_rect){0, 0, 0, 0};
	return view->bounds;
}
