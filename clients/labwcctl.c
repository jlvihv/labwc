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
	CONTROL_ACTIVATE,
	CONTROL_CLOSE,
	CONTROL_MAXIMIZE,
	CONTROL_MINIMIZE,
	CONTROL_FULLSCREEN,
	CONTROL_STICKY,
};

static struct wl_display *display;
static struct wl_list outputs;
static struct wl_list toplevels;
static struct zcosmic_toplevel_info_v1 *toplevel_info;
static struct labwc_control_v1 *control;
static unsigned int pending_handles;
static unsigned int geometry_events;
static unsigned int geometry_target;
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
		if (!toplevel->selected) {
			continue;
		}
		matches++;
		if (!toplevel->control && control) {
			toplevel->control = labwc_control_v1_get_toplevel(control,
				toplevel->foreign);
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

		printf(",\n    \"geometry\": [");
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

static int
control_command(struct selector *selector, enum control_command command,
		bool on, int32_t x, int32_t y, int32_t width, int32_t height)
{
	if (!control) {
		fprintf(stderr, "compositor does not support labwc_control_v1\n");
		return 1;
	}

	/* Settle before matching, so that identifiers and geometry are known */
	for (int i = 0; i < MAX_ROUNDTRIPS && pending_handles; i++) {
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
			labwc_control_toplevel_v1_move_to(toplevel->control, x, y);
			break;
		case CONTROL_RESIZE:
			labwc_control_toplevel_v1_resize_to(toplevel->control,
				width, height);
			break;
		case CONTROL_MOVE_RESIZE:
			labwc_control_toplevel_v1_move_resize_to(toplevel->control,
				x, y, width, height);
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
		}
	}

	/*
	 * Report what actually happened rather than what we asked for, so that
	 * callers can tell whether the request had the intended effect.
	 */
	bool ok = false;
	switch (command) {
	case CONTROL_MOVE:
	case CONTROL_RESIZE:
	case CONTROL_MOVE_RESIZE:
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
	}
	if (!ok) {
		fprintf(stderr, "timed out waiting for the compositor\n");
	}
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

static void
usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s <command> [args...]\n"
		"\n"
		"Commands:\n"
		"  list                                  Print a JSON snapshot of all toplevels\n"
		"  move <selector> <x> <y>               Move matching toplevels\n"
		"  resize <selector> <width> <height>    Resize matching toplevels\n"
		"  move-resize <selector> <x> <y> <width> <height>\n"
		"                                        Move and resize matching toplevels\n"
		"  focus <selector>                      Make matching toplevels the active window\n"
		"  close <selector>                      Ask matching toplevels to close\n"
		"  maximize <selector> [on|off]          Maximize or restore matching toplevels\n"
		"  minimize <selector> [on|off]          Minimize or restore matching toplevels\n"
		"  fullscreen <selector> [on|off]        Fullscreen or restore matching toplevels\n"
		"  sticky <selector> [on|off]            Show matching toplevels on all workspaces\n"
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

	while ((c = getopt_long(argc, argv, "hw", long_options, NULL)) != -1) {
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
	int32_t x = 0, y = 0, width = 0, height = 0;
	enum control_command control_command_type = CONTROL_MOVE;
	bool on = true;

	if (!strcmp(command, "list")) {
		if (nr_args) {
			usage(argv[0]);
		}
	} else if (!strcmp(command, "move")) {
		if (nr_args != 3 || !selector_parse(args[0], &selector)
				|| parse_int(args[1], &x)
				|| parse_int(args[2], &y)) {
			usage(argv[0]);
		}
		control_command_type = CONTROL_MOVE;
	} else if (!strcmp(command, "resize")) {
		if (nr_args != 3 || !selector_parse(args[0], &selector)
				|| parse_int(args[1], &width)
				|| parse_int(args[2], &height)) {
			usage(argv[0]);
		}
		control_command_type = CONTROL_RESIZE;
	} else if (!strcmp(command, "move-resize")) {
		if (nr_args != 5 || !selector_parse(args[0], &selector)
				|| parse_int(args[1], &x)
				|| parse_int(args[2], &y)
				|| parse_int(args[3], &width)
				|| parse_int(args[4], &height)) {
			usage(argv[0]);
		}
		control_command_type = CONTROL_MOVE_RESIZE;
	} else if (!strcmp(command, "focus")) {
		if (nr_args != 1 || !selector_parse(args[0], &selector)) {
			usage(argv[0]);
		}
		control_command_type = CONTROL_ACTIVATE;
	} else if (!strcmp(command, "close")) {
		if (nr_args != 1 || !selector_parse(args[0], &selector)) {
			usage(argv[0]);
		}
		control_command_type = CONTROL_CLOSE;
	} else if (!strcmp(command, "maximize") || !strcmp(command, "minimize")
			|| !strcmp(command, "fullscreen")
			|| !strcmp(command, "sticky")) {
		if (nr_args < 1 || nr_args > 2
				|| !selector_parse(args[0], &selector)) {
			usage(argv[0]);
		}
		if (nr_args == 2) {
			if (!strcmp(args[1], "on")) {
				on = true;
			} else if (!strcmp(args[1], "off")) {
				on = false;
			} else {
				usage(argv[0]);
			}
		}
		if (!strcmp(command, "maximize")) {
			control_command_type = CONTROL_MAXIMIZE;
		} else if (!strcmp(command, "minimize")) {
			control_command_type = CONTROL_MINIMIZE;
		} else if (!strcmp(command, "fullscreen")) {
			control_command_type = CONTROL_FULLSCREEN;
		} else {
			control_command_type = CONTROL_STICKY;
		}
	} else {
		usage(argv[0]);
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

	if (!strcmp(command, "list")) {
		list_command(watch);
		return 0;
	}

	return control_command(&selector, control_command_type, on,
		x, y, width, height);
}
