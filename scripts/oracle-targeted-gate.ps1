# Fixed small-gate adapter for oracle bootstrap verification.
# It captures ordinary tool output and emits one closed typed JSON receipt.
[CmdletBinding()]
param(
  [ValidateSet('mode_a_targeted_tests', 'mode_a_smoke', 'convention_tests', 'mode_a_smoke_tune', 'residual_floor', 'mode_b_targeted_tests', 'mode_b_smoke_tune')]
  [string]$Gate
)

$ErrorActionPreference = 'Stop'

function Get-OracleTargetedGateSpec([string]$GateId) {
  $buildScript = Join-Path $PSScriptRoot 'atx-build.ps1'
  switch ($GateId) {
    'mode_a_targeted_tests' { return [pscustomobject]@{ Program = 'powershell'; Arguments = @('-NoProfile', '-File', $buildScript, '-Preset', 'dev', '-Ctest', '-R', '^mode_a_targeted_tests$') } }
    'mode_a_smoke' { return [pscustomobject]@{ Program = 'atx-vol-oracle-bench'; Arguments = @('--cohort', 'smoke', '--mode', 'A', '--aggregate-only') } }
    'convention_tests' { return [pscustomobject]@{ Program = 'powershell'; Arguments = @('-NoProfile', '-File', $buildScript, '-Preset', 'dev', '-Ctest', '-R', '^convention_tests$') } }
    'mode_a_smoke_tune' { return [pscustomobject]@{ Program = 'atx-vol-oracle-bench'; Arguments = @('--cohort', 'smoke,tune', '--mode', 'A', '--aggregate-only') } }
    'residual_floor' { return [pscustomobject]@{ Program = 'atx-vol-oracle-bench'; Arguments = @('--cohort', 'smoke,tune', '--mode', 'A', '--residual-floor', '--aggregate-only') } }
    'mode_b_targeted_tests' { return [pscustomobject]@{ Program = 'powershell'; Arguments = @('-NoProfile', '-File', $buildScript, '-Preset', 'dev', '-Ctest', '-R', '^mode_b_targeted_tests$') } }
    'mode_b_smoke_tune' { return [pscustomobject]@{ Program = 'atx-vol-oracle-bench'; Arguments = @('--cohort', 'smoke,tune', '--mode', 'B', '--aggregate-only') } }
    default { throw "unknown oracle targeted gate: $GateId" }
  }
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
  return [ordered]@{
    schema_version = 1
    status = 'PASS'
    observations = $observations
    command_id = $GateId
    raw_output_sha256 = Get-OracleTextSha256 $raw
  }
}

# Pester imports the production functions and injects only the process invoker.
if ($MyInvocation.InvocationName -eq '.') { return }
if (-not $Gate) { throw '-Gate is required' }
Invoke-OracleTargetedGate $Gate $null | ConvertTo-Json -Compress
