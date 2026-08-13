#include "server.h"

#include "dispatch.h"
#include "layout.h"
#include "log.h"
#include "render.h"

#if INIS_HAVE_SWC
#include <swc.h>
#endif

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
load_user_config(struct inis_server *server)
{
	const char *home;
	char path[512];
	FILE *probe;

	home = getenv("HOME");
	if (home == NULL || home[0] == '\0')
		return;

	snprintf(path, sizeof(path), "%s/.config/inis/inis.conf", home);

	probe = fopen(path, "r");
	if (probe == NULL)
		return;
	fclose(probe);

	/*
	 * User config exists — clear default bindings so the config file
	 * has full ownership. Without this, duplicate keys register twice
	 * and the default always wins (it was registered first).
	 */
	server->binding_count = 0;
	server->rule_count = 0;

	if (inis_config_load_file(&server->config, server->bindings,
	    &server->binding_count, server->rules, &server->rule_count, path) == 0) {
		inis_info("loaded config: %s", path);
		inis_info("loaded %zu bindings and %zu window rules",
		    server->binding_count, server->rule_count);
	}
}

static void
add_default_bind(struct inis_server *server, const char *line)
{
	if (server->binding_count >= INIS_MAX_BINDINGS)
		return;
	if (inis_bind_parse_line(&server->bindings[server->binding_count], line) == 0)
		server->binding_count++;
	else
		inis_warn("invalid default binding: %s", line);
}

static void
load_default_binds(struct inis_server *server)
{
	add_default_bind(server, "bind $mainMod Q exec kitty");
	add_default_bind(server, "bind $mainMod C killactive");
	add_default_bind(server, "bind $mainMod M shutdownmenu");
	add_default_bind(server, "bind $mainMod E exec dolphin");
	add_default_bind(server, "bind $mainMod V togglefloating");
	add_default_bind(server, "bind $mainMod R exec hyprlauncher");
	add_default_bind(server, "bind $mainMod P pseudo");
	add_default_bind(server, "bind $mainMod J togglesplit");
	add_default_bind(server, "bind $mainMod S togglespecialworkspace magic");
	add_default_bind(server, "bind $mainMod Left movefocus left");
	add_default_bind(server, "bind $mainMod Down movefocus down");
	add_default_bind(server, "bind $mainMod Up movefocus up");
	add_default_bind(server, "bind $mainMod Right movefocus right");
	add_default_bind(server, "bind $mainMod 1 workspace 1");
	add_default_bind(server, "bind $mainMod 2 workspace 2");
	add_default_bind(server, "bind $mainMod 3 workspace 3");
	add_default_bind(server, "bind $mainMod 4 workspace 4");
	add_default_bind(server, "bind $mainMod 5 workspace 5");
	add_default_bind(server, "bind $mainMod 6 workspace 6");
	add_default_bind(server, "bind $mainMod 7 workspace 7");
	add_default_bind(server, "bind $mainMod 8 workspace 8");
	add_default_bind(server, "bind $mainMod 9 workspace 9");
	add_default_bind(server, "bind $mainMod 0 workspace 10");
	add_default_bind(server, "bind $mainMod SHIFT 1 movetoworkspace 1");
	add_default_bind(server, "bind $mainMod SHIFT 2 movetoworkspace 2");
	add_default_bind(server, "bind $mainMod SHIFT 3 movetoworkspace 3");
	add_default_bind(server, "bind $mainMod SHIFT 4 movetoworkspace 4");
	add_default_bind(server, "bind $mainMod SHIFT 5 movetoworkspace 5");
	add_default_bind(server, "bind $mainMod SHIFT 6 movetoworkspace 6");
	add_default_bind(server, "bind $mainMod SHIFT 7 movetoworkspace 7");
	add_default_bind(server, "bind $mainMod SHIFT 8 movetoworkspace 8");
	add_default_bind(server, "bind $mainMod SHIFT 9 movetoworkspace 9");
	add_default_bind(server, "bind $mainMod SHIFT 0 movetoworkspace 10");
	add_default_bind(server, "bind $mainMod SHIFT S movetoworkspace special:magic");
	add_default_bind(server, "bindm $mainMod mouse_down workspace e+1");
	add_default_bind(server, "bindm $mainMod mouse_up workspace e-1");
	add_default_bind(server, "bindm $mainMod mouse:272 movewindow");
	add_default_bind(server, "bindm $mainMod mouse:273 resizewindow");
}

static void
ignore_sigchld(void)
{
	signal(SIGCHLD, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);
}

static void
run_startup_commands(struct inis_server *server)
{
	size_t i;

	for (i = 0; i < server->config.startup_command_count; i++) {
		inis_dispatch(server, "exec", server->config.startup_commands[i]);
		inis_debug("exec-once started: %s",
		    server->config.startup_commands[i]);
	}
}

static int
workspace_find(struct inis_server *server, const char *name)
{
	size_t i;

	for (i = 0; i < server->workspace_count; i++) {
		if (strcmp(server->workspaces[i].name, name) == 0)
			return (int)i;
	}
	return -1;
}

static int
workspace_get_or_create(struct inis_server *server, const char *name, bool special)
{
	int index;

	index = workspace_find(server, name);
	if (index >= 0)
		return index;

	if (server->workspace_count >= INIS_MAX_WORKSPACES) {
		inis_warn("workspace ignored: limit reached");
		return -1;
	}

	index = (int)server->workspace_count++;
	inis_workspace_init(&server->workspaces[index], name, special);
	return index;
}

static unsigned int
server_active_workspace(const struct inis_server *server)
{
	if (server->monitor_count == 0)
		return 0;
	return server->monitors[0].active_workspace;
}

static bool
window_is_focusable(const struct inis_server *server,
    const struct inis_window *window)
{
	const struct inis_workspace *workspace;

	if (window == NULL || !window->mapped)
		return false;
	if (window->workspace_index >= server->workspace_count)
		return false;
	workspace = &server->workspaces[window->workspace_index];
	return window->workspace_index == server_active_workspace(server) ||
	    (workspace->special && workspace->visible);
}

static struct inis_workspace *
active_workspace_struct(struct inis_server *server)
{
	unsigned int index;

	if (server->monitor_count == 0)
		return NULL;
	index = server->monitors[0].active_workspace;
	if (index >= server->workspace_count)
		return NULL;
	return &server->workspaces[index];
}

static void
sync_layout_config(struct inis_server *server, struct inis_workspace *workspace)
{
	struct wc_layout_config *config;

	config = &workspace->layout.config;
	config->outer_gap = server->config.gaps_out;
	config->inner_gap = server->config.gaps_in;
	config->border_width = server->config.border_size;
	config->master_ratio = (float)server->config.master_ratio;
}

static bool
window_should_be_tiled(const struct inis_server *server,
    const struct inis_window *window)
{
	if (window == NULL || !window->mapped)
		return false;
	if (window->state != INIS_WINDOW_TILED)
		return false;
	if (window->workspace_index >= server->workspace_count)
		return false;
	return !server->workspaces[window->workspace_index].special;
}

static bool
window_in_special_workspace(const struct inis_server *server,
    const struct inis_window *window)
{
	if (window == NULL || window->workspace_index >= server->workspace_count)
		return false;
	return server->workspaces[window->workspace_index].special;
}

#if INIS_HAVE_SWC
static struct inis_window *
find_window_by_swc(struct inis_server *server, const struct swc_window *swc)
{
	size_t i;

	if (swc == NULL)
		return NULL;
	for (i = 0; i < server->window_count; i++) {
		if (!server->windows[i].mapped)
			continue;
		if (server->windows[i].swc == swc)
			return &server->windows[i];
	}
	return NULL;
}
#endif

static void
sync_window_layout_membership(struct inis_server *server, struct inis_window *window)
{
	struct inis_workspace *workspace;

	if (window == NULL)
		return;

	if (!window_should_be_tiled(server, window)) {
		if (window->layout_view.workspace != NULL)
			layout_remove_view(window->layout_view.workspace,
			    &window->layout_view);
		return;
	}

	workspace = &server->workspaces[window->workspace_index];
	sync_layout_config(server, workspace);
	if (window->layout_view.workspace != &workspace->layout) {
		if (window->layout_view.workspace != NULL)
			layout_remove_view(window->layout_view.workspace,
			    &window->layout_view);
		layout_add_view(&workspace->layout, &window->layout_view);
	}
}

static void
rebuild_workspace_layout(struct inis_server *server, size_t workspace_index)
{
	struct inis_workspace *workspace;
	struct inis_window *focused;
	size_t i;

	if (server == NULL || workspace_index >= server->workspace_count)
		return;

	workspace = &server->workspaces[workspace_index];
	focused = server->focused_window;
	layout_finish_workspace(&workspace->layout);
	sync_layout_config(server, workspace);

	for (i = 0; i < server->window_count; i++) {
		struct inis_window *window = &server->windows[i];

		if (!window->mapped || window->workspace_index != workspace_index)
			continue;
		if (!window_should_be_tiled(server, window))
			continue;
		layout_add_view(&workspace->layout, &window->layout_view);
	}

	if (focused != NULL && focused->mapped &&
	    focused->workspace_index == workspace_index &&
	    focused->layout_view.workspace == &workspace->layout)
		layout_focus_view(&workspace->layout, &focused->layout_view);
}

static struct inis_rect
window_current_rect(const struct inis_server *server,
    const struct inis_window *window)
{
	if (window != NULL && window->state == INIS_WINDOW_FULLSCREEN) {
		if (server->monitor_count > window->monitor_index)
			return server->monitors[window->monitor_index].geometry;
		/* Monitor removed while window was fullscreen — use first monitor. */
		if (server->monitor_count > 0)
			return server->monitors[0].geometry;
	}
	if (window->state == INIS_WINDOW_FLOATING ||
	    window->state == INIS_WINDOW_FULLSCREEN)
		return window->floating;
	return window->tiled;
}

/*
 * Recompute a monitor's effective usable area: start from the backend usable
 * area and inset each edge by the space reserved for external bars.  Reserved
 * insets are measured from the monitor geometry edges, so a top bar of height
 * H reserves the topmost H pixels regardless of where the backend usable area
 * begins.  If the reservations would leave nothing usable, they are ignored
 * and the backend usable area is kept.
 */
static void
monitor_recompute_usable(struct inis_monitor *monitor)
{
	const struct inis_rect *g = &monitor->geometry;
	const struct inis_rect *b = &monitor->backend_usable;
	int bx0 = b->x;
	int by0 = b->y;
	int bx1 = b->x + b->w;
	int by1 = b->y + b->h;
	int rx0 = g->x + monitor->reserved.left;
	int ry0 = g->y + monitor->reserved.top;
	int rx1 = g->x + g->w - monitor->reserved.right;
	int ry1 = g->y + g->h - monitor->reserved.bottom;
	int x0 = bx0 > rx0 ? bx0 : rx0;
	int y0 = by0 > ry0 ? by0 : ry0;
	int x1 = bx1 < rx1 ? bx1 : rx1;
	int y1 = by1 < ry1 ? by1 : ry1;

	if (x1 <= x0 || y1 <= y0) {
		inis_warn("reserved insets exceed monitor %s usable area; ignoring",
		    monitor->name);
		monitor->usable = monitor->backend_usable;
		return;
	}

	monitor->usable.x = x0;
	monitor->usable.y = y0;
	monitor->usable.w = x1 - x0;
	monitor->usable.h = y1 - y0;
}

static struct inis_rect
window_usable_area(const struct inis_server *server,
    const struct inis_window *window)
{
	if (window != NULL && server->monitor_count > window->monitor_index)
		return server->monitors[window->monitor_index].usable;
	return (struct inis_rect){ 0, 0, 800, 600 };
}

static bool
window_make_directly_managed(struct inis_server *server,
    struct inis_window *window)
{
	struct inis_rect area;

	area = window_usable_area(server, window);
	return inis_window_make_floating(window, &area);
}

static void
raise_visible_non_tiled_windows(struct inis_server *server)
{
	struct inis_window *focused;
	const struct inis_workspace *workspace;
	size_t active_workspace;
	size_t i;

	if (server->monitor_count == 0)
		return;

	active_workspace = server->monitors[0].active_workspace;
	focused = server->focused_window;

	for (i = 0; i < server->window_count; i++) {
		if (!server->windows[i].mapped)
			continue;
		if (server->windows[i].workspace_index >= server->workspace_count)
			continue;
		if (server->windows[i].state == INIS_WINDOW_TILED)
			continue;
		if (&server->windows[i] == focused)
			continue;
		if (server->windows[i].workspace_index != active_workspace) {
			workspace = &server->workspaces[server->windows[i].workspace_index];
			if (!workspace->special || !workspace->visible)
				continue;
		}
		inis_backend_raise_window(&server->backend, &server->windows[i]);
	}

	if (focused == NULL || !focused->mapped ||
	    focused->workspace_index >= server->workspace_count ||
	    focused->state == INIS_WINDOW_TILED)
		return;
	if (focused->workspace_index != active_workspace) {
		workspace = &server->workspaces[focused->workspace_index];
		if (!workspace->special || !workspace->visible)
			return;
	}
	inis_backend_raise_window(&server->backend, focused);
}

static void
damage_window(struct inis_server *server, const struct inis_window *window,
    const char *reason)
{
	struct inis_rect rect;

	if (window == NULL)
		return;
	rect = window_current_rect(server, window);
	inis_server_mark_damage(server, &rect, reason);
}

static void
warp_pointer_to_window(struct inis_server *server, const struct inis_window *window)
{
	struct inis_rect rect;

	if (server == NULL || window == NULL || !window->mapped)
		return;
	rect = window_current_rect(server, window);
	if (rect.w <= 0 || rect.h <= 0)
		return;
	inis_backend_warp_pointer(&server->backend,
	    rect.x + rect.w / 2, rect.y + rect.h / 2);
}

static void
damage_monitor(struct inis_server *server, const struct inis_monitor *monitor,
    const char *reason)
{
	if (monitor == NULL)
		return;
	inis_server_mark_damage(server, &monitor->geometry, reason);
}

static void
damage_all_monitors(struct inis_server *server, const char *reason)
{
	size_t i;

	for (i = 0; i < server->monitor_count; i++)
		damage_monitor(server, &server->monitors[i], reason);
}

static void
mark_damage_on_outputs(struct inis_server *server, const struct inis_rect *rect,
    const char *reason)
{
	size_t i;

	for (i = 0; i < server->monitor_count; i++) {
		struct inis_monitor *mon = &server->monitors[i];
		struct inis_rect inter;
		int x1, y1, x2, y2;
		int mx2, my2;

		mx2 = mon->geometry.x + mon->geometry.w;
		my2 = mon->geometry.y + mon->geometry.h;
		x1 = rect->x > mon->geometry.x ? rect->x : mon->geometry.x;
		y1 = rect->y > mon->geometry.y ? rect->y : mon->geometry.y;
		x2 = (rect->x + rect->w) < mx2 ? (rect->x + rect->w) : mx2;
		y2 = (rect->y + rect->h) < my2 ? (rect->y + rect->h) : my2;

		if (x2 <= x1 || y2 <= y1)
			continue;

		inter.x = x1;
		inter.y = y1;
		inter.w = x2 - x1;
		inter.h = y2 - y1;
		inis_damage_add_rect(&mon->damage, &inter, reason);
	}
}

static int
clamp_int(int value, int min, int max)
{
	if (value < min)
		return min;
	if (value > max)
		return max;
	return value;
}

static int
abs_int(int value)
{
	return value < 0 ? -value : value;
}

static int
rect_center_x(const struct inis_rect *rect)
{
	return rect->x + rect->w / 2;
}

static int
rect_center_y(const struct inis_rect *rect)
{
	return rect->y + rect->h / 2;
}

static bool
rects_overlap_on_secondary_axis(const struct inis_rect *a,
    const struct inis_rect *b, enum wc_direction direction)
{
	if (direction == WC_DIRECTION_LEFT || direction == WC_DIRECTION_RIGHT)
		return a->y < b->y + b->h && b->y < a->y + a->h;
	return a->x < b->x + b->w && b->x < a->x + a->w;
}

static void
center_rect_in_area(struct inis_rect *rect, const struct inis_rect *anchor,
    const struct inis_rect *area)
{
	int min_x, max_x, min_y, max_y;

	if (rect == NULL || anchor == NULL || area == NULL)
		return;

	rect->x = anchor->x + (anchor->w - rect->w) / 2;
	rect->y = anchor->y + (anchor->h - rect->h) / 2;

	min_x = area->x;
	min_y = area->y;
	max_x = area->x + area->w - rect->w;
	max_y = area->y + area->h - rect->h;
	if (max_x < min_x)
		max_x = min_x;
	if (max_y < min_y)
		max_y = min_y;

	rect->x = clamp_int(rect->x, min_x, max_x);
	rect->y = clamp_int(rect->y, min_y, max_y);
}

static bool
sync_transient_window(struct inis_server *server, struct inis_window *window)
{
#if INIS_HAVE_SWC
	struct inis_window *parent;
	struct inis_rect area;
	struct inis_rect parent_rect;
	bool was_transient;
	bool changed = false;
	bool placed = false;

	if (window == NULL || window->swc == NULL) {
		if (window != NULL)
			window->transient = false;
		return false;
	}

	was_transient = window->transient;
	window->transient = window->swc->parent != NULL;
	if (!window->transient)
		return was_transient;

	parent = find_window_by_swc(server, window->swc->parent);
	if (parent == NULL || !parent->mapped)
		return was_transient != window->transient;

	if (window->workspace_index != parent->workspace_index) {
		window->workspace_index = parent->workspace_index;
		changed = true;
	}
	if (window->monitor_index != parent->monitor_index) {
		window->monitor_index = parent->monitor_index;
		changed = true;
	}

	area = window_usable_area(server, parent);
	if (window->state == INIS_WINDOW_FULLSCREEN) {
		inis_window_set_fullscreen(window, false);
		changed = true;
	}
	if (window->state != INIS_WINDOW_FLOATING) {
		if (!inis_window_rect_valid(&window->floating))
			inis_window_ensure_floating_rect(window, &area);
		inis_window_set_floating(window, true);
		changed = true;
		placed = true;
	} else if (!inis_window_rect_valid(&window->floating)) {
		inis_window_ensure_floating_rect(window, &area);
		changed = true;
		placed = true;
	} else if (!was_transient) {
		placed = true;
	}

	if (placed) {
		parent_rect = window_current_rect(server, parent);
		center_rect_in_area(&window->floating, &parent_rect, &area);
	}

	return changed;
#else
	(void)server;
	if (window != NULL)
		window->transient = false;
	return false;
#endif
}

static int
monitor_index_from_backend(const struct inis_server *server, void *backend_monitor)
{
	size_t i;

	if (backend_monitor == NULL)
		return -1;
	for (i = 0; i < server->monitor_count; i++) {
		if (server->monitors[i].swc == backend_monitor)
			return (int)i;
	}
	return -1;
}

static void
apply_window_rules(struct inis_server *server, struct inis_window *window)
{
	size_t i;
	struct inis_rect area;

	area = window_usable_area(server, window);

	for (i = 0; i < server->rule_count; i++) {
		struct inis_rule *rule = &server->rules[i];
		int a;
		int b;
		int workspace_index;

		if (!inis_rule_matches(rule, window))
			continue;

		switch (rule->action) {
		case INIS_RULE_FLOAT:
			(void)inis_window_make_floating(window, &area);
			break;
		case INIS_RULE_TILE:
			inis_window_set_floating(window, false);
			break;
		case INIS_RULE_FULLSCREEN:
			inis_window_set_fullscreen(window, true);
			break;
		case INIS_RULE_CENTER:
			(void)inis_window_make_floating(window, &area);
			inis_window_center_floating(window, &area);
			break;
		case INIS_RULE_WORKSPACE:
			workspace_index = workspace_get_or_create(server, rule->args,
			    strncmp(rule->args, "special:", 8) == 0);
			if (workspace_index >= 0) {
				window->workspace_index = (unsigned int)workspace_index;
				if (server->workspaces[workspace_index].special)
					(void)inis_window_make_floating_centered(window, &area);
			}
			break;
		case INIS_RULE_SIZE:
			if (sscanf(rule->args, "%d %d", &a, &b) == 2 && a > 0 && b > 0) {
				(void)inis_window_make_floating(window, &area);
				window->floating.w = a;
				window->floating.h = b;
			}
			break;
		case INIS_RULE_MOVE:
			if (sscanf(rule->args, "%d %d", &a, &b) == 2) {
				(void)inis_window_make_floating(window, &area);
				window->floating.x = a;
				window->floating.y = b;
			}
			break;
		case INIS_RULE_NOBORDER:
			window->no_border = true;
			break;
		case INIS_RULE_NOANIM:
			window->no_anim = true;
			break;
		case INIS_RULE_MONITOR:
			inis_debug("monitor rule is parsed but not implemented yet: %s", rule->args);
			break;
		}
	}
}

int
inis_server_init(struct inis_server *server)
{
	memset(server, 0, sizeof(*server));
	server->running = true;
	server->socket_name = "inis-0";

	inis_config_init(&server->config);
	server->next_window_order = 1;
	inis_backend_init(&server->backend, server);
	inis_damage_init(&server->damage);
	inis_ipc_init(&server->ipc, server->socket_name);
	load_default_binds(server);
	load_user_config(server);

	inis_workspace_init(&server->workspaces[0], "1", false);
	server->workspaces[0].active = true;
	server->workspace_count = 1;
	ignore_sigchld();
	inis_ipc_start(server);

	inis_info("phase 0 init complete");
	return 0;
}

int
inis_server_run(struct inis_server *server)
{
	if (inis_backend_start(&server->backend) != 0) {
		inis_error("backend init failed: swc_initialize returned false");
		inis_dispatch(server, "exit", "");
		return -1;
	}

	run_startup_commands(server);
	return inis_backend_run(&server->backend);
}

void
inis_server_shutdown(struct inis_server *server)
{
	size_t i;

	inis_ipc_finish(&server->ipc);
	inis_backend_finish(&server->backend);
	for (i = 0; i < server->workspace_count; i++)
		inis_workspace_finish(&server->workspaces[i]);
	inis_info("shutdown complete");
}

void
inis_server_request_exit(struct inis_server *server)
{
	server->running = false;
	inis_backend_request_stop(&server->backend);
}

struct inis_monitor *
inis_server_add_monitor(struct inis_server *server, const char *name,
    const struct inis_rect *geometry, const struct inis_rect *usable,
    void *backend_monitor)
{
	struct inis_monitor *monitor;

	if (server->monitor_count >= INIS_MAX_MONITORS) {
		inis_warn("monitor ignored: limit reached");
		return NULL;
	}

	monitor = &server->monitors[server->monitor_count++];
	inis_monitor_init(monitor, name);
	monitor->geometry = *geometry;
	monitor->backend_usable = *usable;
	monitor->reserved.top = server->config.reserved_top;
	monitor->reserved.bottom = server->config.reserved_bottom;
	monitor->reserved.left = server->config.reserved_left;
	monitor->reserved.right = server->config.reserved_right;
	monitor_recompute_usable(monitor);
	monitor->swc = backend_monitor;
	monitor->active_workspace = 0;
	if (server->focused_monitor == NULL)
		server->focused_monitor = monitor;

	inis_info("monitor added: %s %dx%d+%d+%d usable %dx%d+%d+%d",
	    monitor->name,
	    monitor->geometry.w, monitor->geometry.h,
	    monitor->geometry.x, monitor->geometry.y,
	    monitor->usable.w, monitor->usable.h,
	    monitor->usable.x, monitor->usable.y);
	damage_monitor(server, monitor, "monitor-add");
	inis_server_flush_damage(server, "monitor-add");
	return monitor;
}

struct inis_window *
inis_server_add_window(struct inis_server *server, const char *app_id,
    const char *title, void *backend_window)
{
	struct inis_window *window;
	size_t i;

	for (i = 0; i < server->window_count; i++) {
		if (!server->windows[i].mapped)
			break;
	}

	if (i == server->window_count && server->window_count >= INIS_MAX_WINDOWS) {
		inis_warn("window ignored: limit reached");
		return NULL;
	}

	if (i == server->window_count)
		server->window_count++;

	window = &server->windows[i];
	inis_window_init(window);
	snprintf(window->app_id, sizeof(window->app_id), "%s", app_id ? app_id : "");
	snprintf(window->title, sizeof(window->title), "%s", title ? title : "");
	window->workspace_index = 0;
	window->monitor_index = 0;
	window->layout_order = server->next_window_order++;
	window->layout_view.id = window->layout_order;
	window->mapped = true;
	window->swc = backend_window;

	if (server->focused_monitor != NULL)
		window->workspace_index = server->focused_monitor->active_workspace;
	apply_window_rules(server, window);
	(void)sync_transient_window(server, window);
	sync_window_layout_membership(server, window);
	if (window->workspace_index < server->workspace_count)
		server->workspaces[window->workspace_index].window_count++;

	inis_info("window added: app_id=%s title=%s",
	    window->app_id, window->title);
	damage_window(server, window, "window-add");
	/*
	 * Defer arrange to the next event-loop idle.  Calling
	 * inis_server_arrange here (inside swc's new_window callback) would
	 * invoke swc_window_set_geometry on already-shown windows before they
	 * have acked their first configure.  Sending a second configure while
	 * a configure is still pending corrupts xdg_shell state and crashes
	 * swc — reproducible when two windows are opened within ~1 second.
	 */
	inis_backend_schedule_arrange(&server->backend);
	return window;
}

void
inis_server_remove_window(struct inis_server *server, struct inis_window *window)
{
	size_t workspace_index;
	size_t i;

	if (window == NULL)
		return;

	i = (size_t)(window - server->windows);
	if (i >= server->window_count || !window->mapped)
		return;

	workspace_index = window->workspace_index;
	if (window->workspace_index < server->workspace_count &&
	    server->workspaces[window->workspace_index].window_count > 0)
		server->workspaces[window->workspace_index].window_count--;

	if (window->layout_view.workspace != NULL)
		layout_remove_view(window->layout_view.workspace,
		    &window->layout_view);

	damage_window(server, window, "window-remove");
	if (server->focused_window == window)
		server->focused_window = NULL;
	if (server->pending_focus_window == window)
		server->pending_focus_window = NULL;

	window->mapped = false;
	window->focused = false;
	window->swc = NULL;

	rebuild_workspace_layout(server, workspace_index);

	/*
	 * Do NOT call inis_backend_focus_window(backend, NULL) here.  This runs
	 * inside swc's destroy handler (window_unmanage → handler->destroy),
	 * which fires from the xdg_toplevel resource destructor.  swc's
	 * swc_window_focus(NULL) unfocuses the current keyboard focus by calling
	 * the outgoing window's unfocus impl, which sends an xdg configure on
	 * toplevel->resource.  When a multi-window client (e.g. OBS) disconnects,
	 * wl_client_destroy tears its resources down in arbitrary order, so the
	 * focused sibling's toplevel resource may already be freed — posting the
	 * configure is a use-after-free that crashes the whole session.  swc
	 * clears keyboard focus safely by itself right after this handler, via
	 * the view destroy signal (handle_focus_view_destroy), without sending
	 * any event.  The idle arrange below picks the next focus target.
	 */

	/*
	 * Defer arrange to the next event-loop idle.  When a multi-window
	 * client exits (e.g. OBS), libwayland calls our destroy callback for
	 * every window synchronously inside wl_client_destroy.  Calling
	 * inis_server_arrange here would call swc_window_set_geometry on the
	 * other dying windows before their resources are cleaned up, causing
	 * use-after-free or reentrancy corruption.  The idle fires after all
	 * destroy callbacks for this batch complete.
	 */
	inis_backend_schedule_arrange(&server->backend);
}

void
inis_server_arrange(struct inis_server *server)
{
	struct inis_workspace *active_ws;
	struct wc_box area;
	unsigned int active_workspace = 0;
	size_t i;

	if (server->monitor_count == 0)
		return;

	/* repaint all outputs so removed/toggled windows leave no ghost */
	for (i = 0; i < server->monitor_count; i++) {
		inis_damage_add_rect(&server->monitors[i].damage,
		    &server->monitors[i].geometry, "arrange");
		inis_damage_add_rect(&server->damage,
		    &server->monitors[i].geometry, "arrange");
	}

	active_workspace = server->monitors[0].active_workspace;
	active_ws = active_workspace < server->workspace_count ?
	    &server->workspaces[active_workspace] : NULL;

	for (i = 0; i < server->workspace_count; i++)
		sync_layout_config(server, &server->workspaces[i]);

	for (i = 0; i < server->window_count; i++) {
		struct inis_workspace *workspace;
		bool show;

		if (!server->windows[i].mapped)
			continue;
		(void)sync_transient_window(server, &server->windows[i]);
		sync_window_layout_membership(server, &server->windows[i]);
		if (server->windows[i].workspace_index >= server->workspace_count) {
			inis_backend_set_window_visible(&server->backend, &server->windows[i], false);
			continue;
		}
		workspace = &server->workspaces[server->windows[i].workspace_index];
		show = server->windows[i].workspace_index == active_workspace ||
		    (workspace->special && workspace->visible);
		if (!show) {
			inis_backend_set_window_visible(&server->backend, &server->windows[i], false);
			continue;
		}

		if (server->windows[i].state != INIS_WINDOW_TILED) {
			inis_backend_set_window_visible(&server->backend, &server->windows[i], true);
			continue;
		}
	}

	if (active_ws != NULL) {
		area.x = server->monitors[0].usable.x;
		area.y = server->monitors[0].usable.y;
		area.w = server->monitors[0].usable.w;
		area.h = server->monitors[0].usable.h;
		if (server->focused_window != NULL &&
		    server->focused_window->state == INIS_WINDOW_TILED &&
		    server->focused_window->workspace_index == active_workspace)
			layout_focus_view(&active_ws->layout,
			    &server->focused_window->layout_view);
		layout_arrange(&active_ws->layout, area);
	}

	for (i = 0; i < server->window_count; i++) {
		struct inis_rect area;

		if (!server->windows[i].mapped)
			continue;
		if (server->windows[i].workspace_index >= server->workspace_count)
			continue;
		area = window_usable_area(server, &server->windows[i]);
		if (server->windows[i].state == INIS_WINDOW_TILED) {
			if (!window_in_special_workspace(server, &server->windows[i]))
				goto apply_tiled_window;
			(void)inis_window_make_floating_centered(&server->windows[i], &area);
		}
		/* ensure floating windows always have a valid rect */
		if (server->windows[i].state == INIS_WINDOW_FLOATING)
			inis_window_ensure_floating_rect(&server->windows[i], &area);
		inis_backend_apply_window(&server->backend, &server->windows[i]);
		continue;

apply_tiled_window:
		if (server->windows[i].layout_view.workspace != NULL &&
		    server->windows[i].workspace_index == active_workspace) {
			damage_window(server, &server->windows[i], "layout-old");
			server->windows[i].tiled.x =
			    server->windows[i].layout_view.pending_geometry.x;
			server->windows[i].tiled.y =
			    server->windows[i].layout_view.pending_geometry.y;
			server->windows[i].tiled.w =
			    server->windows[i].layout_view.pending_geometry.w;
			server->windows[i].tiled.h =
			    server->windows[i].layout_view.pending_geometry.h;
			damage_window(server, &server->windows[i], "layout-new");
			inis_backend_set_window_visible(&server->backend,
			    &server->windows[i],
			    server->windows[i].layout_view.tiled_visible);
		} else {
			inis_backend_set_window_visible(&server->backend,
			    &server->windows[i], false);
		}
		inis_backend_apply_window(&server->backend, &server->windows[i]);
	}

	raise_visible_non_tiled_windows(server);
}

/*
 * Compute and apply a freshly mapped window's geometry synchronously, before
 * it is shown.  In Wayland the client renders into whatever size the compositor
 * configures it with; if we only set geometry in the deferred arrange (one
 * event-loop tick later), the client has already committed a buffer at swc's
 * default tiled size — the whole output — so the window flashes full-screen and
 * then snaps to its tile.  An X11 WM like nvwm avoids this by resizing the
 * window before mapping it; this is the Wayland equivalent: send the correct
 * configure as part of the same iteration the window was created in.
 *
 * Only this one window is touched.  The full arrange (which would also
 * re-configure the OTHER, already-mapped windows mid-configure and crash swc)
 * stays deferred; the layout pass here is pure inis-side computation and writes
 * swc geometry for the new window alone — its first configure, which is safe.
 */
void
inis_server_preplace_window(struct inis_server *server,
    struct inis_window *window)
{
	struct inis_workspace *workspace;
	struct inis_rect usable;
	struct wc_box area;

	if (server == NULL || window == NULL || !window->mapped)
		return;
	if (window->workspace_index >= server->workspace_count)
		return;

	workspace = &server->workspaces[window->workspace_index];
	usable = window_usable_area(server, window);

	if (window->state == INIS_WINDOW_TILED &&
	    !window_in_special_workspace(server, window) &&
	    window->layout_view.workspace != NULL) {
		sync_layout_config(server, workspace);
		area.x = usable.x;
		area.y = usable.y;
		area.w = usable.w;
		area.h = usable.h;
		/*
		 * Compute the layout WITHOUT changing the focused view, so the
		 * geometry matches what the deferred arrange will produce (focus
		 * only moves to this window later, after the settle delay).  This
		 * keeps the pre-placed position pixel-identical to the final one,
		 * so there is no second, smaller hop either.
		 */
		layout_arrange(&workspace->layout, area);
		window->tiled.x = window->layout_view.pending_geometry.x;
		window->tiled.y = window->layout_view.pending_geometry.y;
		window->tiled.w = window->layout_view.pending_geometry.w;
		window->tiled.h = window->layout_view.pending_geometry.h;
	} else if (window->state == INIS_WINDOW_FLOATING) {
		inis_window_ensure_floating_rect(window, &usable);
	}

	inis_backend_apply_window(&server->backend, window);
}

void
inis_server_mark_damage(struct inis_server *server,
    const struct inis_rect *rect, const char *reason)
{
	if (rect == NULL)
		return;
	inis_damage_add_rect(&server->damage, rect, reason);
	mark_damage_on_outputs(server, rect, reason);
}

void
inis_server_flush_damage(struct inis_server *server, const char *why)
{
	inis_render_flush_damage(server, why);
}

static int
focus_window(struct inis_server *server, struct inis_window *window, bool warp)
{
	struct inis_window *old;

	if (window != NULL && !window_is_focusable(server, window))
		return -1;
	if (server->focused_window == window) {
		if (window != NULL) {
			inis_backend_focus_window(&server->backend, window);
			if (warp)
				warp_pointer_to_window(server, window);
			if (window->state != INIS_WINDOW_TILED)
				inis_backend_raise_window(&server->backend, window);
		}
		return 0;
	}

	old = server->focused_window;
	if (old != NULL) {
		damage_window(server, old, "focus-old");
		old->focused = false;
		inis_backend_update_window_style(&server->backend, old);
	}

	server->focused_window = window;
	if (window != NULL) {
		window->focused = true;
		if (window->state == INIS_WINDOW_TILED &&
		    window->workspace_index < server->workspace_count)
			layout_focus_view(&server->workspaces[window->workspace_index].layout,
			    &window->layout_view);
		inis_backend_focus_window(&server->backend, window);
		if (warp)
			warp_pointer_to_window(server, window);
		if (window->state != INIS_WINDOW_TILED)
			inis_backend_raise_window(&server->backend, window);
		damage_window(server, window, "focus-new");
		inis_backend_update_window_style(&server->backend, window);
	} else {
		/*
		 * Do not push a focus(NULL) round-trip through swc here.  During
		 * synchronous client shutdown the current keyboard focus may still
		 * belong to a window that is in the middle of being destroyed.
		 * Keeping the logical focus cleared and letting the next arrange or
		 * explicit focus change pick a new target is safer than forcing an
		 * immediate backend unfocus from inside the destroy path.
		 */
	}

	raise_visible_non_tiled_windows(server);
	return 0;
}

int
inis_server_focus_window(struct inis_server *server, struct inis_window *window)
{
	return focus_window(server, window, false);
}

int
inis_server_focus_window_with_warp(struct inis_server *server,
    struct inis_window *window)
{
	return focus_window(server, window, true);
}

int
inis_server_focus_next(struct inis_server *server, int direction)
{
	struct inis_workspace *workspace;

	workspace = active_workspace_struct(server);
	if (workspace != NULL) {
		if (direction > 0)
			layout_cycle_next(&workspace->layout);
		else
			layout_cycle_prev(&workspace->layout);
		if (workspace->layout.focused_view != NULL) {
			struct inis_window *window =
			    workspace->layout.focused_view->user_data;

			if (window != NULL)
				return inis_server_focus_window_with_warp(server, window);
		}
	}

	struct inis_window *candidate = NULL;
	size_t i;
	size_t focused_index = INIS_MAX_WINDOWS;

	if (direction == 0)
		direction = 1;

	for (i = 0; i < server->window_count; i++) {
		if (&server->windows[i] == server->focused_window) {
			focused_index = i;
			break;
		}
	}

	if (direction > 0) {
		size_t start = focused_index == INIS_MAX_WINDOWS ? 0 : focused_index + 1;

		for (i = start; i < server->window_count; i++) {
			if (window_is_focusable(server, &server->windows[i])) {
				candidate = &server->windows[i];
				break;
			}
		}
		if (candidate == NULL) {
			for (i = 0; i < start && i < server->window_count; i++) {
				if (window_is_focusable(server, &server->windows[i])) {
					candidate = &server->windows[i];
					break;
				}
			}
		}
	} else {
		size_t start = focused_index == INIS_MAX_WINDOWS ?
		    server->window_count : focused_index;

		for (i = start; i > 0; i--) {
			if (window_is_focusable(server, &server->windows[i - 1])) {
				candidate = &server->windows[i - 1];
				break;
			}
		}
		if (candidate == NULL) {
			for (i = server->window_count; i > start; i--) {
				if (window_is_focusable(server, &server->windows[i - 1])) {
					candidate = &server->windows[i - 1];
					break;
				}
			}
		}
	}

	if (candidate == NULL)
		return -1;
	return inis_server_focus_window_with_warp(server, candidate);
}

int
inis_server_focus_direction(struct inis_server *server, enum wc_direction direction)
{
	struct inis_window *focused;
	struct inis_window *best = NULL;
	struct inis_rect focused_rect;
	int focused_cx;
	int focused_cy;
	int best_score = 0;
	size_t i;

	focused = server->focused_window;
	if (focused == NULL || !focused->mapped)
		return -1;

	focused_rect = window_current_rect(server, focused);
	if (focused_rect.w <= 0 || focused_rect.h <= 0)
		return -1;
	focused_cx = rect_center_x(&focused_rect);
	focused_cy = rect_center_y(&focused_rect);

	for (i = 0; i < server->window_count; i++) {
		struct inis_window *candidate = &server->windows[i];
		struct inis_rect candidate_rect;
		int candidate_cx;
		int candidate_cy;
		int primary;
		int secondary;
		int score;
		bool valid = false;
		bool overlap;

		if (candidate == focused || !window_is_focusable(server, candidate))
			continue;

		candidate_rect = window_current_rect(server, candidate);
		if (candidate_rect.w <= 0 || candidate_rect.h <= 0)
			continue;
		candidate_cx = rect_center_x(&candidate_rect);
		candidate_cy = rect_center_y(&candidate_rect);

		switch (direction) {
		case WC_DIRECTION_LEFT:
			valid = candidate_cx < focused_cx;
			primary = focused_cx - candidate_cx;
			secondary = abs_int(candidate_cy - focused_cy);
			break;
		case WC_DIRECTION_RIGHT:
			valid = candidate_cx > focused_cx;
			primary = candidate_cx - focused_cx;
			secondary = abs_int(candidate_cy - focused_cy);
			break;
		case WC_DIRECTION_UP:
			valid = candidate_cy < focused_cy;
			primary = focused_cy - candidate_cy;
			secondary = abs_int(candidate_cx - focused_cx);
			break;
		case WC_DIRECTION_DOWN:
			valid = candidate_cy > focused_cy;
			primary = candidate_cy - focused_cy;
			secondary = abs_int(candidate_cx - focused_cx);
			break;
		default:
			return -1;
		}
		if (!valid)
			continue;

		overlap = rects_overlap_on_secondary_axis(&focused_rect,
		    &candidate_rect, direction);
		score = primary * 1000 + secondary;
		if (!overlap)
			score += 100000000;
		if (best == NULL || score < best_score) {
			best = candidate;
			best_score = score;
		}
	}

	if (best == NULL)
		return -1;
	return inis_server_focus_window_with_warp(server, best);
}

int
inis_server_switch_workspace(struct inis_server *server, const char *name)
{
	int index;
	size_t i;

	if (name == NULL || name[0] == '\0')
		return -1;

	index = workspace_get_or_create(server, name, false);
	if (index < 0)
		return -1;

	if (server->monitor_count == 0) {
		server->workspaces[index].active = true;
		return 0;
	}

	server->monitors[0].previous_workspace = server->monitors[0].active_workspace;
	server->monitors[0].active_workspace = (unsigned int)index;

	for (i = 0; i < server->workspace_count; i++)
		server->workspaces[i].active = i == (size_t)index;

	damage_all_monitors(server, "workspace-switch");
	inis_server_focus_window(server, NULL);
	inis_backend_focus_window(&server->backend, NULL);
	for (i = server->window_count; i > 0; i--) {
		if (server->windows[i - 1].mapped &&
		    server->windows[i - 1].workspace_index == (unsigned int)index) {
			inis_server_focus_window(server, &server->windows[i - 1]);
			break;
		}
	}

	inis_info("workspace switched: %s", server->workspaces[index].name);
	inis_server_arrange(server);
	inis_server_flush_damage(server, "workspace-switch");
	return 0;
}

int
inis_server_switch_previous_workspace(struct inis_server *server)
{
	unsigned int previous;
	char name[INIS_MAX_NAME];

	if (server->monitor_count == 0)
		return -1;

	previous = server->monitors[0].previous_workspace;
	if (previous >= server->workspace_count)
		return -1;

	snprintf(name, sizeof(name), "%s", server->workspaces[previous].name);
	return inis_server_switch_workspace(server, name);
}

int
inis_server_switch_relative_workspace(struct inis_server *server, int delta)
{
	unsigned int active;
	int current;
	int next;
	char name[INIS_MAX_NAME];

	if (server->monitor_count == 0)
		return -1;

	active = server->monitors[0].active_workspace;
	if (active >= server->workspace_count)
		return -1;

	current = atoi(server->workspaces[active].name);
	if (current < 1 || current > 10)
		return -1;

	next = current + delta;
	while (next < 1)
		next += 10;
	while (next > 10)
		next -= 10;

	snprintf(name, sizeof(name), "%d", next);
	return inis_server_switch_workspace(server, name);
}

int
inis_server_move_focused_to_workspace(struct inis_server *server,
    const char *name, bool silent)
{
	int index;
	struct inis_window *window;
	struct inis_workspace *workspace;

	if (server->focused_window == NULL || name == NULL || name[0] == '\0')
		return -1;

	index = workspace_get_or_create(server, name,
	    strncmp(name, "special:", 8) == 0);
	if (index < 0)
		return -1;

	window = server->focused_window;
	damage_window(server, window, "workspace-move-old");
	if (window->workspace_index < server->workspace_count &&
	    server->workspaces[window->workspace_index].window_count > 0)
		server->workspaces[window->workspace_index].window_count--;
	if (window->layout_view.workspace != NULL)
		layout_remove_view(window->layout_view.workspace,
		    &window->layout_view);

	window->workspace_index = (unsigned int)index;
	workspace = &server->workspaces[index];
	workspace->window_count++;
	if (workspace->special) {
		struct inis_rect area = window_usable_area(server, window);

		(void)inis_window_make_floating_centered(window, &area);
	}
	sync_window_layout_membership(server, window);

	if (workspace->special) {
		if (!workspace->visible) {
			inis_server_focus_window(server, NULL);
			(void)inis_server_focus_next(server, 1);
		} else {
			(void)inis_server_focus_window(server, window);
		}
		inis_server_arrange(server);
	} else if (!silent) {
		inis_server_switch_workspace(server, name);
	} else {
		inis_server_arrange(server);
		inis_server_flush_damage(server, "workspace-move-silent");
	}

	inis_info("window moved to workspace: %s", name);
	return 0;
}

int
inis_server_toggle_special_workspace(struct inis_server *server, const char *name)
{
	char special_name[INIS_MAX_NAME];
	struct inis_workspace *workspace;
	int index;
	size_t i;

	if (name == NULL || name[0] == '\0')
		name = "magic";

	if (strncmp(name, "special:", 8) == 0)
		snprintf(special_name, sizeof(special_name), "%s", name);
	else
		snprintf(special_name, sizeof(special_name), "special:%s", name);

	index = workspace_get_or_create(server, special_name, true);
	if (index < 0)
		return -1;

	workspace = &server->workspaces[index];
	workspace->special = true;
	workspace->visible = !workspace->visible;

	damage_all_monitors(server, "special-workspace");

	if (!workspace->visible && server->focused_window != NULL &&
	    server->focused_window->workspace_index == (unsigned int)index)
		inis_server_focus_window(server, NULL);
	if (!workspace->visible)
		inis_backend_focus_window(&server->backend, NULL);

	if (workspace->visible) {
		for (i = server->window_count; i > 0; i--) {
			if (server->windows[i - 1].mapped &&
			    server->windows[i - 1].workspace_index == (unsigned int)index) {
				inis_server_focus_window(server, &server->windows[i - 1]);
				break;
			}
		}
	} else if (server->focused_window == NULL) {
		(void)inis_server_focus_next(server, 1);
	}

	inis_server_arrange(server);
	inis_info("special workspace %s: %s", workspace->name,
	    workspace->visible ? "visible" : "hidden");
	inis_server_flush_damage(server, "special-workspace");
	return 0;
}

int
inis_server_center_focused(struct inis_server *server)
{
	struct inis_window *window = server->focused_window;
	struct inis_rect area;

	if (window == NULL)
		return -1;
	area = window_usable_area(server, window);
	if (window_make_directly_managed(server, window))
		inis_server_arrange(server);
	damage_window(server, window, "center-old");
	inis_window_center_floating(window, &area);
	damage_window(server, window, "center-new");
	inis_backend_apply_window(&server->backend, window);
	inis_server_flush_damage(server, "centerwindow");
	return 0;
}

int
inis_server_move_focused(struct inis_server *server, int dx, int dy)
{
	struct inis_window *window = server->focused_window;

	if (window == NULL)
		return -1;
	if (window_make_directly_managed(server, window))
		inis_server_arrange(server);
	damage_window(server, window, "move-old");
	window->floating.x += dx;
	window->floating.y += dy;
	damage_window(server, window, "move-new");
	inis_backend_apply_window(&server->backend, window);
	inis_server_flush_damage(server, "moveactive");
	return 0;
}

int
inis_server_resize_focused(struct inis_server *server, int dw, int dh)
{
	struct inis_window *window = server->focused_window;
	struct inis_rect area;
	int max_w;
	int max_h;
	int magnitude;
	float delta;

	if (window == NULL)
		return -1;
	if (window->state == INIS_WINDOW_TILED) {
		magnitude = abs(dw) >= abs(dh) ? dw : dh;
		if (magnitude == 0)
			return -1;
		delta = magnitude > 0 ? 0.05f : -0.05f;
		if (window->workspace_index < server->workspace_count) {
			layout_focus_view(&server->workspaces[window->workspace_index].layout,
			    &window->layout_view);
			layout_resize_focused(
			    &server->workspaces[window->workspace_index].layout, delta);
			inis_server_arrange(server);
			inis_server_flush_damage(server, "resizeactive");
			return 0;
		}
	}
	if (window_make_directly_managed(server, window))
		inis_server_arrange(server);
	damage_window(server, window, "resize-old");
	area = window_usable_area(server, window);

	max_w = area.w > 0 ? area.w : 8192;
	max_h = area.h > 0 ? area.h : 8192;
	window->floating.w = clamp_int(window->floating.w + dw, 50, max_w);
	window->floating.h = clamp_int(window->floating.h + dh, 50, max_h);
	damage_window(server, window, "resize-new");
	inis_backend_apply_window(&server->backend, window);
	inis_server_flush_damage(server, "resizeactive");
	return 0;
}

int
inis_server_swap_focused(struct inis_server *server, int direction)
{
	struct inis_window *focused = server->focused_window;
	unsigned int active_workspace;
	struct inis_workspace *workspace;

	if (focused == NULL || focused->state != INIS_WINDOW_TILED)
		return -1;
	if (server->monitor_count == 0)
		return -1;

	active_workspace = server->monitors[0].active_workspace;
	if (focused->workspace_index != active_workspace)
		return -1;

	workspace = &server->workspaces[active_workspace];
	layout_focus_view(&workspace->layout, &focused->layout_view);
	layout_swap_focused_neighbor(&workspace->layout, direction);
	inis_server_arrange(server);
	inis_server_flush_damage(server, "swapwindow");
	return 0;
}

int
inis_server_toggle_split(struct inis_server *server)
{
	struct inis_window *window = server->focused_window;

	if (window == NULL || window->state != INIS_WINDOW_TILED)
		return -1;
	if (window->workspace_index >= server->workspace_count)
		return -1;
	layout_focus_view(&server->workspaces[window->workspace_index].layout,
	    &window->layout_view);
	layout_toggle_split(&server->workspaces[window->workspace_index].layout);
	inis_server_arrange(server);
	inis_server_flush_damage(server, "togglesplit");
	return 0;
}

int
inis_server_begin_mouse_move(struct inis_server *server)
{
	struct inis_window *window = server->focused_window;

	if (window == NULL)
		return -1;
	if (window_make_directly_managed(server, window))
		inis_server_arrange(server);
	damage_window(server, window, "mouse-move");
	window->interactive_grab = true;
	inis_backend_apply_window(&server->backend, window);
	(void)inis_server_focus_window(server, window);
	inis_backend_begin_move(&server->backend, window);
	inis_server_flush_damage(server, "movewindow");
	return 0;
}

int
inis_server_begin_mouse_resize(struct inis_server *server, uint32_t edges)
{
	struct inis_window *window = server->focused_window;

	if (window == NULL)
		return -1;
	if (window_make_directly_managed(server, window))
		inis_server_arrange(server);
	damage_window(server, window, "mouse-resize");
	window->interactive_grab = true;
	inis_backend_apply_window(&server->backend, window);
	(void)inis_server_focus_window(server, window);
	inis_backend_begin_resize(&server->backend, window, edges);
	inis_server_flush_damage(server, "resizewindow");
	return 0;
}

int
inis_server_request_fullscreen(struct inis_server *server,
    struct inis_window *window, void *backend_monitor, bool fullscreen)
{
	int monitor_index;

	if (server == NULL || window == NULL || !window->mapped)
		return -1;

	monitor_index = monitor_index_from_backend(server, backend_monitor);
	if (monitor_index >= 0)
		window->monitor_index = (unsigned int)monitor_index;

	inis_window_set_fullscreen(window, fullscreen);
	inis_server_arrange(server);
	(void)inis_server_focus_window(server, window);
	inis_server_flush_damage(server,
	    fullscreen ? "client-fullscreen-on" : "client-fullscreen-off");
	return 0;
}

void
inis_server_reload_config(struct inis_server *server)
{
	size_t i;

	inis_backend_reload_bindings(&server->backend);

	/* Re-apply the configured default reserved insets to every monitor. */
	for (i = 0; i < server->monitor_count; i++) {
		server->monitors[i].reserved.top = server->config.reserved_top;
		server->monitors[i].reserved.bottom = server->config.reserved_bottom;
		server->monitors[i].reserved.left = server->config.reserved_left;
		server->monitors[i].reserved.right = server->config.reserved_right;
		monitor_recompute_usable(&server->monitors[i]);
	}

	inis_server_arrange(server);
	inis_server_flush_damage(server, "reload");
	inis_info("config reload complete");
}

void
inis_server_monitor_layout_changed(struct inis_server *server,
    struct inis_monitor *monitor)
{
	if (monitor == NULL)
		return;
	monitor_recompute_usable(monitor);
	/*
	 * Coalesce into the deferred-arrange idle.  This is reached from swc
	 * geometry callbacks, so running arrange synchronously here would risk
	 * re-entering swc mid-update; the idle batches rapid changes into one
	 * arrange.
	 */
	inis_backend_schedule_arrange(&server->backend);
}

int
inis_server_set_monitor_reserved(struct inis_server *server, const char *name,
    int top, int bottom, int left, int right)
{
	size_t i;
	int matched = 0;

	if (top < 0)
		top = 0;
	if (bottom < 0)
		bottom = 0;
	if (left < 0)
		left = 0;
	if (right < 0)
		right = 0;

	for (i = 0; i < server->monitor_count; i++) {
		struct inis_monitor *monitor = &server->monitors[i];

		if (name != NULL && name[0] != '\0' &&
		    strcmp(name, monitor->name) != 0)
			continue;
		monitor->reserved.top = top;
		monitor->reserved.bottom = bottom;
		monitor->reserved.left = left;
		monitor->reserved.right = right;
		monitor_recompute_usable(monitor);
		matched++;
	}

	if (matched > 0) {
		inis_server_arrange(server);
		inis_server_flush_damage(server, "monitor-reserved");
	}
	return matched;
}
