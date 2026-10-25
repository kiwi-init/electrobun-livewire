#!/usr/bin/env bash
# Run a command inside a headless wlroots compositor with no X11 display, so
# GTK can only open a native Wayland display: anything that still needs X11
# fails here instead of passing through XWayland.
#
#   run-under-wayland.sh [--compositor sway|labwc] [--scale S] [--screenshots DIR] -- command args...
#
# sway (the default) tiles windows and has no maximized state; labwc stacks
# them like GNOME and KDE, so maximize and focus changes behave as on most
# desktops. --screenshots DIR saves a grim screenshot of the session every 2
# seconds while the command runs. Exits with the command's status. The
# compositor's log is printed when it fails.
set -euo pipefail

compositor=sway
scale=1
screenshots=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --compositor) compositor="$2"; shift 2 ;;
    --scale) scale="$2"; shift 2 ;;
    --screenshots) screenshots="$2"; shift 2 ;;
    --) shift; break ;;
    *) break ;;
  esac
done
if [[ "$compositor" != sway && "$compositor" != labwc ]]; then
  echo "--compositor must be sway or labwc, got: $compositor" >&2
  exit 2
fi
if [[ ! "$scale" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
  echo "--scale must be a number, got: $scale" >&2
  exit 2
fi
if [[ $# -eq 0 ]]; then
  echo "usage: $0 [--compositor sway|labwc] [--scale S] [--screenshots DIR] -- command args..." >&2
  exit 2
fi

work="$(mktemp -d)"
runtime_dir="$work/runtime"
mkdir -m 0700 "$runtime_dir"
compositor_log="$work/$compositor.log"

export XDG_RUNTIME_DIR="$runtime_dir"
export XDG_SESSION_TYPE=wayland
export WLR_BACKENDS=headless
export WLR_RENDERER=pixman
export WLR_LIBINPUT_NO_DEVICES=1
unset DISPLAY WAYLAND_DISPLAY

case "$compositor" in
  sway)
    cat > "$work/sway.config" <<CONFIG
xwayland disable
output * resolution 1920x1080 position 0 0 scale $scale
CONFIG
    sway --config "$work/sway.config" > "$compositor_log" 2>&1 &
    ;;
  labwc)
    # labwc has no option to turn XWayland off; nothing here exports
    # DISPLAY, so the app still can't reach it.
    mkdir -p "$work/labwc"
    cat > "$work/labwc/rc.xml" <<'CONFIG'
<?xml version="1.0"?>
<labwc_config>
  <core><decoration>server</decoration></core>
</labwc_config>
CONFIG
    labwc --config-dir "$work/labwc" > "$compositor_log" 2>&1 &
    ;;
esac
compositor_pid=$!
shooter_pid=""
cleanup() {
  [[ -n "$shooter_pid" ]] && kill "$shooter_pid" 2> /dev/null || true
  kill "$compositor_pid" 2> /dev/null || true
  wait "$compositor_pid" 2> /dev/null || true
}
trap cleanup EXIT

socket=""
for _ in $(seq 1 100); do
  if ! kill -0 "$compositor_pid" 2> /dev/null; then
    echo "$compositor exited before it was ready:" >&2
    cat "$compositor_log" >&2
    exit 1
  fi
  socket="$(find "$runtime_dir" -maxdepth 1 -name 'wayland-*' -type s -printf '%f\n' | head -n 1)"
  [[ -n "$socket" ]] && break
  sleep 0.1
done
if [[ -z "$socket" ]]; then
  echo "$compositor did not open a Wayland socket in 10s:" >&2
  cat "$compositor_log" >&2
  exit 1
fi

export WAYLAND_DISPLAY="$socket"
export GDK_BACKEND=wayland
# Lets Kitchen tests know there's no real keyboard or pointer.
export KITCHEN_HEADLESS_WAYLAND="$compositor"

case "$compositor" in
  sway)
    swaymsg -t get_outputs -r | jq -c '.[] | {name, current_mode, scale}' || true
    ;;
  labwc)
    # The headless output starts at 1280x720; match sway's 1920x1080.
    # wlr-randr 0.3 (Ubuntu 24.04) has no --json: the first word of its
    # first line is the output name.
    output="$(wlr-randr | awk 'NR == 1 { print $1 }')"
    wlr-randr --output "$output" --custom-mode 1920x1080 --scale "$scale"
    wlr-randr
    ;;
esac
echo "$compositor ready on $WAYLAND_DISPLAY (scale $scale, no X11 display)"

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
  echo "--- $compositor log ---" >&2
  cat "$compositor_log" >&2
fi
exit "$status"
