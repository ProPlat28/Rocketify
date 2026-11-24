#!/usr/bin/env bash
# Builds Rocketify for Windows x64 with MinGW, plus the installers:
#   installer/Rocketify-Setup.exe   (Windows setup wizard, needs makensis)
#   installer/rocketify-install.sh  (Linux installer)
# The DLLs / launcher are compiled into a temporary folder that is removed afterwards.
set -euo pipefail
cd "$(dirname "$0")"

VERSION="1.0.0"
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
  src/rocketify.cpp src/engine.cpp src/input.cpp "${STATIC[@]}"

echo "==> Rocketify.exe"
"$CXX" "${FLAGS[@]}" -municode -mwindows -o "$OUT/Rocketify.exe" src/launcher.cpp "${STATIC[@]}"

echo "==> hello_mod.dll"
"$CXX" "${FLAGS[@]}" -shared -o "$OUT/hello_mod.dll" examples/hello_mod.cpp "${STATIC[@]}"

echo "==> Rocketify-Setup.exe (Windows setup wizard for Steam and Epic Games)"
if command -v makensis >/dev/null 2>&1; then
  makensis -V2 -DBUILD="$OUT" -DOUTDIR="$PWD/installer" installer/rocketify.nsi
else
  echo "   skipped: makensis not found (install the 'nsis' package)"
fi

echo "==> rocketify-install.sh (Linux installer with the files above embedded)"
STAGE="$(mktemp -d)"
mkdir -p "$STAGE/rocketify/mods"
cp "$OUT/Rocketify.exe" "$STAGE/"
cp "$OUT/rocketify.dll" "$STAGE/rocketify/"
cp "$OUT/hello_mod.dll" "$STAGE/rocketify/mods/"
{
  sed "s/__VERSION__/$VERSION/" installer/install-linux.sh
  echo "__PAYLOAD_BELOW__"
  tar -czf - -C "$STAGE" . | base64
} > installer/rocketify-install.sh
chmod +x installer/rocketify-install.sh
rm -rf "$STAGE"

ls -la installer/Rocketify-Setup.exe installer/rocketify-install.sh
