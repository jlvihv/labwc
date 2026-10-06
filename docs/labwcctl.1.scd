labwcctl(1)

# NAME

labwcctl - Query and control toplevels

# SYNOPSIS

_labwcctl_ [options...] _list_ [--watch]

_labwcctl_ [options...] _move_ <selector> <x> <y>

_labwcctl_ [options...] _resize_ <selector> <width> <height>

_labwcctl_ [options...] _move-resize_ <selector> <x> <y> <width> <height>

_labwcctl_ [options...] _focus_ <selector>

_labwcctl_ [options...] _close_ <selector>

_labwcctl_ [options...] _maximize_|_minimize_|_fullscreen_|_sticky_ <selector> [on|off]

# DESCRIPTION

_labwcctl_ reads the toplevel list, state and geometry, and acts on matching
toplevels.

The toplevel list comes from *ext-foreign-toplevel-list-v1*, which provides a
stable _identifier_ for each toplevel. Geometry and state come from
*zcosmic-toplevel-info-v1*. All other commands are carried out with
*labwc_control_v1*.

*labwc_control_v1* is used rather than the standard
*zwlr_foreign_toplevel_management_v1* because a zwlr handle cannot be
correlated with an ext handle: only the latter carries the stable identifier
needed to tell two toplevels with the same title and app id apart. Guessing
would mean risking acting on the wrong window.

The commands that change something are restricted by the _privilegedInterfaces_
setting in *rc.xml*. By default all privileged interfaces are allowed. See
*labwc-config*(5).

# OPTIONS

*-h, --help*
	Show help message and quit.

*-w, --watch*
	With _list_, keep running and print a new snapshot whenever something
	changes instead of printing one snapshot and exiting.

# COMMANDS

*list*
	Print a JSON snapshot of all toplevels.

*move* <selector> <x> <y>
	Move matching toplevels to the given position, keeping their size.

*resize* <selector> <width> <height>
	Resize matching toplevels, keeping their position.

*move-resize* <selector> <x> <y> <width> <height>
	Move and resize matching toplevels in one step.

*focus* <selector>
	Raise matching toplevels and give them keyboard focus. With more than
	one match, the last one wins.

*close* <selector>
	Ask matching toplevels to close. The client is free to ignore this, for
	example to ask the user to save unsaved changes first, so this does not
	guarantee that the window goes away.

*maximize* <selector> [on|off]
	Maximize matching toplevels, or restore the geometry they had before
	they were maximized. Defaults to _on_.

*minimize* <selector> [on|off]
	Hide matching toplevels, or show them again. Defaults to _on_.

*fullscreen* <selector> [on|off]
	Make matching toplevels fullscreen, or take them out of fullscreen.
	Defaults to _on_.

*sticky* <selector> [on|off]
	Show matching toplevels on all workspaces, or stop doing so. Defaults to
	_on_. This is what labwc calls omnipresent.

All commands except _list_ print the resulting snapshot, so that callers can
tell whether the request had the intended effect. Note that none of these
requests are acknowledged, and that a change is not synchronous: the
compositor sends a configure event, the client has to commit, and only then
is the new state reported. _labwcctl_ waits up to 500ms for that to happen,
and keeps listening until the events stop arriving so that the snapshot does
not show a new state next to the geometry that went with the old one.

# SELECTORS

The first argument of every geometry command selects which toplevels to act
on. All matching toplevels are affected.

<identifier>
	Exact match on the stable identifier reported by
	*ext-foreign-toplevel-list-v1*.

identifier:<string>
	Same as above, spelled out.

app_id:<string>
	Exact match on the application ID (X11 WM_CLASS for XWayland clients).

title:<string>
	Exact match on the window title.

\*
	All toplevels.

# OUTPUT

The output is a JSON array. Each element has the following keys:

_app_id_
	The application ID (X11 WM_CLASS for XWayland clients).

_title_
	The window title.

_identifier_
	A stable identifier which is unique for the lifetime of the toplevel.

_states_
	Any of _maximized_, _minimized_, _activated_, _fullscreen_ and
	_sticky_.

_geometry_
	An array with one entry per output the toplevel is visible on. Each
	entry has _output_ (the output name), _x_ and _y_ (relative to that
	output), _width_ and _height_, plus _global_x_ and _global_y_
	(relative to the whole output layout).

The commands that set geometry take _global_x_ / _global_y_ as input, so the
values reported by _list_ can be fed straight back in.

# EXAMPLES

Print the position of every window:

	labwcctl list | jq -r '.[] | "\(.app_id)\t\(.geometry[0].global_x),\(.geometry[0].global_y)"'

Put every terminal in the top left quadrant:

	labwcctl list | jq -r '.[] | select(.app_id == "foot") | .identifier' |
	while read -r id; do labwcctl move-resize "$id" 0 0 640 480; done

Wait for a window to appear:

	labwcctl list --watch | jq -c '.[0]' | head -n 1

Focus a window by its stable identifier:

	id=$(labwcctl list | jq -r '.[] | select(.app_id == "foot") | .identifier')
	labwcctl focus "$id"

Close every window of an application:

	labwcctl close 'app_id:foot'

Tile two windows over the left and right half of a 1280x720 output:

	labwcctl move-resize 'app_id:foot' 0 0 640 720
	labwcctl move-resize 'title:no-snap-test' 640 0 640 720

Make a window fullscreen, then put it back:

	labwcctl fullscreen 'app_id:mpv'
	labwcctl fullscreen 'app_id:mpv' off

# SEE ALSO

_labwc_(1), _labwc-actions_(5), _labwc-config_(5)

# AUTHORS

Maintained by the labwc contributors.
