#!/usr/bin/env bash
# Builds Rocketify for Windows x64 with MinGW, plus the installers:
#   installer/Rocketify-Setup.exe   (Windows setup wizard, needs makensis)
#   installer/rocketify-install.sh  (Linux installer)
# The DLLs / launcher are compiled into a temporary folder that is removed afterwards.
set -euo pipefail
cd "$(dirname "$0")"

VERSION="0.0.2-alpha"
CXX=x86_64-w64-mingw32-g++

# On the host (e.g. Bazzite) MinGW lives in the "claude" distrobox: re-run there.
if ! command -v "$CXX" >/dev/null 2>&1; then
  if command -v distrobox >/dev/null 2>&1 && [ -z "${ROCKETIFY_IN_BOX:-}" ]; then
    echo "MinGW not found here - building inside the 'claude' distrobox..."
    exec distrobox enter claude -- env ROCKETIFY_IN_BOX=1 bash "$PWD/build.sh"
  fi
  echo "error: $CXX not found. Install mingw-w64 (e.g. 'sudo apt install mingw-w64')." >&2
  exit 1
fi

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

FLAGS=(-O2 -std=c++17 -DUNICODE -D_UNICODE -Iinclude -Isrc -Wall)
STATIC=(-static -static-libgcc -static-libstdc++ -s)

echo "==> rocketify.dll"
"$CXX" "${FLAGS[@]}" -shared -o "$OUT/rocketify.dll" \
  src/rocketify.cpp src/engine.cpp src/input.cpp src/splash.cpp "${STATIC[@]}"

echo "==> Rocketify.exe"
"$CXX" "${FLAGS[@]}" -municode -mwindows -o "$OUT/Rocketify.exe" src/launcher.cpp "${STATIC[@]}" -lgdiplus -lgdi32

echo "==> hello_mod.dll"
"$CXX" "${FLAGS[@]}" -shared -o "$OUT/hello_mod.dll" examples/hello_mod.cpp "${STATIC[@]}"

echo "==> rocketify command line tool"
mkdir -p dist/cli
# everything a mod is built with goes into the program itself
{
  echo 'static const struct { const char *path, *text; } kFiles[] = {'
  for f in include/rocketify include/rocketify.h include/rocketify.hpp CMakeLists.txt cmake/mingw.cmake \
           template/CMakeLists.txt template/.gitignore template/src/main.cpp \
           template/.vscode/c_cpp_properties.json; do
    printf '{"%s", R"_RKT_(' "$f"; cat "$f"; printf ')_RKT_"},\n'
  done
  echo '};'
} > "$OUT/embedded.inc"
echo '1 24 "cli/rocketify.manifest"' | x86_64-w64-mingw32-windres -o "$OUT/manifest.o"
"$CXX" -O2 -std=c++17 -Wall -Iinclude -I"$OUT" -o dist/cli/rocketify.exe cli/rocketify.cpp "$OUT/manifest.o" "${STATIC[@]}"
if command -v g++ >/dev/null 2>&1; then
  g++ -O2 -std=c++17 -Wall -Iinclude -I"$OUT" -static -s -o dist/cli/rocketify cli/rocketify.cpp
else
  echo "   Linux version skipped: g++ not found"
fi

echo "==> Rocketify-Setup.exe (Windows setup wizard for Steam and Epic Games)"
if command -v makensis >/dev/null 2>&1; then
  makensis -V2 -DBUILD="$OUT" -DOUTDIR="$PWD/installer" installer/rocketify.nsi
else
  echo "   skipped: makensis not found (install the 'nsis' package)"
fi

echo "==> rocketify-install.sh (Linux installer with the files above embedded)"
# the script part of the installer is kept; only the embedded files are replaced
TEMPLATE="$(sed '/^__PAYLOAD_BELOW__$/,$d' installer/rocketify-install.sh)"
STAGE="$(mktemp -d)"
mkdir -p "$STAGE/rocketify/mods"
cp "$OUT/Rocketify.exe" "$STAGE/"
cp "$OUT/rocketify.dll" "$STAGE/rocketify/"
cp "$OUT/hello_mod.dll" "$STAGE/rocketify/mods/"
{
  printf '%s\n' "$TEMPLATE"
  echo "__PAYLOAD_BELOW__"
  tar -czf - -C "$STAGE" . | base64
} > installer/rocketify-install.sh
chmod +x installer/rocketify-install.sh
rm -rf "$STAGE"

ls -la installer/Rocketify-Setup.exe installer/rocketify-install.sh
