// SPDX-License-Identifier: GPL-2.0-only
#include "foreign-toplevel/cosmic.h"

#include <assert.h>
#include <stdlib.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>

#include "common/macros.h"
#include "common/mem.h"
#include "cosmic-toplevel-info-unstable-v1-protocol.h"
#include "foreign-toplevel/foreign.h"
#include "labwc.h"
#include "output.h"
#include "view.h"

/*
 * We implement version 2 of the protocol, which is the version that
 * introduced the geometry event. Clients binding version 2 obtain their
 * handles with get_cosmic_toplevel() and rely on
 * ext-foreign-toplevel-list-v1 for the title/app_id/closed/done events, so
 * the only events we have to send on a toplevel handle are
 * output_enter/output_leave, state and geometry.
 *
 * TODO: version 3 replaces the workspace_enter/workspace_leave events with
 * their ext-workspace equivalents, which would require us to implement
 * ext-workspace-v1 as well.
 */
#define COSMIC_TOPLEVEL_INFO_VERSION 2

struct cosmic_toplevel_handle {
	struct wl_resource *resource;

	/*
	 * NULL once the view has gone away. The handle object itself is
	 * owned by the client and stays alive until the client destroys it,
	 * which it does in response to ext_foreign_toplevel_handle_v1.closed.
	 */
	struct cosmic_toplevel *toplevel;

	struct wl_list link;    /* cosmic_toplevel_manager.handles */
	struct wl_list outputs; /* struct cosmic_handle_output.link */
};

/* A wl_output resource belonging to a client and the output it maps to */
struct cosmic_handle_output {
	struct output *output;
	struct wl_resource *resource;
	bool keep;
	struct wl_list link; /* cosmic_toplevel_handle.outputs */
};

struct cosmic_toplevel_manager {
	struct wl_list resources; /* zcosmic_toplevel_info_v1 resources */
	struct wl_list handles;   /* all struct cosmic_toplevel_handle */
};

static struct cosmic_toplevel_manager manager;

/*
 * The protocol reports geometry relative to an output and only for outputs
 * which the toplevel actually intersects. Returns false if the view is not
 * visible on the given output.
 */
static bool
view_geometry_on_output(struct view *view, struct output *output,
		struct wlr_box *geometry)
{
	struct wlr_box output_box;
	wlr_output_layout_get_box(server.output_layout, output->wlr_output,
		&output_box);
	if (wlr_box_empty(&output_box)) {
		return false;
	}

	struct wlr_box intersection;
	if (!wlr_box_intersection(&intersection, &view->current, &output_box)) {
		return false;
	}

	*geometry = (struct wlr_box){
		.x = view->current.x - output_box.x,
		.y = view->current.y - output_box.y,
		.width = view->current.width,
		.height = view->current.height,
	};
	return true;
}

static void
handle_send_state(struct cosmic_toplevel_handle *handle)
{
	struct view *view = handle->toplevel->view;

	/*
	 * Build the state array on the stack. wl_array_release() is never
	 * called on it, so nothing is ever freed.
	 */
	uint32_t states[5];
	size_t len = 0;

	if (view->maximized != VIEW_AXIS_NONE) {
		states[len++] = ZCOSMIC_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED;
	}
	if (view->fullscreen) {
		states[len++] = ZCOSMIC_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN;
	}
	if (view->minimized) {
		states[len++] = ZCOSMIC_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED;
	}
	if (handle->toplevel->activated) {
		states[len++] = ZCOSMIC_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED;
	}
	if (view->visible_on_all_workspaces) {
		states[len++] = ZCOSMIC_TOPLEVEL_HANDLE_V1_STATE_STICKY;
	}

	struct wl_array state = {
		.size = len * sizeof(states[0]),
		.alloc = len * sizeof(states[0]),
		.data = states,
	};
	zcosmic_toplevel_handle_v1_send_state(handle->resource, &state);
}

static void
handle_send_geometry(struct cosmic_toplevel_handle *handle)
{
	struct view *view = handle->toplevel->view;
	struct cosmic_handle_output *ho;
	struct wlr_box geometry;

	wl_list_for_each(ho, &handle->outputs, link) {
		if (!view_geometry_on_output(view, ho->output, &geometry)) {
			continue;
		}
		zcosmic_toplevel_handle_v1_send_geometry(handle->resource,
			ho->resource, geometry.x, geometry.y,
			geometry.width, geometry.height);
	}
}

static void
handle_sync_outputs(struct cosmic_toplevel_handle *handle)
{
	struct view *view = handle->toplevel->view;
	struct wl_client *client = wl_resource_get_client(handle->resource);
	struct cosmic_handle_output *ho;
	struct cosmic_handle_output *tmp;
	struct wl_resource *resource;
	struct output *output;

	wl_list_for_each(ho, &handle->outputs, link) {
		ho->keep = false;
	}

	wl_list_for_each(output, &server.outputs, link) {
		if (!view_on_output(view, output)) {
			continue;
		}
		/*
		 * wlr_output tracks the wl_output resources it has handed
		 * out. Note that a client may have bound the same output more
		 * than once.
		 */
		wl_resource_for_each(resource, &output->wlr_output->resources) {
			if (wl_resource_get_client(resource) != client) {
				continue;
			}

			struct cosmic_handle_output *existing = NULL;
			wl_list_for_each(ho, &handle->outputs, link) {
				if (ho->output == output
						&& ho->resource == resource) {
					existing = ho;
					break;
				}
			}
			if (existing) {
				existing->keep = true;
				continue;
			}

			struct wlr_box geometry;
			ho = znew(*ho);
			ho->output = output;
			ho->resource = resource;
			ho->keep = true;
			wl_list_insert(&handle->outputs, &ho->link);

			zcosmic_toplevel_handle_v1_send_output_enter(
				handle->resource, resource);
			if (view_geometry_on_output(view, output, &geometry)) {
				zcosmic_toplevel_handle_v1_send_geometry(
					handle->resource, resource,
					geometry.x, geometry.y,
					geometry.width, geometry.height);
			}
		}
	}

	wl_list_for_each_safe(ho, tmp, &handle->outputs, link) {
		if (ho->keep) {
			continue;
		}
		zcosmic_toplevel_handle_v1_send_output_leave(handle->resource,
			ho->resource);
		wl_list_remove(&ho->link);
		free(ho);
	}
}

/*
 * Mark the end of a batch of changes for every client which has a handle on
 * this toplevel. Extra done events are harmless.
 */
static void
toplevel_send_done(struct cosmic_toplevel *toplevel)
{
	struct cosmic_toplevel_handle *handle;
	wl_list_for_each(handle, &manager.handles, link) {
		if (handle->toplevel != toplevel) {
			continue;
		}
		struct wl_client *client =
			wl_resource_get_client(handle->resource);
		struct wl_resource *resource;
		wl_resource_for_each(resource, &manager.resources) {
			if (wl_resource_get_client(resource) == client) {
				zcosmic_toplevel_info_v1_send_done(resource);
			}
		}
	}
}

static void
toplevel_sync_outputs(struct cosmic_toplevel *toplevel)
{
	struct cosmic_toplevel_handle *handle;
	wl_list_for_each(handle, &manager.handles, link) {
		if (handle->toplevel == toplevel) {
			handle_sync_outputs(handle);
		}
	}
	toplevel_send_done(toplevel);
}

static void
toplevel_sync_geometry(struct cosmic_toplevel *toplevel)
{
	struct cosmic_toplevel_handle *handle;
	wl_list_for_each(handle, &manager.handles, link) {
		if (handle->toplevel == toplevel) {
			handle_send_geometry(handle);
		}
	}
	toplevel_send_done(toplevel);
}

static void
toplevel_sync_state(struct cosmic_toplevel *toplevel)
{
	struct cosmic_toplevel_handle *handle;
	wl_list_for_each(handle, &manager.handles, link) {
		if (handle->toplevel == toplevel) {
			handle_send_state(handle);
		}
	}
	toplevel_send_done(toplevel);
}

/* Compositor signals */
static void
handle_new_geometry(struct wl_listener *listener, void *data)
{
	struct cosmic_toplevel *toplevel =
		wl_container_of(listener, toplevel, on_view.new_geometry);

	if (toplevel->has_last_geometry
			&& wlr_box_equal(&toplevel->last_geometry,
				&toplevel->view->current)) {
		return;
	}
	toplevel->last_geometry = toplevel->view->current;
	toplevel->has_last_geometry = true;

	toplevel_sync_geometry(toplevel);
}

static void
handle_new_outputs(struct wl_listener *listener, void *data)
{
	struct cosmic_toplevel *toplevel =
		wl_container_of(listener, toplevel, on_view.new_outputs);
	toplevel_sync_outputs(toplevel);
}

static void
handle_new_omnipresent(struct wl_listener *listener, void *data)
{
	struct cosmic_toplevel *toplevel =
		wl_container_of(listener, toplevel, on_view.new_omnipresent);
	toplevel_sync_state(toplevel);
}

static void
handle_maximized(struct wl_listener *listener, void *data)
{
	struct cosmic_toplevel *toplevel =
		wl_container_of(listener, toplevel, on_view.maximized);
	toplevel_sync_state(toplevel);
}

static void
handle_minimized(struct wl_listener *listener, void *data)
{
	struct cosmic_toplevel *toplevel =
		wl_container_of(listener, toplevel, on_view.minimized);
	toplevel_sync_state(toplevel);
}

static void
handle_fullscreened(struct wl_listener *listener, void *data)
{
	struct cosmic_toplevel *toplevel =
		wl_container_of(listener, toplevel, on_view.fullscreened);
	toplevel_sync_state(toplevel);
}

static void
handle_activated(struct wl_listener *listener, void *data)
{
	struct cosmic_toplevel *toplevel =
		wl_container_of(listener, toplevel, on_view.activated);

	toplevel->activated = *(bool *)data;
	toplevel_sync_state(toplevel);
}

/* Client requests */
static void
handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct zcosmic_toplevel_handle_v1_interface handle_impl = {
	.destroy = handle_destroy,
};

static void
handle_resource_destroy(struct wl_resource *resource)
{
	struct cosmic_toplevel_handle *handle =
		wl_resource_get_user_data(resource);
	if (!handle) {
		return;
	}

	wl_list_remove(&handle->link);

	struct cosmic_handle_output *ho, *tmp;
	wl_list_for_each_safe(ho, tmp, &handle->outputs, link) {
		wl_list_remove(&ho->link);
		free(ho);
	}

	free(handle);
}

static void
info_get_cosmic_toplevel(struct wl_client *client, struct wl_resource *resource,
		uint32_t id, struct wl_resource *foreign_toplevel)
{
	/*
	 * ext-foreign.c stores the view in handle->data so that the
	 * ext-image-copy-capture implementation can find it.
	 */
	struct wlr_ext_foreign_toplevel_handle_v1 *ext_handle =
		wlr_ext_foreign_toplevel_handle_v1_from_resource(foreign_toplevel);
	struct view *view = ext_handle ? ext_handle->data : NULL;
	struct cosmic_toplevel *toplevel = view && view->foreign_toplevel
		? foreign_toplevel_get_cosmic(view->foreign_toplevel) : NULL;

	struct wl_resource *toplevel_resource = wl_resource_create(client,
		&zcosmic_toplevel_handle_v1_interface,
		wl_resource_get_version(resource), id);
	if (!toplevel_resource) {
		wl_client_post_no_memory(client);
		return;
	}

	if (!toplevel) {
		/*
		 * The toplevel is already gone (or was never exposed to
		 * foreign toplevel clients). Hand out an inert object so that
		 * the client's book keeping stays sane.
		 */
		wl_resource_set_implementation(toplevel_resource, &handle_impl,
			NULL, NULL);
		return;
	}

	struct cosmic_toplevel_handle *handle = znew(*handle);
	handle->resource = toplevel_resource;
	handle->toplevel = toplevel;
	wl_list_init(&handle->outputs);
	wl_list_insert(&manager.handles, &handle->link);

	wl_resource_set_implementation(toplevel_resource, &handle_impl, handle,
		handle_resource_destroy);

	/* All initial properties are sent immediately, as required */
	handle_send_state(handle);
	handle_sync_outputs(handle);
	toplevel_send_done(toplevel);
}

static void
info_stop(struct wl_client *client, struct wl_resource *resource)
{
	/*
	 * Only meaningful for version 1 clients and deprecated since
	 * version 2, which is all we support.
	 */
}

static const struct zcosmic_toplevel_info_v1_interface info_impl = {
	.stop = info_stop,
	.get_cosmic_toplevel = info_get_cosmic_toplevel,
};

static void
info_resource_destroy(struct wl_resource *resource)
{
	wl_list_remove(wl_resource_get_link(resource));
}

static void
info_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&zcosmic_toplevel_info_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &info_impl, NULL,
		info_resource_destroy);
	wl_list_insert(&manager.resources, wl_resource_get_link(resource));
}

/* Internal API */
void
cosmic_toplevel_create_global(struct wl_display *display)
{
	wl_list_init(&manager.resources);
	wl_list_init(&manager.handles);

	struct wl_global *global = wl_global_create(display,
		&zcosmic_toplevel_info_v1_interface,
		COSMIC_TOPLEVEL_INFO_VERSION, NULL, info_bind);
	if (!global) {
		wlr_log(WLR_ERROR,
			"unable to create zcosmic_toplevel_info_v1 global");
	}
}

void
cosmic_toplevel_init(struct cosmic_toplevel *toplevel, struct view *view)
{
	toplevel->view = view;
	toplevel->last_geometry = view->current;
	toplevel->has_last_geometry = false;
	toplevel->activated = (view == server.active_view);

	CONNECT_SIGNAL(view, &toplevel->on_view, new_geometry);
	CONNECT_SIGNAL(view, &toplevel->on_view, new_outputs);
	CONNECT_SIGNAL(view, &toplevel->on_view, new_omnipresent);
	CONNECT_SIGNAL(view, &toplevel->on_view, maximized);
	CONNECT_SIGNAL(view, &toplevel->on_view, minimized);
	CONNECT_SIGNAL(view, &toplevel->on_view, fullscreened);
	CONNECT_SIGNAL(view, &toplevel->on_view, activated);
}

void
cosmic_toplevel_finish(struct cosmic_toplevel *toplevel)
{
	wl_list_remove(&toplevel->on_view.new_geometry.link);
	wl_list_remove(&toplevel->on_view.new_outputs.link);
	wl_list_remove(&toplevel->on_view.new_omnipresent.link);
	wl_list_remove(&toplevel->on_view.maximized.link);
	wl_list_remove(&toplevel->on_view.minimized.link);
	wl_list_remove(&toplevel->on_view.fullscreened.link);
	wl_list_remove(&toplevel->on_view.activated.link);

	/*
	 * The handles are owned by their clients, so we only disassociate
	 * them to make sure no further events are sent. Clients destroy
	 * them once they see ext_foreign_toplevel_handle_v1.closed.
	 */
	struct cosmic_toplevel_handle *handle;
	wl_list_for_each(handle, &manager.handles, link) {
		if (handle->toplevel == toplevel) {
			handle->toplevel = NULL;
		}
	}
}
