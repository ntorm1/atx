<#
.SYNOPSIS
  Throughput regression gate over Google Benchmark JSON (Lane 2).

.DESCRIPTION
  Compares a fresh `--benchmark_format=json` (or `--benchmark_out=<f>
  --benchmark_out_format=json`) run against a checked-in baseline and FAILS
  (exit 1) when any benchmark present in both is slower than the baseline by more
  than -Threshold (default 0.20 = 20%). Times are compared as real_time normalized
  to nanoseconds, so a baseline recorded in ms and a run in us compare correctly.
  Only plain iteration rows are compared (aggregates such as _mean/_stddev are
  skipped); benchmarks missing from either side are listed, never fatal.

  Exit codes: 0 = pass, 1 = regression, 2 = bad input.

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
  [switch] $Update
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
  foreach ($b in $doc.benchmarks) {
    $runType = if ($b.PSObject.Properties.Name -contains 'run_type') { $b.run_type } else { 'iteration' }
    if ($runType -ne 'iteration') { continue }
    if ($b.name -notmatch $Filter) { continue }
    $map[$b.name] = [double]$b.real_time * (Get-UnitScale $b.time_unit)
  }
  return @{ doc = $doc; times = $map }
}

$cur = Read-BenchJson $Current

if ($Update) {
  Copy-Item -LiteralPath $Current -Destination $Baseline -Force
  Write-Host "bench-gate: baseline updated from $Current ($($cur.times.Count) benchmarks)"
  exit 0
}

$base = Read-BenchJson $Baseline
$regressions = 0
$rows = @()
foreach ($name in ($base.times.Keys | Sort-Object)) {
  if (-not $cur.times.ContainsKey($name)) {
    $rows += [pscustomobject]@{ Benchmark = $name; BaseNs = $base.times[$name]; CurNs = $null; Ratio = $null; Status = 'MISSING' }
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
if ($regressions -gt 0) {
  Write-Host "bench-gate: FAIL - $regressions benchmark(s) regressed by more than $([math]::Round($Threshold * 100))%"
  exit 1
}
Write-Host "bench-gate: PASS (threshold $([math]::Round($Threshold * 100))%)"
exit 0
