#ifndef INIS_BACKEND_SWC_H
#define INIS_BACKEND_SWC_H

#include "backend.h"

#include <stdint.h>

int inis_backend_swc_start(struct inis_backend *backend);
int inis_backend_swc_run(struct inis_backend *backend);
void inis_backend_swc_request_stop(struct inis_backend *backend);
void inis_backend_swc_finish(struct inis_backend *backend);
void inis_backend_swc_close_window(struct inis_backend *backend,
    struct inis_window *window);
void inis_backend_swc_focus_window(struct inis_backend *backend,
    struct inis_window *window);
void inis_backend_swc_warp_pointer(struct inis_backend *backend, int x, int y);
void inis_backend_swc_update_window_style(struct inis_backend *backend,
    struct inis_window *window);
void inis_backend_swc_apply_window(struct inis_backend *backend,
    struct inis_window *window);
void inis_backend_swc_set_window_visible(struct inis_backend *backend,
    struct inis_window *window, bool visible);
void inis_backend_swc_raise_window(struct inis_backend *backend,
    struct inis_window *window);
void inis_backend_swc_begin_move(struct inis_backend *backend,
    struct inis_window *window);
void inis_backend_swc_begin_resize(struct inis_backend *backend,
    struct inis_window *window, uint32_t edges);
void inis_backend_swc_reload_bindings(struct inis_backend *backend);
void inis_backend_swc_schedule_arrange(struct inis_backend *backend);

#endif
