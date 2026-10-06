// SPDX-License-Identifier: GPL-2.0-only
/*
 * labwcctl - query and control toplevels
 *
 * 'list' prints a JSON snapshot of every toplevel: its stable identifier, its
 * state, and its geometry. The toplevel list comes from
 * ext-foreign-toplevel-list-v1, geometry and state come from
 * zcosmic-toplevel-info-v1.
 *
 * The other commands act on matching toplevels through labwc_control_v1:
 * 'move', 'resize' and 'move-resize' set the absolute geometry, 'focus'
 * activates, and 'close' asks the client to close. That protocol is the only
 * way to do any of this: the standard foreign-toplevel protocols allow a
 * request to activate, close, maximize, minimize and fullscreen a toplevel,
 * but not to move or resize it, and their handles cannot be correlated with
 * the stable identifier this tool selects on.
 *
 * Note that zcosmic-toplevel-info-v1 reports geometry relative to the output
 * a toplevel is on. This tool resolves each output's position from
 * wl_output.geometry and reports compositor-global coordinates, which is also
 * the coordinate space labwc_control_v1 expects.
 *
 * Examples:
 *   labwcctl list
 *   labwcctl list --watch
 *   labwcctl focus 'app_id:foot'
 *   labwcctl move 'app_id:foot' 100 100
 *   labwcctl move-resize 1c6ecfaa9a06140a4d3d54e54d4d8e06 0 0 640 480
 *   labwcctl close '*'
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-client-protocol.h>

#include "cosmic-toplevel-info-unstable-v1-client-protocol.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "labwc-control-v1-client-protocol.h"

/*
 * Version 2 is where the geometry event of zcosmic_toplevel_info_v1 was
 * introduced. Clients binding version 2 get their handles from
 * get_cosmic_toplevel() and rely on ext-foreign-toplevel-list-v1 for
 * title/app_id/closed.
 */
#define COSMIC_TOPLEVEL_INFO_VERSION 2

#define MAX_GEOMETRIES 16
#define MAX_STATES 8
#define MAX_ROUNDTRIPS 10
/* How long to wait for the compositor and the client to agree on a change */
#define CONTROL_TIMEOUT_MS 500
/* How long to keep listening after the requested change has been observed */
#define SETTLE_QUIET_MS 60

struct output {
	struct wl_output *wl_output;
	int32_t x, y;
	int32_t width, height;
	char *name;
	char *model;
	struct wl_list link;
};

struct geometry {
	struct wl_output *wl_output;
	int32_t x, y, width, height;
};

struct toplevel {
	struct ext_foreign_toplevel_handle_v1 *foreign;
	struct zcosmic_toplevel_handle_v1 *cosmic;
	struct labwc_control_toplevel_v1 *control;
	char *title;
	char *app_id;
	char *identifier;

	struct geometry geometries[MAX_GEOMETRIES];
	size_t nr_geometries;

	uint32_t states[MAX_STATES];
	size_t nr_states;

	/*
	 * Properties which zcosmic_toplevel_info_v1 does not report and
	 * labwc_control_v1 does. have_control_state is false until the first
	 * one arrives, and stays false for a compositor which does not offer
	 * labwc_control_v1 at all.
	 */
	int32_t layer;
	int32_t shaded;
	int32_t decorations;
	bool have_control_state;

	bool selected;
	struct wl_list link;
};

enum selector_type {
	SELECTOR_IDENTIFIER,
	SELECTOR_APP_ID,
	SELECTOR_TITLE,
	SELECTOR_ALL,
};

struct selector {
	enum selector_type type;
	const char *value;
};

enum control_command {
	CONTROL_MOVE,
	CONTROL_RESIZE,
	CONTROL_MOVE_RESIZE,
	CONTROL_MOVE_BY,
	CONTROL_RESIZE_BY,
	CONTROL_CENTER,
	CONTROL_ACTIVATE,
	CONTROL_CLOSE,
	CONTROL_MAXIMIZE,
	CONTROL_MINIMIZE,
	CONTROL_FULLSCREEN,
	CONTROL_STICKY,
	CONTROL_SHADE,
	CONTROL_SNAP_TO_EDGE,
	CONTROL_GROW_TO_EDGE,
	CONTROL_SHRINK_TO_EDGE,
	CONTROL_LAYER,
	CONTROL_DECORATIONS,
};

static struct wl_display *display;
static struct wl_list outputs;
static struct wl_list toplevels;
static struct zcosmic_toplevel_info_v1 *toplevel_info;
static struct labwc_control_v1 *control;
static unsigned int pending_handles;
static unsigned int pending_control;
static unsigned int geometry_events;
static unsigned int geometry_target;
static unsigned int state_events;
static unsigned int state_target;
static unsigned int done_events;
static unsigned int done_target;
static uint32_t wait_state;
static bool wait_state_on;
static bool updated;

/*
 * wl_output.geometry make/model and wl_output.name may be NULL, whereas
 * strdup() requires a non-NULL source.
 */
static void
replace_string(char **dst, const char *src)
{
	if (!src) {
		return;
	}
	char *copy = strdup(src);
	if (!copy) {
		perror("strdup");
		exit(EXIT_FAILURE);
	}
	free(*dst);
	*dst = copy;
}

/* The client does not link src/common/, so provide the few helpers we need */
static void *
zalloc(size_t size)
{
	void *ptr = calloc(1, size);
	if (!ptr) {
		perror("calloc");
		exit(EXIT_FAILURE);
	}
	return ptr;
}

static int64_t
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ------------------------------- output ------------------------------- */

static void
output_geometry(void *data, struct wl_output *wl_output, int32_t x, int32_t y,
		int32_t physical_width, int32_t physical_height, int32_t subpixel,
		const char *make, const char *model, int32_t transform)
{
	struct output *output = data;
	output->x = x;
	output->y = y;
	replace_string(&output->model, model);
}

static void
output_mode(void *data, struct wl_output *wl_output, uint32_t flags,
		int32_t width, int32_t height, int32_t refresh)
{
	struct output *output = data;
	if (flags & WL_OUTPUT_MODE_CURRENT) {
		output->width = width;
		output->height = height;
	}
}

static void
output_done(void *data, struct wl_output *wl_output)
{
}

static void
output_scale(void *data, struct wl_output *wl_output, int32_t factor)
{
}

static void
output_name(void *data, struct wl_output *wl_output, const char *name)
{
	struct output *output = data;
	replace_string(&output->name, name);
}

static void
output_description(void *data, struct wl_output *wl_output,
		const char *description)
{
}

static const struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
	.name = output_name,
	.description = output_description,
};

static struct output *
output_from_wl_output(struct wl_output *wl_output)
{
	struct output *output;
	wl_list_for_each(output, &outputs, link) {
		if (output->wl_output == wl_output) {
			return output;
		}
	}
	return NULL;
}

/*
 * The requests of labwc_control_v1 for a toplevel are only sent once
 * zcosmic_toplevel_info_v1 has told us the toplevel exists, and both report
 * their initial state asynchronously.
 */
static bool
initial_state_pending(void)
{
	return pending_handles || pending_control;
}

/* -------------------------- cosmic toplevels -------------------------- */

static void
cosmic_geometry(void *data, struct zcosmic_toplevel_handle_v1 *handle,
		struct wl_output *wl_output, int32_t x, int32_t y,
		int32_t width, int32_t height)
{
	struct toplevel *toplevel = data;
	size_t i;

	geometry_events++;

	for (i = 0; i < toplevel->nr_geometries; i++) {
		struct geometry *geometry = &toplevel->geometries[i];
		if (geometry->wl_output == wl_output) {
			geometry->x = x;
			geometry->y = y;
			geometry->width = width;
			geometry->height = height;
			return;
		}
	}

	/* The compositor sends geometry for every output it enters */
	if (toplevel->nr_geometries == MAX_GEOMETRIES) {
		return;
	}
	toplevel->geometries[toplevel->nr_geometries++] = (struct geometry){
		.wl_output = wl_output,
		.x = x,
		.y = y,
		.width = width,
		.height = height,
	};
}

static void
cosmic_output_enter(void *data, struct zcosmic_toplevel_handle_v1 *handle,
		struct wl_output *wl_output)
{
}

static void
cosmic_output_leave(void *data, struct zcosmic_toplevel_handle_v1 *handle,
		struct wl_output *wl_output)
{
	struct toplevel *toplevel = data;
	size_t i;

	for (i = 0; i < toplevel->nr_geometries; i++) {
		if (toplevel->geometries[i].wl_output != wl_output) {
			continue;
		}
		memmove(&toplevel->geometries[i], &toplevel->geometries[i + 1],
			(toplevel->nr_geometries - i - 1) *
				sizeof(toplevel->geometries[0]));
		toplevel->nr_geometries--;
		return;
	}
}

static void
cosmic_state(void *data, struct zcosmic_toplevel_handle_v1 *handle,
		struct wl_array *state)
{
	struct toplevel *toplevel = data;
	size_t nr = state->size / sizeof(uint32_t);

	state_events++;
	toplevel->nr_states = nr < MAX_STATES ? nr : MAX_STATES;
	memcpy(toplevel->states, state->data,
		toplevel->nr_states * sizeof(toplevel->states[0]));
}

static void
cosmic_done(void *data, struct zcosmic_toplevel_handle_v1 *handle)
{
	/* Not emitted for clients binding version 2 */
}

static void
cosmic_closed(void *data, struct zcosmic_toplevel_handle_v1 *handle)
{
	/* Not emitted for clients binding version 2 */
}

static void
cosmic_workspace_enter(void *data, struct zcosmic_toplevel_handle_v1 *handle,
		struct zcosmic_workspace_handle_v1 *workspace)
{
}

static void
cosmic_workspace_leave(void *data, struct zcosmic_toplevel_handle_v1 *handle,
		struct zcosmic_workspace_handle_v1 *workspace)
{
}

static void
cosmic_ext_workspace_enter(void *data, struct zcosmic_toplevel_handle_v1 *handle,
		struct ext_workspace_handle_v1 *workspace)
{
}

static void
cosmic_ext_workspace_leave(void *data, struct zcosmic_toplevel_handle_v1 *handle,
		struct ext_workspace_handle_v1 *workspace)
{
}

static const struct zcosmic_toplevel_handle_v1_listener cosmic_handle_listener = {
	.geometry = cosmic_geometry,
	.output_enter = cosmic_output_enter,
	.output_leave = cosmic_output_leave,
	.state = cosmic_state,
	.done = cosmic_done,
	.closed = cosmic_closed,
	.workspace_enter = cosmic_workspace_enter,
	.workspace_leave = cosmic_workspace_leave,
	.ext_workspace_enter = cosmic_ext_workspace_enter,
	.ext_workspace_leave = cosmic_ext_workspace_leave,
};

/* ----------------------- labwc control toplevels ----------------------- */

static void
control_layer(void *data, struct labwc_control_toplevel_v1 *handle,
		uint32_t layer)
{
	struct toplevel *toplevel = data;
	toplevel->layer = (int32_t)layer;
	toplevel->have_control_state = true;
	updated = true;
}

static void
control_shaded(void *data, struct labwc_control_toplevel_v1 *handle,
		int32_t shaded)
{
	struct toplevel *toplevel = data;
	toplevel->shaded = shaded;
	toplevel->have_control_state = true;
	updated = true;
}

static void
control_decorations(void *data, struct labwc_control_toplevel_v1 *handle,
		uint32_t mode)
{
	struct toplevel *toplevel = data;
	toplevel->decorations = (int32_t)mode;
	toplevel->have_control_state = true;
	updated = true;
}

static void
control_done(void *data, struct labwc_control_toplevel_v1 *handle)
{
	done_events++;
	if (pending_control) {
		pending_control--;
	}
	updated = true;
}

static const struct labwc_control_toplevel_v1_listener control_listener = {
	.layer = control_layer,
	.shaded = control_shaded,
	.decorations = control_decorations,
	.done = control_done,
};

/* --------------------------- toplevel list ---------------------------- */

static void
foreign_title(void *data, struct ext_foreign_toplevel_handle_v1 *handle,
		const char *title)
{
	struct toplevel *toplevel = data;
	replace_string(&toplevel->title, title);
	updated = true;
}

static void
foreign_app_id(void *data, struct ext_foreign_toplevel_handle_v1 *handle,
		const char *app_id)
{
	struct toplevel *toplevel = data;
	replace_string(&toplevel->app_id, app_id);
	updated = true;
}

static void
foreign_identifier(void *data, struct ext_foreign_toplevel_handle_v1 *handle,
		const char *identifier)
{
	struct toplevel *toplevel = data;
	replace_string(&toplevel->identifier, identifier);
	updated = true;
}

static void
foreign_done(void *data, struct ext_foreign_toplevel_handle_v1 *handle)
{
	updated = true;
}

static void
toplevel_free(struct toplevel *toplevel)
{
	wl_list_remove(&toplevel->link);
	free(toplevel->title);
	free(toplevel->app_id);
	free(toplevel->identifier);
	free(toplevel);
}

static void
foreign_closed(void *data, struct ext_foreign_toplevel_handle_v1 *handle)
{
	struct toplevel *toplevel = data;

	/*
	 * All of our proxies for this toplevel are now useless, so tear the
	 * whole thing down.
	 */
	if (toplevel->control) {
		labwc_control_toplevel_v1_destroy(toplevel->control);
	}
	if (toplevel->cosmic) {
		zcosmic_toplevel_handle_v1_destroy(toplevel->cosmic);
	}
	ext_foreign_toplevel_handle_v1_destroy(handle);
	toplevel_free(toplevel);
	updated = true;
}

static const struct ext_foreign_toplevel_handle_v1_listener foreign_handle_listener = {
	.title = foreign_title,
	.app_id = foreign_app_id,
	.identifier = foreign_identifier,
	.done = foreign_done,
	.closed = foreign_closed,
};

static void
foreign_toplevel(void *data, struct ext_foreign_toplevel_list_v1 *list,
		struct ext_foreign_toplevel_handle_v1 *handle)
{
	struct toplevel *toplevel = zalloc(sizeof(*toplevel));
	toplevel->foreign = handle;
	wl_list_insert(&toplevels, &toplevel->link);
	updated = true;

	ext_foreign_toplevel_handle_v1_add_listener(handle,
		&foreign_handle_listener, toplevel);

	if (!toplevel_info) {
		return;
	}
	toplevel->cosmic = zcosmic_toplevel_info_v1_get_cosmic_toplevel(
		toplevel_info, handle);
	zcosmic_toplevel_handle_v1_add_listener(toplevel->cosmic,
		&cosmic_handle_listener, toplevel);
	pending_handles++;

	/*
	 * Created up front rather than on demand, so that the properties only
	 * labwc_control_v1 reports are part of every snapshot.
	 */
	if (control) {
		toplevel->control = labwc_control_v1_get_toplevel(control,
			handle);
		labwc_control_toplevel_v1_add_listener(toplevel->control,
			&control_listener, toplevel);
		pending_control++;
	}
}

static void
foreign_finished(void *data, struct ext_foreign_toplevel_list_v1 *list)
{
}

static const struct ext_foreign_toplevel_list_v1_listener foreign_list_listener = {
	.toplevel = foreign_toplevel,
	.finished = foreign_finished,
};

/* --------------------------- cosmic manager --------------------------- */

/*
 * Sent after a batch of toplevel changes. This is the only synchronisation
 * point available to version 2 clients.
 */
static void
info_done(void *data, struct zcosmic_toplevel_info_v1 *info)
{
	if (pending_handles) {
		pending_handles--;
	}
	updated = true;
}

static void
info_toplevel(void *data, struct zcosmic_toplevel_info_v1 *info,
		struct zcosmic_toplevel_handle_v1 *toplevel)
{
	/* Not emitted for clients binding version 2 */
}

static void
info_finished(void *data, struct zcosmic_toplevel_info_v1 *info)
{
}

static const struct zcosmic_toplevel_info_v1_listener info_listener = {
	.toplevel = info_toplevel,
	.finished = info_finished,
	.done = info_done,
};

/* ------------------------------ registry ------------------------------ */

static void
registry_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version)
{
	if (!strcmp(interface, wl_output_interface.name)) {
		uint32_t bind_version = version < 4 ? version : 4;
		struct output *output = zalloc(sizeof(*output));
		output->wl_output = wl_registry_bind(registry, name,
			&wl_output_interface, bind_version);
		wl_list_insert(&outputs, &output->link);
		wl_output_add_listener(output->wl_output, &output_listener,
			output);
	} else if (!strcmp(interface,
			ext_foreign_toplevel_list_v1_interface.name)) {
		struct ext_foreign_toplevel_list_v1 *list =
			wl_registry_bind(registry, name,
				&ext_foreign_toplevel_list_v1_interface, 1);
		ext_foreign_toplevel_list_v1_add_listener(list,
			&foreign_list_listener, NULL);
	} else if (!strcmp(interface,
			zcosmic_toplevel_info_v1_interface.name)) {
		uint32_t bind_version = version < COSMIC_TOPLEVEL_INFO_VERSION
			? version : COSMIC_TOPLEVEL_INFO_VERSION;
		toplevel_info = wl_registry_bind(registry, name,
			&zcosmic_toplevel_info_v1_interface, bind_version);
		zcosmic_toplevel_info_v1_add_listener(toplevel_info,
			&info_listener, NULL);
	} else if (!strcmp(interface, labwc_control_v1_interface.name)) {
		control = wl_registry_bind(registry, name,
			&labwc_control_v1_interface, 1);
	}
}

static void
registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* ------------------------------ selectors ----------------------------- */

static bool
selector_parse(const char *arg, struct selector *selector)
{
	if (!*arg) {
		return false;
	}
	if (!strcmp(arg, "*")) {
		selector->type = SELECTOR_ALL;
		return true;
	}
	if (!strncmp(arg, "app_id:", 7)) {
		selector->type = SELECTOR_APP_ID;
		selector->value = arg + 7;
		return *selector->value;
	}
	if (!strncmp(arg, "title:", 6)) {
		selector->type = SELECTOR_TITLE;
		selector->value = arg + 6;
		return *selector->value;
	}
	if (!strncmp(arg, "identifier:", 11)) {
		selector->type = SELECTOR_IDENTIFIER;
		selector->value = arg + 11;
		return *selector->value;
	}
	/* A bare argument is an ext-foreign-toplevel identifier */
	selector->type = SELECTOR_IDENTIFIER;
	selector->value = arg;
	return true;
}

static bool
selector_matches(struct selector *selector, struct toplevel *toplevel)
{
	const char *value = NULL;

	switch (selector->type) {
	case SELECTOR_ALL:
		return true;
	case SELECTOR_APP_ID:
		value = toplevel->app_id;
		break;
	case SELECTOR_TITLE:
		value = toplevel->title;
		break;
	case SELECTOR_IDENTIFIER:
		value = toplevel->identifier;
		break;
	}
	return value && !strcmp(value, selector->value);
}

static unsigned int
select_toplevels(struct selector *selector)
{
	struct toplevel *toplevel;
	unsigned int matches = 0;

	wl_list_for_each(toplevel, &toplevels, link) {
		toplevel->selected = selector_matches(selector, toplevel);
		if (toplevel->selected) {
			matches++;
		}
	}

	return matches;
}

/* -------------------------------- output ------------------------------ */

static const char *state_names[] = {
	"maximized",
	"minimized",
	"activated",
	"fullscreen",
	"sticky",
};

static const char *layer_names[] = {
	"normal",
	"top",
	"bottom",
};

static const char *decoration_names[] = {
	"none",
	"border",
	"full",
};

static void
print_string(const char *str)
{
	if (!str) {
		printf("null");
		return;
	}
	putchar('"');
	for (; *str; str++) {
		switch (*str) {
		case '"':
			printf("\\\"");
			break;
		case '\\':
			printf("\\\\");
			break;
		case '\n':
			printf("\\n");
			break;
		case '\r':
			printf("\\r");
			break;
		case '\t':
			printf("\\t");
			break;
		default:
			if ((unsigned char)*str < 0x20) {
				printf("\\u%04x", (unsigned char)*str);
			} else {
				putchar(*str);
			}
		}
	}
	putchar('"');
}

static void
print_snapshot(void)
{
	struct toplevel *toplevel;
	bool first = true;

	printf("[");
	wl_list_for_each(toplevel, &toplevels, link) {
		printf("%s\n  {", first ? "" : ",");
		first = false;

		printf("\n    \"app_id\": ");
		print_string(toplevel->app_id);
		printf(",\n    \"title\": ");
		print_string(toplevel->title);
		printf(",\n    \"identifier\": ");
		print_string(toplevel->identifier);

		printf(",\n    \"states\": [");
		for (size_t i = 0; i < toplevel->nr_states; i++) {
			uint32_t state = toplevel->states[i];
			printf("%s\"%s\"", i ? ", " : "",
				state < 5 ? state_names[state] : "unknown");
		}
		printf("]");

		/*
		 * Reported by labwc_control_v1 rather than by
		 * zcosmic_toplevel_info_v1, so null if the compositor does not
		 * offer it.
		 */
		printf(",\n    \"layer\": ");
		if (!toplevel->have_control_state) {
			printf("null");
		} else {
			print_string(toplevel->layer >= 0 && toplevel->layer < 3
				? layer_names[toplevel->layer] : "unknown");
		}
		printf(",\n    \"shaded\": %s", !toplevel->have_control_state
			? "null" : (toplevel->shaded ? "true" : "false"));
		printf(",\n    \"decorations\": ");
		if (!toplevel->have_control_state) {
			printf("null");
		} else {
			print_string(toplevel->decorations >= 0
				&& toplevel->decorations < 3
				? decoration_names[toplevel->decorations]
				: "unknown");
		}

		printf(",\n    \"geometry\": [");;
		for (size_t i = 0; i < toplevel->nr_geometries; i++) {
			struct geometry *geometry = &toplevel->geometries[i];
			struct output *output =
				output_from_wl_output(geometry->wl_output);
			int32_t ox = output ? output->x : 0;
			int32_t oy = output ? output->y : 0;
			const char *name = NULL;
			if (output) {
				name = output->name ? output->name : output->model;
			}

			printf("%s\n      {", i ? "," : "");
			printf("\n        \"output\": ");
			print_string(name);
			printf(",\n        \"x\": %d,\n        \"y\": %d",
				geometry->x, geometry->y);
			printf(",\n        \"width\": %d,\n        \"height\": %d",
				geometry->width, geometry->height);
			printf(",\n        \"global_x\": %d,\n        \"global_y\": %d",
				geometry->x + ox, geometry->y + oy);
			printf("\n      }");
		}
		if (toplevel->nr_geometries) {
			printf("\n    ");
		}
		printf("]");
		printf("\n  }");
	}
	printf("%s]\n", first ? "" : "\n");
	fflush(stdout);
}

/* ------------------------------- waiting ------------------------------ */

/*
 * Wait for events and dispatch them. Returns false if nothing arrived within
 * <timeout_ms> or if the connection broke.
 */
static bool
dispatch_events(int timeout_ms)
{
	struct pollfd pfd = {
		.fd = wl_display_get_fd(display),
		.events = POLLIN,
	};

	while (wl_display_prepare_read(display) != 0) {
		if (wl_display_dispatch_pending(display) == -1) {
			return false;
		}
	}
	if (wl_display_flush(display) == -1 && errno != EAGAIN) {
		wl_display_cancel_read(display);
		return false;
	}
	if (poll(&pfd, 1, timeout_ms) <= 0) {
		wl_display_cancel_read(display);
		return false;
	}
	if (wl_display_read_events(display) == -1) {
		return false;
	}
	return wl_display_dispatch_pending(display) != -1;
}

/*
 * Dispatch events until <done>() returns true or <timeout_ms> have passed.
 *
 * No request of labwc_control_v1 is acknowledged. Moving a window means the
 * compositor sends a configure, the client has to commit, and only then does
 * the compositor report the new geometry, so the only way to report what
 * actually happened is to watch the events that come back.
 */
static bool
dispatch_until(bool (*done)(void), int timeout_ms)
{
	int64_t deadline = now_ms() + timeout_ms;

	while (!done()) {
		int64_t remaining = deadline - now_ms();
		if (remaining <= 0) {
			break;
		}
		if (!dispatch_events((int)remaining)) {
			break;
		}
	}

	return done();
}

/*
 * Keep dispatching until events stop arriving.
 *
 * A state change and the geometry that goes with it do not arrive together:
 * the compositor reports the new state as soon as it has configured the
 * client, but the geometry only once the client has committed. Without this
 * the snapshot would show the new state with the geometry it had before.
 */
static void
dispatch_drain(int quiet_ms, int timeout_ms)
{
	int64_t deadline = now_ms() + timeout_ms;

	while (now_ms() < deadline) {
		int64_t remaining = deadline - now_ms();
		int wait = remaining < quiet_ms ? (int)remaining : quiet_ms;
		if (!dispatch_events(wait)) {
			break;
		}
	}
}

static bool
geometry_updated(void)
{
	return geometry_events >= geometry_target;
}

/* Any toplevel reported a state change, which is all we can wait for when
 * the request does not name a toplevel, as with cycle. */
static bool
state_changed(void)
{
	return state_events >= state_target;
}

/* The compositor has acknowledged the requests it reports on itself */
static bool
control_done_received(void)
{
	return done_events >= done_target;
}

static bool
toplevel_has_state(struct toplevel *toplevel, uint32_t state)
{
	for (size_t i = 0; i < toplevel->nr_states; i++) {
		if (toplevel->states[i] == state) {
			return true;
		}
	}
	return false;
}

/* At least one of the toplevels we acted on reports the state now */
static bool
any_selected_state(void)
{
	struct toplevel *toplevel;
	wl_list_for_each(toplevel, &toplevels, link) {
		if (toplevel->selected
				&& toplevel_has_state(toplevel, wait_state)) {
			return true;
		}
	}
	return false;
}

/* All of the toplevels we acted on report (or no longer report) the state */
static bool
all_selected_state(void)
{
	struct toplevel *toplevel;
	wl_list_for_each(toplevel, &toplevels, link) {
		if (!toplevel->selected) {
			continue;
		}
		if (toplevel_has_state(toplevel, wait_state) != wait_state_on) {
			return false;
		}
	}
	return true;
}

/* All of the toplevels we acted on are gone */
static bool
selected_closed(void)
{
	struct toplevel *toplevel;
	wl_list_for_each(toplevel, &toplevels, link) {
		if (toplevel->selected) {
			return false;
		}
	}
	return true;
}

/* ------------------------------- commands ----------------------------- */

static void
list_command(bool watch)
{
	if (!watch) {
		/*
		 * get_cosmic_toplevel() requests were queued while dispatching
		 * the toplevel events above and their replies arrive in later
		 * roundtrips, so keep spinning until every handle has been
		 * reported on.
		 */
		for (int i = 0; i < MAX_ROUNDTRIPS && pending_handles; i++) {
			if (wl_display_roundtrip(display) == -1) {
				break;
			}
		}
		print_snapshot();
		return;
	}

	while (wl_display_dispatch(display) != -1) {
		if (updated) {
			updated = false;
			print_snapshot();
		}
	}
}

/*
 * Whether a toplevel reports the value that was asked for. Commands which
 * have nothing reported about them always match.
 */
static bool
control_state_matches(enum control_command command, bool on,
		const int32_t v[4], struct toplevel *toplevel)
{
	switch (command) {
	case CONTROL_LAYER:
		return toplevel->layer == v[0];
	case CONTROL_SHADE:
		return (toplevel->shaded != 0) == on;
	case CONTROL_DECORATIONS:
		return toplevel->decorations == v[0];
	default:
		return true;
	}
}

/*
 * How many of the toplevels we acted on did not end up with the value we asked
 * for. Only meaningful for commands whose effect labwc_control_v1 reports.
 */
static unsigned int
count_refused(enum control_command command, bool on, const int32_t v[4])
{
	struct toplevel *toplevel;
	unsigned int refused = 0;

	wl_list_for_each(toplevel, &toplevels, link) {
		if (!toplevel->selected || !toplevel->have_control_state) {
			continue;
		}
		if (!control_state_matches(command, on, v, toplevel)) {
			refused++;
		}
	}
	return refused;
}

static int
control_command(struct selector *selector, enum control_command command,
		bool on, const int32_t v[4])
{
	if (!control) {
		fprintf(stderr, "compositor does not support labwc_control_v1\n");
		return 1;
	}

	/* Settle before matching, so that identifiers and geometry are known */
	for (int i = 0; i < MAX_ROUNDTRIPS && initial_state_pending(); i++) {
		if (wl_display_roundtrip(display) == -1) {
			fprintf(stderr, "error communicating with the compositor\n");
			return 1;
		}
	}

	unsigned int matches = select_toplevels(selector);
	if (!matches) {
		fprintf(stderr, "no toplevel matches the given selector\n");
		return 1;
	}

	struct toplevel *toplevel;
	wl_list_for_each(toplevel, &toplevels, link) {
		if (!toplevel->selected || !toplevel->control) {
			continue;
		}
		switch (command) {
		case CONTROL_MOVE:
			labwc_control_toplevel_v1_move_to(toplevel->control,
				v[0], v[1]);
			break;
		case CONTROL_RESIZE:
			labwc_control_toplevel_v1_resize_to(toplevel->control,
				v[0], v[1]);
			break;
		case CONTROL_MOVE_RESIZE:
			labwc_control_toplevel_v1_move_resize_to(toplevel->control,
				v[0], v[1], v[2], v[3]);
			break;
		case CONTROL_MOVE_BY:
			labwc_control_toplevel_v1_move_by(toplevel->control,
				v[0], v[1]);
			break;
		case CONTROL_RESIZE_BY:
			/* Grow or shrink the right and bottom edges */
			labwc_control_toplevel_v1_resize_by(toplevel->control,
				0, v[0], 0, v[1]);
			break;
		case CONTROL_CENTER:
			labwc_control_toplevel_v1_center(toplevel->control);
			break;
		case CONTROL_ACTIVATE:
			labwc_control_toplevel_v1_activate(toplevel->control);
			break;
		case CONTROL_CLOSE:
			labwc_control_toplevel_v1_close(toplevel->control);
			break;
		case CONTROL_MAXIMIZE:
			if (on) {
				labwc_control_toplevel_v1_set_maximized(
					toplevel->control);
			} else {
				labwc_control_toplevel_v1_unset_maximized(
					toplevel->control);
			}
			break;
		case CONTROL_MINIMIZE:
			if (on) {
				labwc_control_toplevel_v1_set_minimized(
					toplevel->control);
			} else {
				labwc_control_toplevel_v1_unset_minimized(
					toplevel->control);
			}
			break;
		case CONTROL_FULLSCREEN:
			if (on) {
				labwc_control_toplevel_v1_set_fullscreen(
					toplevel->control);
			} else {
				labwc_control_toplevel_v1_unset_fullscreen(
					toplevel->control);
			}
			break;
		case CONTROL_STICKY:
			if (on) {
				labwc_control_toplevel_v1_set_sticky(
					toplevel->control);
			} else {
				labwc_control_toplevel_v1_unset_sticky(
					toplevel->control);
			}
			break;
		case CONTROL_SHADE:
			if (on) {
				labwc_control_toplevel_v1_set_shaded(
					toplevel->control);
			} else {
				labwc_control_toplevel_v1_unset_shaded(
					toplevel->control);
			}
			break;
		case CONTROL_SNAP_TO_EDGE:
			labwc_control_toplevel_v1_snap_to_edge(toplevel->control,
				(uint32_t)v[0], v[1]);
			break;
		case CONTROL_GROW_TO_EDGE:
			labwc_control_toplevel_v1_grow_to_edge(toplevel->control,
				(uint32_t)v[0]);
			break;
		case CONTROL_SHRINK_TO_EDGE:
			labwc_control_toplevel_v1_shrink_to_edge(toplevel->control,
				(uint32_t)v[0]);
			break;
		case CONTROL_LAYER:
			labwc_control_toplevel_v1_set_layer(toplevel->control,
				(uint32_t)v[0]);
			break;
		case CONTROL_DECORATIONS:
			labwc_control_toplevel_v1_set_decorations(
				toplevel->control, (uint32_t)v[0]);
			break;
		}
	}

	/*
	 * Report what actually happened rather than what we asked for, so that
	 * callers can tell whether the request had the intended effect. Some of
	 * these have nothing in zcosmic_toplevel_info_v1 to wait for, so the
	 * default is to consider them done and rely on the drain below.
	 */
	bool ok = true;
	switch (command) {
	case CONTROL_MOVE:
	case CONTROL_RESIZE:
	case CONTROL_MOVE_RESIZE:
	case CONTROL_MOVE_BY:
	case CONTROL_RESIZE_BY:
	case CONTROL_CENTER:
	case CONTROL_SNAP_TO_EDGE:
	case CONTROL_GROW_TO_EDGE:
	case CONTROL_SHRINK_TO_EDGE:
		geometry_target = geometry_events + matches;
		ok = dispatch_until(geometry_updated, CONTROL_TIMEOUT_MS);
		break;
	case CONTROL_ACTIVATE:
		/* Only one toplevel can be the active one */
		wait_state = ZCOSMIC_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED;
		ok = dispatch_until(any_selected_state, CONTROL_TIMEOUT_MS);
		break;
	case CONTROL_MAXIMIZE:
		wait_state = ZCOSMIC_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED;
		wait_state_on = on;
		ok = dispatch_until(all_selected_state, CONTROL_TIMEOUT_MS);
		break;
	case CONTROL_MINIMIZE:
		wait_state = ZCOSMIC_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED;
		wait_state_on = on;
		ok = dispatch_until(all_selected_state, CONTROL_TIMEOUT_MS);
		break;
	case CONTROL_FULLSCREEN:
		wait_state = ZCOSMIC_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN;
		wait_state_on = on;
		ok = dispatch_until(all_selected_state, CONTROL_TIMEOUT_MS);
		break;
	case CONTROL_STICKY:
		wait_state = ZCOSMIC_TOPLEVEL_HANDLE_V1_STATE_STICKY;
		wait_state_on = on;
		ok = dispatch_until(all_selected_state, CONTROL_TIMEOUT_MS);
		break;
	case CONTROL_CLOSE:
		ok = dispatch_until(selected_closed, CONTROL_TIMEOUT_MS);
		break;
	case CONTROL_SHADE:
	case CONTROL_LAYER:
	case CONTROL_DECORATIONS:
		/*
		 * These are the requests whose effect labwc_control_v1 reports
		 * itself, so the compositor acknowledges them with done. Waiting
		 * for that is what turns "sent" into "the value read back is the
		 * one asked for, or the request was refused".
		 */
		done_target = done_events + matches;
		ok = dispatch_until(control_done_received, CONTROL_TIMEOUT_MS);
		break;
	}
	/*
	 * A request that changes nothing produces no events, and that is
	 * indistinguishable from a request the compositor ignored: an already
	 * centered toplevel, a client that snaps its size to a grid, or a
	 * client that declines to close all look the same. So a wait that
	 * found nothing is not reported as an error; the snapshot is the
	 * answer. The cases where the outcome is unambiguous are reported on
	 * stderr.
	 */
	if (command == CONTROL_CLOSE) {
		unsigned int left = 0;
		struct toplevel *t;
		wl_list_for_each(t, &toplevels, link) {
			if (t->selected) {
				left++;
			}
		}
		if (!ok && left) {
			fprintf(stderr,
				"note: %u toplevel(s) did not close\n", left);
		}
	} else {
		unsigned int refused = count_refused(command, on, v);
		if (refused) {
			fprintf(stderr,
				"note: %u toplevel(s) refused the request\n",
				refused);
		}
	}
	dispatch_drain(SETTLE_QUIET_MS, CONTROL_TIMEOUT_MS);
	print_snapshot();
	return 0;
}

/*
 * cycle acts on whichever toplevel is active rather than on a selector, so it
 * cannot use control_command().
 */
static int
cycle_command(bool forward)
{
	if (!control) {
		fprintf(stderr, "compositor does not support labwc_control_v1\n");
		return 1;
	}

	for (int i = 0; i < MAX_ROUNDTRIPS && initial_state_pending(); i++) {
		if (wl_display_roundtrip(display) == -1) {
			fprintf(stderr, "error communicating with the compositor\n");
			return 1;
		}
	}

	if (forward) {
		labwc_control_v1_cycle_next(control);
	} else {
		labwc_control_v1_cycle_prev(control);
	}

	/* We cannot know which toplevel will be focused, so wait for any state */
	state_target = state_events + 1;
	dispatch_until(state_changed, CONTROL_TIMEOUT_MS);
	dispatch_drain(SETTLE_QUIET_MS, CONTROL_TIMEOUT_MS);
	print_snapshot();
	return 0;
}

static int
parse_int(const char *str, int32_t *value)
{
	char *end = NULL;
	long parsed;

	errno = 0;
	parsed = strtol(str, &end, 10);
	if (errno || end == str || *end || parsed < INT32_MIN
			|| parsed > INT32_MAX) {
		return -1;
	}
	*value = (int32_t)parsed;
	return 0;
}

static int
parse_onoff(const char *str, bool *on)
{
	if (!strcmp(str, "on")) {
		*on = true;
		return 0;
	}
	if (!strcmp(str, "off")) {
		*on = false;
		return 0;
	}
	return -1;
}

/*
 * Named arguments are matched to the enum values of the protocol, so the
 * order of <names> is significant.
 */
static int
parse_named(const char *str, const char * const *names, size_t nr_names,
		int32_t *value)
{
	for (size_t i = 0; i < nr_names; i++) {
		if (!strcmp(str, names[i])) {
			*value = (int32_t)i;
			return 0;
		}
	}
	return -1;
}

#define NR_NAMES(names) (sizeof(names) / sizeof((names)[0]))

enum command_args {
	ARGS_NONE,
	ARGS_ONOFF,
	ARGS_TWO_INT,
	ARGS_FOUR_INT,
	ARGS_EDGE,
	ARGS_EDGE_SNAP,
	ARGS_LAYER,
	ARGS_DECORATION,
};

static const struct command_spec {
	const char *name;
	enum control_command command;
	enum command_args args;
} command_specs[] = {
	{ "move", CONTROL_MOVE, ARGS_TWO_INT },
	{ "move-by", CONTROL_MOVE_BY, ARGS_TWO_INT },
	{ "resize", CONTROL_RESIZE, ARGS_TWO_INT },
	{ "resize-by", CONTROL_RESIZE_BY, ARGS_TWO_INT },
	{ "move-resize", CONTROL_MOVE_RESIZE, ARGS_FOUR_INT },
	{ "center", CONTROL_CENTER, ARGS_NONE },
	{ "focus", CONTROL_ACTIVATE, ARGS_NONE },
	{ "close", CONTROL_CLOSE, ARGS_NONE },
	{ "maximize", CONTROL_MAXIMIZE, ARGS_ONOFF },
	{ "minimize", CONTROL_MINIMIZE, ARGS_ONOFF },
	{ "fullscreen", CONTROL_FULLSCREEN, ARGS_ONOFF },
	{ "sticky", CONTROL_STICKY, ARGS_ONOFF },
	{ "shade", CONTROL_SHADE, ARGS_ONOFF },
	{ "snap-to-edge", CONTROL_SNAP_TO_EDGE, ARGS_EDGE_SNAP },
	{ "grow-to-edge", CONTROL_GROW_TO_EDGE, ARGS_EDGE },
	{ "shrink-to-edge", CONTROL_SHRINK_TO_EDGE, ARGS_EDGE },
	{ "layer", CONTROL_LAYER, ARGS_LAYER },
	{ "decorations", CONTROL_DECORATIONS, ARGS_DECORATION },
};

static void
usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s <command> [args...]\n"
		"\n"
		"Commands:\n"
		"  list                                  Print a JSON snapshot of all toplevels\n"
		"  cycle [next|prev]                     Focus the next or previous toplevel\n"
		"\n"
		"  move <selector> <x> <y>               Move to a position\n"
		"  move-by <selector> <dx> <dy>          Move relative to the current position\n"
		"  resize <selector> <width> <height>    Resize\n"
		"  resize-by <selector> <width> <height> Grow or shrink the right and bottom edges\n"
		"  move-resize <selector> <x> <y> <width> <height>\n"
		"                                        Move and resize in one step\n"
		"  center <selector>                     Center on the output\n"
		"  snap-to-edge <selector> <left|right|up|down> [screen|windows]\n"
		"                                        Move against an edge of the output,\n"
		"                                        or of another toplevel\n"
		"  grow-to-edge <selector> <left|right|up|down>\n"
		"                                        Grow towards an edge by up to 50%%\n"
		"  shrink-to-edge <selector> <left|right|up|down>\n"
		"                                        Shrink away from an edge\n"
		"\n"
		"  focus <selector>                      Make the active window\n"
		"  close <selector>                      Ask to close\n"
		"  maximize <selector> [on|off]          Maximize or restore\n"
		"  minimize <selector> [on|off]          Minimize or restore\n"
		"  fullscreen <selector> [on|off]        Fullscreen or restore\n"
		"  sticky <selector> [on|off]            Show on all workspaces\n"
		"  shade <selector> [on|off]             Roll up to the title bar\n"
		"  layer <selector> <normal|top|bottom>  Stacking layer\n"
		"  decorations <selector> <none|border|full>\n"
		"                                        How much of the decorations to draw\n"
		"\n"
		"Options:\n"
		"  -h, --help    Show help message and quit\n"
		"  -w, --watch   With 'list': print a new snapshot on every change\n"
		"\n"
		"Selectors:\n"
		"  <identifier>       Exact ext-foreign-toplevel identifier\n"
		"  identifier:<str>   Same as above, explicitly\n"
		"  app_id:<str>       Exact match on the application ID\n"
		"  title:<str>        Exact match on the window title\n"
		"  *                  All toplevels\n"
		"\n"
		"Coordinates are in the compositor's global layout coordinate space.\n",
		argv0);
	exit(1);
}

int
main(int argc, char **argv)
{
	static const struct option long_options[] = {
		{"help", no_argument, NULL, 'h'},
		{"watch", no_argument, NULL, 'w'},
		{0, 0, 0, 0},
	};
	bool watch = false;
	int c;

	/*
	 * The '+' stops option parsing at the first non-option argument, so that
	 * negative numbers can be given as arguments (getopt would otherwise
	 * read '-100' as an option). Flags belonging to a command are parsed
	 * from its own arguments instead, see the 'list' branch below.
	 */
	while ((c = getopt_long(argc, argv, "+hw", long_options, NULL)) != -1) {
		switch (c) {
		case 'w':
			watch = true;
			break;
		default:
			usage(argv[0]);
		}
	}
	if (optind >= argc) {
		usage(argv[0]);
	}

	const char *command = argv[optind];
	int nr_args = argc - optind - 1;
	char **args = &argv[optind + 1];
	struct selector selector = { 0 };
	int32_t v[4] = { 0 };
	bool on = true;
	bool forward = true;
	bool is_list = !strcmp(command, "list");
	bool is_cycle = !strcmp(command, "cycle");
	const struct command_spec *spec = NULL;

	for (size_t i = 0; i < NR_NAMES(command_specs); i++) {
		if (!strcmp(command, command_specs[i].name)) {
			spec = &command_specs[i];
			break;
		}
	}
	if (!is_list && !is_cycle && !spec) {
		usage(argv[0]);
	}

	if (is_list) {
		for (int i = 0; i < nr_args; i++) {
			if (!strcmp(args[i], "--watch")
					|| !strcmp(args[i], "-w")) {
				watch = true;
			} else {
				usage(argv[0]);
			}
		}
	} else if (is_cycle) {
		if (nr_args > 1) {
			usage(argv[0]);
		}
		if (nr_args == 1) {
			if (!strcmp(args[0], "next")) {
				forward = true;
			} else if (!strcmp(args[0], "prev")) {
				forward = false;
			} else {
				usage(argv[0]);
			}
		}
	} else {
		static const char * const edge_names[] =
			{ "left", "right", "up", "down" };
		static const char * const layer_args[] =
			{ "normal", "top", "bottom" };
		static const char * const decoration_args[] =
			{ "none", "border", "full" };

		/* Every other command takes a selector first */
		if (nr_args < 1 || !selector_parse(args[0], &selector)) {
			usage(argv[0]);
		}
		char * const *rest = args + 1;
		int nr_rest = nr_args - 1;

		switch (spec->args) {
		case ARGS_NONE:
			if (nr_rest) {
				usage(argv[0]);
			}
			break;
		case ARGS_ONOFF:
			if (nr_rest > 1 || (nr_rest == 1
					&& parse_onoff(rest[0], &on))) {
				usage(argv[0]);
			}
			break;
		case ARGS_TWO_INT:
			if (nr_rest != 2 || parse_int(rest[0], &v[0])
					|| parse_int(rest[1], &v[1])) {
				usage(argv[0]);
			}
			break;
		case ARGS_FOUR_INT:
			if (nr_rest != 4 || parse_int(rest[0], &v[0])
					|| parse_int(rest[1], &v[1])
					|| parse_int(rest[2], &v[2])
					|| parse_int(rest[3], &v[3])) {
				usage(argv[0]);
			}
			break;
		case ARGS_EDGE:
			if (nr_rest != 1 || parse_named(rest[0], edge_names,
					NR_NAMES(edge_names), &v[0])) {
				usage(argv[0]);
			}
			break;
		case ARGS_EDGE_SNAP:
			/*
			 * Default to the output edge: snapping to the edge of
			 * another toplevel, which is what the MoveToEdge action does
			 * by default, is a separate thing to ask for.
			 */
			if (nr_rest < 1 || nr_rest > 2
					|| parse_named(rest[0], edge_names,
						NR_NAMES(edge_names), &v[0])) {
				usage(argv[0]);
			}
			if (nr_rest == 2) {
				static const char * const snap_names[] =
					{ "screen", "windows" };
				if (parse_named(rest[1], snap_names,
						NR_NAMES(snap_names), &v[1])) {
					usage(argv[0]);
				}
			}
			break;
		case ARGS_LAYER:
			if (nr_rest != 1 || parse_named(rest[0], layer_args,
					NR_NAMES(layer_args), &v[0])) {
				usage(argv[0]);
			}
			break;
		case ARGS_DECORATION:
			if (nr_rest != 1 || parse_named(rest[0], decoration_args,
					NR_NAMES(decoration_args), &v[0])) {
				usage(argv[0]);
			}
			break;
		}
	}

	wl_list_init(&outputs);
	wl_list_init(&toplevels);

	display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "unable to connect to a Wayland display\n");
		return 1;
	}

	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);

	/* Globals first, then the initial toplevel events */
	if (wl_display_roundtrip(display) == -1
			|| wl_display_roundtrip(display) == -1) {
		fprintf(stderr, "error communicating with the compositor\n");
		return 1;
	}

	if (!toplevel_info) {
		fprintf(stderr, "compositor does not support "
			"zcosmic_toplevel_info_v1\n");
		return 1;
	}

	if (is_list) {
		list_command(watch);
		return 0;
	}
	if (is_cycle) {
		return cycle_command(forward);
	}

	return control_command(&selector, spec->command, on, v);
}
