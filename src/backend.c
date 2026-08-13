#include "backend.h"

#include "backend_swc.h"
#include "log.h"

void
inis_backend_init(struct inis_backend *backend, struct inis_server *server)
{
	backend->started = false;
	backend->server = server;
	backend->display = NULL;
	backend->sigint_source = NULL;
	backend->sigterm_source = NULL;
	backend->ipc_source = NULL;
	backend->arrange_idle = NULL;
	backend->arrange_scheduled = false;
	backend->grab_timer = NULL;
	backend->wayland_socket = NULL;
	backend->wayland_env[0] = '\0';
	backend->backend_binding_count = 0;
	backend->focus_retry_timer = NULL;
	backend->grab_mode = INIS_BACKEND_GRAB_NONE;
	backend->grab_window = NULL;
	backend->grab_start_cx_fixed = 0;
	backend->grab_start_cy_fixed = 0;
	backend->grab_initial_rect = (struct inis_rect){ 0, 0, 0, 0 };
	backend->pending_focus_due_ms = 0;
	backend->pending_focus_deadline_ms = 0;
	backend->pending_focus_warp = false;
}

int
inis_backend_start(struct inis_backend *backend)
{
	return inis_backend_swc_start(backend);
}

int
inis_backend_run(struct inis_backend *backend)
{
	return inis_backend_swc_run(backend);
}

void
inis_backend_request_stop(struct inis_backend *backend)
{
	inis_backend_swc_request_stop(backend);
}

void
inis_backend_finish(struct inis_backend *backend)
{
	if (!backend->started)
		return;
	inis_backend_swc_finish(backend);
	backend->started = false;
	inis_info("backend stopped");
}

void
inis_backend_close_window(struct inis_backend *backend,
    struct inis_window *window)
{
	if (window != NULL)
		inis_backend_swc_close_window(backend, window);
}

void
inis_backend_focus_window(struct inis_backend *backend,
    struct inis_window *window)
{
	inis_backend_swc_focus_window(backend, window);
}

void
inis_backend_warp_pointer(struct inis_backend *backend, int x, int y)
{
	inis_backend_swc_warp_pointer(backend, x, y);
}

void
inis_backend_update_window_style(struct inis_backend *backend,
    struct inis_window *window)
{
	if (window != NULL)
		inis_backend_swc_update_window_style(backend, window);
}

void
inis_backend_apply_window(struct inis_backend *backend,
    struct inis_window *window)
{
	if (window != NULL)
		inis_backend_swc_apply_window(backend, window);
}

void
inis_backend_set_window_visible(struct inis_backend *backend,
    struct inis_window *window, bool visible)
{
	if (window != NULL)
		inis_backend_swc_set_window_visible(backend, window, visible);
}

void
inis_backend_raise_window(struct inis_backend *backend,
    struct inis_window *window)
{
	if (window != NULL)
		inis_backend_swc_raise_window(backend, window);
}

void
inis_backend_begin_move(struct inis_backend *backend,
    struct inis_window *window)
{
	if (window != NULL)
		inis_backend_swc_begin_move(backend, window);
}

void
inis_backend_begin_resize(struct inis_backend *backend,
    struct inis_window *window, uint32_t edges)
{
	if (window != NULL)
		inis_backend_swc_begin_resize(backend, window, edges);
}

void
inis_backend_reload_bindings(struct inis_backend *backend)
{
	inis_backend_swc_reload_bindings(backend);
}

void
inis_backend_schedule_arrange(struct inis_backend *backend)
{
	inis_backend_swc_schedule_arrange(backend);
}
