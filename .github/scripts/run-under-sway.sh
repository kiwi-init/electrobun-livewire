#!/usr/bin/env bash
# Run a command inside a headless sway (wlroots) session with XWayland off,
# so GTK can only open a native Wayland display: anything that still needs
# X11 fails here instead of passing through XWayland.
#
#   run-under-sway.sh [--scale S] [--screenshots DIR] -- command args...
#
# --screenshots DIR saves a grim screenshot of the session every 2 seconds
# while the command runs. Exits with the command's status. Sway's log is
# printed when it fails.
set -euo pipefail

scale=1
screenshots=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --scale) scale="$2"; shift 2 ;;
    --screenshots) screenshots="$2"; shift 2 ;;
    --) shift; break ;;
    *) break ;;
  esac
done
if [[ ! "$scale" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
  echo "--scale must be a number, got: $scale" >&2
  exit 2
fi
if [[ $# -eq 0 ]]; then
  echo "usage: $0 [--scale S] [--screenshots DIR] -- command args..." >&2
  exit 2
fi

work="$(mktemp -d)"
runtime_dir="$work/runtime"
mkdir -m 0700 "$runtime_dir"
config="$work/sway.config"
sway_log="$work/sway.log"
cat > "$config" <<CONFIG
xwayland disable
output * resolution 1920x1080 position 0 0 scale $scale
CONFIG

export XDG_RUNTIME_DIR="$runtime_dir"
export XDG_SESSION_TYPE=wayland
export WLR_BACKENDS=headless
export WLR_RENDERER=pixman
export WLR_LIBINPUT_NO_DEVICES=1
unset DISPLAY WAYLAND_DISPLAY

sway --config "$config" > "$sway_log" 2>&1 &
sway_pid=$!
shooter_pid=""
cleanup() {
  [[ -n "$shooter_pid" ]] && kill "$shooter_pid" 2> /dev/null || true
  kill "$sway_pid" 2> /dev/null || true
  wait "$sway_pid" 2> /dev/null || true
}
trap cleanup EXIT

socket=""
for _ in $(seq 1 100); do
  if ! kill -0 "$sway_pid" 2> /dev/null; then
    echo "sway exited before it was ready:" >&2
    cat "$sway_log" >&2
    exit 1
  fi
  socket="$(find "$runtime_dir" -maxdepth 1 -name 'wayland-*' -type s -printf '%f\n' | head -n 1)"
  [[ -n "$socket" ]] && break
  sleep 0.1
done
if [[ -z "$socket" ]]; then
  echo "sway did not open a Wayland socket in 10s:" >&2
  cat "$sway_log" >&2
  exit 1
fi

export WAYLAND_DISPLAY="$socket"
export GDK_BACKEND=wayland
echo "sway ready on $WAYLAND_DISPLAY (scale $scale, XWayland off)"
swaymsg -t get_outputs -r | jq -c '.[] | {name, current_mode, scale}' || true

if [[ -n "$screenshots" ]]; then
  mkdir -p "$screenshots"
  (
    n=0
    while sleep 2; do
      n=$((n + 1))
      grim "$screenshots/$(printf '%04d' "$n").png" 2> /dev/null || true
    done
  ) &
  shooter_pid=$!
fi

status=0
"$@" || status=$?
if [[ $status -ne 0 ]]; then
  echo "--- sway log ---" >&2
  cat "$sway_log" >&2
fi
exit "$status"
