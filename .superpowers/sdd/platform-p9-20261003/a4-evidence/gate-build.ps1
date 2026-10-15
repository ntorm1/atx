# A4 build under HOST-2 / HOST-3: wait (60 s polls, 90 min cap) until no other ninja runs on the host and the
# research-build memory admission would pass, then run scripts\research-build.ps1 once. Lowest priority: any
# running ninja (another pool's build) means wait. Exit 9 = not admitted within 90 minutes (BLOCKED-HOST).
param([Parameter(Mandatory = $true)][string] $Tag, [Parameter(Mandatory = $true)][string] $Targets,
      [string] $Preset = 'equity-dev')
$root = 'C:\atx-wt\pool-27'
$log = Join-Path $root ".superpowers\sdd\platform-p9-20261003\a4-evidence\build-$Tag.out"
$deadline = (Get-Date).AddMinutes(90)
while ($true) {
  $os = Get-CimInstance Win32_OperatingSystem
  $free = [int]($os.FreePhysicalMemory / 1024)
  $mem = Get-CimInstance Win32_PerfFormattedData_PerfOS_Memory
  $commit = [int](($mem.CommitLimit - $mem.CommittedBytes) / 1MB)
  $busy = @(Get-CimInstance Win32_Process -Filter "Name='ninja.exe'").Count -gt 0
  if (-not $busy -and $free -ge 1100 -and $commit -ge 2600) { break }
  if ((Get-Date) -gt $deadline) { "not admitted in 90 min: free=$free commit=$commit busy=$busy" | Out-File $log; exit 9 }
  Start-Sleep -Seconds 60
}
Set-Location $root
powershell -NoProfile -File scripts\research-build.ps1 -Tag $Tag -Targets $Targets -Preset $Preset *> $log
$code = $LASTEXITCODE
"exit=$code free_at_start=$free commit_at_start=$commit" | Out-File -Append $log
exit $code
