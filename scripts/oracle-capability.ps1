# Aggregate-only capability probe for the SpiderRock oracle workflow.
# It checks committed receipt existence and reads only holdout.sha256; it never
# opens cohort membership or licensed row data.
$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$canonicalRef = 'refs/heads/oracle/canonical'
$baseRef = 'main'
$dataRoot = 'C:\atx-cache\oracle\spiderrock'

function Resolve-Commit([string]$Ref) {
  $sha = (& git -C $repoRoot rev-parse --verify ($Ref + '^{commit}') 2>$null)
  if ($LASTEXITCODE -ne 0 -or [string]$sha -notmatch '^[0-9a-fA-F]{40}$') {
    throw ('cannot resolve commit: ' + $Ref)
  }
  return ([string]$sha).Trim().ToLowerInvariant()
}

function Test-CommittedPath([string]$Sha, [string]$Path) {
  $savedPreference = $ErrorActionPreference
  try {
    $ErrorActionPreference = 'SilentlyContinue'
    & git -C $repoRoot cat-file -e ($Sha + ':' + $Path) 2>$null
    $exitCode = $LASTEXITCODE
  } finally { $ErrorActionPreference = $savedPreference }
  return $exitCode -eq 0
}

$canonicalExists = $true
try { $baseSha = Resolve-Commit $canonicalRef }
catch {
  $canonicalExists = $false
  $baseSha = Resolve-Commit $baseRef
}
$resolvedBaseRef = if ($canonicalExists) { $canonicalRef } else { $baseRef }

$cohortPrefix = 'atx-vol/bench/oracle/cohorts/'
$digestPath = $cohortPrefix + 'holdout.sha256'
$cohortReceiptsPresent = @('smoke.json', 'tune.json', 'holdout.json', 'holdout.sha256') |
  ForEach-Object { Test-CommittedPath $baseSha ($cohortPrefix + $_) } |
  Where-Object { -not $_ } |
  Measure-Object |
  Select-Object -ExpandProperty Count
$cohortReceiptsPresent = $cohortReceiptsPresent -eq 0
$dataManifestPresent = @(Get-ChildItem -LiteralPath $DataRoot -Filter 'oracle_manifest_*.json' -File -ErrorAction SilentlyContinue).Count -gt 0

$holdoutDigest = ''
if (Test-CommittedPath $baseSha $digestPath) {
  $holdoutDigest = ([string](& git -C $repoRoot show ($baseSha + ':' + $digestPath))).Trim().ToLowerInvariant()
  if ($LASTEXITCODE -ne 0 -or $holdoutDigest -notmatch '^[0-9a-f]{64}$') { throw 'committed holdout digest receipt is invalid' }
}

if (-not $dataManifestPresent -or -not $cohortReceiptsPresent -or -not $holdoutDigest) {
  $state = 'missing_data'
} elseif (-not (Test-CommittedPath $baseSha 'atx-vol/bench/oracle/bootstrap/mode-a.json')) {
  $state = 'missing_mode_a'
} elseif (-not (Test-CommittedPath $baseSha 'atx-vol/bench/oracle/CONVENTIONS.md') -or
        -not (Test-CommittedPath $baseSha 'atx-vol/bench/oracle/scorecards/iter-000.json')) {
  $state = 'missing_conventions'
} elseif (-not (Test-CommittedPath $baseSha 'atx-vol/bench/oracle/bootstrap/mode-b.json')) {
  $state = 'missing_mode_b'
} else {
  $state = 'ready'
}

$nextNumber = 0
$scorecardNames = @(& git -C $repoRoot ls-tree -r --name-only $baseSha -- 'atx-vol/bench/oracle/scorecards' 2>$null)
if ($LASTEXITCODE -eq 0) {
  foreach ($name in $scorecardNames) {
    if ([string]$name -match '/iter-([0-9]+)\.json$') {
      $nextNumber = [Math]::Max($nextNumber, [int]$Matches[1] + 1)
    }
  }
}
$nextIter = 'iter-' + $nextNumber.ToString('000')
$probeCommand = 'powershell scripts\oracle-capability.ps1'
$probeOutput = 'state=' + $state + ' canonical_exists=' + $canonicalExists.ToString().ToLowerInvariant() +
  ' data_manifest_present=' + $dataManifestPresent.ToString().ToLowerInvariant() +
  ' cohort_receipts_present=' + $cohortReceiptsPresent.ToString().ToLowerInvariant()

[ordered]@{
  state = $state
  canonical_ref = $canonicalRef
  canonical_exists = $canonicalExists
  base_ref = $resolvedBaseRef
  base_sha = $baseSha
  holdout_digest_receipt = $holdoutDigest
  next_iter = $nextIter
  evidence = @([ordered]@{ command = $probeCommand; exit_code = 0; output = $probeOutput })
} | ConvertTo-Json -Depth 5 -Compress
