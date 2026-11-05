#!/usr/bin/env bash
set -euo pipefail
raiz="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
jobs="${BPCB_JOBS:-4}"
if ! [[ "$jobs" =~ ^[1-9][0-9]*$ ]] || [ "$jobs" -gt 64 ]; then
    echo 'BPCB_JOBS deve ser um inteiro entre 1 e 64.' >&2
    exit 1
fi
if [ "$(uname -s)" != Linux ]; then
    echo 'Este script prepara a compilação nativa no Linux.' >&2
    exit 1
fi
python3 "$raiz/scripts/preparar-fltk.py"
dep="$raiz/dependencias/locais/fltk-1.4.5"
fltk="$raiz/trabalho/fltk-install"
cmake -S "$dep" -B "$raiz/trabalho/fltk-build" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$fltk" \
    -DFLTK_BACKEND_WAYLAND=OFF -DFLTK_BACKEND_X11=ON \
    -DFLTK_BUILD_EXAMPLES=OFF -DFLTK_BUILD_TEST=OFF \
    -DFLTK_BUILD_FLUID=OFF -DFLTK_BUILD_FLTK_OPTIONS=OFF \
    -DFLTK_BUILD_FORMS=OFF -DFLTK_BUILD_GL=OFF \
    -DFLTK_BUILD_SHARED_LIBS=OFF -DFLTK_OPTION_SVG=OFF \
    -DFLTK_OPTION_PRINT_SUPPORT=OFF -DFLTK_USE_PANGO=OFF \
    -DFLTK_USE_SYSTEM_LIBJPEG=OFF -DFLTK_USE_SYSTEM_LIBPNG=OFF -DFLTK_USE_SYSTEM_ZLIB=OFF
cmake --build "$raiz/trabalho/fltk-build" -j "$jobs"
cmake --install "$raiz/trabalho/fltk-build"
cmake -S "$raiz" -B "$raiz/trabalho/build" \
    -DFLTK_DIR="$fltk/share/fltk" -DCMAKE_BUILD_TYPE=Debug
cmake --build "$raiz/trabalho/build" -j "$jobs"
ctest --test-dir "$raiz/trabalho/build" --output-on-failure -j1
