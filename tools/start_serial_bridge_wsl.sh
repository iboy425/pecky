#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
script="$(wslpath -w "$root/tools/serial_bridge.py")"
exec powershell.exe -NoProfile -Command "& py -3 '$script'"
