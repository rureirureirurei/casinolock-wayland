#!/bin/bash
# Run swaylock inside a nested sway window, so a crash only turns that window red.
# Usage: test/nested.sh [swaylock args...]   F12 inside the window locks again.
# Headless (no window): HEADLESS=1 test/nested.sh ...
dir=$(cd "$(dirname "$0")/.." && pwd)
lock="$dir/build/swaylock -c 202020 $(printf '%q ' "$@")"
cfg=$(mktemp)
trap 'rm -f "$cfg"' EXIT
cat > "$cfg" <<CFG
output * bg #303030 solid_color
output HEADLESS-1 resolution 900x400
exec $lock
bindsym F12 exec $lock
CFG
if [ -n "$HEADLESS" ]; then
	WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 sway -c "$cfg"
else
	WLR_BACKENDS=wayland sway -c "$cfg"
fi
