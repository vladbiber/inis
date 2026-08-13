#include "dispatch.h"

#include "backend.h"
#include "log.h"
#include "server.h"
#include "window.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

static void dispatch_exec(struct inis_server *server, const char *args);
static void dispatch_exit(struct inis_server *server, const char *args);
static void dispatch_shutdownmenu(struct inis_server *server, const char *args);
static void dispatch_killactive(struct inis_server *server, const char *args);
static void dispatch_closewindow(struct inis_server *server, const char *args);
static void dispatch_togglefloating(struct inis_server *server, const char *args);
static void dispatch_fullscreen(struct inis_server *server, const char *args);
static void dispatch_centerwindow(struct inis_server *server, const char *args);
static void dispatch_moveactive(struct inis_server *server, const char *args);
static void dispatch_resizeactive(struct inis_server *server, const char *args);
static void dispatch_swapwindow(struct inis_server *server, const char *args);
static void dispatch_pseudo(struct inis_server *server, const char *args);
static void dispatch_togglesplit(struct inis_server *server, const char *args);
static void dispatch_workspace(struct inis_server *server, const char *args);
static void dispatch_movetoworkspace(struct inis_server *server, const char *args);
static void dispatch_movetoworkspacesilent(struct inis_server *server, const char *args);
static void dispatch_previous(struct inis_server *server, const char *args);
static void dispatch_movefocus(struct inis_server *server, const char *args);
static void dispatch_cyclenext(struct inis_server *server, const char *args);
static void dispatch_cycleprev(struct inis_server *server, const char *args);
static void dispatch_togglespecialworkspace(struct inis_server *server, const char *args);
static void dispatch_movewindow(struct inis_server *server, const char *args);
static void dispatch_resizewindow(struct inis_server *server, const char *args);
static void dispatch_reserved(struct inis_server *server, const char *args);
static void dispatch_reload(struct inis_server *server, const char *args);

static const struct inis_dispatcher dispatchers[] = {
	{ "exec", dispatch_exec },
	{ "exit", dispatch_exit },
	{ "shutdownmenu", dispatch_shutdownmenu },
	{ "killactive", dispatch_killactive },
	{ "closewindow", dispatch_closewindow },
	{ "togglefloating", dispatch_togglefloating },
	{ "fullscreen", dispatch_fullscreen },
	{ "centerwindow", dispatch_centerwindow },
	{ "moveactive", dispatch_moveactive },
	{ "resizeactive", dispatch_resizeactive },
	{ "swapwindow", dispatch_swapwindow },
	{ "pseudo", dispatch_pseudo },
	{ "togglesplit", dispatch_togglesplit },
	{ "workspace", dispatch_workspace },
	{ "movetoworkspace", dispatch_movetoworkspace },
	{ "movetoworkspacesilent", dispatch_movetoworkspacesilent },
	{ "previous", dispatch_previous },
	{ "movefocus", dispatch_movefocus },
	{ "cyclenext", dispatch_cyclenext },
	{ "cycleprev", dispatch_cycleprev },
	{ "togglespecialworkspace", dispatch_togglespecialworkspace },
	{ "movewindow", dispatch_movewindow },
	{ "resizewindow", dispatch_resizewindow },
	{ "reserved", dispatch_reserved },
	{ "reload", dispatch_reload },
};

int
inis_dispatch(struct inis_server *server, const char *name, const char *args)
{
	size_t i;

	for (i = 0; i < sizeof(dispatchers) / sizeof(dispatchers[0]); i++) {
		if (strcmp(dispatchers[i].name, name) == 0) {
			dispatchers[i].run(server, args ? args : "");
			return 0;
		}
	}

	inis_warn("unknown dispatcher: %s", name);
	return -1;
}

static void
dispatch_exec(struct inis_server *server, const char *args)
{
	pid_t pid;

	(void)server;
	if (args == NULL || args[0] == '\0') {
		inis_warn("exec requires a command");
		return;
	}

	pid = fork();
	if (pid < 0) {
		inis_warn("fork failed for exec: %s", strerror(errno));
		return;
	}
	if (pid == 0) {
		setsid();
		execl("/bin/sh", "sh", "-c", args, (char *)NULL);
		_exit(127);
	}
}

static void
dispatch_exit(struct inis_server *server, const char *args)
{
	(void)args;
	inis_server_request_exit(server);
}

static bool
path_has_executable(const char *name)
{
	char path[1024];
	char full[1024];
	char *dir;
	const char *env;

	env = getenv("PATH");
	if (env == NULL || env[0] == '\0')
		return false;

	snprintf(path, sizeof(path), "%s", env);
	dir = strtok(path, ":");
	while (dir != NULL) {
		if (dir[0] == '\0')
			dir = ".";
		snprintf(full, sizeof(full), "%s/%s", dir, name);
		if (access(full, X_OK) == 0)
			return true;
		dir = strtok(NULL, ":");
	}

	return false;
}

static void
dispatch_shutdownmenu(struct inis_server *server, const char *args)
{
	(void)args;
	if (path_has_executable("hyprshutdown")) {
		dispatch_exec(server, "hyprshutdown");
		return;
	}
	inis_info("hyprshutdown not found; exiting compositor");
	inis_server_request_exit(server);
}

static void
dispatch_killactive(struct inis_server *server, const char *args)
{
	(void)args;
	if (server->focused_window == NULL) {
		inis_debug("killactive ignored: no focused window");
		return;
	}
	inis_backend_close_window(&server->backend, server->focused_window);
}

static void
dispatch_closewindow(struct inis_server *server, const char *args)
{
	dispatch_killactive(server, args);
}

static void
dispatch_togglefloating(struct inis_server *server, const char *args)
{
	struct inis_window *window;
	struct inis_rect area;
	bool special;
	bool transient;

	(void)args;
	if (server->focused_window == NULL)
		return;
	window = server->focused_window;
	special = window->workspace_index < server->workspace_count &&
	    server->workspaces[window->workspace_index].special;
	transient = window->transient;
	if (server->monitor_count > window->monitor_index)
		area = server->monitors[window->monitor_index].usable;
	else
		area = (struct inis_rect){ 0, 0, 800, 600 };
	if (special || transient) {
		if (!inis_window_make_floating_centered(window, &area))
			inis_window_center_floating(window, &area);
	} else if (window->state == INIS_WINDOW_FLOATING) {
		inis_window_set_floating(window, false);
	} else if (!inis_window_make_floating_centered(window, &area)) {
		return;
	}
	inis_server_arrange(server);
	(void)inis_server_focus_window(server, window);
	inis_server_flush_damage(server, "togglefloating");
}

static void
dispatch_fullscreen(struct inis_server *server, const char *args)
{
	(void)args;
	if (server->focused_window == NULL)
		return;
	(void)inis_server_request_fullscreen(server, server->focused_window, NULL,
	    server->focused_window->state != INIS_WINDOW_FULLSCREEN);
}

static int
parse_delta(const char *args, int *a, int *b)
{
	if (args == NULL || args[0] == '\0')
		return -1;
	return sscanf(args, "%d %d", a, b) == 2 ? 0 : -1;
}

static void
dispatch_centerwindow(struct inis_server *server, const char *args)
{
	(void)args;
	if (inis_server_center_focused(server) != 0)
		inis_debug("centerwindow ignored: no focused window");
}

static void
dispatch_moveactive(struct inis_server *server, const char *args)
{
	int dx;
	int dy;

	if (parse_delta(args, &dx, &dy) != 0) {
		inis_warn("moveactive requires: DX DY");
		return;
	}

	/*
	 * In tiling mode: move = swap position with the adjacent tiled window
	 * in the requested direction (positive dx/dy → next, negative → prev).
	 * This keeps the window tiled and never converts it to floating.
	 * In floating/fullscreen mode: move by pixel delta as before.
	 */
	if (server->focused_window != NULL &&
	    server->focused_window->state == INIS_WINDOW_TILED) {
		int direction = (dx > 0 || dy > 0) ? 1 : -1;
		if (inis_server_swap_focused(server, direction) != 0)
			inis_debug("moveactive (tiled): no neighbor in this direction");
		return;
	}

	if (inis_server_move_focused(server, dx, dy) != 0)
		inis_debug("moveactive ignored: no focused window");
}

static void
dispatch_resizeactive(struct inis_server *server, const char *args)
{
	int dw;
	int dh;

	if (parse_delta(args, &dw, &dh) != 0) {
		inis_warn("resizeactive requires: DW DH");
		return;
	}
	if (inis_server_resize_focused(server, dw, dh) != 0)
		inis_debug("resizeactive ignored: no focused window");
}

static void
dispatch_swapwindow(struct inis_server *server, const char *args)
{
	int direction = 1;

	if (args != NULL &&
	    (strcmp(args, "left") == 0 || strcmp(args, "up") == 0))
		direction = -1;
	if (inis_server_swap_focused(server, direction) != 0)
		inis_debug("swapwindow ignored: no tiled neighbor");
}

static void
dispatch_pseudo(struct inis_server *server, const char *args)
{
	(void)server;
	(void)args;
	inis_debug("pseudo ignored: dwindle pseudo mode is not implemented");
}

static void
dispatch_togglesplit(struct inis_server *server, const char *args)
{
	(void)args;
	if (inis_server_toggle_split(server) != 0)
		inis_debug("togglesplit ignored: no tiled focused window");
}

static void
dispatch_workspace(struct inis_server *server, const char *args)
{
	if (strcmp(args, "e+1") == 0 || strcmp(args, "+1") == 0) {
		if (inis_server_switch_relative_workspace(server, 1) != 0)
			inis_warn("workspace next failed");
		return;
	}
	if (strcmp(args, "e-1") == 0 || strcmp(args, "-1") == 0) {
		if (inis_server_switch_relative_workspace(server, -1) != 0)
			inis_warn("workspace previous failed");
		return;
	}
	if (strcmp(args, "previous") == 0) {
		if (inis_server_switch_previous_workspace(server) != 0)
			inis_warn("workspace previous failed");
		return;
	}
	if (inis_server_switch_workspace(server, args) != 0)
		inis_warn("workspace failed: %s", args);
}

static void
dispatch_movetoworkspace(struct inis_server *server, const char *args)
{
	if (inis_server_move_focused_to_workspace(server, args, false) != 0)
		inis_warn("movetoworkspace failed: %s", args);
}

static void
dispatch_movetoworkspacesilent(struct inis_server *server, const char *args)
{
	if (inis_server_move_focused_to_workspace(server, args, true) != 0)
		inis_warn("movetoworkspacesilent failed: %s", args);
}

static void
dispatch_previous(struct inis_server *server, const char *args)
{
	(void)args;
	if (inis_server_switch_previous_workspace(server) != 0)
		inis_warn("previous workspace failed");
}

static void
dispatch_movefocus(struct inis_server *server, const char *args)
{
	if (args != NULL) {
		if (strcmp(args, "left") == 0) {
			if (inis_server_focus_direction(server, WC_DIRECTION_LEFT) != 0)
				inis_debug("movefocus left ignored");
			return;
		}
		if (strcmp(args, "right") == 0) {
			if (inis_server_focus_direction(server, WC_DIRECTION_RIGHT) != 0)
				inis_debug("movefocus right ignored");
			return;
		}
		if (strcmp(args, "up") == 0) {
			if (inis_server_focus_direction(server, WC_DIRECTION_UP) != 0)
				inis_debug("movefocus up ignored");
			return;
		}
		if (strcmp(args, "down") == 0) {
			if (inis_server_focus_direction(server, WC_DIRECTION_DOWN) != 0)
				inis_debug("movefocus down ignored");
			return;
		}
	}
	inis_debug("movefocus ignored: no focusable window");
}

static void
dispatch_cyclenext(struct inis_server *server, const char *args)
{
	(void)args;
	if (inis_server_focus_next(server, 1) != 0)
		inis_debug("cyclenext ignored: no focusable window");
}

static void
dispatch_cycleprev(struct inis_server *server, const char *args)
{
	(void)args;
	if (inis_server_focus_next(server, -1) != 0)
		inis_debug("cycleprev ignored: no focusable window");
}

static void
dispatch_togglespecialworkspace(struct inis_server *server, const char *args)
{
	if (inis_server_toggle_special_workspace(server, args) != 0)
		inis_warn("togglespecialworkspace failed: %s", args);
}

static void
dispatch_movewindow(struct inis_server *server, const char *args)
{
	(void)args;
	if (inis_server_begin_mouse_move(server) != 0)
		inis_debug("movewindow ignored: no focused window");
}

static void
dispatch_resizewindow(struct inis_server *server, const char *args)
{
	(void)args;
	if (inis_server_begin_mouse_resize(server, INIS_WINDOW_EDGE_AUTO) != 0)
		inis_debug("resizewindow ignored: no focused window");
}

static void
dispatch_reserved(struct inis_server *server, const char *args)
{
	char name[INIS_MAX_NAME];
	int top, bottom, left, right;
	int matched;

	if (args == NULL || args[0] == '\0') {
		inis_warn("reserved requires: [MONITOR] TOP BOTTOM LEFT RIGHT");
		return;
	}

	/* Named-monitor form: MONITOR TOP BOTTOM LEFT RIGHT. */
	if (sscanf(args, "%63s %d %d %d %d", name, &top, &bottom, &left,
	    &right) == 5) {
		matched = inis_server_set_monitor_reserved(server, name, top,
		    bottom, left, right);
		if (matched == 0)
			inis_warn("reserved: no monitor named %s", name);
		return;
	}

	/* Global form: TOP BOTTOM LEFT RIGHT (all monitors). */
	if (sscanf(args, "%d %d %d %d", &top, &bottom, &left, &right) == 4) {
		matched = inis_server_set_monitor_reserved(server, NULL, top,
		    bottom, left, right);
		if (matched == 0)
			inis_warn("reserved: no monitors connected");
		return;
	}

	inis_warn("reserved requires: [MONITOR] TOP BOTTOM LEFT RIGHT");
}

static void
dispatch_reload(struct inis_server *server, const char *args)
{
	(void)args;
	inis_server_reload_config(server);
}
