#ifndef INIS_SERVER_H
#define INIS_SERVER_H

#include "inis.h"

#include "monitor.h"
#include "window.h"
#include "workspace.h"
#include "backend.h"
#include "config.h"
#include "damage.h"
#include "ipc.h"
#include "bind.h"
#include "rules.h"

struct inis_server {
	bool running;
	const char *socket_name;

	struct inis_backend backend;
	struct inis_config config;
	struct inis_ipc ipc;

	struct inis_monitor monitors[INIS_MAX_MONITORS];
	size_t monitor_count;

	struct inis_workspace workspaces[INIS_MAX_WORKSPACES];
	size_t workspace_count;

	struct inis_window windows[INIS_MAX_WINDOWS];
	size_t window_count;
	unsigned int next_window_order;

	struct inis_binding bindings[INIS_MAX_BINDINGS];
	size_t binding_count;

	struct inis_rule rules[INIS_MAX_RULES];
	size_t rule_count;

	struct inis_window *focused_window;
	struct inis_window *pending_focus_window;
	struct inis_monitor *focused_monitor;

	struct inis_damage damage;
};

int inis_server_init(struct inis_server *server);
int inis_server_run(struct inis_server *server);
void inis_server_shutdown(struct inis_server *server);
void inis_server_request_exit(struct inis_server *server);
void inis_server_arrange(struct inis_server *server);
void inis_server_mark_damage(struct inis_server *server,
    const struct inis_rect *rect, const char *reason);
void inis_server_flush_damage(struct inis_server *server, const char *why);
int inis_server_focus_window(struct inis_server *server,
    struct inis_window *window);
int inis_server_focus_window_with_warp(struct inis_server *server,
    struct inis_window *window);
int inis_server_focus_next(struct inis_server *server, int direction);
int inis_server_focus_direction(struct inis_server *server,
    enum wc_direction direction);
int inis_server_switch_workspace(struct inis_server *server, const char *name);
int inis_server_switch_previous_workspace(struct inis_server *server);
int inis_server_switch_relative_workspace(struct inis_server *server, int delta);
int inis_server_move_focused_to_workspace(struct inis_server *server,
    const char *name, bool silent);
int inis_server_toggle_special_workspace(struct inis_server *server,
    const char *name);
int inis_server_center_focused(struct inis_server *server);
int inis_server_move_focused(struct inis_server *server, int dx, int dy);
int inis_server_resize_focused(struct inis_server *server, int dw, int dh);
int inis_server_swap_focused(struct inis_server *server, int direction);
int inis_server_toggle_split(struct inis_server *server);
int inis_server_begin_mouse_move(struct inis_server *server);
int inis_server_begin_mouse_resize(struct inis_server *server, uint32_t edges);
int inis_server_request_fullscreen(struct inis_server *server,
    struct inis_window *window, void *backend_monitor, bool fullscreen);
struct inis_monitor *inis_server_add_monitor(struct inis_server *server,
    const char *name, const struct inis_rect *geometry,
    const struct inis_rect *usable, void *backend_monitor);
struct inis_window *inis_server_add_window(struct inis_server *server,
    const char *app_id, const char *title, void *backend_window);
void inis_server_preplace_window(struct inis_server *server,
    struct inis_window *window);
void inis_server_remove_window(struct inis_server *server,
    struct inis_window *window);
void inis_server_reload_config(struct inis_server *server);
void inis_server_monitor_layout_changed(struct inis_server *server,
    struct inis_monitor *monitor);
int inis_server_set_monitor_reserved(struct inis_server *server,
    const char *name, int top, int bottom, int left, int right);

#endif
