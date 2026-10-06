// SPDX-License-Identifier: GPL-2.0-only
#include "control.h"

#include <assert.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>

#include "common/mem.h"
#include "config/rcxml.h"
#include "cycle.h"
#include "labwc-control-v1-protocol.h"
#include "labwc.h"
#include "view.h"

struct control_toplevel {
	struct wl_resource *resource;

	/*
	 * NULL once the toplevel has been unmapped. The ext-foreign-toplevel
	 * handle is destroyed before the view it belongs to, and our destroy
	 * listener clears the pointer, so a non-NULL handle guarantees that
	 * ext_handle->data is a live view, and that the view listeners below
	 * are still connected.
	 */
	struct wlr_ext_foreign_toplevel_handle_v1 *ext_handle;

	struct wl_listener ext_handle_destroy;

	/* View state changes, so that clients can read back what they set */
	struct wl_listener view_always_on_top;
	struct wl_listener view_shaded;
	struct wl_listener view_decorations;
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

static struct view *
control_toplevel_get_view(struct control_toplevel *toplevel)
{
	return toplevel->ext_handle ? toplevel->ext_handle->data : NULL;
}

static void
control_toplevel_send_state(struct control_toplevel *toplevel)
{
	struct view *view = control_toplevel_get_view(toplevel);
	if (!view) {
		return;
	}
	labwc_control_toplevel_v1_send_layer(toplevel->resource,
		(uint32_t)view->layer);
	labwc_control_toplevel_v1_send_shaded(toplevel->resource,
		view->shaded);
	labwc_control_toplevel_v1_send_decorations(toplevel->resource,
		(uint32_t)view->ssd_mode);
}

/*
 * Acknowledge a request whose effect is reported by this object, so that a
 * client can tell a value it has been sent from the value it asked for. Sent
 * even when nothing changed, since that is exactly the case a client cannot
 * distinguish otherwise.
 */
static void
control_toplevel_done(struct control_toplevel *toplevel)
{
	labwc_control_toplevel_v1_send_done(toplevel->resource);
}

/* View signals */
static void
handle_view_always_on_top(struct wl_listener *listener, void *data)
{
	struct control_toplevel *toplevel =
		wl_container_of(listener, toplevel, view_always_on_top);
	struct view *view = control_toplevel_get_view(toplevel);
	if (view) {
		labwc_control_toplevel_v1_send_layer(toplevel->resource,
			(uint32_t)view->layer);
	}
}

static void
handle_view_shaded(struct wl_listener *listener, void *data)
{
	struct control_toplevel *toplevel =
		wl_container_of(listener, toplevel, view_shaded);
	struct view *view = control_toplevel_get_view(toplevel);
	if (view) {
		labwc_control_toplevel_v1_send_shaded(toplevel->resource,
			view->shaded);
	}
}

static void
handle_view_decorations(struct wl_listener *listener, void *data)
{
	struct control_toplevel *toplevel =
		wl_container_of(listener, toplevel, view_decorations);
	struct view *view = control_toplevel_get_view(toplevel);
	if (view) {
		labwc_control_toplevel_v1_send_decorations(toplevel->resource,
			(uint32_t)view->ssd_mode);
	}
}

static void
control_toplevel_connect_view(struct control_toplevel *toplevel,
		struct view *view)
{
	toplevel->view_always_on_top.notify = handle_view_always_on_top;
	wl_signal_add(&view->events.always_on_top,
		&toplevel->view_always_on_top);
	toplevel->view_shaded.notify = handle_view_shaded;
	wl_signal_add(&view->events.shaded, &toplevel->view_shaded);
	toplevel->view_decorations.notify = handle_view_decorations;
	wl_signal_add(&view->events.decorations, &toplevel->view_decorations);
}

static void
control_toplevel_disconnect_view(struct control_toplevel *toplevel)
{
	if (!toplevel->ext_handle) {
		return;
	}
	wl_list_remove(&toplevel->view_always_on_top.link);
	wl_list_remove(&toplevel->view_shaded.link);
	wl_list_remove(&toplevel->view_decorations.link);
	wl_list_remove(&toplevel->ext_handle_destroy.link);
	toplevel->ext_handle = NULL;
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
	struct view *view = control_toplevel_get_view(toplevel);
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

static void
toplevel_activate(struct wl_client *client, struct wl_resource *resource)
{
	struct control_toplevel *toplevel = wl_resource_get_user_data(resource);
	struct view *view = control_toplevel_get_view(toplevel);
	if (view) {
		desktop_focus_view(view, /*raise*/ true);
	}
}

static void
toplevel_close(struct wl_client *client, struct wl_resource *resource)
{
	struct control_toplevel *toplevel = wl_resource_get_user_data(resource);
	struct view *view = control_toplevel_get_view(toplevel);
	if (view) {
		view_close(view);
	}
}

static void
toplevel_set_maximized(struct wl_client *client, struct wl_resource *resource)
{
	struct control_toplevel *toplevel = wl_resource_get_user_data(resource);
	struct view *view = control_toplevel_get_view(toplevel);
	if (view) {
		view_maximize(view, VIEW_AXIS_BOTH);
	}
}

static void
toplevel_unset_maximized(struct wl_client *client, struct wl_resource *resource)
{
	struct control_toplevel *toplevel = wl_resource_get_user_data(resource);
	struct view *view = control_toplevel_get_view(toplevel);
	if (view) {
		view_maximize(view, VIEW_AXIS_NONE);
	}
}

static void
toplevel_set_minimized(struct wl_client *client, struct wl_resource *resource)
{
	struct control_toplevel *toplevel = wl_resource_get_user_data(resource);
	struct view *view = control_toplevel_get_view(toplevel);
	if (view) {
		view_minimize(view, true);
	}
}

static void
toplevel_unset_minimized(struct wl_client *client, struct wl_resource *resource)
{
	struct control_toplevel *toplevel = wl_resource_get_user_data(resource);
	struct view *view = control_toplevel_get_view(toplevel);
	if (view) {
		view_minimize(view, false);
	}
}

static void
toplevel_set_fullscreen(struct wl_client *client, struct wl_resource *resource)
{
	struct control_toplevel *toplevel = wl_resource_get_user_data(resource);
	struct view *view = control_toplevel_get_view(toplevel);
	if (view) {
		view_set_fullscreen(view, true);
	}
}

static void
toplevel_unset_fullscreen(struct wl_client *client, struct wl_resource *resource)
{
	struct control_toplevel *toplevel = wl_resource_get_user_data(resource);
	struct view *view = control_toplevel_get_view(toplevel);
	if (view) {
		view_set_fullscreen(view, false);
	}
}

static void
toplevel_set_sticky(struct wl_client *client, struct wl_resource *resource)
{
	struct control_toplevel *toplevel = wl_resource_get_user_data(resource);
	struct view *view = control_toplevel_get_view(toplevel);
	if (view) {
		view_set_visible_on_all_workspaces(view, true);
	}
}

static void
toplevel_unset_sticky(struct wl_client *client, struct wl_resource *resource)
{
	struct control_toplevel *toplevel = wl_resource_get_user_data(resource);
	struct view *view = control_toplevel_get_view(toplevel);
	if (view) {
		view_set_visible_on_all_workspaces(view, false);
	}
}

static void
toplevel_center(struct wl_client *client, struct wl_resource *resource)
{
	struct control_toplevel *toplevel = wl_resource_get_user_data(resource);
	struct view *view = control_toplevel_get_view(toplevel);
	if (view) {
		/* A NULL reference box means the usable area of the output */
		view_center(view, NULL);
	}
}

static void
toplevel_move_by(struct wl_client *client, struct wl_resource *resource,
		int32_t dx, int32_t dy)
{
	struct control_toplevel *toplevel = wl_resource_get_user_data(resource);
	struct view *view = control_toplevel_get_view(toplevel);
	if (view) {
		view_move_relative(view, dx, dy);
	}
}

static void
toplevel_resize_by(struct wl_client *client, struct wl_resource *resource,
		int32_t left, int32_t right, int32_t top, int32_t bottom)
{
	struct control_toplevel *toplevel = wl_resource_get_user_data(resource);
	struct view *view = control_toplevel_get_view(toplevel);
	if (view) {
		view_resize_relative(view, left, right, top, bottom);
	}
}

/*
 * The protocol names directions the way the MoveToEdge action documents them,
 * whereas lab_edge names them after the edges they refer to.
 */
static enum lab_edge
edge_from_protocol(uint32_t edge)
{
	switch (edge) {
	case LABWC_CONTROL_TOPLEVEL_V1_EDGE_LEFT:
		return LAB_EDGE_LEFT;
	case LABWC_CONTROL_TOPLEVEL_V1_EDGE_RIGHT:
		return LAB_EDGE_RIGHT;
	case LABWC_CONTROL_TOPLEVEL_V1_EDGE_UP:
		return LAB_EDGE_TOP;
	case LABWC_CONTROL_TOPLEVEL_V1_EDGE_DOWN:
		return LAB_EDGE_BOTTOM;
	default:
		return LAB_EDGE_NONE;
	}
}

static void
toplevel_snap_to_edge(struct wl_client *client, struct wl_resource *resource,
		uint32_t edge, int32_t snap_to_windows)
{
	struct control_toplevel *toplevel = wl_resource_get_user_data(resource);
	struct view *view = control_toplevel_get_view(toplevel);
	if (view) {
		view_move_to_edge(view, edge_from_protocol(edge),
			snap_to_windows != 0);
	}
}

static void
toplevel_grow_to_edge(struct wl_client *client, struct wl_resource *resource,
		uint32_t edge)
{
	struct control_toplevel *toplevel = wl_resource_get_user_data(resource);
	struct view *view = control_toplevel_get_view(toplevel);
	if (view) {
		view_grow_to_edge(view, edge_from_protocol(edge));
	}
}

static void
toplevel_shrink_to_edge(struct wl_client *client, struct wl_resource *resource,
		uint32_t edge)
{
	struct control_toplevel *toplevel = wl_resource_get_user_data(resource);
	struct view *view = control_toplevel_get_view(toplevel);
	if (view) {
		view_shrink_to_edge(view, edge_from_protocol(edge));
	}
}

static void
toplevel_set_layer(struct wl_client *client, struct wl_resource *resource,
		uint32_t layer)
{
	struct control_toplevel *toplevel = wl_resource_get_user_data(resource);
	struct view *view = control_toplevel_get_view(toplevel);
	if (view && layer <= LABWC_CONTROL_TOPLEVEL_V1_LAYER_ALWAYS_ON_BOTTOM) {
		view_set_layer(view, (enum view_layer)layer);
	}
	control_toplevel_done(toplevel);
}

static void
toplevel_set_shaded(struct wl_client *client, struct wl_resource *resource)
{
	struct control_toplevel *toplevel = wl_resource_get_user_data(resource);
	struct view *view = control_toplevel_get_view(toplevel);
	if (view) {
		view_set_shade(view, true);
	}
	control_toplevel_done(toplevel);
}

static void
toplevel_unset_shaded(struct wl_client *client, struct wl_resource *resource)
{
	struct control_toplevel *toplevel = wl_resource_get_user_data(resource);
	struct view *view = control_toplevel_get_view(toplevel);
	if (view) {
		view_set_shade(view, false);
	}
	control_toplevel_done(toplevel);
}

static void
toplevel_set_decorations(struct wl_client *client, struct wl_resource *resource,
		uint32_t mode)
{
	struct control_toplevel *toplevel = wl_resource_get_user_data(resource);
	struct view *view = control_toplevel_get_view(toplevel);
	if (view && mode <= LABWC_CONTROL_TOPLEVEL_V1_DECORATION_MODE_FULL) {
		view_set_decorations(view, (enum lab_ssd_mode)mode,
			/* force_ssd */ false);
	}
	control_toplevel_done(toplevel);
}

static const struct labwc_control_toplevel_v1_interface toplevel_impl = {
	.destroy = handle_destroy,
	.move_to = toplevel_move_to,
	.resize_to = toplevel_resize_to,
	.move_resize_to = toplevel_move_resize_to,
	.activate = toplevel_activate,
	.close = toplevel_close,
	.set_maximized = toplevel_set_maximized,
	.unset_maximized = toplevel_unset_maximized,
	.set_minimized = toplevel_set_minimized,
	.unset_minimized = toplevel_unset_minimized,
	.set_fullscreen = toplevel_set_fullscreen,
	.unset_fullscreen = toplevel_unset_fullscreen,
	.set_sticky = toplevel_set_sticky,
	.unset_sticky = toplevel_unset_sticky,
	.center = toplevel_center,
	.move_by = toplevel_move_by,
	.resize_by = toplevel_resize_by,
	.snap_to_edge = toplevel_snap_to_edge,
	.grow_to_edge = toplevel_grow_to_edge,
	.shrink_to_edge = toplevel_shrink_to_edge,
	.set_layer = toplevel_set_layer,
	.set_shaded = toplevel_set_shaded,
	.unset_shaded = toplevel_unset_shaded,
	.set_decorations = toplevel_set_decorations,
};

static void
handle_ext_handle_destroy(struct wl_listener *listener, void *data)
{
	struct control_toplevel *toplevel =
		wl_container_of(listener, toplevel, ext_handle_destroy);

	control_toplevel_disconnect_view(toplevel);
}

static void
toplevel_resource_destroy(struct wl_resource *resource)
{
	struct control_toplevel *toplevel = wl_resource_get_user_data(resource);
	if (!toplevel) {
		return;
	}

	control_toplevel_disconnect_view(toplevel);
	free(toplevel);
}

static void
control_get_toplevel(struct wl_client *client, struct wl_resource *resource,
		uint32_t id, struct wl_resource *foreign_toplevel)
{
	struct wlr_ext_foreign_toplevel_handle_v1 *ext_handle =
		wlr_ext_foreign_toplevel_handle_v1_from_resource(foreign_toplevel);
	/* ext-foreign.c stores the view in handle->data */
	struct view *view = ext_handle ? ext_handle->data : NULL;

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
		control_toplevel_connect_view(toplevel, view);
	}

	wl_resource_set_implementation(toplevel_resource, &toplevel_impl,
		toplevel, toplevel_resource_destroy);

	/* All initial properties are sent immediately, as required */
	control_toplevel_send_state(toplevel);
	control_toplevel_done(toplevel);
}

/*
 * Focus the next or previous toplevel without opening the window switcher
 * overlay, which is what the NextWindow keybinding does interactively.
 * Filters default to the configured window switcher settings.
 */
static void
control_cycle(enum lab_cycle_dir direction)
{
	struct cycle_filter filter = {
		.workspace = rc.window_switcher.workspace_filter,
		.output = CYCLE_OUTPUT_ALL,
		.app_id = CYCLE_APP_ID_ALL,
	};
	cycle_immediate(direction, filter);
}

static void
control_cycle_next(struct wl_client *client, struct wl_resource *resource)
{
	control_cycle(LAB_CYCLE_DIR_FORWARD);
}

static void
control_cycle_prev(struct wl_client *client, struct wl_resource *resource)
{
	control_cycle(LAB_CYCLE_DIR_BACKWARD);
}

static const struct labwc_control_v1_interface control_impl = {
	.destroy = handle_destroy,
	.get_toplevel = control_get_toplevel,
	.cycle_next = control_cycle_next,
	.cycle_prev = control_cycle_prev,
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
