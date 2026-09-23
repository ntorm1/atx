<#
.SYNOPSIS
  Throughput regression gate over Google Benchmark JSON (Lane 2).

.DESCRIPTION
  Compares a fresh `--benchmark_format=json` (or `--benchmark_out=<f>
  --benchmark_out_format=json`) run against a checked-in baseline and FAILS
  (exit 1) when any benchmark present in both is slower than the baseline by more
  than -Threshold (default 0.20 = 20%). Times are compared as real_time normalized
  to nanoseconds, so a baseline recorded in ms and a run in us compare correctly.
  Plain iteration rows are compared; when a file was recorded with
  --benchmark_repetitions=N its _median aggregate rows are used instead (keyed by
  run_name) so noisy single repetitions do not trip the gate. Other aggregates
  (_mean/_stddev/_cv) are skipped. A baseline benchmark missing from the current run FAILS the gate
  (exit 1) unless -AllowMissing is passed for a deliberately partial run. A
  current run with no comparable benchmarks (crashed after writing its header,
  wrong filter) exits 2. Benchmarks new in the current run are listed, never fatal.

  Exit codes: 0 = pass, 1 = regression or missing benchmark, 2 = bad input
  (including an empty current run).

.EXAMPLE
  build-equity-rel\bin\atx-engine-bench.exe --benchmark_filter=Wq101 `
      --benchmark_out=wq101.json --benchmark_out_format=json
  powershell -NoProfile -File scripts\bench-gate.ps1 -Current wq101.json `
      -Baseline atx-engine\bench\baselines\alpha_throughput.json

.EXAMPLE
  # Re-baseline after an intentional change (review the diff before committing).
  powershell -NoProfile -File scripts\bench-gate.ps1 -Current wq101.json `
      -Baseline atx-engine\bench\baselines\alpha_throughput.json -Update
#>
param(
  [Parameter(Mandatory = $true)][string] $Current,
  [Parameter(Mandatory = $true)][string] $Baseline,
  [double] $Threshold = 0.20,
  [string] $Filter = '.*',
  [switch] $Update,
  [switch] $AllowMissing
)

$ErrorActionPreference = 'Stop'

function Get-UnitScale([string] $unit) {
  switch ($unit) {
    'ns' { return 1.0 }
    'us' { return 1.0e3 }
    'ms' { return 1.0e6 }
    's'  { return 1.0e9 }
    default { throw "bench-gate: unknown time_unit '$unit'" }
  }
}

function Read-BenchJson([string] $path) {
  if (-not (Test-Path -LiteralPath $path)) {
    Write-Host "bench-gate: file not found: $path"
    exit 2
  }
  $raw = Get-Content -LiteralPath $path -Raw
  # A stdout capture may carry console lines before the JSON object.
  $start = $raw.IndexOf('{')
  if ($start -lt 0) {
    Write-Host "bench-gate: no JSON object in $path"
    exit 2
  }
  $doc = $raw.Substring($start) | ConvertFrom-Json
  $map = @{}
  $medians = @{}
  foreach ($b in $doc.benchmarks) {
    $props = $b.PSObject.Properties.Name
    $runType = if ($props -contains 'run_type') { $b.run_type } else { 'iteration' }
    $ns = [double]$b.real_time * (Get-UnitScale $b.time_unit)
    if ($runType -eq 'aggregate') {
      if (($props -contains 'aggregate_name') -and $b.aggregate_name -eq 'median') {
        $key = if ($props -contains 'run_name') { $b.run_name } else { $b.name -replace '_median$', '' }
        if ($key -match $Filter) { $medians[$key] = $ns }
      }
      continue
    }
    if ($runType -ne 'iteration') { continue }
    if ($b.name -notmatch $Filter) { continue }
    # With repetitions each iteration row repeats the same name; the median
    # aggregate (applied below) supersedes them.
    $map[$b.name] = $ns
  }
  foreach ($k in $medians.Keys) { $map[$k] = $medians[$k] }
  return @{ doc = $doc; times = $map }
}

$cur = Read-BenchJson $Current

if ($Update) {
  Copy-Item -LiteralPath $Current -Destination $Baseline -Force
  Write-Host "bench-gate: baseline updated from $Current ($($cur.times.Count) benchmarks)"
  exit 0
}

if ($cur.times.Count -eq 0) {
  Write-Host "bench-gate: current run has no comparable benchmarks (filter '$Filter') in $Current"
  exit 2
}

$base = Read-BenchJson $Baseline
$regressions = 0
$missing = 0
$rows = @()
foreach ($name in ($base.times.Keys | Sort-Object)) {
  if (-not $cur.times.ContainsKey($name)) {
    $rows += [pscustomobject]@{ Benchmark = $name; BaseNs = $base.times[$name]; CurNs = $null; Ratio = $null; Status = 'MISSING' }
    $missing++
    continue
  }
  $b = $base.times[$name]
  $c = $cur.times[$name]
  $ratio = if ($b -gt 0) { $c / $b } else { 1.0 }
  $status = 'ok'
  if ($ratio -gt (1.0 + $Threshold)) {
    $status = 'REGRESSION'
    $regressions++
  } elseif ($ratio -lt (1.0 - $Threshold)) {
    $status = 'faster'
  }
  $rows += [pscustomobject]@{ Benchmark = $name; BaseNs = [math]::Round($b); CurNs = [math]::Round($c); Ratio = [math]::Round($ratio, 3); Status = $status }
}
foreach ($name in ($cur.times.Keys | Sort-Object)) {
  if (-not $base.times.ContainsKey($name)) {
    $rows += [pscustomobject]@{ Benchmark = $name; BaseNs = $null; CurNs = [math]::Round($cur.times[$name]); Ratio = $null; Status = 'NEW' }
  }
}
$rows | Format-Table -AutoSize | Out-String -Width 200 | Write-Host

if ($base.times.Count -eq 0) {
  Write-Host "bench-gate: baseline has no comparable benchmarks (filter '$Filter')"
  exit 2
}
if ($missing -gt 0 -and -not $AllowMissing) {
  Write-Host "bench-gate: FAIL - $missing baseline benchmark(s) missing from the current run (pass -AllowMissing for a deliberately partial run)"
  exit 1
}
if ($regressions -gt 0) {
  Write-Host "bench-gate: FAIL - $regressions benchmark(s) regressed by more than $([math]::Round($Threshold * 100))%"
  exit 1
}
Write-Host "bench-gate: PASS (threshold $([math]::Round($Threshold * 100))%)"
exit 0
