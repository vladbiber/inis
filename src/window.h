#ifndef INIS_WINDOW_H
#define INIS_WINDOW_H

#include "inis.h"
#include "layout.h"

#if INIS_HAVE_SWC
struct swc_window;
#endif

enum inis_window_state {
	INIS_WINDOW_TILED,
	INIS_WINDOW_FLOATING,
	INIS_WINDOW_FULLSCREEN
};

/*
 * Shadow of the mode last pushed to swc for this window.  swc's
 * set_tiled/set_stacked/set_fullscreen send an xdg configure unconditionally
 * on every call, so re-asserting the mode on every arrange floods clients
 * with configures; the shadow lets apply skip redundant mode calls.
 * UNKNOWN (0) forces the first apply to push a real mode.
 */
enum inis_swc_mode {
	INIS_SWC_MODE_UNKNOWN,
	INIS_SWC_MODE_STACKED,
	INIS_SWC_MODE_TILED,
	INIS_SWC_MODE_FULLSCREEN
};

struct inis_window {
	char app_id[INIS_MAX_NAME];
	char title[INIS_MAX_NAME];
	struct inis_rect tiled;
	struct inis_rect floating;
	enum inis_window_state state;
	unsigned int layout_order;
	unsigned int workspace_index;
	unsigned int monitor_index;
	bool mapped;
	bool focused;
	bool urgent;
	bool no_border;
	bool no_anim;
	bool was_floating;
	bool transient;
	bool interactive_grab;
	enum inis_swc_mode swc_mode;
	unsigned int swc_fullscreen_monitor;
	struct wc_view layout_view;
#if INIS_HAVE_SWC
	struct swc_window *swc;
#else
	void *swc;
#endif
};

void inis_window_init(struct inis_window *window);
bool inis_window_rect_valid(const struct inis_rect *rect);
void inis_window_ensure_floating_rect(struct inis_window *window,
    const struct inis_rect *usable_area);
bool inis_window_make_floating(struct inis_window *window,
    const struct inis_rect *usable_area);
bool inis_window_make_floating_centered(struct inis_window *window,
    const struct inis_rect *usable_area);
void inis_window_center_floating(struct inis_window *window,
    const struct inis_rect *usable_area);
void inis_window_set_floating(struct inis_window *window, bool floating);
void inis_window_set_fullscreen(struct inis_window *window, bool fullscreen);

#endif
