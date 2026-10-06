labwcctl(1)

# NAME

labwcctl - Query and control toplevels

# SYNOPSIS

_labwcctl_ [options...] _list_ [--watch]

_labwcctl_ [options...] _cycle_ [next|prev]

_labwcctl_ [options...] _move_|_move-by_ <selector> <x> <y>

_labwcctl_ [options...] _resize_|_resize-by_ <selector> <width> <height>

_labwcctl_ [options...] _move-resize_ <selector> <x> <y> <width> <height>

_labwcctl_ [options...] _center_ <selector>

_labwcctl_ [options...] _snap-to-edge_|_grow-to-edge_|_shrink-to-edge_ <selector> <edge>

_labwcctl_ [options...] _focus_ <selector>

_labwcctl_ [options...] _close_ <selector>

_labwcctl_ [options...] _maximize_|_minimize_|_fullscreen_|_sticky_|_shade_ <selector> [on|off]

_labwcctl_ [options...] _layer_ <selector> <normal|top|bottom>

_labwcctl_ [options...] _decorations_ <selector> <none|border|full>

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

*cycle* [next|prev]
	Move keyboard focus to the next or previous toplevel in the window
	switcher order, without opening the window switcher overlay. Defaults to
	_next_. This acts on whichever toplevel is active, so it takes no
	selector.

*move* <selector> <x> <y>
	Move matching toplevels to the given position, keeping their size.

*move-by* <selector> <dx> <dy>
	Move matching toplevels relative to their current position. Negative
	values move left or up. Has no effect on a fullscreen toplevel.

*resize* <selector> <width> <height>
	Resize matching toplevels, keeping their position.

*resize-by* <selector> <width> <height>
	Grow or shrink matching toplevels by moving their right and bottom
	edges. Negative values shrink. Has no effect on a fullscreen or
	maximized toplevel.

*move-resize* <selector> <x> <y> <width> <height>
	Move and resize matching toplevels in one step.

*center* <selector>
	Center matching toplevels in the usable area of the output they are on.

*snap-to-edge* <selector> <left|right|up|down> [screen|windows]
	Move matching toplevels as far as they can go in the given direction.
	_screen_ stops at the edge of the output, _windows_ also stops at the
	edge of another toplevel. Defaults to _screen_.

*grow-to-edge* <selector> <left|right|up|down>
	Grow matching toplevels towards the given edge, by a maximum of 50%.

*shrink-to-edge* <selector> <left|right|up|down>
	The reverse of _grow-to-edge_. Note that the direction names the edge
	that stays put: _shrink-to-edge right_ keeps the right edge and pulls
	the left edge in.

*focus* <selector>
	Raise matching toplevels and give them keyboard focus. With more than
	one match, the last one wins.

*close* <selector>
	Ask matching toplevels to close. The client is free to ignore this, for
	example to ask the user to save unsaved changes first, so this does not
	guarantee that the window goes away. A toplevel that is still there
	after being asked to close is reported on stderr.

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

*shade* <selector> [on|off]
	Roll matching toplevels up so that only their title bar is left, or
	unroll them. Defaults to _on_. Has no effect on toplevels without
	server side decorations.

*layer* <selector> <normal|top|bottom>
	Put matching toplevels in the given stacking layer. _top_ is what labwc
	calls always-on-top, _bottom_ always-on-bottom.

*decorations* <selector> <none|border|full>
	Set how much of the decorations matching toplevels get. Ignored when
	the toplevel requests decorations from the client and the requested mode
	would add to them, in the same way as the SetDecorations action.

Every command except _list_ prints the resulting snapshot, so that callers
can tell whether the request had the intended effect.

A change is not synchronous: the compositor sends a configure event, the
client has to commit, and only then is the new geometry reported. _labwcctl_
therefore waits for the specific thing it asked for rather than for a fixed
period:

- geometry commands wait for a geometry event from each toplevel they acted on
- _focus_ waits for the active toplevel to change
- _maximize_ and _fullscreen_ wait for the state *and* the geometry, because
  the compositor reports the new state as soon as it has configured the
  client, and waiting for the state alone would report the new state next to
  the geometry that went with the old one
- _minimize_ and _sticky_ wait for the state, which is all they change
- _close_ waits for the toplevel to go away
- _layer_, _shade_ and _decorations_ wait for the acknowledgement described
  below

Each wait is bounded by 500ms, and a short quiet period afterwards collects
any follow-up events from a client which acknowledges a configure in stages.
A request that changes nothing produces no events at all, and that cannot be
told apart from a request the compositor ignored: an already centered
toplevel, a client that snaps its size to a grid, and a client that declines
to close all look the same. A wait that finds nothing is therefore not an
error, and the snapshot is the answer.

The three properties which *zcosmic-toplevel-info-v1* does not report - the
layer, whether the toplevel is shaded, and its decoration mode - are read back
from *labwc_control_v1* instead. Those commands are therefore verified: a
value in the snapshot which does not match what was asked for means the
request was refused, which is reported on stderr. This also means _list_ can
report them, as _null_ if the compositor does not offer *labwc_control_v1*.

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

_layer_
	Any of _normal_, _top_ and _bottom_, or _null_ if the compositor does
	not offer *labwc_control_v1*.

_shaded_
	Whether the toplevel is rolled up, or _null_ if the compositor does
	not offer *labwc_control_v1*.

_decorations_
	Any of _none_, _border_ and _full_, or _null_ if the compositor does
	not offer *labwc_control_v1*.

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

Lay out two windows side by side using the edges of the output:

	labwcctl snap-to-edge 'app_id:foot' left
	labwcctl snap-to-edge 'title:no-snap-test' right

Move a window against its neighbour rather than the screen edge:

	labwcctl snap-to-edge 'app_id:foot' right windows

Step through the windows without opening the switcher overlay:

	labwcctl cycle next
	labwcctl cycle prev

# SEE ALSO

_labwc_(1), _labwc-actions_(5), _labwc-config_(5)

# AUTHORS

Maintained by the labwc contributors.
