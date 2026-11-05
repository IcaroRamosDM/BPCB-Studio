#!/usr/bin/env bash
set -euo pipefail
if ! command -v apt-get >/dev/null; then
    printf '%s\n' 'Este script foi preparado para Ubuntu/Debian com apt-get.' >&2
    exit 1
fi
if [ "$(id -u)" -eq 0 ]; then elevated=(); else elevated=(sudo); fi
"${elevated[@]}" apt-get update
"${elevated[@]}" apt-get install -y build-essential cmake pkg-config \
    libjson-c-dev libarchive-dev libssl-dev libusb-1.0-0-dev \
    libpoppler-glib-dev librsvg2-dev libpango1.0-dev libcairo2-dev \
    libfontconfig1-dev libx11-dev libxext-dev libxft-dev libxinerama-dev \
    libxcursor-dev libxrender-dev libxfixes-dev \
    xvfb xauth xdotool fonts-dejavu-core python3 python3-venv python3-usb \
    usbutils xdg-utils
printf '%s\n' 'Dependencias instaladas. Execute ./scripts/compilar.sh.'
