#!/bin/sh
# Reproduce the inis crash under AddressSanitizer/UBSan and capture a full trace.
# Run this from a FREE virtual terminal (e.g. Ctrl+Alt+F3), logged in as your user
# — NOT from inside an existing graphical session.
set -u
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
bin="$here/inis-asan"

log_dir="${XDG_STATE_HOME:-$HOME/.local/state}/inis"
mkdir -p "$log_dir"
log="$log_dir/asan.log"

# ASan/UBSan: stop at first error, symbolized stacks, no leak noise at exit.
export ASAN_OPTIONS="halt_on_error=1:abort_on_error=1:detect_leaks=0:strict_string_checks=1:detect_stack_use_after_return=1:print_stats=0:log_path=$log_dir/asan"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=0"

# swc-launch must be setuid root and in PATH (you already use it for the session).
if ! command -v swc-launch >/dev/null 2>&1; then
	_neu="${INIS_NEU_PREFIX:-$HOME/.local/inis-neu-prefix}"
	export PATH="$_neu/bin:$PATH"
fi

if [ -z "${XDG_RUNTIME_DIR:-}" ] || [ ! -d "${XDG_RUNTIME_DIR:-}" ]; then
	export XDG_RUNTIME_DIR="/run/user/$(id -u)"
fi
unset WAYLAND_DISPLAY DISPLAY

echo "logging ASan output to: $log_dir/asan.<pid>  (and console)"
echo "launching: $bin"
# Tee console + file. ASan also writes to asan.<pid> via log_path.
exec swc-launch -- "$bin" 2>&1 | tee "$log"
