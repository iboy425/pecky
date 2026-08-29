param(
  [Parameter(Mandatory = $true)][string]$ScriptPath,
  [Parameter(Mandatory = $true)][ValidateSet('hat', 'chair', 'all')][string]$Device,
  [Parameter(Mandatory = $true)][ValidateSet('start', 'pause', 'status', 'calibrate')][string]$Action
)

& py -3 $ScriptPath $Device $Action
exit $LASTEXITCODE
