#!/usr/bin/env bash
# Keep the demo in WSL while delegating only Windows-owned COM7/COM8 access.
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "用法：bash tools/control_devices_wsl.sh {hat|chair|all} {start|pause|status|calibrate}" >&2
  exit 2
fi
case "$1" in hat|chair|all) ;; *) echo "设备必须是 hat、chair 或 all" >&2; exit 2;; esac
case "$2" in start|pause|status|calibrate) ;; *) echo "操作必须是 start、pause、status 或 calibrate" >&2; exit 2;; esac

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
windows_script="$(wslpath -w "$root/tools/control_devices.py")"
windows_bridge="$(wslpath -w "$root/tools/control_devices_windows.ps1")"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$windows_bridge" -ScriptPath "$windows_script" -Device "$1" -Action "$2"
