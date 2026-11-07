#!/usr/bin/env bash
# Regenerates the README screenshots / GIFs from the production drawing code.
#   tools/hs02-wasm-preview/render_docs.sh          -> docs/images/{ru,en}/*.png, boot.gif, banner
# Needs: g++, node, python3 + Pillow. bdflib is optional (only CJK fonts need it; a stub is used).
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# 1) The host build includes the generated EN font tables (legacy glyphs). Generate them.
mkdir -p "$work/stubs/bdflib" "$root/source/Core/Gen"
printf 'class _F(dict):\n    pass\ndef read_bdf(f): return _F()\n' > "$work/stubs/bdflib/reader.py"
printf 'class Font: pass\nclass Glyph: pass\n' > "$work/stubs/bdflib/model.py"
touch "$work/stubs/bdflib/__init__.py"
python3 -c "import bdflib" 2>/dev/null && stub="" || stub="$work/stubs"
echo '#include <configuration.h>' | g++ -x c++ -dM -E -DMODEL_HS02A -DMODEL_HS02 -I"$root/source/Core/BSP/Fnirsi" - > "$root/source/Core/Gen/macros.txt"
PYTHONPATH="$stub" python3 "$root/Translations/make_translation.py" --macros "$root/source/Core/Gen/macros.txt" \
  -o "$root/source/Core/Gen/Translation.EN.cpp" --output-pickled "$work/EN.pickle" \
  --small-font default_12x16 --large-font default_24x32 EN > /dev/null 2>&1

# 2) Build and run the renderer once per UI language.
g++ -std=c++17 -O1 -DMODEL_HS02 -DMODEL_HS02A \
    -I"$root/tools/hs02-wasm-preview/src" -I"$root/tools/hs02-wasm-preview/host_include" \
    -I"$root/source/Core/BSP/Fnirsi" -I"$root/source/Core/Inc" -I"$root/source/Core/Drivers" \
  "$root/tools/hs02-wasm-preview/native/render_docs.cpp" -o "$work/render"
mkdir -p "$work/ru" "$work/en"
# The standby GIF example (custom/standby.gif) through the same encoder as the web constructor.
node "$root/tools/hs02-gif-studio/cli.mjs" --standby "$root/custom/standby.gif" --container "$work/assets.bin" > /dev/null
"$work/render" "$work/ru" 1 "$work/assets.bin"
"$work/render" "$work/en" 2 "$work/assets.bin"

# 3) Frame, scale and assemble.
python3 "$root/tools/hs02-wasm-preview/native/make_images.py" "$work" "$root/docs/images" "$root/tools/hs02-ui-fonts/fonts"
echo "docs/images updated"
