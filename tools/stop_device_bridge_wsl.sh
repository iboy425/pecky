#!/usr/bin/env bash
# Reliably stop Windows-owned COM7/COM8 bridges from any WSL terminal.
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "用法：bash tools/stop_device_bridge_wsl.sh {hat|chair|all}" >&2
  exit 2
fi

case "$1" in
  hat) targets=("hat:8765:serial_bridge.py") ;;
  chair) targets=("chair:8766:chair_serial_bridge.py") ;;
  all) targets=("hat:8765:serial_bridge.py" "chair:8766:chair_serial_bridge.py") ;;
  *) echo "设备必须是 hat、chair 或 all" >&2; exit 2 ;;
esac

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
control_script="$(wslpath -w "$root/tools/control_devices.py")"

for target in "${targets[@]}"; do
  IFS=: read -r device port script_name <<<"$target"
  powershell.exe -NoProfile -Command "
    \$listeners = Get-NetTCPConnection -LocalPort $port -State Listen -ErrorAction SilentlyContinue
    foreach (\$listener in \$listeners) {
      \$process = Get-CimInstance Win32_Process -Filter \"ProcessId=\$(\$listener.OwningProcess)\"
      if (\$process.CommandLine -like \"*$script_name*\") {
        Stop-Process -Id \$listener.OwningProcess -Force
      }
    }
  "
  sleep 0.4
  powershell.exe -NoProfile -Command "& py -3 '$control_script' '$device' pause"
done
