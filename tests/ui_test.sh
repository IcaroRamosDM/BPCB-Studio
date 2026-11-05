#!/usr/bin/env bash
set -euo pipefail
build="$1"
mkdir -p "$build/ui-evidence"
exec xvfb-run -a -s "-screen 0 1920x1080x24" env GDK_SCALE=1 "$build/bpcb_studio" --ui-check "$build/ui-evidence"
