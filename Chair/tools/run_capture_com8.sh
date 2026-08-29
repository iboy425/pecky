#!/usr/bin/env bash
set -euo pipefail

# The VS Code terminal for this project runs inside WSL, while COM8 belongs to
# Windows.  Keep the Python collector in the repository but execute it with the
# Windows Python 3.12 installation so it can open COM8 directly.
launcher_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
export CHAIR_CAPTURE_SCRIPT_WIN
CHAIR_CAPTURE_SCRIPT_WIN="$(wslpath -w "$launcher_dir/capture_actions_com8.py")"
export CHAIR_CAPTURE_ARGS_JSON
CHAIR_CAPTURE_ARGS_JSON="$(python3 -c 'import json, sys; print(json.dumps(sys.argv[1:]))' "$@")"
export WSLENV="${WSLENV:+$WSLENV:}CHAIR_CAPTURE_SCRIPT_WIN:CHAIR_CAPTURE_ARGS_JSON"

powershell.exe -NoProfile -Command \
  '& { $collectorArgs = @(ConvertFrom-Json $env:CHAIR_CAPTURE_ARGS_JSON); & py -3.12 $env:CHAIR_CAPTURE_SCRIPT_WIN @collectorArgs; exit $LASTEXITCODE }'
