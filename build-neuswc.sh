#!/bin/sh
set -eu

prefix="${NEU_PREFIX:-$HOME/.local/inis-neu-prefix}"
# Persistent by default: /tmp loses the checkouts (and any local state) on
# reboot, which cost us the exact source tree of a deployed build once.
src_root="${NEU_SRC_ROOT:-$HOME/.local/src}"
jobs="${JOBS:-}"

neuwld_dir="$src_root/neuwld"
neuswc_dir="$src_root/neuswc"
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# Single combined patch over upstream neuswc, applied with `git apply`:
#   - swc_cursor_warp()           (inis warps the pointer on focus)
#   - wl_shm fallback for GPU bufs (swc's linear renderer black-screens on EGL)
#   - XWayland for modern Xwayland (24.1): -listen -> -listenfd, and reset
#     SIGCHLD to SIG_DFL in the Xwayland child so its xkbcomp waitpid() works.
#   - 2026-08-13 crash fixes (multi-window client teardown, OBS/Quickshell):
#     set_parent(nil) NULL-deref, dangling parent pointers on destroy, stale
#     view handler links, end_interaction in window_finalize, popup
#     ack_configure type confusion, shm pool resize invalidating buffers
#     (MREMAP_MAYMOVE), screen freed before layer surfaces, keyboard leave
#     NULL guard, swc_keyboard_has_focus() for WM focus recovery.
#   NOTE: generated against neuswc commit 975ad56; `git pull` may make it
#   conflict — pin with NO_UPDATE=1 or rebase the patch if upstream moved.
integration_patch="$script_dir/patches/neuswc-inis-integration.patch"
# Patch over upstream neuwld (the wld renderer), applied with `git apply`:
#   - copy_region composites with PIXMAN_OP_OVER, not SRC, so translucent
#     layer-shell surfaces (e.g. a Quickshell shell like noctalia) blend
#     instead of painting their transparent pixels as solid black.
neuwld_patch="$script_dir/patches/neuwld-inis-blend.patch"

run_compile() {
	dir=$1
	if [ -n "$jobs" ]; then
		meson compile -C "$dir/build" -j "$jobs"
	else
		meson compile -C "$dir/build"
	fi
}

clone_or_update() {
	url=$1
	dir=$2

	if [ -d "$dir/.git" ]; then
		if [ "${NO_UPDATE:-0}" = "1" ]; then
			echo "using existing source: $dir"
		else
			# fetch, not pull: the checkout is pinned to a commit below.
			git -C "$dir" fetch --quiet
		fi
	else
		if [ "${NO_UPDATE:-0}" = "1" ]; then
			echo "missing source and NO_UPDATE=1: $dir" >&2
			exit 1
		fi
		git clone "$url" "$dir"
	fi
}

clone_or_update "https://git.sr.ht/~shrub900/neuwld" "$neuwld_dir"
clone_or_update "https://git.sr.ht/~shrub900/neuswc" "$neuswc_dir"

# PINNED upstream commits — the patches are generated against exactly these.
# Building upstream HEAD once regressed rendering (neuwld driver-selection
# changes switched the renderer on real hardware: ghost trails + black
# layer surfaces).  Bump deliberately, regenerating both patches.
neuwld_commit="bb5d247e7b3d0f68dda3990f9b2100aaaec85d28"
neuswc_commit="975ad56221d4545bdd44d14fdd4cac796de207d9"
git -C "$neuwld_dir" checkout --quiet "$neuwld_commit"
git -C "$neuswc_dir" checkout --quiet "$neuswc_commit"

# Apply the combined integration patch idempotently: `--reverse --check`
# succeeds only when it is already applied, in which case we skip.
if ! git -C "$neuswc_dir" apply --reverse --check "$integration_patch" 2>/dev/null; then
	git -C "$neuswc_dir" apply "$integration_patch"
	echo "applied neuswc integration patch"
else
	echo "neuswc integration patch already applied"
fi

if ! git -C "$neuwld_dir" apply --reverse --check "$neuwld_patch" 2>/dev/null; then
	git -C "$neuwld_dir" apply "$neuwld_patch"
	echo "applied neuwld blend patch"
else
	echo "neuwld blend patch already applied"
fi

if [ ! -d "$neuwld_dir/build" ]; then
	meson setup "$neuwld_dir/build" "$neuwld_dir" --prefix="$prefix"
fi
run_compile "$neuwld_dir"
meson install -C "$neuwld_dir/build"

export PKG_CONFIG_PATH="$prefix/lib64/pkgconfig:$prefix/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

if [ ! -d "$neuswc_dir/build" ]; then
	meson setup "$neuswc_dir/build" "$neuswc_dir" --prefix="$prefix"
fi
run_compile "$neuswc_dir"
meson install -C "$neuswc_dir/build"

echo "neuswc stack installed to: $prefix"
echo "Use:"
echo "  PKG_CONFIG_PATH=$prefix/lib64/pkgconfig:$prefix/lib/pkgconfig NEUSWC_PREFIX=$prefix make USE_NEUSWC=1"
echo "  ./inis --features  # rpath embeds library path, no LD_LIBRARY_PATH needed"
echo "  make USE_NEUSWC=1 install  # instala in /usr/local"
