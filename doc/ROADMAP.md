# Roadmap

Current backend decision: `inis` uses neuswc through its swc-compatible C API.
wlroots is not part of this project path.

Current stage: phases 0-4 are implemented enough to build and exercise the
model. Phase 5 is mostly implemented for the single-monitor MVP. Phase 6 is the
next architectural boundary: explicit damage/frame discipline in `inis` rather
than relying only on backend behavior.

## Phase 0: Repository Skeleton

Compiles: `inis` binary with logging, structs, dispatcher table, layout function,
and startup/shutdown.

Tests: `make`, `./inis -d`.

Status: done.

## Phase 1: Display and Backend

Compiles with neuswc. Creates Wayland display, initializes outputs and input,
exports `WAYLAND_DISPLAY`, enters the event loop, and exits cleanly.

Tests: run from TTY or nested backend if available; verify clean shutdown.

Status: implemented against local neuswc builds. Needs more real-session
testing.

## Phase 2: One Wayland Window

Support xdg-shell toplevel map/unmap/configure, keyboard focus, pointer focus,
and close active window.

Tests: run a terminal, focus it, close it, check popups are not tiled.

Status: basic toplevel window callbacks, focus, and close exist through neuswc.
Popup policy is delegated to neuswc for now and needs real app testing.

## Phase 3: Models and Tiling

Add monitor, workspace, and window membership. Implement master layout.

Tests: open multiple terminals, verify deterministic rectangles.

Status: implemented for the single-monitor MVP. Multi-monitor ownership is
still mostly structural.

## Phase 4: Keybinds and Dispatchers

Add official-style Hyprland example keybinds: exec kitty, killactive,
shutdownmenu, workspaces, move-to-workspace, special workspace, mouse move,
mouse resize, pseudo/togglesplit placeholders, and workspace scroll commands.
The neuswc adapter registers a small explicit key/button subset through
`swc_add_binding`.

Tests: Super+Q, Super+C, Super+M, Super+E, Super+R, Super+1..0,
Super+Shift+1..0.

Status: implemented. Scroll-wheel binds may need axis handling beyond the
current button binding path.

## Phase 5: Floating, Fullscreen, Special Workspace

Add floating toggle, mouse move/resize, fullscreen, and one special workspace.

Tests: pavucontrol floating rule, fullscreen terminal/browser, scratchpad.

Status: implemented for focused windows and one named special workspace. Mouse
move/resize now call neuswc interactive move/resize. Real pointer-interaction
testing is still required.

## Phase 6: Damage and Frame Discipline

Add per-output damage tracking, frame callback discipline, and no idle repaint.

Tests: idle CPU close to 0%, terminal typing does not globally repaint.

Status: next major phase. Current code has placeholder damage/render modules;
explicit compositor-owned damage policy is not complete.

## Phase 7: Layer Shell

Add enough layer-shell for Waybar/yambar/sfwbar: layers, anchors, margins,
exclusive zones, keyboard interactivity, output assignment, and damage.

Tests: Waybar exclusive zone, clock update does not repaint all clients.

Status: partial. The full layer-shell protocol (layer surfaces, anchors,
keyboard interactivity) is still postponed — the installed neuswc `swc.h` does
not expose a layer-surface API for `inis` to drive. As an interim step, `inis`
now owns reserved-space policy: configurable per-edge exclusive zones for
external bars (`reserved TOP BOTTOM LEFT RIGHT` in config and
`inisctl dispatch reserved [MONITOR] ...` at runtime), and it tracks the
backend's `usable_geometry_changed` notification so the layout reacts when the
backend reserves space.

## Phase 8: XWayland

Support XWayland windows, class/title rules, focus, close, workspace movement,
and common floating rules.

Tests: X11 apps, games, legacy utilities.

Status: postponed.

## Phase 9: inisctl IPC

Add Unix socket, text commands, optional JSON, and stable scriptable queries.

Tests: `inisctl workspaces`, `inisctl clients`, `inisctl dispatch workspace 2`.

Status: basic text IPC and `inisctl` exist. Optional JSON is postponed.

## Phase 10: Polish

Better rules, monitor rules, named workspaces, optional opacity, optional
event-driven animations, and optional border effects.
