// SPDX-License-Identifier: GPL-2.0-only
#include "control.h"

#include <assert.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>

#include "common/mem.h"
#include "labwc-control-v1-protocol.h"
#include "labwc.h"
#include "view.h"

struct control_toplevel {
	struct wl_resource *resource;

	/*
	 * NULL once the toplevel has been unmapped. The ext-foreign-toplevel
	 * handle is destroyed before the view it belongs to, and our destroy
	 * listener clears the pointer, so a non-NULL handle guarantees that
	 * ext_handle->data is a live view.
	 */
	struct wlr_ext_foreign_toplevel_handle_v1 *ext_handle;

	struct wl_listener ext_handle_destroy;
};

/*
 * An absolute geometry is only meaningful for a floating view, and a client
 * of this protocol cannot know what state the view is in. Take it out of
 * anything which would override the geometry we are about to set.
 *
 * This mirrors what view_move_relative() does for MoveRelative, plus
 * fullscreen and shading, which the action variants assume the caller has
 * already dealt with.
 */
static void
ensure_floating(struct view *view)
{
	if (view->fullscreen) {
		view_set_fullscreen(view, false);
	}
	view_maximize(view, VIEW_AXIS_NONE);
	if (view_is_tiled(view)) {
		view_set_untiled(view);
	}
	view_set_shade(view, false);
}

static void
apply_geometry(struct view *view, int32_t x, int32_t y,
		int32_t width, int32_t height)
{
	/* The view has no geometry to work with until it has been configured */
	if (width <= 0 || height <= 0) {
		return;
	}

	view_move_resize(view, (struct wlr_box){
		.x = x,
		.y = y,
		.width = width,
		.height = height,
	});
}

/*
 * Resolve the view and take it out of any state which would override the
 * geometry we are about to set. Callers must read view->pending for any
 * component they want to keep *after* this, so that un-maximizing or
 * un-tiling a view does not resurrect the geometry it had in that state.
 */
static struct view *
control_toplevel_prepare(struct control_toplevel *toplevel)
{
	struct view *view = toplevel->ext_handle
		? toplevel->ext_handle->data : NULL;
	if (view) {
		ensure_floating(view);
	}
	return view;
}

/* Client requests */
static void
handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
toplevel_move_to(struct wl_client *client, struct wl_resource *resource,
		int32_t x, int32_t y)
{
	struct control_toplevel *toplevel = wl_resource_get_user_data(resource);
	struct view *view = control_toplevel_prepare(toplevel);
	if (!view) {
		return;
	}
	apply_geometry(view, x, y, view->pending.width, view->pending.height);
}

static void
toplevel_resize_to(struct wl_client *client, struct wl_resource *resource,
		int32_t width, int32_t height)
{
	struct control_toplevel *toplevel = wl_resource_get_user_data(resource);
	struct view *view = control_toplevel_prepare(toplevel);
	if (!view) {
		return;
	}
	apply_geometry(view, view->pending.x, view->pending.y, width, height);
}

static void
toplevel_move_resize_to(struct wl_client *client, struct wl_resource *resource,
		int32_t x, int32_t y, int32_t width, int32_t height)
{
	struct control_toplevel *toplevel = wl_resource_get_user_data(resource);
	struct view *view = control_toplevel_prepare(toplevel);
	if (!view) {
		return;
	}
	apply_geometry(view, x, y, width, height);
}

static const struct labwc_control_toplevel_v1_interface toplevel_impl = {
	.destroy = handle_destroy,
	.move_to = toplevel_move_to,
	.resize_to = toplevel_resize_to,
	.move_resize_to = toplevel_move_resize_to,
};

static void
handle_ext_handle_destroy(struct wl_listener *listener, void *data)
{
	struct control_toplevel *toplevel =
		wl_container_of(listener, toplevel, ext_handle_destroy);

	wl_list_remove(&toplevel->ext_handle_destroy.link);
	toplevel->ext_handle = NULL;
}

static void
toplevel_resource_destroy(struct wl_resource *resource)
{
	struct control_toplevel *toplevel = wl_resource_get_user_data(resource);
	if (!toplevel) {
		return;
	}

	if (toplevel->ext_handle) {
		wl_list_remove(&toplevel->ext_handle_destroy.link);
	}
	free(toplevel);
}

static void
control_get_toplevel(struct wl_client *client, struct wl_resource *resource,
		uint32_t id, struct wl_resource *foreign_toplevel)
{
	struct wlr_ext_foreign_toplevel_handle_v1 *ext_handle =
		wlr_ext_foreign_toplevel_handle_v1_from_resource(foreign_toplevel);

	struct wl_resource *toplevel_resource = wl_resource_create(client,
		&labwc_control_toplevel_v1_interface,
		wl_resource_get_version(resource), id);
	if (!toplevel_resource) {
		wl_client_post_no_memory(client);
		return;
	}

	struct control_toplevel *toplevel = znew(*toplevel);
	toplevel->resource = toplevel_resource;
	toplevel->ext_handle = ext_handle;
	if (ext_handle) {
		toplevel->ext_handle_destroy.notify = handle_ext_handle_destroy;
		wl_signal_add(&ext_handle->events.destroy,
			&toplevel->ext_handle_destroy);
	}

	wl_resource_set_implementation(toplevel_resource, &toplevel_impl,
		toplevel, toplevel_resource_destroy);
}

static const struct labwc_control_v1_interface control_impl = {
	.destroy = handle_destroy,
	.get_toplevel = control_get_toplevel,
};

static void
control_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&labwc_control_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &control_impl, NULL, NULL);
}

/* Internal API */
void
control_create_global(struct wl_display *display)
{
	struct wl_global *global = wl_global_create(display,
		&labwc_control_v1_interface, 1, NULL, control_bind);
	if (!global) {
		wlr_log(WLR_ERROR, "unable to create labwc_control_v1 global");
	}
}
