#!/bin/sh
# CI: runs an assembled check app under Xvfb + openbox, as Kitchen's X11 job
# does, and exits with the app's exit code.
# Usage: run-xvfb.sh <app folder>
exec dbus-run-session -- xvfb-run -a -s '-screen 0 1280x800x24' \
	sh -c 'openbox > /dev/null 2>&1 & exec timeout 120 "$0/bin/launcher"' "$1"
