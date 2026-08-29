#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
bash "$root/tools/stop_device_bridge_wsl.sh" hat >/dev/null 2>&1 || true
script="$(wslpath -w "$root/tools/serial_bridge.py")"
exec powershell.exe -NoProfile -Command "& py -3 '$script'"
