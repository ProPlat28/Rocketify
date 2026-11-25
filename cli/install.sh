#!/usr/bin/env bash
# Installs the rocketify command in ~/.local/bin.
set -euo pipefail
main() {
  local dir="$HOME/.local/bin" tmp
  mkdir -p "$dir"
  tmp="$(mktemp "$dir/rocketify.XXXXXX")"
  curl -fL "${ROCKETIFY_CLI_URL:-https://raw.githubusercontent.com/ProPlat28/Rocketify/refs/heads/main/dist/cli/rocketify}" -o "$tmp" || { rm -f "$tmp"; exit 1; }
  chmod 755 "$tmp"
  mv -f "$tmp" "$dir/rocketify"
  case ":$PATH:" in *":$dir:"*) ;; *) echo "Add ~/.local/bin to your PATH to use the rocketify command." ;; esac
  echo "Installed. Next: rocketify install dependencies"
}
main "$@"
