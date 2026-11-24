#!/usr/bin/env bash
# Developer install: runs the freshly built Linux installer on a Rocket League install.
# Usage: ./install.sh [path-to-rocketleague]   (default: ~/Games/Heroic/rocketleague)
set -euo pipefail
cd "$(dirname "$0")"
[ -x installer/rocketify-install.sh ] || { echo "Run ./build.sh first."; exit 1; }
exec installer/rocketify-install.sh --yes --dir "${1:-$HOME/Games/Heroic/rocketleague}"
