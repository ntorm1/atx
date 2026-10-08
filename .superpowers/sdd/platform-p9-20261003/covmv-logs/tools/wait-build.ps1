# Lane COV-MV (R9) wait-then-build tool, HOST-2 / HOST-3: waits until no C++ build runs on the host
# (ninja / clang-cl / lld-link / cmake --build / another research-build.ps1), then a grace window so a
# higher-priority agent polling at <= 60 s starts first, then runs scripts\research-build.ps1 from pool-31.
# An admission refusal ("memory admission") writes no receipt, so it is retried; the bound is
# -BoundMinutes (90 under HOST-2 / HOST-3; R10 passes 180, HOST-4). -LogPrefix names the round's logs.
param(
  [Parameter(Mandatory = $true)][string] $Tag,
  [Parameter(Mandatory = $true)][string] $Targets,
  [string] $Preset = 'equity-dev',
  [switch] $Canary,
  [switch] $Recache,
  [int] $BoundMinutes = 90,
  [string] $LogPrefix = 'r9'
)
$ErrorActionPreference = 'Stop'
$pool = 'C:\atx-wt\pool-31'
$logDir = Join-Path $pool '.superpowers\sdd\platform-p9-20261003\covmv-logs'
$waitLog = Join-Path $logDir "$LogPrefix-wait-$Tag.log"
$self = $PID

function Get-Busy {
  @(Get-CimInstance Win32_Process | Where-Object {
      ($_.Name -match '^(ninja|clang-cl|lld-link)\.exe$') -or
      ($_.Name -eq 'cmake.exe' -and $_.CommandLine -match '--build') -or
      ($_.Name -eq 'powershell.exe' -and $_.ProcessId -ne $self -and $_.CommandLine -match 'research-build')
    })
}
function Get-FreeMiB { [int][math]::Floor((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory / 1024) }
# A reader holding the log open must never stop the tool: the line is retried, then dropped.
function Write-WaitLog([string] $Line) {
  for ($i = 0; $i -lt 5; $i++) {
    try { [System.IO.File]::AppendAllText($waitLog, $Line + "`r`n"); return } catch { Start-Sleep -Seconds 1 }
  }
}

$deadline = (Get-Date).AddMinutes($BoundMinutes)
while ((Get-Date) -lt $deadline) {
  $busy = Get-Busy
  $free = Get-FreeMiB
  if ($busy.Count -eq 0 -and $free -ge 1000) {
    Write-WaitLog "$(Get-Date -Format s) idle free=$free; grace 90 s"
    Start-Sleep -Seconds 90
    $busy = Get-Busy
    $free = Get-FreeMiB
    if ($busy.Count -eq 0 -and $free -ge 1000) {
      Write-WaitLog "$(Get-Date -Format s) launch $Tag free=$free"
      Set-Location -LiteralPath $pool
      $env:PYTHONDONTWRITEBYTECODE = '1'
      if ($Recache) { $env:CCACHE_RECACHE = '1' }
      $args2 = @('-NoProfile', '-File', 'scripts\research-build.ps1', '-Tag', $Tag, '-Targets', $Targets,
        '-Preset', $Preset)
      if ($Canary) { $args2 += '-Canary' }
      $ErrorActionPreference = 'Continue'
      $out = & powershell @args2 2>&1 | Out-String
      $code = $LASTEXITCODE
      $ErrorActionPreference = 'Stop'
      try { $out | Add-Content (Join-Path $logDir "$LogPrefix-wrapper-$Tag.log") } catch { Write-WaitLog "wrapper log write failed" }
      if ($out -match 'memory admission') {
        Write-WaitLog "$(Get-Date -Format s) admission refused; retry"
        Start-Sleep -Seconds 60
        continue
      }
      Write-WaitLog "$(Get-Date -Format s) done $Tag exit=$code"
      exit $code
    }
  }
  $names = ($busy | ForEach-Object { $_.Name }) -join ','
  Write-WaitLog "$(Get-Date -Format s) wait busy=[$names] free=$free"
  Start-Sleep -Seconds 60
}
Write-WaitLog "$(Get-Date -Format s) BLOCKED-HOST: no admission within $BoundMinutes minutes free=$(Get-FreeMiB)"
exit 99
