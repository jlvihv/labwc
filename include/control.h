/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_CONTROL_H
#define LABWC_CONTROL_H

struct wl_display;

/*
 * Create the labwc_control_v1 global, which lets privileged clients set the
 * absolute geometry of toplevels owned by other clients.
 *
 * This fills a gap left by the standard protocols: wlr- and
 * ext-foreign-toplevel expose a toplevel's state and allow requests to
 * activate, close, maximize, minimize and fullscreen it, but there is no
 * protocol for moving or resizing a toplevel owned by somebody else.
 *
 * The interface name is listed in parse_privileged_interface(), so it can be
 * restricted with <privilegedInterfaces> in rc.xml.
 */
void control_create_global(struct wl_display *display);

#endif /* LABWC_CONTROL_H */
