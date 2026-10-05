// SPDX-License-Identifier: GPL-2.0-only
/*
 * labwc-toplevels - print the geometry and state of all toplevels as JSON
 *
 * The toplevel list comes from ext-foreign-toplevel-list-v1, which provides
 * a stable identifier for each toplevel. Geometry, state and output
 * membership come from zcosmic-toplevel-info-v1.
 *
 * Note that zcosmic-toplevel-info-v1 reports geometry relative to the output
 * the toplevel is visible on. This tool resolves each output's position from
 * wl_output.geometry and also reports compositor-global coordinates.
 *
 * Examples:
 *   labwc-toplevels              # one snapshot, then exit
 *   labwc-toplevels --watch      # a snapshot whenever something changes
 *   labwc-toplevels | jq -r '.[] | "\(.app_id) \(.geometry[0].global_x),\(.geometry[0].global_y)"'
 */
#define _POSIX_C_SOURCE 200809L
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-client-protocol.h>

#include "cosmic-toplevel-info-unstable-v1-client-protocol.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"

/*
 * Version 2 is where the geometry event was introduced. Clients binding
 * version 2 get their handles from get_cosmic_toplevel() and rely on
 * ext-foreign-toplevel-list-v1 for title/app_id/closed.
 */
#define COSMIC_TOPLEVEL_INFO_VERSION 2

#define MAX_GEOMETRIES 16
#define MAX_STATES 8
#define MAX_ROUNDTRIPS 10

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
	char *title;
	char *app_id;
	char *identifier;

	struct geometry geometries[MAX_GEOMETRIES];
	size_t nr_geometries;

	uint32_t states[MAX_STATES];
	size_t nr_states;

	struct wl_list link;
};

static struct wl_display *display;
static struct wl_list outputs;
static struct wl_list toplevels;
static struct zcosmic_toplevel_info_v1 *toplevel_info;
static unsigned int pending_handles;
static bool updated;

/*
 * wl_output.geometry make/model and wl_output.name may be NULL, whereas
 * xstrdup() requires a non-NULL source.
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
	/*
	 * Not emitted for clients binding version 2, which use the done event
	 * on zcosmic_toplevel_info_v1 instead.
	 */
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
	 * All of our proxies for this toplevel are now useless, including the
	 * cosmic one, so tear the whole thing down.
	 */
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

/* ------------------------------- output ------------------------------- */

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

static void
usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s [options...]\n"
		"  -h, --help    Show help message and quit\n"
		"  -w, --watch   Print a new snapshot whenever something changes\n",
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
	if (optind < argc) {
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
		return 0;
	}

	while (wl_display_dispatch(display) != -1) {
		if (updated) {
			updated = false;
			print_snapshot();
		}
	}

	fprintf(stderr, "error communicating with the compositor\n");
	return 1;
}
