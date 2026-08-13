/* For clock_gettime/CLOCK_MONOTONIC under -std=c11. */
#define _POSIX_C_SOURCE 200809L

#include "backend_swc.h"

#include "bind.h"
#include "config.h"
#include "dispatch.h"
#include "ipc.h"
#include "log.h"
#include "rules.h"
#include "server.h"

#include <swc.h>
#include <wayland-server-core.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>


struct libinput_device;

int putenv(char *string);
int setenv(const char *name, const char *value, int overwrite);

static void
ensure_xdg_runtime_dir(void)
{
	char buf[256];
	const char *v = getenv("XDG_RUNTIME_DIR");

	if (v != NULL && v[0] != '\0' && access(v, W_OK) == 0)
		return;

	snprintf(buf, sizeof(buf), "/run/user/%d", (int)getuid());
	if (access(buf, W_OK) == 0) {
		setenv("XDG_RUNTIME_DIR", buf, 1);
		inis_info("set XDG_RUNTIME_DIR=%s", buf);
		return;
	}

	snprintf(buf, sizeof(buf), "/tmp/runtime-%d", (int)getuid());
	mkdir(buf, 0700);
	setenv("XDG_RUNTIME_DIR", buf, 1);
	inis_info("set XDG_RUNTIME_DIR=%s (fallback)", buf);
}

struct swc_window_data {
	struct inis_window *window;
};

static void swc_new_screen(struct swc_screen *screen);
static void swc_screen_destroy(void *data);
static void swc_screen_geometry_changed(void *data);
static void swc_screen_usable_geometry_changed(void *data);
static void swc_new_window(struct swc_window *window);
static void swc_new_device(struct libinput_device *device);
static void swc_activate(void);
static void swc_deactivate(void);
static void swc_window_destroy(void *data);
static void swc_window_title_changed(void *data);
static void swc_window_app_id_changed(void *data);
static void swc_window_parent_changed(void *data);
static void swc_window_entered(void *data);
static void swc_window_move(void *data);
static void swc_window_resize(void *data);
static int swc_signal(int signal_number, void *data);
static int swc_ipc(int fd, uint32_t mask, void *data);
static int grab_timer_cb(void *data);
static int focus_retry_cb(void *data);
static void schedule_focus_retry(struct inis_backend *backend, int delay_ms);
static void request_pending_focus(struct inis_backend *backend,
    struct inis_window *window, bool warp);
static void apply_pending_focus(struct inis_backend *backend);
static void swc_binding(void *data, uint32_t time, uint32_t value,
    uint32_t state);
static void install_bindings(struct inis_backend *backend);

static struct inis_backend *active_backend;

static const struct swc_manager manager = {
	.new_screen = swc_new_screen,
	.new_window = swc_new_window,
	.new_device = swc_new_device,
	.activate   = swc_activate,
	.deactivate = swc_deactivate,
};

static const struct swc_window_handler window_handler = {
	.destroy        = swc_window_destroy,
	.title_changed  = swc_window_title_changed,
	.app_id_changed = swc_window_app_id_changed,
	.parent_changed = swc_window_parent_changed,
	.entered        = swc_window_entered,
	.move           = swc_window_move,
	.resize         = swc_window_resize,
};

static const struct swc_screen_handler screen_handler = {
	.destroy                = swc_screen_destroy,
	.geometry_changed       = swc_screen_geometry_changed,
	.usable_geometry_changed = swc_screen_usable_geometry_changed,
};

static struct inis_rect
rect_from_swc(const struct swc_rectangle *rect)
{
	struct inis_rect out;

	out.x = rect->x;
	out.y = rect->y;
	out.w = (int)rect->width;
	out.h = (int)rect->height;
	return out;
}

static bool
rect_equal(const struct inis_rect *a, const struct inis_rect *b)
{
	return a->x == b->x && a->y == b->y && a->w == b->w && a->h == b->h;
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

/*
 * Delay focus slightly so clients can finish their configure/commit path.
 * CLOCK_MONOTONIC, not gettimeofday: the focus due/deadline stamps must not
 * jump with NTP steps or suspend/resume — a backwards wall-clock step would
 * leave pending_focus_due_ms in the future and stall focus indefinitely.
 */
static uint64_t
monotonic_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000u);
}

static struct inis_rect
grab_usable_area(struct inis_backend *backend, struct inis_window *window)
{
	if (backend->server != NULL && window != NULL &&
	    window->monitor_index < backend->server->monitor_count)
		return backend->server->monitors[window->monitor_index].usable;
	return (struct inis_rect){ 0, 0, 800, 600 };
}

static bool
window_is_transient_child_of(const struct inis_window *window,
    const struct inis_window *parent)
{
	struct swc_window *swc_parent;

	if (window == NULL || parent == NULL || window->swc == NULL ||
	    parent->swc == NULL)
		return false;

	swc_parent = window->swc->parent;
	while (swc_parent != NULL) {
		if (swc_parent == parent->swc)
			return true;
		swc_parent = swc_parent->parent;
	}
	return false;
}

static void
sync_transient_geometry(struct inis_window *window)
{
	struct swc_rectangle geometry;

	if (window == NULL || !window->transient || window->swc == NULL)
		return;
	if (!swc_window_get_geometry(window->swc, &geometry))
		return;
	if (geometry.width == 0 || geometry.height == 0)
		return;

	window->floating = rect_from_swc(&geometry);
}

static void
update_grabbed_window(struct inis_backend *backend)
{
	struct inis_server *server = backend->server;
	struct inis_window *window = backend->grab_window;
	struct inis_rect old_rect;
	struct inis_rect new_rect;
	struct inis_rect area;
	int32_t cx_fixed, cy_fixed;
	int dx, dy;
	int max_w, max_h;

	if (server == NULL || window == NULL ||
	    backend->grab_mode == INIS_BACKEND_GRAB_NONE)
		return;
	if (!swc_cursor_position(&cx_fixed, &cy_fixed))
		return;

	dx = (cx_fixed - backend->grab_start_cx_fixed) / 256;
	dy = (cy_fixed - backend->grab_start_cy_fixed) / 256;
	old_rect = window->floating;
	new_rect = backend->grab_initial_rect;
	area = grab_usable_area(backend, window);

	if (backend->grab_mode == INIS_BACKEND_GRAB_MOVE) {
		int min_x = area.x;
		int min_y = area.y;
		int max_x = area.x + area.w - new_rect.w;
		int max_y = area.y + area.h - new_rect.h;

		if (max_x < min_x)
			max_x = min_x;
		if (max_y < min_y)
			max_y = min_y;
		new_rect.x = clamp_int(new_rect.x + dx, min_x, max_x);
		new_rect.y = clamp_int(new_rect.y + dy, min_y, max_y);
	} else if (backend->grab_mode == INIS_BACKEND_GRAB_RESIZE) {
		max_w = area.w > 0 ? area.w : 8192;
		max_h = area.h > 0 ? area.h : 8192;
		new_rect.w = clamp_int(new_rect.w + dx, 50, max_w);
		new_rect.h = clamp_int(new_rect.h + dy, 50, max_h);
	}

	if (new_rect.x == old_rect.x && new_rect.y == old_rect.y &&
	    new_rect.w == old_rect.w && new_rect.h == old_rect.h)
		return;

	inis_server_mark_damage(server, &old_rect, "grab-old");
	window->floating = new_rect;
	inis_server_mark_damage(server, &window->floating, "grab-new");
	inis_backend_apply_window(backend, window);
	inis_server_flush_damage(server, "mouse-grab");
}

static void
start_pointer_grab(struct inis_backend *backend, struct inis_window *window,
    enum inis_backend_grab_mode mode)
{
	struct wl_event_loop *loop;

	if (backend == NULL || backend->display == NULL || window == NULL)
		return;
	if (!swc_cursor_position(&backend->grab_start_cx_fixed,
	        &backend->grab_start_cy_fixed))
		return;

	backend->grab_mode = mode;
	backend->grab_window = window;
	backend->grab_initial_rect = window->floating;

	if (backend->grab_timer == NULL) {
		loop = wl_display_get_event_loop(backend->display);
		backend->grab_timer = wl_event_loop_add_timer(loop, grab_timer_cb,
		    backend);
	}
	if (backend->grab_timer != NULL)
		wl_event_source_timer_update(backend->grab_timer, 8);
}

static void
end_pointer_grab(struct inis_backend *backend)
{
	if (backend == NULL || backend->grab_mode == INIS_BACKEND_GRAB_NONE)
		return;

	update_grabbed_window(backend);
	if (backend->grab_window != NULL)
		backend->grab_window->interactive_grab = false;
	backend->grab_mode = INIS_BACKEND_GRAB_NONE;
	backend->grab_window = NULL;
	if (backend->grab_timer != NULL)
		wl_event_source_timer_update(backend->grab_timer, 0);
}

static int
grab_timer_cb(void *data)
{
	struct inis_backend *backend = data;

	if (backend == NULL || backend->grab_mode == INIS_BACKEND_GRAB_NONE)
		return 0;

	update_grabbed_window(backend);
	if (backend->grab_timer != NULL)
		wl_event_source_timer_update(backend->grab_timer, 8);
	return 0;
}

static void
schedule_focus_retry(struct inis_backend *backend, int delay_ms)
{
	if (backend == NULL || backend->display == NULL)
		return;

	if (backend->focus_retry_timer == NULL) {
		struct wl_event_loop *loop = wl_display_get_event_loop(backend->display);
		backend->focus_retry_timer = wl_event_loop_add_timer(loop,
		    focus_retry_cb, backend);
	}
	if (backend->focus_retry_timer != NULL)
		wl_event_source_timer_update(backend->focus_retry_timer, delay_ms);
}

/*
 * A window is "swc-ready" once swc holds a committed view with real dimensions
 * for it.  swc_window_focus() internally unfocuses the previously focused
 * surface by walking its view pointer chain (view->0x48->0x30); that chain
 * stays NULL until the surface commits its first frame.  Probing geometry is
 * the only readiness signal swc exposes — it returns false (or a zero size)
 * until the first commit lands, which is exactly when the chain becomes safe to
 * dereference.  A NULL window->swc means swc has already dropped the window, so
 * there is nothing left for it to dereference: treat that as ready.
 */
static bool
window_swc_ready(struct inis_window *window)
{
	struct swc_rectangle geometry;

	if (window == NULL || window->swc == NULL)
		return true;
	if (!swc_window_get_geometry(window->swc, &geometry))
		return false;
	return geometry.width > 0 && geometry.height > 0;
}

/*
 * Focusing `pending` makes swc unfocus whatever currently holds focus.  That is
 * only safe once the outgoing window is swc-ready; otherwise swc dereferences a
 * NULL view chain and crashes the whole compositor.  Returns true when the
 * focus may proceed now.
 */
static bool
pending_focus_unfocus_safe(struct inis_server *server,
    struct inis_window *pending)
{
	struct inis_window *outgoing = server->focused_window;

	if (outgoing == NULL || outgoing == pending)
		return true;
	return window_swc_ready(outgoing);
}

static void
request_pending_focus(struct inis_backend *backend, struct inis_window *window,
    bool warp)
{
	struct inis_server *server;

	if (backend == NULL || window == NULL)
		return;

	server = backend->server;
	if (server == NULL)
		return;

	server->pending_focus_window = window;
	/*
	 * Short settle delay before focusing a freshly mapped window — kept only
	 * so opening a window feels instant rather than racing the very first
	 * frame.  Crash-safety is NOT this delay's job: the readiness probe in
	 * apply_pending_focus / focus_retry_cb gates the actual focus on the
	 * outgoing window having committed a frame (window_swc_ready), so a slow
	 * client that takes longer than 120 ms can no longer trigger the
	 * NULL-deref-on-unfocus crash inside swc.
	 */
	backend->pending_focus_due_ms = monotonic_ms() + 120u;
	/*
	 * Hard deadline for the readiness gate below.  If the outgoing window
	 * never commits a frame (e.g. a hung client), we stop retrying and skip
	 * the focus rather than risk a NULL-deref crash inside swc.
	 */
	backend->pending_focus_deadline_ms = monotonic_ms() + 2000u;
	backend->pending_focus_warp = warp;
	schedule_focus_retry(backend, 120);
}

static void
apply_pending_focus(struct inis_backend *backend)
{
	struct inis_server *server = backend->server;
	struct inis_window *pending;

	if (server == NULL || server->pending_focus_window == NULL)
		return;

	pending = server->pending_focus_window;
	if (backend->pending_focus_due_ms > monotonic_ms()) {
		uint64_t remaining = backend->pending_focus_due_ms - monotonic_ms();
		schedule_focus_retry(backend, remaining > 200 ? 200 : (int)remaining);
		return;
	}

	if (!pending->mapped || pending->swc == NULL) {
		server->pending_focus_window = NULL;
		backend->pending_focus_due_ms = 0;
		backend->pending_focus_warp = false;
		return;
	}

	if (server->focused_window != NULL &&
	    server->focused_window->state == INIS_WINDOW_FULLSCREEN &&
	    !window_is_transient_child_of(pending, server->focused_window) &&
	    server->focused_window->swc != NULL) {
		schedule_focus_retry(backend, 200);
		return;
	}

	if (!pending_focus_unfocus_safe(server, pending)) {
		if (monotonic_ms() >= backend->pending_focus_deadline_ms) {
			inis_warn("focus skipped: outgoing window never became "
			    "swc-ready before deadline");
			server->pending_focus_window = NULL;
			backend->pending_focus_due_ms = 0;
			backend->pending_focus_warp = false;
			return;
		}
		schedule_focus_retry(backend, 30);
		return;
	}

	server->pending_focus_window = NULL;
	backend->pending_focus_due_ms = 0;
	if (backend->pending_focus_warp)
		(void)inis_server_focus_window_with_warp(server, pending);
	else
		(void)inis_server_focus_window(server, pending);
	backend->pending_focus_warp = false;
}

static int
focus_retry_cb(void *data)
{
	struct inis_backend *backend = data;
	struct inis_server *server = backend->server;
	struct inis_window *pending;

	if (server == NULL || server->pending_focus_window == NULL)
		return 0;

	pending = server->pending_focus_window;
	if (backend->pending_focus_due_ms > monotonic_ms()) {
		uint64_t remaining = backend->pending_focus_due_ms - monotonic_ms();
		schedule_focus_retry(backend, remaining > 200 ? 200 : (int)remaining);
		return 0;
	}

	if (!pending->mapped || pending->swc == NULL) {
		server->pending_focus_window = NULL;
		backend->pending_focus_due_ms = 0;
		backend->pending_focus_warp = false;
		return 0;
	}

	if (server->focused_window != NULL &&
	    server->focused_window->state == INIS_WINDOW_FULLSCREEN &&
	    !window_is_transient_child_of(pending, server->focused_window) &&
	    server->focused_window->swc != NULL) {
		schedule_focus_retry(backend, 200);
		return 0;
	}

	if (!pending_focus_unfocus_safe(server, pending)) {
		if (monotonic_ms() >= backend->pending_focus_deadline_ms) {
			inis_warn("focus skipped: outgoing window never became "
			    "swc-ready before deadline");
			server->pending_focus_window = NULL;
			backend->pending_focus_due_ms = 0;
			backend->pending_focus_warp = false;
			return 0;
		}
		schedule_focus_retry(backend, 30);
		return 0;
	}

	server->pending_focus_window = NULL;
	backend->pending_focus_due_ms = 0;
	if (backend->pending_focus_warp)
		(void)inis_server_focus_window_with_warp(server, pending);
	else
		(void)inis_server_focus_window(server, pending);
	backend->pending_focus_warp = false;
	inis_server_flush_damage(server, "focus-retry");
	return 0;
}

static void
swc_new_screen(struct swc_screen *screen)
{
	char name[INIS_MAX_NAME];
	struct inis_rect geometry;
	struct inis_rect usable;
	struct inis_server *server;
	struct inis_monitor *monitor;

	inis_info("swc screen: %dx%d at %d,%d usable %dx%d at %d,%d",
	    (int)screen->geometry.width, (int)screen->geometry.height,
	    (int)screen->geometry.x, (int)screen->geometry.y,
	    (int)screen->usable_geometry.width,
	    (int)screen->usable_geometry.height,
	    (int)screen->usable_geometry.x, (int)screen->usable_geometry.y);

	if (active_backend == NULL || active_backend->server == NULL)
		return;

	server = active_backend->server;
	snprintf(name, sizeof(name), "swc-%zu", server->monitor_count);
	geometry = rect_from_swc(&screen->geometry);
	usable   = rect_from_swc(&screen->usable_geometry);
	monitor = inis_server_add_monitor(server, name, &geometry, &usable, screen);
	if (monitor != NULL)
		swc_screen_set_handler(screen, &screen_handler, monitor);
}

/*
 * Resolve the inis monitor that owns this screen.  The handler data pointer is
 * the monitor itself, but it may have been invalidated; the screen pointer is
 * the source of truth, so the geometry/usable callbacks re-read it directly.
 */
static void
swc_screen_apply_geometry(struct inis_monitor *monitor)
{
	struct swc_screen *screen;
	struct inis_rect geometry;
	struct inis_rect usable;

	if (monitor == NULL || active_backend == NULL ||
	    active_backend->server == NULL)
		return;
	screen = monitor->swc;
	if (screen == NULL)
		return;

	geometry = rect_from_swc(&screen->geometry);
	usable = rect_from_swc(&screen->usable_geometry);

	/*
	 * swc emits usable_geometry_changed on every layer-surface relayout, so
	 * an animated bar (e.g. a Quickshell panel like noctalia) can fire it
	 * every frame.  React only when the geometry or backend usable area
	 * actually changed: this avoids a per-frame arrange storm and, since the
	 * new values are committed before scheduling, breaks any re-entrant loop
	 * where arranging would itself nudge swc to recompute usable geometry.
	 */
	if (rect_equal(&geometry, &monitor->geometry) &&
	    rect_equal(&usable, &monitor->backend_usable))
		return;

	monitor->geometry = geometry;
	monitor->backend_usable = usable;
	inis_server_monitor_layout_changed(active_backend->server, monitor);
}

static void
swc_screen_destroy(void *data)
{
	struct inis_monitor *monitor = data;

	if (monitor == NULL)
		return;
	inis_info("swc screen destroyed: %s", monitor->name);
	monitor->swc = NULL;
	monitor->enabled = false;
	/*
	 * Defer the re-arrange to the idle handler rather than running it inside
	 * swc's destroy callback, where the screen is being torn down.
	 */
	if (active_backend != NULL && active_backend->server != NULL)
		inis_backend_schedule_arrange(active_backend);
}

static void
swc_screen_geometry_changed(void *data)
{
	swc_screen_apply_geometry(data);
}

static void
swc_screen_usable_geometry_changed(void *data)
{
	swc_screen_apply_geometry(data);
}

static void
swc_new_window(struct swc_window *window)
{
	struct swc_window_data *data;
	struct inis_window *inis_window;

	if (active_backend == NULL || active_backend->server == NULL)
		return;

	data = calloc(1, sizeof(*data));
	if (data == NULL) {
		inis_warn("failed to allocate swc window data");
		return;
	}

	/*
	 * Set handler and tiled mode BEFORE add_window so swc already knows the
	 * window is tiled by the time the deferred arrange applies its geometry.
	 */
	swc_window_set_handler(window, &window_handler, data);
	swc_window_set_tiled(window);

	inis_window = inis_server_add_window(active_backend->server,
	    window->app_id, window->title, window);
	if (inis_window == NULL) {
		/*
		 * Must clear the handler before freeing data.  swc already has a
		 * pointer to data from swc_window_set_handler above; if we free it
		 * without clearing, swc_window_destroy will be called later with a
		 * stale pointer — use-after-free.
		 */
		swc_window_set_handler(window, NULL, NULL);
		free(data);
		return;
	}
	data->window = inis_window;
	/* swc_window_set_tiled above already pushed TILED to swc. */
	inis_window->swc_mode = INIS_SWC_MODE_TILED;

	/*
	 * Send the window its real geometry NOW, in the same event-loop iteration
	 * it was created in and before it is shown.  swc configures a tiled window
	 * with no explicit geometry to the full output size, so without this the
	 * client renders a full-screen first buffer and the window flashes before
	 * the deferred arrange (one tick later) snaps it to its tile.  Pre-placing
	 * here sends the correct configure up front — the equivalent of an X11 WM
	 * resizing a window before mapping it — so it appears directly in place.
	 *
	 * Only the new window is configured; the other windows are still resized
	 * by the deferred arrange, which avoids re-configuring them mid-configure
	 * (that crashes swc).
	 */
	inis_server_preplace_window(active_backend->server, inis_window);

	/* Now reveal it: geometry is already correct, so no full-screen flash. */
	swc_window_show(window);
	sync_transient_geometry(inis_window);

	/*
	 * Do NOT call swc_window_focus() here.  We are inside swc's new_window
	 * callback.  The currently focused window may not yet have committed its
	 * first rendered frame: swc_window_focus() would attempt to unfocus it
	 * by dereferencing view->0x48->0x30 without a null guard, and that
	 * pointer is NULL until the xdg configure round-trip completes (which
	 * can take >1 second on a loaded GPU).  Result: compositor segfault.
	 *
	 * Instead, record the new window as the pending focus target.  The
	 * arrange-idle callback checks whether the currently focused window's
	 * internal swc state is safe to unfocus (using the same pointer chain),
	 * and either applies focus immediately or starts a 200 ms retry timer.
	 *
	 * Warp the pointer to the new window: opening something new is one of the
	 * two cases (the other being arrow-key movefocus) where focus is driven by
	 * the keyboard rather than the cursor, so the cursor should follow.  This
	 * keeps focus-follows-mouse consistent — without the warp the pointer would
	 * stay over the old window and immediately steal focus back on the next
	 * motion event.
	 */
	request_pending_focus(active_backend, inis_window, true);
}

static void
swc_new_device(struct libinput_device *device)
{
	(void)device;
	inis_debug("swc input device detected");
}

static void
swc_activate(void)
{
	inis_info("swc session activated");
}

static void
swc_deactivate(void)
{
	inis_info("swc session deactivated");
}

static void
swc_window_destroy(void *data)
{
	struct swc_window_data *window_data = data;

	if (window_data == NULL || window_data->window == NULL)
		return;

	/*
	 * Clear any active pointer grab on this window.  We cannot call
	 * end_pointer_grab here because that function calls
	 * update_grabbed_window → inis_backend_apply_window → swc API, but
	 * we are already inside a swc destroy callback so the window is being
	 * torn down.  Directly clear the grab state instead; the timer will
	 * see grab_mode == NONE on its next tick and stop itself.
	 */
	if (active_backend != NULL &&
	    active_backend->grab_window == window_data->window) {
		active_backend->grab_mode = INIS_BACKEND_GRAB_NONE;
		active_backend->grab_window = NULL;
	}

	/*
	 * Null swc BEFORE remove_window.  The swc_window is being torn down;
	 * any call to swc_window_get_geometry or swc_window_focus inside
	 * remove_window → damage_window → sync_window_geometry would be a
	 * use-after-free.  Nulling here makes all backend guards (window->swc
	 * == NULL → return) fire safely instead of crashing.
	 */
	window_data->window->swc = NULL;
	window_data->window->interactive_grab = false;

	if (active_backend != NULL && active_backend->server != NULL)
		inis_server_remove_window(active_backend->server,
		    window_data->window);
	free(window_data);
}

static void
swc_window_title_changed(void *data)
{
	struct swc_window_data *window_data = data;
	struct swc_window *swc;

	if (window_data == NULL || window_data->window == NULL)
		return;
	swc = window_data->window->swc;
	if (swc != NULL)
		snprintf(window_data->window->title,
		    sizeof(window_data->window->title), "%s",
		    swc->title != NULL ? swc->title : "");
}

static void
swc_window_app_id_changed(void *data)
{
	struct swc_window_data *window_data = data;
	struct swc_window *swc;

	if (window_data == NULL || window_data->window == NULL)
		return;
	swc = window_data->window->swc;
	if (swc != NULL)
		snprintf(window_data->window->app_id,
		    sizeof(window_data->window->app_id), "%s",
		    swc->app_id != NULL ? swc->app_id : "");
}

static void
swc_window_parent_changed(void *data)
{
	struct swc_window_data *window_data = data;

	if (window_data == NULL || window_data->window == NULL ||
	    active_backend == NULL || active_backend->server == NULL)
		return;

	window_data->window->transient = window_data->window->swc != NULL &&
	    window_data->window->swc->parent != NULL;
	sync_transient_geometry(window_data->window);
	inis_server_arrange(active_backend->server);
	inis_server_flush_damage(active_backend->server, "parent-changed");
}

static void
swc_window_entered(void *data)
{
	struct swc_window_data *window_data = data;
	struct inis_server *server;

	if (window_data == NULL || window_data->window == NULL ||
	    active_backend == NULL || active_backend->server == NULL)
		return;

	server = active_backend->server;

	/*
	 * A fullscreen window holds exclusive focus.  Cursor entry into any
	 * other window must not steal it — the user is in an immersive context
	 * and only an explicit fullscreen toggle or window close should move
	 * focus away.
	 */
	if (server->focused_window != NULL &&
	    server->focused_window->state == INIS_WINDOW_FULLSCREEN &&
	    !window_is_transient_child_of(window_data->window,
	        server->focused_window) &&
	    window_data->window != server->focused_window)
		return;

	(void)inis_server_focus_window(server, window_data->window);
}

static void
swc_window_move(void *data)
{
	struct swc_window_data *window_data = data;
	struct inis_server *server;

	if (window_data == NULL || window_data->window == NULL ||
	    active_backend == NULL || active_backend->server == NULL)
		return;

	server = active_backend->server;
	(void)inis_server_focus_window(server, window_data->window);
	if (inis_server_begin_mouse_move(server) != 0)
		inis_debug("swc interactive move ignored");
}

static void
swc_window_resize(void *data)
{
	struct swc_window_data *window_data = data;
	struct inis_server *server;

	if (window_data == NULL || window_data->window == NULL ||
	    active_backend == NULL || active_backend->server == NULL)
		return;

	server = active_backend->server;
	(void)inis_server_focus_window(server, window_data->window);
	if (inis_server_begin_mouse_resize(server, INIS_WINDOW_EDGE_AUTO) != 0)
		inis_debug("swc interactive resize ignored");
}

static int
swc_signal(int signal_number, void *data)
{
	struct inis_backend *backend = data;

	inis_info("signal %d received, stopping compositor", signal_number);
	if (backend->server != NULL)
		inis_server_request_exit(backend->server);
	if (backend->display != NULL)
		wl_display_terminate(backend->display);
	return 0;
}

static int
swc_ipc(int fd, uint32_t mask, void *data)
{
	struct inis_backend *backend = data;

	(void)fd;
	if ((mask & WL_EVENT_READABLE) == 0)
		return 0;
	if (backend->server != NULL)
		inis_ipc_accept(backend->server);
	return 0;
}

static int
arrange_idle_cb(void *data)
{
	struct inis_backend *backend = data;
	struct inis_server *server;
	size_t i;

	/*
	 * Keep the timer source alive for reuse; only clear the scheduled flag.
	 * Nulling the pointer without wl_event_source_remove leaked one event
	 * source per arrange — with a layer-shell bar (Quickshell) triggering
	 * frequent relayouts, thousands of dead sources accumulated in the event
	 * loop and degraded it over time.
	 */
	backend->arrange_scheduled = false;
	server = backend->server;
	if (server == NULL)
		return 0;

	/*
	 * Re-focus after window removal.  All destroy callbacks in the batch
	 * have completed by the time this idle fires, so every dead window's
	 * swc pointer is NULL and its mapped flag is false.  Long-lived windows
	 * have completed their configure cycle — their swc internal state is
	 * fully initialized, so swc_window_focus is safe here.
	 *
	 * Only run the removal-refocus if there is no pending_focus_window;
	 * a new window may have been added in the same batch and already
	 * claimed the pending focus slot.
	 */
	if (server->focused_window == NULL &&
	    server->pending_focus_window == NULL) {
		for (i = server->window_count; i > 0; i--) {
			struct inis_window *candidate = &server->windows[i - 1];

			if (!candidate->mapped || candidate->swc == NULL)
				continue;
			request_pending_focus(backend, candidate, false);
			if (server->pending_focus_window == candidate)
				break;
		}
	}

	inis_server_arrange(server);
	inis_server_flush_damage(server, "deferred-arrange");

	/*
	 * Re-assert keyboard focus on the window inis believes is focused.  A
	 * layer-shell surface with keyboard interactivity (e.g. a Quickshell
	 * launcher) that dies drops swc's keyboard focus to NULL without
	 * telling the WM, leaving the keyboard dead until a manual refocus.
	 * Only when swc's focus is actually empty: re-asserting while a
	 * keyboard-interactive layer surface legitimately holds focus (open
	 * launcher) would steal its input mid-typing.
	 */
	if (server->focused_window != NULL &&
	    server->pending_focus_window == NULL &&
	    server->focused_window->mapped &&
	    server->focused_window->swc != NULL &&
	    !swc_keyboard_has_focus())
		inis_backend_focus_window(backend, server->focused_window);

	/*
	 * Apply pending focus after the deferred arrange.  The retry logic is
	 * time-based rather than peeking into swc internals, which keeps the
	 * compositor out of fragile pointer-chasing during window teardown.
	 */
	apply_pending_focus(backend);
	return 0;
}

static void
swc_binding(void *data, uint32_t time, uint32_t value, uint32_t state)
{
	struct inis_backend_binding *binding = data;
	struct inis_server *server;

	(void)time;
	(void)value;

	if (state == 0) {
		/*
		 * Released.  For mouse button bindings: if an interactive move/
		 * resize was in progress, sync the final geometry, end the grab,
		 * and clear the tracking flag.  For key bindings: no-op.
		 */
		if (binding->swc_type != SWC_BINDING_BUTTON)
			return;
		server = binding->server;
		if (server == NULL)
			return;
		if (server->backend.grab_mode == INIS_BACKEND_GRAB_NONE)
			return;
		end_pointer_grab(&server->backend);
		return;
	}
	/*
	 * state=1 is initial key/button press; state=2 is key repeat.
	 * Suppress repeat for all bindings — exec commands must not spawn
	 * multiple processes just because the user held a key.
	 */
	if (binding->swc_type == SWC_BINDING_KEY && state != 1)
		return;
	inis_dispatch(binding->server, binding->dispatcher, binding->args);
}

static void
install_bindings(struct inis_backend *backend)
{
	struct inis_server *server = backend->server;
	size_t i;

	if (server == NULL)
		return;

	backend->backend_binding_count = 0;
	for (i = 0; i < server->binding_count; i++) {
		struct inis_binding *binding = &server->bindings[i];
		struct inis_backend_binding *bb;
		enum swc_binding_type type;
		unsigned int value;
		unsigned int mods;

		if (inis_bind_numeric_value(binding, &value) != 0) {
			inis_warn("skipping unsupported binding key: %s",
			    binding->key);
			continue;
		}
		if (backend->backend_binding_count >= INIS_MAX_BINDINGS)
			break;

		bb = &backend->backend_bindings[
		    backend->backend_binding_count++];
		bb->server = server;
		snprintf(bb->dispatcher, sizeof(bb->dispatcher), "%s",
		    binding->dispatcher);
		snprintf(bb->args, sizeof(bb->args), "%s", binding->args);

		type = binding->mouse ? SWC_BINDING_BUTTON : SWC_BINDING_KEY;
		mods = inis_bind_modifier_mask(binding);
		bb->swc_type  = (uint32_t)type;
		bb->swc_mods  = mods;
		bb->swc_value = value;
		if (swc_add_binding(type, mods, value, swc_binding, bb) != 0)
			inis_warn("failed to register binding: %s %s",
			    binding->mods, binding->key);
		else
			inis_debug("registered binding: %s %s -> %s %s",
			    binding->mods, binding->key,
			    binding->dispatcher, binding->args);
	}
}

int
inis_backend_swc_start(struct inis_backend *backend)
{
	struct wl_event_loop *loop;

	ensure_xdg_runtime_dir();
	active_backend = backend;
	backend->display = wl_display_create();
	if (backend->display == NULL) {
		inis_error("failed to create Wayland display");
		active_backend = NULL;
		return -1;
	}

	backend->wayland_socket = wl_display_add_socket_auto(backend->display);
	if (backend->wayland_socket == NULL) {
		inis_error("failed to create Wayland socket");
		wl_display_destroy(backend->display);
		backend->display = NULL;
		active_backend = NULL;
		return -1;
	}
	snprintf(backend->wayland_env, sizeof(backend->wayland_env),
	    "WAYLAND_DISPLAY=%s", backend->wayland_socket);
	putenv(backend->wayland_env);
	inis_info("Wayland socket: %s", backend->wayland_socket);

	if (!swc_initialize(backend->display, NULL, &manager)) {
		inis_error("swc_initialize failed");
		wl_display_destroy(backend->display);
		backend->display = NULL;
		backend->wayland_socket = NULL;
		active_backend = NULL;
		return -1;
	}

	loop = wl_display_get_event_loop(backend->display);
	backend->sigint_source  = wl_event_loop_add_signal(loop, SIGINT,
	    swc_signal, backend);
	backend->sigterm_source = wl_event_loop_add_signal(loop, SIGTERM,
	    swc_signal, backend);
	if (backend->sigint_source == NULL || backend->sigterm_source == NULL) {
		inis_error("failed to install signal handlers");
		swc_finalize();
		wl_display_destroy(backend->display);
		backend->display = NULL;
		backend->wayland_socket = NULL;
		active_backend = NULL;
		return -1;
	}
	if (backend->server != NULL &&
	    inis_ipc_get_fd(&backend->server->ipc) >= 0) {
		backend->ipc_source = wl_event_loop_add_fd(loop,
		    inis_ipc_get_fd(&backend->server->ipc),
		    WL_EVENT_READABLE, swc_ipc, backend);
		if (backend->ipc_source == NULL)
			inis_warn("failed to add IPC fd to event loop");
	}

	backend->started = true;
	install_bindings(backend);
	inis_info("swc backend initialized");
	return 0;
}

int
inis_backend_swc_run(struct inis_backend *backend)
{
	if (backend->display == NULL)
		return -1;
	inis_info("entering swc event loop");
	wl_display_run(backend->display);
	inis_info("left swc event loop");
	return 0;
}

void
inis_backend_swc_request_stop(struct inis_backend *backend)
{
	if (backend->display != NULL)
		wl_display_terminate(backend->display);
}

void
inis_backend_swc_finish(struct inis_backend *backend)
{
	if (backend->arrange_idle != NULL) {
		wl_event_source_remove(backend->arrange_idle);
		backend->arrange_idle = NULL;
	}
	if (backend->focus_retry_timer != NULL) {
		wl_event_source_remove(backend->focus_retry_timer);
		backend->focus_retry_timer = NULL;
	}
	if (backend->grab_timer != NULL) {
		wl_event_source_remove(backend->grab_timer);
		backend->grab_timer = NULL;
	}
	if (backend->sigint_source != NULL) {
		wl_event_source_remove(backend->sigint_source);
		backend->sigint_source = NULL;
	}
	if (backend->sigterm_source != NULL) {
		wl_event_source_remove(backend->sigterm_source);
		backend->sigterm_source = NULL;
	}
	if (backend->ipc_source != NULL) {
		wl_event_source_remove(backend->ipc_source);
		backend->ipc_source = NULL;
	}
	swc_finalize();
	active_backend = NULL;
	if (backend->display != NULL) {
		wl_display_destroy(backend->display);
		backend->display = NULL;
	}
	backend->wayland_socket = NULL;
}

void
inis_backend_swc_close_window(struct inis_backend *backend,
    struct inis_window *window)
{
	(void)backend;
	if (window->swc != NULL)
		swc_window_close(window->swc);
}

void
inis_backend_swc_set_window_visible(struct inis_backend *backend,
    struct inis_window *window, bool visible)
{
	(void)backend;
	if (window->swc == NULL)
		return;
	if (visible)
		swc_window_show(window->swc);
	else
		swc_window_hide(window->swc);
}

void
inis_backend_swc_raise_window(struct inis_backend *backend,
    struct inis_window *window)
{
	size_t n;

	if (window == NULL || window->swc == NULL)
		return;
	/*
	 * swc_window_set_fullscreen() places the window in a special
	 * compositor layer above all stacked windows.  Calling
	 * swc_window_stack() on it would pull it back into the stacked
	 * layer and undo the fullscreen state.
	 */
	if (window->state == INIS_WINDOW_FULLSCREEN)
		return;

	/*
	 * swc_window_stack(w, -1) moves w one step towards the front.
	 * Calling it window_count times guarantees w reaches the top
	 * regardless of its starting position.
	 */
	n = backend->server != NULL ? backend->server->window_count : 1;
	while (n-- > 0)
		swc_window_stack(window->swc, -1);
}

void
inis_backend_swc_focus_window(struct inis_backend *backend,
    struct inis_window *window)
{
	(void)backend;
	if (window != NULL && window->swc != NULL)
		swc_window_focus(window->swc);
	else
		swc_window_focus(NULL);
}

void
inis_backend_swc_warp_pointer(struct inis_backend *backend, int x, int y)
{
	(void)backend;
	(void)swc_cursor_warp(x * 256, y * 256);
}

void
inis_backend_swc_update_window_style(struct inis_backend *backend,
    struct inis_window *window)
{
	if (window == NULL || window->swc == NULL)
		return;

	swc_window_set_border(window->swc,
	    window->focused
	        ? INIS_COLOR_FOCUSED_BORDER
	        : INIS_COLOR_NORMAL_BORDER,
	    window->no_border
	        ? 0
	        : (uint32_t)backend->server->config.border_size,
	    INIS_COLOR_NORMAL_BORDER, 0);
}

void
inis_backend_swc_apply_window(struct inis_backend *backend,
    struct inis_window *window)
{
	struct inis_rect *rect;

	if (window->swc == NULL)
		return;

	inis_backend_swc_update_window_style(backend, window);

	/*
	 * Only push a mode change to swc when it actually changed.  swc's mode
	 * setters send an xdg configure unconditionally, so re-asserting the
	 * mode for every window on every arrange (frequent with an animating
	 * layer-shell bar) floods clients with configure events.
	 */
	if (window->state == INIS_WINDOW_FULLSCREEN) {
		if (backend->server != NULL &&
		    window->monitor_index < backend->server->monitor_count &&
		    (window->swc_mode != INIS_SWC_MODE_FULLSCREEN ||
		     window->swc_fullscreen_monitor != window->monitor_index)) {
			swc_window_set_fullscreen(window->swc,
			    backend->server->monitors[
			        window->monitor_index].swc);
			window->swc_mode = INIS_SWC_MODE_FULLSCREEN;
			window->swc_fullscreen_monitor = window->monitor_index;
		}
		return;
	}

	if (window->state == INIS_WINDOW_FLOATING) {
		rect = &window->floating;
		if (window->transient) {
			if (window->swc_mode != INIS_SWC_MODE_STACKED) {
				swc_window_set_stacked(window->swc);
				window->swc_mode = INIS_SWC_MODE_STACKED;
			}
			swc_window_set_position(window->swc, rect->x, rect->y);
			return;
		}
	} else {
		rect = &window->tiled;
	}
	if (window->swc_mode != INIS_SWC_MODE_TILED) {
		swc_window_set_tiled(window->swc);
		window->swc_mode = INIS_SWC_MODE_TILED;
	}

	swc_window_set_geometry(window->swc,
	    &(struct swc_rectangle){
	        .x      = rect->x,
	        .y      = rect->y,
	        .width  = (uint32_t)rect->w,
	        .height = (uint32_t)rect->h,
	    });
}

void
inis_backend_swc_begin_move(struct inis_backend *backend,
    struct inis_window *window)
{
	start_pointer_grab(backend, window, INIS_BACKEND_GRAB_MOVE);
}

void
inis_backend_swc_begin_resize(struct inis_backend *backend,
    struct inis_window *window, uint32_t edges)
{
	(void)edges;
	start_pointer_grab(backend, window, INIS_BACKEND_GRAB_RESIZE);
}

static void
uninstall_bindings(struct inis_backend *backend)
{
	size_t i;

	for (i = 0; i < backend->backend_binding_count; i++) {
		struct inis_backend_binding *bb = &backend->backend_bindings[i];

		swc_remove_binding((enum swc_binding_type)bb->swc_type,
		    bb->swc_mods, bb->swc_value);
	}
	backend->backend_binding_count = 0;
}

void
inis_backend_swc_schedule_arrange(struct inis_backend *backend)
{
	struct wl_event_loop *loop;

	if (backend->arrange_scheduled)
		return;
	if (backend->display == NULL)
		return;
	/*
	 * Use a short timer instead of an idle callback.  Closing heavy clients
	 * can keep the Wayland loop busy enough that idle arrange is delayed,
	 * leaving stale layout gaps until another event happens.  The timer
	 * source is created once and re-armed thereafter.
	 */
	if (backend->arrange_idle == NULL) {
		loop = wl_display_get_event_loop(backend->display);
		backend->arrange_idle = wl_event_loop_add_timer(loop,
		    arrange_idle_cb, backend);
	}
	if (backend->arrange_idle != NULL) {
		wl_event_source_timer_update(backend->arrange_idle, 1);
		backend->arrange_scheduled = true;
	}
}


void
inis_backend_swc_reload_bindings(struct inis_backend *backend)
{
	struct inis_server *server = backend->server;

	if (server == NULL || !backend->started)
		return;

	uninstall_bindings(backend);

	server->binding_count = 0;
	server->rule_count = 0;
	inis_config_init(&server->config);

	{
		const char *home = getenv("HOME");
		char path[512];

		if (home != NULL && home[0] != '\0') {
			snprintf(path, sizeof(path),
			    "%s/.config/inis/inis.conf", home);
			if (inis_config_load_file(&server->config,
			    server->bindings, &server->binding_count,
			    server->rules, &server->rule_count, path) == 0)
				inis_info("config reloaded: %s", path);
			else
				inis_warn("config reload failed or not found: %s",
				    path);
		}
	}

	install_bindings(backend);
	inis_info("bindings reinstalled: %zu", backend->backend_binding_count);
}
