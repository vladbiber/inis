# Config

`inis` has two configuration paths:

1. `config.def.h` / `config.h` for compile-time defaults.
2. `~/.config/inis/inis.conf` later, as a small runtime format.

The runtime format is intentionally a Hyprland-like subset, not full Hyprland
compatibility.

## Example

```conf
set mainMod SUPER
set terminal kitty
set menu hyprlauncher
set fileManager dolphin

exec-once waybar
exec-once swaybg -i ~/wallpapers/wall.png -m fill

general gaps_in 4
general gaps_out 8
general border_size 2
general layout master

decoration rounding 0
decoration opacity false
decoration shadows false
decoration blur false

animations enabled false

bind $mainMod Q exec $terminal
bind $mainMod C killactive
bind $mainMod M shutdownmenu
bind $mainMod E exec $fileManager
bind $mainMod V togglefloating
bind $mainMod R exec $menu
bind $mainMod P pseudo
bind $mainMod J togglesplit
bind $mainMod S togglespecialworkspace magic
bind $mainMod Left movefocus left
bind $mainMod Down movefocus down
bind $mainMod Up movefocus up
bind $mainMod Right movefocus right

bind $mainMod 1 workspace 1
bind $mainMod 2 workspace 2
bind $mainMod 3 workspace 3
bind $mainMod 4 workspace 4
bind $mainMod 5 workspace 5
bind $mainMod 6 workspace 6
bind $mainMod 7 workspace 7
bind $mainMod 8 workspace 8
bind $mainMod 9 workspace 9
bind $mainMod 0 workspace 10

bind $mainMod SHIFT 1 movetoworkspace 1
bind $mainMod SHIFT 2 movetoworkspace 2
bind $mainMod SHIFT 3 movetoworkspace 3
bind $mainMod SHIFT 4 movetoworkspace 4
bind $mainMod SHIFT 5 movetoworkspace 5
bind $mainMod SHIFT 6 movetoworkspace 6
bind $mainMod SHIFT 7 movetoworkspace 7
bind $mainMod SHIFT 8 movetoworkspace 8
bind $mainMod SHIFT 9 movetoworkspace 9
bind $mainMod SHIFT 0 movetoworkspace 10
bind $mainMod SHIFT S movetoworkspace special:magic

bindm $mainMod mouse_down workspace e+1
bindm $mainMod mouse_up workspace e-1
bindm $mainMod mouse:272 movewindow
bindm $mainMod mouse:273 resizewindow

windowrule float app_id:pavucontrol
windowrule center app_id:pavucontrol
windowrule workspace 2 app_id:librewolf
```

## Rules

The parser is deliberately small:

- no nested blocks;
- no scripting;
- no runtime language;
- no includes at first;
- variable substitution only for simple `$name` values;
- unknown keys should warn and continue where possible.

The config parser must populate plain C structs. It must not call render code or
backend code directly.

## Current Variables And Startup

`set NAME VALUE` stores a simple variable. `$NAME` is expanded in later config
lines before bind/rule/startup parsing. Unknown variables are left unchanged so
shell variables such as `$HOME` can still be passed to `exec-once` commands.

Built-in variables:

- `$mainMod` defaults to `SUPER`
- `$terminal` defaults to `kitty`
- `$menu` defaults to `hyprlauncher`
- `$fileManager` defaults to `dolphin`

`exec-once COMMAND` stores startup commands. The server starts them after the
Wayland backend is initialized, so external tools inherit `WAYLAND_DISPLAY`.
They are ordinary child processes launched through the existing `exec`
dispatcher; the compositor does not become a bar, wallpaper daemon, or service
manager.

## Current Window Rules

The MVP applies matching window rules in config order when a new window appears.
Current supported actions:

- `float`
- `tile`
- `fullscreen`
- `center`
- `workspace NAME`
- `size W H`
- `move X Y`
- `noborder`
- `noanim` as stored metadata only

Current match keys:

- `app_id:VALUE`
- `title:VALUE`

Examples:

```conf
windowrule float app_id:pavucontrol
windowrule center app_id:pavucontrol
windowrule size 900 600 app_id:pavucontrol
windowrule workspace 2 app_id:librewolf
windowrule noborder title:Picture-in-Picture
```

`monitor NAME` is parsed but not applied yet.

## Reserved Space for External Bars

`inis` keeps bars out of core, so it does not draw a bar itself. Until full
layer-shell support lands (ROADMAP Phase 7), you can reserve screen edges for an
external bar (Waybar, yambar, sfwbar, …) so tiled and floating windows never
cover it.

Reserve space in the config with four pixel values, in `TOP BOTTOM LEFT RIGHT`
order, applied to every output:

```conf
# Reserve a 34px strip at the top for a bar.
reserved 34 0 0 0
exec-once waybar
```

The reservation is subtracted from the area the backend already reports as
usable, so it composes with anything the backend itself reserves.

### Runtime control

A bar that knows its own size can claim or release space at runtime through
`inisctl`, for all outputs or a single named one:

```sh
# All monitors: reserve 34px at the top.
inisctl dispatch reserved 34 0 0 0

# Only output swc-0: reserve a 40px bottom bar.
inisctl dispatch reserved swc-0 0 40 0 0

# Release the reservation again.
inisctl dispatch reserved 0 0 0 0
```

`inisctl monitors` reports each output's geometry, effective `usable` area, and
current `reserved` insets (as `top,bottom,left,right`), so a bar can read back
what the compositor applied. `inisctl reload` re-applies the configured
`reserved` defaults to every output.

If the requested insets would leave no usable area, they are ignored and the
backend usable area is kept, with a warning in the log.

`inis` also now tracks the backend's `usable_geometry_changed` notification, so
if the backend reserves space for a surface, the layout responds automatically.

## Current Binding Support

The swc adapter can register the current binding table through `swc_add_binding`.
The first supported key subset is intentionally small:

- modifiers: `SUPER`, `$mainMod`, `SHIFT`, `CTRL`, `CONTROL`, `ALT`;
- single-character keys: `A`-`Z`, `0`-`9`;
- named keys: `Return`, `Enter`, `Escape`, `Esc`, `Left`, `Right`, `Up`,
  `Down`;
- mouse buttons: `mouse:N`.

Unsupported key names are skipped with a warning rather than guessed.

Current focus dispatchers:

- `movefocus left|right|up|down`
- `cyclenext`
- `cycleprev`

For now, directional focus is a compact compatibility layer over next/previous
window cycling. Real geometry-aware directional focus belongs after the window
model and layout code are more complete.

Current workspace dispatchers:

- `workspace NAME`
- `workspace e+1`
- `workspace e-1`
- `workspace previous`
- `previous`
- `movetoworkspace NAME`
- `movetoworkspacesilent NAME`
- `togglespecialworkspace NAME`

The first special workspace implementation is intentionally small. It stores
special workspaces as normal workspace structs marked `special`; toggling only
changes visibility, not the active normal workspace. Windows moved to
`special:NAME` are made floating for predictable scratchpad behavior.

Current window geometry dispatchers:

- `centerwindow`
- `moveactive DX DY`
- `resizeactive DW DH`
- `swapwindow left|right|up|down`
- `closewindow` as an alias for `killactive`

These operate on the focused window. If the focused window is tiled,
`moveactive`, `resizeactive`, and `centerwindow` convert it to floating first.
Tiled resize semantics are deliberately postponed; they need layout state, not
ad hoc mutation inside the dispatcher.

`swapwindow` is implemented for tiled windows on the active normal workspace. It
swaps explicit layout order values instead of moving window structs in memory,
because the backend keeps pointers to those structs.

Current planned Hyprland-compatible no-op dispatchers:

- `pseudo`
- `togglesplit`

They exist so the official-style default binds register cleanly. Real behavior
belongs to the future dwindle-like layout, not the current master layout.

`shutdownmenu` tries to run `hyprshutdown`. If it is not available in `PATH`, it
exits the compositor. This matches the practical Hyprland-style expectation
without making a shutdown UI part of the compositor.

`mouse_down` and `mouse_up` are parsed as compatibility names for workspace
scroll binds. The current swc/neuswc binding path registers key/button binds;
real scroll-axis behavior may still need explicit axis handling in the backend
adapter.
