/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_FOREIGN_TOPLEVEL_H
#define LABWC_FOREIGN_TOPLEVEL_H

struct view;
struct foreign_toplevel;
struct cosmic_toplevel;

struct foreign_toplevel *foreign_toplevel_create(struct view *view);
void foreign_toplevel_set_parent(struct foreign_toplevel *toplevel,
	struct foreign_toplevel *parent);
void foreign_toplevel_destroy(struct foreign_toplevel *toplevel);

/* Per-view state for the zcosmic_toplevel_info_v1 implementation */
struct cosmic_toplevel *foreign_toplevel_get_cosmic(
	struct foreign_toplevel *toplevel);

#endif /* LABWC_FOREIGN_TOPLEVEL_H */
