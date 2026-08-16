# Fixed small-gate adapter for oracle bootstrap verification.
# It runs only worktree-local binaries, consumes the real OracleBench scorecard,
# and emits one closed aggregate-only typed JSON receipt.
[CmdletBinding()]
param(
  [ValidateSet('mode_a_targeted_tests', 'mode_a_smoke', 'convention_tests', 'mode_a_smoke_tune', 'residual_floor', 'mode_b_targeted_tests', 'mode_b_smoke_tune', 'sprint_american_greeks_delta_put', 'sprint_adjusted_greeks_flat_smile')]
  [string]$Gate
)

$ErrorActionPreference = 'Stop'
$script:OracleRepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$script:OracleStoreRoot = 'C:\atx-cache\oracle\spiderrock'
$script:ModeAMetricMap = [ordered]@{
  price = 'mode_a_price_mae'
  vol = 'mode_a_vol_mae'
  de = 'mode_a_delta_rel'
  ga = 'mode_a_gamma_rel'
  th = 'mode_a_theta_rel'
  ve = 'mode_a_vega_rel'
  rh = 'mode_a_rho_rel'
  ph = 'mode_a_phi_rel'
  vo = 'mode_a_volga_rel'
  va = 'mode_a_vanna_rel'
  deDecay = 'mode_a_delta_decay_rel'
}

function Get-OracleGitIdentity {
  $sha = (& git -C $script:OracleRepoRoot rev-parse --verify HEAD 2>$null | Out-String).Trim().ToLowerInvariant()
  if ($LASTEXITCODE -ne 0 -or $sha -notmatch '^[0-9a-f]{40}$') { throw 'oracle targeted gate cannot resolve worktree HEAD' }
  $tree = (& git -C $script:OracleRepoRoot rev-parse --verify 'HEAD^{tree}' 2>$null | Out-String).Trim().ToLowerInvariant()
  if ($LASTEXITCODE -ne 0 -or $tree -notmatch '^[0-9a-f]{40}$') { throw 'oracle targeted gate cannot resolve worktree tree' }
  return [pscustomobject]@{ Sha = $sha; Tree = $tree }
}

function Get-OracleTargetedGateSpec([string]$GateId, $Identity) {
  if (-not $Identity) { $Identity = Get-OracleGitIdentity }
  $buildScript = Join-Path $script:OracleRepoRoot 'scripts\atx-build.ps1'
  $testExe = Join-Path $script:OracleRepoRoot 'build\bin\atx-vol-tests.exe'
  $benchExe = Join-Path $script:OracleRepoRoot 'build\bin\atx-vol-oracle-bench.exe'
  $smokeCohort = Join-Path $script:OracleRepoRoot 'atx-vol\bench\oracle\cohorts\smoke.json'
  $outputRoot = Join-Path $script:OracleRepoRoot 'build\oracle-gates'
  switch ($GateId) {
    'mode_a_targeted_tests' {
      return [pscustomobject]@{
        Kind = 'ctest'; Program = 'powershell'; OutputPath = ''
        RequiredExecutables = @($testExe, $benchExe)
        PrepareProgram = 'powershell'
        PrepareArguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $buildScript, '-Preset', 'dev', 'build', 'atx-vol-tests', 'atx-vol-oracle-bench', '--parallel', '2')
        Arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $buildScript, '-Preset', 'dev', '-Ctest', '-R', '^OracleBench.*$', '--no-tests=error')
      }
    }
    'mode_a_smoke' {
      $out = Join-Path $outputRoot ('mode-a-smoke-' + $Identity.Sha + '.json')
      return [pscustomobject]@{
        Kind = 'oracle_bench'; Program = $benchExe; OutputPath = $out
        RequiredExecutables = @($benchExe)
        Arguments = @('--cohort', $smokeCohort, '--store', $script:OracleStoreRoot, '--out', $out, '--iter', '0', '--git-sha', $Identity.Sha)
      }
    }
    'convention_tests' { return [pscustomobject]@{ Kind = 'ctest'; Program = 'powershell'; OutputPath = ''; RequiredExecutables = @($testExe); Arguments = @('-NoProfile', '-File', $buildScript, '-Preset', 'dev', '-Ctest', '-R', '^convention_tests$', '--no-tests=error') } }
    'mode_a_smoke_tune' { return [pscustomobject]@{ Kind = 'oracle_bench'; Program = 'atx-vol-oracle-bench'; OutputPath = ''; RequiredExecutables = @(); Arguments = @('--cohort', 'smoke,tune', '--mode', 'A', '--aggregate-only') } }
    'residual_floor' { return [pscustomobject]@{ Kind = 'oracle_bench'; Program = 'atx-vol-oracle-bench'; OutputPath = ''; RequiredExecutables = @(); Arguments = @('--cohort', 'smoke,tune', '--mode', 'A', '--residual-floor', '--aggregate-only') } }
    'mode_b_targeted_tests' { return [pscustomobject]@{ Kind = 'ctest'; Program = 'powershell'; OutputPath = ''; RequiredExecutables = @($testExe); Arguments = @('-NoProfile', '-File', $buildScript, '-Preset', 'dev', '-Ctest', '-R', '^mode_b_targeted_tests$', '--no-tests=error') } }
    'mode_b_smoke_tune' { return [pscustomobject]@{ Kind = 'oracle_bench'; Program = 'atx-vol-oracle-bench'; OutputPath = ''; RequiredExecutables = @(); Arguments = @('--cohort', 'smoke,tune', '--mode', 'B', '--aggregate-only') } }
    'sprint_american_greeks_delta_put' { return [pscustomobject]@{ Kind = 'ctest'; Program = 'powershell'; OutputPath = ''; RequiredExecutables = @($testExe); Arguments = @('-NoProfile', '-File', $buildScript, '-Preset', 'dev', '-Ctest', '-R', '^AmericanGreeks.Delta_MatchesFd_Put$', '--no-tests=error') } }
    'sprint_adjusted_greeks_flat_smile' { return [pscustomobject]@{ Kind = 'ctest'; Program = 'powershell'; OutputPath = ''; RequiredExecutables = @($testExe); Arguments = @('-NoProfile', '-File', $buildScript, '-Preset', 'dev', '-Ctest', '-R', '^AdjustedGreeks.FlatSmileLeavesDeltaUnchanged$', '--no-tests=error') } }
    default { throw "unknown oracle targeted gate: $GateId" }
  }
}

function Get-OracleRequiredMetricIds([string]$GateId) {
  if ($GateId -in @('mode_a_smoke', 'mode_a_smoke_tune', 'residual_floor')) {
    return @($script:ModeAMetricMap.Values | ForEach-Object { [string]$_ })
  }
  if ($GateId -eq 'mode_b_smoke_tune') {
    return @('mode_b_price_mae', 'mode_b_vol_mae', 'mode_b_delta_rel', 'mode_b_gamma_rel', 'mode_b_theta_rel', 'mode_b_vega_rel', 'mode_b_rho_rel', 'mode_b_phi_rel', 'mode_b_volga_rel', 'mode_b_vanna_rel', 'mode_b_delta_decay_rel')
  }
  return @()
}

function Test-OracleExactStringSet($Values, [string[]]$Expected) {
  $actual = @($Values | ForEach-Object { [string]$_ })
  return $actual.Count -eq $Expected.Count -and @($actual | Select-Object -Unique).Count -eq $Expected.Count -and -not (Compare-Object ($actual | Sort-Object) ($Expected | Sort-Object))
}

function Test-OracleExactKeys($Value, [string[]]$Expected) {
  if (-not $Value) { return $false }
  return Test-OracleExactStringSet @($Value.PSObject.Properties.Name) $Expected
}

function Test-OracleNonnegativeInteger($Value) {
  if ($null -eq $Value) { return $false }
  $number = 0L
  return [long]::TryParse(([string]$Value), [ref]$number) -and $number -ge 0 -and [double]$Value -eq [double]$number
}

function Test-OracleFiniteNumber($Value) {
  if ($null -eq $Value) { return $false }
  $number = 0.0
  return [double]::TryParse(([string]$Value), [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$number) -and -not [double]::IsNaN($number) -and -not [double]::IsInfinity($number)
}

function Get-OracleTextSha256([string]$Text) {
  $bytes = [System.Text.UTF8Encoding]::new($false).GetBytes($Text)
  try {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    return ([System.BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
  } finally { if ($sha) { $sha.Dispose() } }
}

function Assert-OracleGateExecutables($Spec) {
  foreach ($path in @($Spec.RequiredExecutables)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw ('oracle targeted gate executable missing: ' + $path) }
  }
}

function Invoke-OracleNativeProcess([string]$Program, [string[]]$Arguments) {
  # Windows PowerShell 5 surfaces a native program's ordinary stderr as
  # NativeCommandError records. OracleBench intentionally reports progress on
  # stderr, so capture those records without letting the script-wide Stop
  # policy abort a successful native process.
  $savedPreference = $ErrorActionPreference
  try {
    $ErrorActionPreference = 'Continue'
    $lines = @(& $Program @Arguments 2>&1 | ForEach-Object { [string]$_ })
    $exitCode = $LASTEXITCODE
  } finally { $ErrorActionPreference = $savedPreference }
  return [pscustomobject]@{ ExitCode = [int]$exitCode; Lines = $lines }
}

function ConvertFrom-OracleBenchScorecard([string]$ScorecardText, [string]$GateId, $Identity) {
  try { $scorecard = $ScorecardText | ConvertFrom-Json } catch { throw "oracle targeted gate $GateId scorecard is not JSON" }
  if ($GateId -ne 'mode_a_smoke') { throw "oracle targeted gate $GateId has no production scorecard adapter yet" }
  if (-not (Test-OracleExactKeys $scorecard @('iter', 'git_sha', 'cohort', 'modes', 'tolerances', 'cells')) -or
      $scorecard.git_sha -ne $Identity.Sha -or $scorecard.cohort -ne 'smoke' -or -not (Test-OracleExactKeys $scorecard.modes @('a'))) {
    throw "oracle targeted gate $GateId scorecard identity/schema mismatch"
  }
  $mode = $scorecard.modes.a
  $modeKeys = @('rows_total', 'rows_priced', 'rows_null_sentinel', 'rows_bad_input', 'rows_engine_error', 'wall_seconds', 'rows_per_second')
  if (-not (Test-OracleExactKeys $mode $modeKeys)) { throw "oracle targeted gate $GateId scorecard mode schema mismatch" }
  foreach ($name in @('rows_total', 'rows_priced', 'rows_null_sentinel', 'rows_bad_input', 'rows_engine_error')) {
    if (-not (Test-OracleNonnegativeInteger $mode.$name)) { throw "oracle targeted gate $GateId scorecard has invalid $name" }
  }
  $rowsProcessed = [long]$mode.rows_priced
  $accounted = $rowsProcessed + [long]$mode.rows_null_sentinel + [long]$mode.rows_bad_input + [long]$mode.rows_engine_error
  if ($rowsProcessed -le 0 -or [long]$mode.rows_total -ne $accounted -or -not (Test-OracleFiniteNumber $mode.wall_seconds) -or [double]$mode.wall_seconds -le 0 -or
      -not (Test-OracleFiniteNumber $mode.rows_per_second) -or [double]$mode.rows_per_second -le 0) {
    throw "oracle targeted gate $GateId scorecard reports empty/inconsistent priced work"
  }
  $cellProperties = @($scorecard.cells.PSObject.Properties)
  if (-not $cellProperties.Count) { throw "oracle targeted gate $GateId scorecard has no aggregate cells" }
  $seenMetrics = @{}
  $cellPattern = '^a\.(price|vol|de|ga|th|ve|rh|ph|vo|va|deDecay)\.(deep-itm|itm|atm|otm|deep-otm)\.(0-7|8-30|31-90|90\+)\.(c|p)$'
  $statKeys = @('n', 'mae', 'rmse', 'p50', 'p95', 'p99', 'max', 'within_tol_rate')
  foreach ($property in $cellProperties) {
    if ($property.Name -notmatch $cellPattern) { throw "oracle targeted gate $GateId scorecard contains an unknown cell ID" }
    $metric = $Matches[1]
    $stats = $property.Value
    if (-not (Test-OracleExactKeys $stats $statKeys) -or -not (Test-OracleNonnegativeInteger $stats.n) -or [long]$stats.n -le 0) {
      throw "oracle targeted gate $GateId scorecard contains an invalid aggregate cell"
    }
    foreach ($field in @('mae', 'rmse', 'p50', 'p95', 'p99', 'max', 'within_tol_rate')) {
      if (-not (Test-OracleFiniteNumber $stats.$field)) { throw "oracle targeted gate $GateId scorecard contains a non-finite aggregate" }
    }
    if ([double]$stats.mae -lt 0 -or [double]$stats.rmse -lt 0 -or [double]$stats.p50 -lt 0 -or [double]$stats.p95 -lt 0 -or
        [double]$stats.p99 -lt 0 -or [double]$stats.max -lt 0 -or [double]$stats.within_tol_rate -lt 0 -or [double]$stats.within_tol_rate -gt 1) {
      throw "oracle targeted gate $GateId scorecard contains an out-of-range aggregate"
    }
    if (-not $seenMetrics.ContainsKey($metric)) { $seenMetrics[$metric] = 0L }
    $seenMetrics[$metric] += [long]$stats.n
  }
  $expectedMetrics = @($script:ModeAMetricMap.Keys | ForEach-Object { [string]$_ })
  if (-not (Test-OracleExactStringSet @($seenMetrics.Keys) $expectedMetrics) -or @($seenMetrics.Values | Where-Object { [long]$_ -le 0 }).Count) {
    throw "oracle targeted gate $GateId scorecard metric coverage is incomplete"
  }
  return [pscustomobject]@{
    RowsProcessed = $rowsProcessed
    MetricIds = @($expectedMetrics | ForEach-Object { [string]$script:ModeAMetricMap[$_] })
  }
}

function Invoke-OracleTargetedGate([string]$GateId, [scriptblock]$Invoker) {
  $identity = Get-OracleGitIdentity
  $spec = Get-OracleTargetedGateSpec $GateId $identity
  if (-not $Invoker) {
    if ($spec.PrepareProgram) {
      $prepare = Invoke-OracleNativeProcess $spec.PrepareProgram @($spec.PrepareArguments)
      $prepareLines = @($prepare.Lines)
      $prepareExitCode = [int]$prepare.ExitCode
      if ($prepareExitCode -ne 0) { throw "oracle targeted gate $GateId target build failed with exit code $prepareExitCode" }
    }
    Assert-OracleGateExecutables $spec
    if ($spec.Kind -eq 'oracle_bench') {
      if (-not (Test-Path -LiteralPath $script:OracleStoreRoot -PathType Container)) { throw 'licensed aggregate oracle store is missing' }
      New-Item -ItemType Directory -Force (Split-Path -Parent $spec.OutputPath) | Out-Null
      if (Test-Path -LiteralPath $spec.OutputPath) { Remove-Item -LiteralPath $spec.OutputPath -Force }
    }
  }
  if ($Invoker) {
    $execution = & $Invoker $spec
    $exitCode = [int]$execution.ExitCode
    $lines = @($execution.Lines | ForEach-Object { [string]$_ })
  } else {
    $execution = Invoke-OracleNativeProcess $spec.Program @($spec.Arguments)
    $lines = @($execution.Lines)
    if ($prepareLines) { $lines = @($prepareLines) + @($lines) }
    $exitCode = [int]$execution.ExitCode
  }
  $identityAfter = Get-OracleGitIdentity
  if ($identityAfter.Sha -ne $identity.Sha -or $identityAfter.Tree -ne $identity.Tree) { throw "oracle targeted gate $GateId changed HEAD/tree while executing" }
  $raw = ($lines -join "`n").Trim()
  $observations = @($lines | Where-Object { $_.Trim() }).Count
  if ($exitCode -ne 0) { throw "oracle targeted gate $GateId failed with exit code $exitCode" }
  if ($observations -lt 1) { throw "oracle targeted gate $GateId emitted no process evidence" }
  $testsExecuted = 0
  $testsPassed = 0
  $rowsProcessed = 0L
  $metricIds = @()
  $rawEvidence = $raw
  if ($spec.Kind -eq 'ctest') {
    if ($raw -match 'No tests were found') { throw "oracle targeted gate $GateId executed zero tests" }
    $summary = [regex]::Match($raw, '(?m)(\d+)% tests passed,\s*(\d+) tests failed out of\s*(\d+)')
    if (-not $summary.Success) { throw "oracle targeted gate $GateId lacks a typed ctest summary" }
    $percent = [int]$summary.Groups[1].Value
    $failed = [int]$summary.Groups[2].Value
    $testsExecuted = [int]$summary.Groups[3].Value
    $testsPassed = $testsExecuted - $failed
    if ($testsExecuted -le 0 -or $failed -ne 0 -or $percent -ne 100 -or $testsPassed -ne $testsExecuted) { throw "oracle targeted gate $GateId did not pass positive test work" }
    $auditSummary = "tests_executed=$testsExecuted tests_passed=$testsPassed"
  } else {
    if ($Invoker -and $execution.PSObject.Properties.Name -contains 'ScorecardJson') {
      $scorecardText = [string]$execution.ScorecardJson
    } else {
      if (-not $spec.OutputPath -or -not (Test-Path -LiteralPath $spec.OutputPath -PathType Leaf)) { throw "oracle targeted gate $GateId did not produce a scorecard" }
      $scorecardText = [System.IO.File]::ReadAllText($spec.OutputPath)
    }
    $aggregate = ConvertFrom-OracleBenchScorecard $scorecardText $GateId $identity
    $rowsProcessed = [long]$aggregate.RowsProcessed
    $metricIds = @($aggregate.MetricIds | ForEach-Object { [string]$_ })
    $requiredMetricIds = Get-OracleRequiredMetricIds $GateId
    if ($rowsProcessed -le 0 -or -not (Test-OracleExactStringSet $metricIds $requiredMetricIds)) { throw "oracle targeted gate $GateId reported empty/incomplete aggregate work" }
    $auditSummary = 'status=PASS rows_processed=' + $rowsProcessed + ' metric_ids=' + (($metricIds | Sort-Object) -join ',')
    $rawEvidence = $raw + "`n--scorecard--`n" + $scorecardText
  }
  return [ordered]@{
    schema_version = 1
    status = 'PASS'
    observations = $observations
    command_id = $GateId
    tested_sha = $identity.Sha
    tested_tree = $identity.Tree
    gate_kind = $spec.Kind
    tests_executed = $testsExecuted
    tests_passed = $testsPassed
    rows_processed = $rowsProcessed
    metric_ids = $metricIds
    audit_summary = $auditSummary
    raw_output_sha256 = Get-OracleTextSha256 $rawEvidence
  }
}

# Pester imports the production functions and injects only the process invoker.
if ($MyInvocation.InvocationName -eq '.') { return }
if (-not $Gate) { throw '-Gate is required' }
Invoke-OracleTargetedGate $Gate $null | ConvertTo-Json -Compress
