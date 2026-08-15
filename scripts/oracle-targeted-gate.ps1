# Fixed small-gate adapter for oracle bootstrap verification.
# It captures ordinary tool output and emits one closed typed JSON receipt.
[CmdletBinding()]
param(
  [ValidateSet('mode_a_targeted_tests', 'mode_a_smoke', 'convention_tests', 'mode_a_smoke_tune', 'residual_floor', 'mode_b_targeted_tests', 'mode_b_smoke_tune', 'sprint_american_greeks_delta_put', 'sprint_adjusted_greeks_flat_smile')]
  [string]$Gate
)

$ErrorActionPreference = 'Stop'

function Get-OracleTargetedGateSpec([string]$GateId) {
  $buildScript = Join-Path $PSScriptRoot 'atx-build.ps1'
  switch ($GateId) {
    'mode_a_targeted_tests' { return [pscustomobject]@{ Kind = 'ctest'; Program = 'powershell'; Arguments = @('-NoProfile', '-File', $buildScript, '-Preset', 'dev', '-Ctest', '-R', '^mode_a_targeted_tests$', '--no-tests=error') } }
    'mode_a_smoke' { return [pscustomobject]@{ Kind = 'oracle_bench'; Program = 'atx-vol-oracle-bench'; Arguments = @('--cohort', 'smoke', '--mode', 'A', '--aggregate-only') } }
    'convention_tests' { return [pscustomobject]@{ Kind = 'ctest'; Program = 'powershell'; Arguments = @('-NoProfile', '-File', $buildScript, '-Preset', 'dev', '-Ctest', '-R', '^convention_tests$', '--no-tests=error') } }
    'mode_a_smoke_tune' { return [pscustomobject]@{ Kind = 'oracle_bench'; Program = 'atx-vol-oracle-bench'; Arguments = @('--cohort', 'smoke,tune', '--mode', 'A', '--aggregate-only') } }
    'residual_floor' { return [pscustomobject]@{ Kind = 'oracle_bench'; Program = 'atx-vol-oracle-bench'; Arguments = @('--cohort', 'smoke,tune', '--mode', 'A', '--residual-floor', '--aggregate-only') } }
    'mode_b_targeted_tests' { return [pscustomobject]@{ Kind = 'ctest'; Program = 'powershell'; Arguments = @('-NoProfile', '-File', $buildScript, '-Preset', 'dev', '-Ctest', '-R', '^mode_b_targeted_tests$', '--no-tests=error') } }
    'mode_b_smoke_tune' { return [pscustomobject]@{ Kind = 'oracle_bench'; Program = 'atx-vol-oracle-bench'; Arguments = @('--cohort', 'smoke,tune', '--mode', 'B', '--aggregate-only') } }
    'sprint_american_greeks_delta_put' { return [pscustomobject]@{ Kind = 'ctest'; Program = 'powershell'; Arguments = @('-NoProfile', '-File', $buildScript, '-Preset', 'dev', '-Ctest', '-R', '^AmericanGreeks.Delta_MatchesFd_Put$', '--no-tests=error') } }
    'sprint_adjusted_greeks_flat_smile' { return [pscustomobject]@{ Kind = 'ctest'; Program = 'powershell'; Arguments = @('-NoProfile', '-File', $buildScript, '-Preset', 'dev', '-Ctest', '-R', '^AdjustedGreeks.FlatSmileLeavesDeltaUnchanged$', '--no-tests=error') } }
    default { throw "unknown oracle targeted gate: $GateId" }
  }
}

function Get-OracleRequiredMetricIds([string]$GateId) {
  if ($GateId -in @('mode_a_smoke', 'mode_a_smoke_tune', 'residual_floor')) {
    return @('mode_a_price_mae', 'mode_a_vol_mae', 'mode_a_delta_rel', 'mode_a_gamma_rel', 'mode_a_theta_rel', 'mode_a_vega_rel')
  }
  if ($GateId -eq 'mode_b_smoke_tune') {
    return @('mode_b_price_mae', 'mode_b_vol_mae', 'mode_b_delta_rel', 'mode_b_gamma_rel', 'mode_b_theta_rel', 'mode_b_vega_rel')
  }
  return @()
}

function Test-OracleExactStringSet($Values, [string[]]$Expected) {
  $actual = @($Values | ForEach-Object { [string]$_ })
  return $actual.Count -eq $Expected.Count -and @($actual | Select-Object -Unique).Count -eq $Expected.Count -and -not (Compare-Object ($actual | Sort-Object) ($Expected | Sort-Object))
}

function Get-OracleTextSha256([string]$Text) {
  $bytes = [System.Text.UTF8Encoding]::new($false).GetBytes($Text)
  try {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    return ([System.BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
  } finally { if ($sha) { $sha.Dispose() } }
}

function Invoke-OracleTargetedGate([string]$GateId, [scriptblock]$Invoker) {
  $spec = Get-OracleTargetedGateSpec $GateId
  if ($Invoker) {
    $execution = & $Invoker $spec
    $exitCode = [int]$execution.ExitCode
    $lines = @($execution.Lines | ForEach-Object { [string]$_ })
  } else {
    $lines = @(& $spec.Program @($spec.Arguments) 2>&1 | ForEach-Object { [string]$_ })
    $exitCode = $LASTEXITCODE
  }
  $raw = ($lines -join "`n").Trim()
  $observations = @($lines | Where-Object { $_.Trim() }).Count
  if ($exitCode -ne 0) { throw "oracle targeted gate $GateId failed with exit code $exitCode" }
  if ($observations -lt 1) { throw "oracle targeted gate $GateId emitted no evidence" }
  $testsExecuted = 0
  $testsPassed = 0
  $rowsProcessed = 0L
  $metricIds = @()
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
    try { $aggregate = $raw | ConvertFrom-Json } catch { throw "oracle targeted gate $GateId lacks typed OracleBench JSON" }
    $requiredMetricIds = Get-OracleRequiredMetricIds $GateId
    $rowsProcessed = [long]$aggregate.rows_processed
    $metricIds = @($aggregate.metric_ids | ForEach-Object { [string]$_ })
    if ($aggregate.status -ne 'PASS' -or $rowsProcessed -le 0 -or -not (Test-OracleExactStringSet $metricIds $requiredMetricIds)) {
      throw "oracle targeted gate $GateId reported empty/incomplete aggregate work"
    }
    $auditSummary = 'status=PASS rows_processed=' + $rowsProcessed + ' metric_ids=' + (($metricIds | Sort-Object) -join ',')
  }
  return [ordered]@{
    schema_version = 1
    status = 'PASS'
    observations = $observations
    command_id = $GateId
    gate_kind = $spec.Kind
    tests_executed = $testsExecuted
    tests_passed = $testsPassed
    rows_processed = $rowsProcessed
    metric_ids = $metricIds
    audit_summary = $auditSummary
    raw_output_sha256 = Get-OracleTextSha256 $raw
  }
}

# Pester imports the production functions and injects only the process invoker.
if ($MyInvocation.InvocationName -eq '.') { return }
if (-not $Gate) { throw '-Gate is required' }
Invoke-OracleTargetedGate $Gate $null | ConvertTo-Json -Compress
