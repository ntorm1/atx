# Continuously renew one lease heartbeat until its authenticated stop signal,
# heartbeat removal/token change, or process termination. Windows PowerShell 5.1.
param(
  [Parameter(Mandatory = $true)][string]$HeartbeatPath,
  [Parameter(Mandatory = $true)][string]$ControlToken,
  [Parameter(Mandatory = $true)][int]$IntervalMilliseconds
)
$ErrorActionPreference = 'Stop'

if ($IntervalMilliseconds -lt 100) { throw 'IntervalMilliseconds must be at least 100' }
if ($ControlToken -notmatch '^[0-9a-f]{32}$') { throw 'invalid keeper control token' }
$stopPath = $HeartbeatPath + '.stop'
$readyPath = $HeartbeatPath + '.ready'
$readyPublished = $false

while ($true) {
  if (-not [System.IO.File]::Exists($HeartbeatPath)) { exit 3 }
  $lines = [System.IO.File]::ReadAllLines($HeartbeatPath)
  if (-not ($lines -contains ('control_token=' + $ControlToken))) { exit 4 }
  if ([System.IO.File]::Exists($stopPath)) {
    $stopToken = [System.IO.File]::ReadAllText($stopPath).Trim()
    if ($stopToken -eq $ControlToken) { exit 0 }
    exit 5
  }
  [System.IO.File]::SetLastWriteTimeUtc($HeartbeatPath, [DateTime]::UtcNow)
  if (-not $readyPublished) {
    [System.IO.File]::WriteAllText($readyPath, $ControlToken, (New-Object System.Text.ASCIIEncoding))
    $readyPublished = $true
  }
  Start-Sleep -Milliseconds $IntervalMilliseconds
}
