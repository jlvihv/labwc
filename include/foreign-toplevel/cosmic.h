/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_FOREIGN_TOPLEVEL_COSMIC_H
#define LABWC_FOREIGN_TOPLEVEL_COSMIC_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/util/box.h>

struct view;

/*
 * Support for the zcosmic_toplevel_info_v1 protocol.
 *
 * The protocol extends ext-foreign-toplevel-list-v1 (which we already
 * implement) with the geometry, state and workspace membership of each
 * toplevel. Clients ask for a zcosmic_toplevel_handle_v1 for any
 * ext_foreign_toplevel_handle_v1 they care about and are then kept up to
 * date. A single view may therefore be tracked by any number of clients
 * at the same time.
 *
 * There is no standard protocol for toplevel geometry, which makes this
 * useful for panels, pagers, window switchers and anything else - such as
 * a scripting or automation tool - that needs to know where a window
 * actually is.
 */
struct cosmic_toplevel {
	struct view *view;

	/*
	 * Last geometry reported to clients. view_moved() is called for
	 * every frame of an interactive move/resize, so this is used to
	 * suppress redundant events.
	 */
	struct wlr_box last_geometry;
	bool has_last_geometry;

	/*
	 * Cached from the view->events.activated payload. server.active_view
	 * is updated by the caller after the signal is emitted, so it cannot
	 * be used to answer "is this view activated?" here.
	 */
	bool activated;

	/* Compositor side state changes */
	struct {
		struct wl_listener new_geometry;
		struct wl_listener new_outputs;
		struct wl_listener new_omnipresent;
		struct wl_listener maximized;
		struct wl_listener minimized;
		struct wl_listener fullscreened;
		struct wl_listener activated;
	} on_view;
};

/*
 * Create the zcosmic_toplevel_info_v1 global. Called once on startup,
 * before any view is mapped.
 */
void cosmic_toplevel_create_global(struct wl_display *display);

void cosmic_toplevel_init(struct cosmic_toplevel *toplevel, struct view *view);
void cosmic_toplevel_finish(struct cosmic_toplevel *toplevel);

#endif /* LABWC_FOREIGN_TOPLEVEL_COSMIC_H */
