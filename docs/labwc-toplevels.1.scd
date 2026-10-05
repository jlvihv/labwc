labwc-toplevels(1)

# NAME

labwc-toplevels - Print the geometry and state of all toplevels as JSON

# SYNOPSIS

_labwc-toplevels_ [options...]

# DESCRIPTION

Prints a JSON array describing every toplevel the compositor exposes to
taskbars, pagers and other privileged clients.

The toplevel list comes from *ext-foreign-toplevel-list-v1*, which provides
a stable _identifier_ for each toplevel. Geometry, state and output
membership come from *zcosmic-toplevel-info-v1*.

Note that *zcosmic-toplevel-info-v1* reports geometry relative to the output
a toplevel is visible on. _labwc-toplevels_ resolves each output's position
from *wl_output.geometry* and reports compositor-global coordinates as well.

# OPTIONS

*-h, --help*
	Show help message and quit.

*-w, --watch*
	Keep running and print a new snapshot whenever something changes
	instead of printing one snapshot and exiting.

# OUTPUT

The output is a JSON array. Each element has the following keys:

	_app_id_
		The application ID (X11 WM_CLASS for XWayland clients).

	_title_
		The window title.

	_identifier_
		A stable identifier which is unique for the lifetime of the
		toplevel. Useful for referring to a window across multiple
		invocations.

	_states_
		Any of _maximized_, _minimized_, _activated_, _fullscreen_ and
		_sticky_.

	_geometry_
		An array with one entry per output the toplevel is visible on.
		Each entry has _output_ (the output name), _x_ and _y_ (relative
		to that output), _width_ and _height_, plus _global_x_ and
		_global_y_ (relative to the whole output layout).

# EXAMPLES

Print the position of every window:

	labwc-toplevels | jq -r '.[] | "\(.app_id)\t\(.geometry[0].global_x),\(.geometry[0].global_y)"'

Wait for the first window to appear and print its geometry:

	labwc-toplevels --watch | jq -c '.[0].geometry[0]' | head -n 1

# SEE ALSO

_labwc_(1)

# AUTHORS

Maintained by the labwc contributors.
