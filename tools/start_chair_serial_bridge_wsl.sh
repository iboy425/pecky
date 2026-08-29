#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
bash "$root/tools/stop_device_bridge_wsl.sh" chair >/dev/null 2>&1 || true
script="$(wslpath -w "$root/tools/chair_serial_bridge.py")"
exec powershell.exe -NoProfile -Command "& py -3 '$script'"
