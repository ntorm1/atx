# B2 fix round 2 (HOST-4, HOST-5): wait until no pool build runs on the host (ninja, `cmake --build`, cl /
# clang-cl whose command line names C:\atx-wt\; HOST-5: a build outside C:\atx-wt\ is foreign and does not count)
# for 120 s (four quiet polls 30 s apart; root's waiter needs 60 s, so root starts first when both wait), bounded
# at 180 minutes; then (optionally) configure equity-dev and build ONE tag of atx-research-eval in pool-14 through
# scripts/research-build.ps1 (its memory gate decides admission; a refusal is waited out within the bound).
# v2 (after the first run): the HOST-5 filter; native stderr no longer aborts the script (PowerShell 5.1 turned the
# configure's first stderr line into a terminating NativeCommandError under ErrorActionPreference Stop).
# Usage: powershell -File r10-build-wait.ps1 -Tag p9-b2-r [-Configure]
param(
  [Parameter(Mandatory = $true)][string] $Tag,
  [switch] $Configure,
  [string] $Targets = 'atx-research-eval'
)
$ErrorActionPreference = 'Stop'
$pool = 'C:\atx-wt\pool-14'
$ev = Join-Path $pool '.superpowers\sdd\platform-p9-20261003\b2-evidence'
$log = Join-Path $ev "r10-build-wait-$Tag.log"

function Get-Builders {
  $procs = Get-CimInstance Win32_Process -Filter "Name='ninja.exe' OR Name='cmake.exe' OR Name='cl.exe' OR Name='clang-cl.exe'"
  @($procs | Where-Object { ($_.Name -ne 'cmake.exe' -or ($_.CommandLine -match '--build')) -and
      ($_.CommandLine -match '(?i)C:[\\/]atx-wt[\\/]') })
}

function Write-Line([string] $text) {
  $stamp = (Get-Date).ToUniversalTime().ToString('yyyy-MM-dd HH:mm:ss') + 'Z'
  Add-Content -Path $log -Value "$stamp $text" -Encoding ascii
}

$deadline = (Get-Date).AddMinutes(180)
$quiet = 0
Write-Line "waiter v2 (HOST-5 filter) tag=$Tag configure=$($Configure.IsPresent)"
while ($quiet -lt 4) {
  if ((Get-Date) -gt $deadline) {
    $os = Get-CimInstance Win32_OperatingSystem
    Write-Line ("BOUND REACHED (180 min) free=" + [int]($os.FreePhysicalMemory / 1024))
    exit 75
  }
  $b = Get-Builders
  $os = Get-CimInstance Win32_OperatingSystem
  $free = [int]($os.FreePhysicalMemory / 1024)
  $commit = [int]($os.FreeVirtualMemory / 1024)
  if ($b.Count -eq 0) { $quiet++ } else { $quiet = 0 }
  $who = ($b | ForEach-Object { if ($_.CommandLine -match '(?i)atx-wt[\\/](pool-\d+)') { $Matches[1] } } |
      Sort-Object -Unique) -join ','
  Write-Line ("pool-builders=" + $b.Count + " pools=" + $who + " free=" + $free + " commitfree=" + $commit +
      " quiet=" + $quiet)
  if ($quiet -lt 4) { Start-Sleep -Seconds 30 }
}
Set-Location $pool
$ErrorActionPreference = 'Continue'
if ($Configure) {
  Write-Line 'starting configure equity-dev'
  cmd /c "powershell -NoProfile -File scripts\atx-build.ps1 configure -Preset equity-dev > `"$ev\r10-configure-equity-dev.log`" 2>&1"
  $rc = $LASTEXITCODE
  Write-Line ("configure exit=" + $rc)
  if ($rc -ne 0) { exit $rc }
}
Write-Line "starting $Tag"
cmd /c "powershell -NoProfile -File scripts\research-build.ps1 -Tag $Tag -Targets $Targets > `"$ev\r10-build-$Tag.log`" 2>&1"
$rc = $LASTEXITCODE
Write-Line ("build $Tag exit=" + $rc)
exit $rc
