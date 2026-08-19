tclzmachine is a loadable Tcl extension that embeds a Z-machine interpreter for running classic Infocom and other Z-code interactive fiction games. It provides a simple Tcl API for creating independent game sessions, sending player commands, and receiving the game's textual response as a Tcl string.

The extension is designed for server-side and automated applications where multiple Z-machine sessions may run simultaneously without requiring interactive terminals or external processes. Each session maintains its own game state, allowing Tcl applications to manage players, persistence, networking, and other higher-level functionality while the native extension handles Z-machine execution.

The project targets Tcl 8.6+ on Linux and is built around an existing open-source Z-machine interpreter core, adapted for embedded, session-oriented operation.
