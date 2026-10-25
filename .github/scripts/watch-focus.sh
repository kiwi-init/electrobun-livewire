#!/bin/bash
# Prints a line each time the frontmost app or the mouse position changes,
# until it's killed. The first line is where things started. Used to prove a
# background-mode Kitchen run leaves the user's focus and cursor alone (T758).
set -u
prev=""
while true; do
  app=$(osascript -e 'tell application "System Events" to get name of first application process whose frontmost is true' 2>/dev/null)
  mouse=$(osascript -l JavaScript -e 'ObjC.import("AppKit"); var p = $.NSEvent.mouseLocation; Math.round(p.x) + "," + Math.round(p.y)' 2>/dev/null)
  current="frontmost=$app mouse=$mouse"
  if [ "$current" != "$prev" ]; then
    echo "$(date +%T) $current"
    prev=$current
  fi
  sleep 0.5
done
