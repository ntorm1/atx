$ErrorActionPreference = 'Stop'
$qualificationRoot = 'C:/atx-wt/pool-2'
Set-Location -LiteralPath $qualificationRoot
if (@(& git status --porcelain).Count -ne 0) { throw 'Qualification requires a clean source tree' }
$sourceIdentity = (& git rev-parse HEAD).Trim()
$availableMiB = [math]::Floor((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory / 1024)
$memoryState = Get-CimInstance Win32_PerfFormattedData_PerfOS_Memory
$commitHeadroomMiB = [math]::Floor(($memoryState.CommitLimit - $memoryState.CommittedBytes) / 1MB)
if ($availableMiB -lt 1000 -or $commitHeadroomMiB -lt 2500) {
  throw "Wait for memory before compilation: $availableMiB MiB physical, $commitHeadroomMiB MiB commit headroom"
}
$workerCount = if ($availableMiB -ge 3400) { 4 } elseif ($availableMiB -ge 2200) { 3 } else { 2 }
$artifactBase = "$qualificationRoot/build-equity/recent-strategy-cash-claims"
if (Test-Path -LiteralPath "$artifactBase-build-receipt.json") { throw 'Preserve existing build receipt' }
$env:CCACHE_STATSLOG = "$artifactBase-cache-stats.log"
$env:CCACHE_LOGFILE = "$artifactBase-cache.log"
$targets = @('atx-equity-strategy','atx-impl-strategy-tests')
[pscustomobject]@{Source=$sourceIdentity; Jobs=$workerCount; FreeMiB=$availableMiB; CommitHeadroomMiB=$commitHeadroomMiB; Targets=$targets} |
  ConvertTo-Json | Set-Content -LiteralPath "$artifactBase-start.json"
$buildWatch = [Diagnostics.Stopwatch]::StartNew()
$ErrorActionPreference = 'Continue'
& "$qualificationRoot/scripts/atx-build.ps1" build -Preset equity-dev -Jobs $workerCount @targets *> "$artifactBase-build.log"
$buildExit = $LASTEXITCODE
$buildWatch.Stop()
$generatedProvenance = Get-Content -LiteralPath "$qualificationRoot/build-equity/atx-impl/generated/build_provenance.cpp" -Raw
$provenanceMatch = [regex]::Match($generatedProvenance, '[a-f0-9]{40}(?:-dirty)?')
[pscustomobject]@{Source=$sourceIdentity; ConfiguredProvenance=$provenanceMatch.Value;
  Jobs=$workerCount; ExitCode=$buildExit; WallSeconds=$buildWatch.Elapsed.TotalSeconds} |
  ConvertTo-Json | Set-Content -LiteralPath "$artifactBase-build-receipt.json"
& C:/atx-cache/bin/ccache.exe --show-log-stats *> "$artifactBase-cache-summary.txt"
Get-Content -LiteralPath "$artifactBase-build.log" -Tail 25
Get-Content -LiteralPath "$artifactBase-build-receipt.json"
exit $buildExit
