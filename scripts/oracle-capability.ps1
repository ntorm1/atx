# Closed, aggregate-only capability probe for the SpiderRock oracle workflow.
# It validates versioned bootstrap receipts and provenance. The fixed probe alone
# opens committed cohort manifests to recompute membership integrity; stdout is
# state plus booleans/digest only and never contains membership or licensed rows.
$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$canonicalRef = 'refs/heads/oracle/canonical'
$baseRef = 'main'
$dataRoot = 'C:\atx-cache\oracle\spiderrock'
$oracleRoot = 'atx-vol/bench/oracle'
$targetA = @('mode_a_price_mae', 'mode_a_vol_mae', 'mode_a_delta_rel', 'mode_a_gamma_rel', 'mode_a_theta_rel', 'mode_a_vega_rel', 'mode_a_rho_rel', 'mode_a_phi_rel', 'mode_a_volga_rel', 'mode_a_vanna_rel', 'mode_a_delta_decay_rel')
$targetB = @('mode_b_price_mae', 'mode_b_vol_mae', 'mode_b_delta_rel', 'mode_b_gamma_rel', 'mode_b_theta_rel', 'mode_b_vega_rel', 'mode_b_rho_rel', 'mode_b_phi_rel', 'mode_b_volga_rel', 'mode_b_vanna_rel', 'mode_b_delta_decay_rel')

function Invoke-GitText([string[]]$GitArgs) {
  $savedPreference = $ErrorActionPreference
  try {
    $ErrorActionPreference = 'SilentlyContinue'
    $output = @(& git -C $repoRoot @GitArgs 2>$null)
    $code = $LASTEXITCODE
  } finally { $ErrorActionPreference = $savedPreference }
  return [pscustomobject]@{ Code = $code; Text = (($output | ForEach-Object { [string]$_ }) -join "`n").Trim() }
}

function Resolve-Commit([string]$Ref) {
  $result = Invoke-GitText @('rev-parse', '--verify', ($Ref + '^{commit}'))
  if ($result.Code -ne 0 -or $result.Text -notmatch '^[0-9a-fA-F]{40}$') { throw ('cannot resolve commit: ' + $Ref) }
  return $result.Text.ToLowerInvariant()
}

function Test-CommittedPath([string]$Sha, [string]$Path) {
  return (Invoke-GitText @('cat-file', '-e', ($Sha + ':' + $Path))).Code -eq 0
}

function Get-BlobOid([string]$Sha, [string]$Path) {
  $result = Invoke-GitText @('rev-parse', '--verify', ($Sha + ':' + $Path))
  if ($result.Code -ne 0 -or $result.Text -notmatch '^[0-9a-fA-F]{40}$') { return '' }
  return $result.Text.ToLowerInvariant()
}

function Get-CommittedJson([string]$Sha, [string]$Path) {
  $result = Invoke-GitText @('show', ($Sha + ':' + $Path))
  if ($result.Code -ne 0 -or -not $result.Text) { return $null }
  try { return $result.Text | ConvertFrom-Json } catch { return $null }
}

function Test-ExactKeys($Value, [string[]]$Keys) {
  if (-not $Value) { return $false }
  $actual = @($Value.PSObject.Properties.Name | Sort-Object)
  $wanted = @($Keys | Sort-Object)
  return $actual.Count -eq $wanted.Count -and -not (Compare-Object $actual $wanted)
}

function Test-StringSet($Values, [string[]]$Expected) {
  $actual = @($Values)
  return $actual.Count -eq $Expected.Count -and @($actual | Select-Object -Unique).Count -eq $Expected.Count -and -not (Compare-Object ($actual | Sort-Object) ($Expected | Sort-Object))
}

function Test-Ancestor([string]$Ancestor, [string]$Descendant) {
  if ($Ancestor -notmatch '^[0-9a-f]{40}$' -or $Descendant -notmatch '^[0-9a-f]{40}$') { return $false }
  return (Invoke-GitText @('merge-base', '--is-ancestor', $Ancestor, $Descendant)).Code -eq 0
}

function Test-Provenance($Receipt, [string]$CurrentSha, [string]$PriorReceiptPath) {
  if (-not (Test-Ancestor ([string]$Receipt.base_sha) ([string]$Receipt.tested_sha)) -or -not (Test-Ancestor ([string]$Receipt.tested_sha) $CurrentSha)) { return $false }
  if ($PriorReceiptPath) {
    $baseOid = Get-BlobOid ([string]$Receipt.base_sha) $PriorReceiptPath
    if (-not $baseOid -or $baseOid -ne (Get-BlobOid $CurrentSha $PriorReceiptPath)) { return $false }
  }
  return $true
}

function Test-CohortJson($Cohort, [string]$ExpectedName) {
  if (-not (Test-ExactKeys $Cohort @('name', 'dates', 'underliers', 'buckets_et', 'notes')) -or $Cohort.name -ne $ExpectedName) { return $false }
  if (-not ($Cohort.notes -is [string]) -or @($Cohort.dates).Count -lt 1 -or @($Cohort.underliers).Count -lt 1 -or @($Cohort.buckets_et).Count -lt 1) { return $false }
  if (@($Cohort.dates | Where-Object { $_ -notmatch '^\d{4}-\d{2}-\d{2}$' }).Count -or
      @($Cohort.underliers | Where-Object { $_ -notmatch '^[A-Z0-9._-]{1,24}$' }).Count -or
      @($Cohort.buckets_et | Where-Object { $_ -notmatch '^(?:0[0-9]|1[0-9]|2[0-3])[0-5][0-9]$' -or $_ -eq '0930' }).Count) { return $false }
  return @($Cohort.dates | Select-Object -Unique).Count -eq @($Cohort.dates).Count -and
    @($Cohort.underliers | Select-Object -Unique).Count -eq @($Cohort.underliers).Count -and
    @($Cohort.buckets_et | Select-Object -Unique).Count -eq @($Cohort.buckets_et).Count
}

function Get-CohortMembershipDigest($Cohort) {
  if (-not $Cohort -or -not (Test-CohortJson $Cohort ([string]$Cohort.name))) { return '' }
  $canonical = [ordered]@{
    schema_version = 1
    name = [string]$Cohort.name
    dates = @($Cohort.dates | ForEach-Object { [string]$_ } | Sort-Object)
    underliers = @($Cohort.underliers | ForEach-Object { [string]$_ } | Sort-Object)
    buckets_et = @($Cohort.buckets_et | ForEach-Object { [string]$_ } | Sort-Object)
  } | ConvertTo-Json -Depth 4 -Compress
  $bytes = [System.Text.UTF8Encoding]::new($false).GetBytes($canonical)
  try {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    return ([System.BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
  } finally { if ($sha) { $sha.Dispose() } }
}

function Test-Disjoint($Left, $Right) {
  $leftSet = @($Left | ForEach-Object { [string]$_ } | Select-Object -Unique)
  $rightSet = @($Right | ForEach-Object { [string]$_ } | Select-Object -Unique)
  return @($leftSet | Where-Object { $rightSet -contains $_ }).Count -eq 0
}

function Test-IngestManifest([string]$Name, [string]$ExpectedSha256) {
  if (-not ($Name -match '^oracle_manifest_(\d{4}-\d{2}-\d{2})\.json$')) { return $false }
  $expectedDate = $Matches[1]
  if ($ExpectedSha256 -notmatch '^[0-9a-f]{64}$') { return $false }
  $path = Join-Path $dataRoot $Name
  if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $ExpectedSha256) { return $false }
  try { $manifest = [System.IO.File]::ReadAllText($path) | ConvertFrom-Json } catch { return $false }
  if (-not (Test-ExactKeys $manifest @('trading_date', 'source_tsv_bytes', 'total_rows', 'buckets', 'top_underliers_by_rows', 'ingested_at')) -or
      $manifest.trading_date -ne $expectedDate -or [long]$manifest.source_tsv_bytes -le 0 -or [long]$manifest.total_rows -le 0) { return $false }
  $bucketTotal = 0L
  foreach ($property in $manifest.buckets.PSObject.Properties) {
    if ($property.Name -notmatch '^(?:0[0-9]|1[0-9]|2[0-3])[0-5][0-9]$' -or $property.Name -eq '0930' -or [long]$property.Value -le 0) { return $false }
    $bucketTotal += [long]$property.Value
  }
  if ($bucketTotal -ne [long]$manifest.total_rows -or @($manifest.top_underliers_by_rows.PSObject.Properties).Count -lt 1) { return $false }
  foreach ($property in $manifest.top_underliers_by_rows.PSObject.Properties) {
    if ($property.Name -notmatch '^[A-Z0-9._-]{1,24}$' -or [long]$property.Value -le 0 -or [long]$property.Value -gt [long]$manifest.total_rows) { return $false }
  }
  $parsedTime = [datetime]::MinValue
  return [datetime]::TryParse([string]$manifest.ingested_at, [ref]$parsedTime)
}

function Test-DataReceipt([string]$Sha, [ref]$DigestOut) {
  $receipt = Get-CommittedJson $Sha ($oracleRoot + '/bootstrap/data.json')
  $keys = @('schema_version', 'transition', 'base_sha', 'tested_sha', 'command_id', 'exit_code', 'ingest_manifest_name', 'ingest_manifest_sha256', 'smoke_blob_oid', 'tune_blob_oid', 'holdout_blob_oid', 'holdout_membership_sha256', 'smoke_schema_valid', 'tune_schema_valid', 'holdout_schema_valid', 'tune_holdout_underliers_disjoint', 'tune_holdout_buckets_disjoint')
  $dataCommandIds = @('oracle_existing_store_adoption', 'oracle_ingest_and_cohort_validate')
  if (-not (Test-ExactKeys $receipt $keys) -or $receipt.schema_version -ne 1 -or $receipt.transition -ne 'data' -or $dataCommandIds -notcontains $receipt.command_id -or $receipt.exit_code -ne 0 -or
      -not $receipt.smoke_schema_valid -or -not $receipt.tune_schema_valid -or -not $receipt.holdout_schema_valid -or -not $receipt.tune_holdout_underliers_disjoint -or -not $receipt.tune_holdout_buckets_disjoint -or -not (Test-Provenance $receipt $Sha '')) { return $false }
  if ($receipt.command_id -eq 'oracle_existing_store_adoption' -and $receipt.base_sha -ne $receipt.tested_sha) { return $false }
  $smokePath = $oracleRoot + '/cohorts/smoke.json'; $tunePath = $oracleRoot + '/cohorts/tune.json'
  $holdoutPath = $oracleRoot + '/cohorts/holdout.json'; $digestPath = $oracleRoot + '/cohorts/holdout.sha256'
  if ((Get-BlobOid $Sha $smokePath) -ne $receipt.smoke_blob_oid -or (Get-BlobOid $Sha $tunePath) -ne $receipt.tune_blob_oid -or
      (Get-BlobOid $Sha $holdoutPath) -ne $receipt.holdout_blob_oid -or -not (Test-CommittedPath $Sha $digestPath)) { return $false }
  $smoke = Get-CommittedJson $Sha $smokePath
  $tune = Get-CommittedJson $Sha $tunePath
  $holdout = Get-CommittedJson $Sha $holdoutPath
  if (-not (Test-CohortJson $smoke 'smoke') -or -not (Test-CohortJson $tune 'tune') -or -not (Test-CohortJson $holdout 'holdout')) { return $false }
  if (-not (Test-Disjoint $tune.underliers $holdout.underliers) -or -not (Test-Disjoint $tune.buckets_et $holdout.buckets_et)) { return $false }
  $digestText = (Invoke-GitText @('show', ($Sha + ':' + $digestPath))).Text.ToLowerInvariant()
  $computedDigest = Get-CohortMembershipDigest $holdout
  if ($digestText -notmatch '^[0-9a-f]{64}$' -or $computedDigest -notmatch '^[0-9a-f]{64}$' -or $digestText -ne $computedDigest -or
      $digestText -ne $receipt.holdout_membership_sha256 -or -not (Test-IngestManifest ([string]$receipt.ingest_manifest_name) ([string]$receipt.ingest_manifest_sha256))) { return $false }
  $DigestOut.Value = $digestText
  return $true
}

function Test-ModeReceipt([string]$Sha, [string]$Mode, $DataReceipt) {
  $slug = $Mode.ToLowerInvariant()
  $receipt = Get-CommittedJson $Sha ($oracleRoot + '/bootstrap/mode-' + $slug + '.json')
  $keys = if ($Mode -eq 'A') { @('schema_version', 'transition', 'base_sha', 'tested_sha', 'command_id', 'exit_code', 'smoke_blob_oid', 'rows_processed', 'target_metric_ids') }
    else { @('schema_version', 'transition', 'base_sha', 'tested_sha', 'command_id', 'exit_code', 'smoke_blob_oid', 'tune_blob_oid', 'rows_processed', 'target_metric_ids') }
  $expectedTargets = if ($Mode -eq 'A') { $targetA } else { $targetB }
  $priorPath = if ($Mode -eq 'A') { $oracleRoot + '/bootstrap/data.json' } else { $oracleRoot + '/bootstrap/conventions.json' }
  if (-not (Test-ExactKeys $receipt $keys) -or $receipt.schema_version -ne 1 -or $receipt.transition -ne ('mode_' + $slug) -or $receipt.command_id -ne ('oracle_mode_' + $slug + '_aggregate') -or
      $receipt.exit_code -ne 0 -or [long]$receipt.rows_processed -le 0 -or -not (Test-StringSet $receipt.target_metric_ids $expectedTargets) -or -not (Test-Provenance $receipt $Sha $priorPath)) { return $false }
  if ($receipt.smoke_blob_oid -ne $DataReceipt.smoke_blob_oid) { return $false }
  return $Mode -eq 'A' -or $receipt.tune_blob_oid -eq $DataReceipt.tune_blob_oid
}

function Test-ConventionsReceipt([string]$Sha, $DataReceipt) {
  $receipt = Get-CommittedJson $Sha ($oracleRoot + '/bootstrap/conventions.json')
  $keys = @('schema_version', 'transition', 'base_sha', 'tested_sha', 'command_id', 'exit_code', 'smoke_blob_oid', 'tune_blob_oid', 'conventions_blob_oid', 'scorecard_blob_oid', 'conventions')
  if (-not (Test-ExactKeys $receipt $keys) -or $receipt.schema_version -ne 1 -or $receipt.transition -ne 'conventions' -or $receipt.command_id -ne 'oracle_conventions_smoke_tune' -or $receipt.exit_code -ne 0 -or
      -not (Test-Provenance $receipt $Sha ($oracleRoot + '/bootstrap/mode-a.json')) -or $receipt.smoke_blob_oid -ne $DataReceipt.smoke_blob_oid -or $receipt.tune_blob_oid -ne $DataReceipt.tune_blob_oid) { return $false }
  $map = $receipt.conventions
  if (-not (Test-ExactKeys $map @('theta_basis', 'vega_basis', 'rate_model', 'dividend_model', 'day_count', 'sign_model')) -or @('per_day', 'per_year') -notcontains $map.theta_basis -or
      @('per_vol_point', 'per_unit_vol') -notcontains $map.vega_basis -or @('continuous', 'simple') -notcontains $map.rate_model -or @('continuous_yield', 'discrete_cash') -notcontains $map.dividend_model -or
      @('ACT_365F', 'ACT_360', 'BUS_252') -notcontains $map.day_count -or $map.sign_model -ne 'spiderrock') { return $false }
  $conventionsPath = $oracleRoot + '/CONVENTIONS.md'; $scorecardPath = $oracleRoot + '/scorecards/iter-000.json'
  if ((Get-BlobOid $Sha $conventionsPath) -ne $receipt.conventions_blob_oid -or (Get-BlobOid $Sha $scorecardPath) -ne $receipt.scorecard_blob_oid) { return $false }
  $scorecard = Get-CommittedJson $Sha $scorecardPath
  if (-not (Test-ExactKeys $scorecard @('schema_version', 'kind', 'base_sha', 'tested_sha', 'command_id', 'exit_code', 'mode', 'cohorts', 'rows_processed', 'target_metric_ids')) -or $scorecard.schema_version -ne 1 -or
      $scorecard.kind -ne 'residual_floor' -or $scorecard.command_id -ne 'mode_a_residual_floor' -or $scorecard.exit_code -ne 0 -or $scorecard.mode -ne 'A' -or
      -not (Test-StringSet $scorecard.cohorts @('smoke', 'tune')) -or -not (Test-StringSet $scorecard.target_metric_ids $targetA) -or [long]$scorecard.rows_processed -le 0 -or
      -not (Test-Ancestor ([string]$scorecard.base_sha) ([string]$scorecard.tested_sha)) -or -not (Test-Ancestor ([string]$scorecard.tested_sha) $Sha)) { return $false }
  return $true
}

# Pester imports the closed validators without running command mode.
if ($MyInvocation.InvocationName -eq '.') { return }

$canonicalExists = $true
try { $baseSha = Resolve-Commit $canonicalRef } catch { $canonicalExists = $false; $baseSha = Resolve-Commit $baseRef }
$resolvedBaseRef = if ($canonicalExists) { $canonicalRef } else { $baseRef }
$holdoutDigest = ''
$dataValid = Test-DataReceipt $baseSha ([ref]$holdoutDigest)
$dataReceipt = if ($dataValid) { Get-CommittedJson $baseSha ($oracleRoot + '/bootstrap/data.json') } else { $null }
$modeAValid = $dataValid -and (Test-ModeReceipt $baseSha 'A' $dataReceipt)
$conventionsValid = $modeAValid -and (Test-ConventionsReceipt $baseSha $dataReceipt)
$modeBValid = $conventionsValid -and (Test-ModeReceipt $baseSha 'B' $dataReceipt)
if (-not $dataValid) { $state = 'missing_data' } elseif (-not $modeAValid) { $state = 'missing_mode_a' } elseif (-not $conventionsValid) { $state = 'missing_conventions' } elseif (-not $modeBValid) { $state = 'missing_mode_b' } else { $state = 'ready' }

$nextNumber = 0
if ($state -eq 'ready') {
  $names = (Invoke-GitText @('ls-tree', '-r', '--name-only', $baseSha, '--', ($oracleRoot + '/scorecards'))).Text -split "`n"
  foreach ($name in $names) { if ($name -match '/iter-([0-9]+)\.json$') { $nextNumber = [Math]::Max($nextNumber, [int]$Matches[1] + 1) } }
}
$probeOutput = 'state=' + $state + ' canonical_exists=' + $canonicalExists.ToString().ToLowerInvariant() + ' data_receipt_valid=' + $dataValid.ToString().ToLowerInvariant() +
  ' mode_a_receipt_valid=' + $modeAValid.ToString().ToLowerInvariant() + ' conventions_receipt_valid=' + $conventionsValid.ToString().ToLowerInvariant() + ' mode_b_receipt_valid=' + $modeBValid.ToString().ToLowerInvariant()
[ordered]@{
  state = $state; canonical_ref = $canonicalRef; canonical_exists = $canonicalExists; base_ref = $resolvedBaseRef; base_sha = $baseSha
  holdout_digest_receipt = $holdoutDigest; next_iter = 'iter-' + $nextNumber.ToString('000')
  evidence = @([ordered]@{ command = 'powershell scripts\oracle-capability.ps1'; exit_code = 0; output = $probeOutput })
} | ConvertTo-Json -Depth 5 -Compress
