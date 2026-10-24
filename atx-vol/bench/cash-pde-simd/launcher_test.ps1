param(
  [Parameter(Mandatory = $true)] [string] $Binary,
  [Parameter(Mandatory = $true)] [string] $Reference,
  [Parameter(Mandatory = $true)] [string] $OutDir
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

# Test-only command interception. The runner must stop before Process.Start;
# this control intentionally supplies a frozen source marker and a live compiler.
function git {
  if ($args -contains 'rev-parse') { 'synthetic-host-refusal-control' }
  $global:LASTEXITCODE = $(if ($global:CashRunnerTestFailStatus -and
                             $args -contains 'status') { 128 }
                           else { 0 })
}

$global:CashRunnerTestFailStatus = $true
$statusRefused = $false
$statusOut = $OutDir + '-status-failure'
try {
  & (Join-Path $PSScriptRoot 'run_trials.ps1') -Binary $Binary -Reference $Reference `
      -SourceRoot (Join-Path $PSScriptRoot '../../..') -OutDir $statusOut -Trials 1
} catch {
  if ($_.Exception.Message -notmatch 'Cannot inspect source cleanliness') { throw }
  $statusRefused = $true
}
if (-not $statusRefused -or (Test-Path -LiteralPath $statusOut)) {
  throw 'Failed git status did not stop before output creation/native work'
}
$global:CashRunnerTestFailStatus = $false
function Get-Process {
  [pscustomobject]@{ Id = 12345; ProcessName = 'clang-cl' }
}

$refused = $false
try {
  & (Join-Path $PSScriptRoot 'run_trials.ps1') -Binary $Binary -Reference $Reference `
      -SourceRoot (Join-Path $PSScriptRoot '../../..') -OutDir $OutDir -Trials 1
} catch {
  if ($_.Exception.Message -notmatch 'Host has active build/native jobs') { throw }
  $refused = $true
}
if (-not $refused) { throw 'Launcher ignored the active compiler' }
$manifest = Get-Content -LiteralPath (Join-Path $OutDir 'manifest.json') -Raw | ConvertFrom-Json
if ($manifest.status -ne 'blocked-or-failed' -or $manifest.timing_qualified -ne $false -or
    $manifest.records.Count -ne 1 -or $manifest.records[0].launched -ne $false -or
    $manifest.records[0].host_jobs[0].name -ne 'clang-cl') {
  throw 'Blocked host did not retain its exact unlaunched receipt'
}
'Launcher git-status failure and active-host refusal PASS; no native process launched.'
