<#
.SYNOPSIS
  Fresh-process cold trials of atx-vol-quote-bench (lane B0; docs/benchmark-clocks.md).

.DESCRIPTION
  Runs -Trials fresh processes, one trial each, every one with ATX_VOL_FIT_WORKERS=1 and
  (with -Affinity) pinned to one logical CPU, and collects each trial's native report into
  one manifest.json: sha256 of the binary, policy and input; the source SHA and dirty flag
  of -SourceRoot; compiler / flags / ISA read from the binary's embedded build info
  (bench.build_info of the first report) or, when absent, from -BuildDir's CMakeCache.txt
  (the manifest says which); per-trial clocks, stages and affinity receipt; the
  min / median / p95 / p99 / max of every clock over the trials; the predictions hash of
  every trial (they must all be equal) and a fast-math assertion.

  Never overwrites: -OutDir must not exist. Each trial is launched through
  System.Diagnostics.Process with its output streams read asynchronously into files (a
  PowerShell-wrapped native call has died silently under PS 5.1 before; this launch form
  records the real exit code and both streams).

.EXAMPLE
  powershell -NoProfile -File atx-vol\bench\run_cold_trials.ps1 `
    -Binary build-vol-rel\bin\atx-vol-quote-bench.exe `
    -InputCsv C:\atx-cache\quote-benchmark\fit-timing-20261017-cohort5\input\quotes-SPY.csv `
    -Policy C:\atx-cache\quote-benchmark\slice-coverage-20261016-policies\graded-v1.json `
    -OutDir C:\atx-cache\quote-benchmark\B0-20261022-g1\spy-10 -Trials 10 -Affinity 6 `
    -RequirePCore -TimingDetail -SourceRoot C:\atx-wt\pool-33 `
    -BuildDir C:\atx-wt\pool-33\build-vol-rel
#>
param(
  [Parameter(Mandatory = $true)] [string] $Binary,
  [Parameter(Mandatory = $true)] [string] $InputCsv,
  [Parameter(Mandatory = $true)] [string] $Policy,
  [Parameter(Mandatory = $true)] [string] $OutDir,
  [ValidateRange(1, 1000)] [int] $Trials = 10,
  [int] $Affinity = -1,
  [switch] $RequirePCore,
  [switch] $TimingDetail,
  [ValidateSet('funding-pv', 'escrowed-pv', 'none')] [string] $Dividends = 'funding-pv',
  [string] $SourceRoot = '',
  [string] $BuildDir = '',
  [string[]] $ExtraArgs = @()
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 1.0

function Get-Sha256([string] $Path) {
  return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

# Linear interpolation between order statistics (bench_clocks.hpp summarize()).
function Get-Summary([double[]] $Values) {
  if ($Values.Count -eq 0) { return $null }
  $sorted = @($Values | Sort-Object)
  $q = {
    param([double] $p)
    $at = $p * ($sorted.Count - 1)
    $lo = [int][math]::Floor($at)
    $hi = [math]::Min($lo + 1, $sorted.Count - 1)
    return $sorted[$lo] + ($at - $lo) * ($sorted[$hi] - $sorted[$lo])
  }
  return [ordered]@{
    n = $sorted.Count; min = $sorted[0]; median = (& $q 0.5); p95 = (& $q 0.95)
    p99 = (& $q 0.99); max = $sorted[$sorted.Count - 1]
  }
}

function Format-Argument([string] $Value) {
  if ($Value -match '[\s"]') { return '"' + ($Value -replace '"', '\"') + '"' }
  return $Value
}

foreach ($path in @($Binary, $InputCsv, $Policy)) {
  if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "missing file: $path" }
}
if (Test-Path -LiteralPath $OutDir) { throw "refusing to overwrite: $OutDir exists" }
if ($RequirePCore -and $Affinity -lt 0) { throw '-RequirePCore requires -Affinity' }
New-Item -ItemType Directory -Path $OutDir | Out-Null
$Binary = (Resolve-Path -LiteralPath $Binary).Path
$InputCsv = (Resolve-Path -LiteralPath $InputCsv).Path
$Policy = (Resolve-Path -LiteralPath $Policy).Path
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path

$source = [ordered]@{ root = $null; sha = $null; dirty = $null; status = 'not provided' }
if ($SourceRoot -ne '') {
  $sha = (& git -C $SourceRoot rev-parse HEAD).Trim()
  $porcelain = @(& git -C $SourceRoot status --porcelain)
  $source = [ordered]@{
    root = $SourceRoot; sha = $sha; dirty = ($porcelain.Count -gt 0)
    status = "git status --porcelain: $($porcelain.Count) lines"
  }
}
$power = (& powercfg /getactivescheme | Out-String).Trim()

$trialRecords = @()
$cold = @(); $legacy = @(); $process = @(); $walls = @()
$predictionHashes = @()
$commandTemplate = $null
for ($t = 1; $t -le $Trials; ++$t) {
  $name = 'trial-{0:D3}' -f $t
  $csv = Join-Path $OutDir "$name.csv"
  $report = Join-Path $OutDir "$name.json"
  $stdoutPath = Join-Path $OutDir "$name.stdout.json"
  $stderrPath = Join-Path $OutDir "$name.stderr.txt"
  $argv = @('--input', $InputCsv, '--output', $csv, '--model', 'quote-slice', '--policy', $Policy,
            '--dividends', $Dividends, '--report', $report)
  if ($TimingDetail) { $argv += '--timing-detail' }
  if ($Affinity -ge 0) { $argv += @('--affinity', "$Affinity") }
  if ($RequirePCore) { $argv += '--require-pcore' }
  $argv += $ExtraArgs
  $line = ($argv | ForEach-Object { Format-Argument $_ }) -join ' '
  if ($null -eq $commandTemplate) { $commandTemplate = "ATX_VOL_FIT_WORKERS=1 `"$Binary`" $line" }
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = $Binary
  $psi.Arguments = $line
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $psi.CreateNoWindow = $true
  $psi.EnvironmentVariables['ATX_VOL_FIT_WORKERS'] = '1'
  $watch = [System.Diagnostics.Stopwatch]::StartNew()
  $proc = [System.Diagnostics.Process]::Start($psi)
  $outTask = $proc.StandardOutput.ReadToEndAsync()
  $errTask = $proc.StandardError.ReadToEndAsync()
  $proc.WaitForExit()
  $watch.Stop()
  [System.IO.File]::WriteAllText($stdoutPath, $outTask.Result)
  [System.IO.File]::WriteAllText($stderrPath, $errTask.Result)
  $code = $proc.ExitCode
  $record = [ordered]@{
    trial = $t; exit_code = $code; launcher_wall_s = $watch.Elapsed.TotalSeconds
  }
  if ($code -ne 0 -or -not (Test-Path -LiteralPath $report)) {
    $record['stderr'] = $errTask.Result.Trim()
    $trialRecords += $record
    Write-Host "$name exit=$code FAILED: $($errTask.Result.Trim())"
    continue
  }
  $json = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
  $bench = $json.bench
  $rep = $bench.per_rep[0]
  $aff = $bench.affinity
  $predictionsSha = Get-Sha256 $csv
  $predictionHashes += $predictionsSha
  $cold += [double]$rep.cold_calibration_s
  $legacy += [double]$rep.legacy_fit_reprice_s
  $process += [double]$bench.clocks.process_wall_s.value
  $walls += $watch.Elapsed.TotalSeconds
  $record['report'] = "$name.json"
  $record['report_sha256'] = Get-Sha256 $report
  $record['predictions_sha256'] = $predictionsSha
  $record['served_rows'] = $json.served_rows
  $record['cold_calibration_s'] = [double]$rep.cold_calibration_s
  $record['legacy_fit_reprice_s'] = [double]$rep.legacy_fit_reprice_s
  $record['process_wall_s'] = [double]$bench.clocks.process_wall_s.value
  $record['process_sub_timers'] = $bench.clocks.process_wall_s.sub_timers
  if ($bench.stages -isnot [string]) {
    $stages = [ordered]@{}
    foreach ($p in $bench.stages.stages.PSObject.Properties) { $stages[$p.Name] = $p.Value.seconds }
    $record['stages'] = $stages
    $record['stages_unattributed_s'] = $bench.stages.unattributed_s
    $record['attribution_valid'] = $bench.stages.attribution_valid
  }
  $record['affinity'] = [ordered]@{
    requested_cpu = $aff.requested_cpu; pinned = $aff.pinned; is_pcore = $aff.is_pcore
    efficiency_class = $aff.efficiency_class; core_mask = $aff.core_mask
    smt_siblings = $aff.smt_siblings; current_cpu_at_receipt = $aff.current_cpu_at_receipt
    process_affinity_mask = $aff.process_affinity_mask
    subject_cpu_busy_pct = $aff.busy.subject_cpu_pct
    smt_sibling_busy_pct = $aff.busy.smt_sibling_pct
    smt_sibling_idle_below_5pct = $aff.busy.smt_sibling_idle_below_5pct
    system_busy_pct = $aff.busy.system_pct; fit_workers = $aff.fit_workers
    nested_pools = $aff.nested_pools; cpu_mhz = $aff.power.cpu_mhz
    timer_resolution_100ns = $aff.timer.timer_resolution_100ns
  }
  $trialRecords += $record
  $message = ("{0} exit=0 cold_calibration_s={1:F4} legacy_s={2:F4} process_wall_s={3:F4} " +
              "cpu={4} pcore={5} sibling_busy_pct={6} sha256={7}") -f $name,
             $record.cold_calibration_s, $record.legacy_fit_reprice_s, $record.process_wall_s,
             $aff.current_cpu_at_receipt, $aff.is_pcore, ($aff.busy.smt_sibling_pct -join '/'),
             $predictionsSha
  Write-Host $message
}

$ok = @($trialRecords | Where-Object { $_.exit_code -eq 0 })
$buildInfo = $null
$buildSource = 'unavailable'
if ($ok.Count -gt 0) {
  $first = Get-Content -LiteralPath (Join-Path $OutDir $ok[0].report) -Raw | ConvertFrom-Json
  if ($null -ne $first.bench.build_info -and $null -ne $first.bench.build_info.cxx_flags) {
    $buildInfo = $first.bench.build_info
    $buildSource = 'embedded in the binary (report bench.build_info)'
  }
}
if ($null -eq $buildInfo -and $BuildDir -ne '') {
  $cache = Get-Content -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt')
  $value = { param($key) (($cache | Where-Object { $_ -like "${key}:*" }) -replace '^[^=]*=', '') }
  $buildInfo = [ordered]@{
    compiler = (& $value 'CMAKE_CXX_COMPILER'); cxx_flags = (& $value 'CMAKE_CXX_FLAGS')
    cxx_flags_release = (& $value 'CMAKE_CXX_FLAGS_RELEASE')
  }
  $buildSource = "configure cache $BuildDir\CMakeCache.txt"
}
$fastMath = $null
if ($null -ne $buildInfo -and $buildInfo.PSObject.Properties.Name -contains 'fast_math') {
  $fastMath = [bool]$buildInfo.fast_math
}
$uniqueHashes = @($predictionHashes | Sort-Object -Unique)
$offCore = @($ok | Where-Object {
    $_.affinity.is_pcore -ne $true -or $_.affinity.current_cpu_at_receipt -ne $Affinity })
$pcoreProof = ($ok.Count -eq $Trials) -and ($Affinity -ge 0) -and ($offCore.Count -eq 0)
$manifest = [ordered]@{
  schema = 'atx-vol-cold-trials-v1'
  definitions = 'atx-vol/docs/benchmark-clocks.md'
  created_utc = (Get-Date).ToUniversalTime().ToString('o')
  host = $env:COMPUTERNAME
  command_template = $commandTemplate
  binary = [ordered]@{ path = $Binary; sha256 = (Get-Sha256 $Binary) }
  policy = [ordered]@{ path = $Policy; sha256 = (Get-Sha256 $Policy) }
  input = [ordered]@{ path = $InputCsv; sha256 = (Get-Sha256 $InputCsv) }
  source = $source
  build_info = $buildInfo
  build_info_source = $buildSource
  fast_math = $fastMath
  power_scheme = $power
  trials_requested = $Trials
  trials_ok = $ok.Count
  exit_codes = @($trialRecords | ForEach-Object { $_.exit_code })
  affinity_requested = $(if ($Affinity -ge 0) { $Affinity } else { $null })
  require_pcore = [bool]$RequirePCore
  pcore_proof = $pcoreProof
  predictions_identical = ($uniqueHashes.Count -eq 1)
  predictions_sha256 = $(if ($uniqueHashes.Count -eq 1) { $uniqueHashes[0] } else { $uniqueHashes })
  summary = [ordered]@{
    cold_calibration_s = (Get-Summary $cold); legacy_fit_reprice_s = (Get-Summary $legacy)
    process_wall_s = (Get-Summary $process); launcher_wall_s = (Get-Summary $walls)
  }
  trials = $trialRecords
}
$manifestPath = Join-Path $OutDir 'manifest.json'
$manifest | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
$s = $manifest.summary.cold_calibration_s
if ($null -ne $s) {
  $message = ("COLD_TRIALS n={0} min={1:F4} median={2:F4} p95={3:F4} p99={4:F4} max={5:F4} " +
              "pcore_proof={6} predictions_identical={7} fast_math={8}") -f $s.n, $s.min,
             $s.median, $s.p95, $s.p99, $s.max, $pcoreProof, $manifest.predictions_identical,
             $fastMath
  Write-Host $message
}
Write-Host "MANIFEST $manifestPath sha256=$(Get-Sha256 $manifestPath)"
if ($ok.Count -ne $Trials) { exit 1 }
if ($fastMath -eq $true) { Write-Host 'FAST_MATH binary refused'; exit 2 }
if ($uniqueHashes.Count -ne 1) { exit 3 }
exit 0
