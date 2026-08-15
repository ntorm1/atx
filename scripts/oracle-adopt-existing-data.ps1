# One-time, fail-closed Stage-1 adoption of an existing aggregate oracle store.
# The command reads committed cohort manifests internally and Parquet metadata
# only. Stdout is an aggregate typed result and never contains membership/rows.
$ErrorActionPreference = 'Stop'

$adoptionScriptRoot = $PSScriptRoot
$adoptionRepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$adoptionDataRoot = 'C:\atx-cache\oracle\spiderrock'
$adoptionOracleRoot = 'atx-vol/bench/oracle'
$adoptionMetadataTool = Join-Path $adoptionRepoRoot 'atx-vol/scripts/oracle_store_metadata.py'

. (Join-Path $PSScriptRoot 'oracle-capability.ps1')

function New-AdoptionResult([string]$Status, [string]$Reason, $Fields = $null) {
  $result = [ordered]@{ schema_version = 1; status = $Status; command_id = 'oracle_existing_store_adoption' }
  if ($Reason) { $result.reason = $Reason }
  if ($Fields) { foreach ($entry in $Fields.GetEnumerator()) { $result[$entry.Key] = $entry.Value } }
  return [pscustomobject]$result
}

function Invoke-OracleStoreMetadata([string]$ManifestPath, [string]$Commit) {
  $raw = @(& python $adoptionMetadataTool --data-root $adoptionDataRoot --manifest $ManifestPath `
    --repo-root $adoptionRepoRoot --commit $Commit 2>$null)
  if ($LASTEXITCODE -ne 0 -or @($raw).Count -ne 1) { return $null }
  try { return ([string]$raw[0]) | ConvertFrom-Json } catch { return $null }
}

function Remove-AdoptionFile([string]$Path) {
  if ([System.IO.File]::Exists($Path)) { [System.IO.File]::Delete($Path) }
}

function Restore-AdoptionTransaction($Transaction, [string]$JournalPath) {
  try {
    foreach ($name in @('digest', 'receipt')) {
      $target = [string]$Transaction.($name + '_target')
      $backup = [string]$Transaction.($name + '_backup')
      $prior = [bool]$Transaction.($name + '_prior')
      if ($prior) {
        if ([System.IO.File]::Exists($backup)) {
          Remove-AdoptionFile $target
          [System.IO.File]::Move($backup, $target)
        } elseif (-not [System.IO.File]::Exists($target)) { return $false }
      } else {
        Remove-AdoptionFile $target
        Remove-AdoptionFile $backup
      }
    }
    Remove-AdoptionFile ([string]$Transaction.digest_stage)
    Remove-AdoptionFile ([string]$Transaction.receipt_stage)
    Remove-AdoptionFile $JournalPath
    return $true
  } catch { return $false }
}

function Restore-PendingAdoptionTransaction {
  $journal = Join-Path $adoptionRepoRoot ($adoptionOracleRoot -replace '/', [System.IO.Path]::DirectorySeparatorChar)
  $journal = Join-Path $journal '.oracle-data-adoption.txn.json'
  if (-not [System.IO.File]::Exists($journal)) { return $true }
  try { $transaction = [System.IO.File]::ReadAllText($journal) | ConvertFrom-Json }
  catch { return $false }
  return Restore-AdoptionTransaction $transaction $journal
}

function Publish-AdoptionTransaction([string]$DigestPath, [string]$ReceiptPath, [string]$DigestText, [string]$ReceiptText) {
  $token = [guid]::NewGuid().ToString('N')
  New-Item -ItemType Directory -Force (Split-Path -Parent $DigestPath), (Split-Path -Parent $ReceiptPath) | Out-Null
  $journal = Join-Path $adoptionRepoRoot ($adoptionOracleRoot -replace '/', [System.IO.Path]::DirectorySeparatorChar)
  $journal = Join-Path $journal '.oracle-data-adoption.txn.json'
  $transaction = [ordered]@{
    schema_version = 1; token = $token
    digest_target = $DigestPath; digest_stage = $DigestPath + '.' + $token + '.stage'; digest_backup = $DigestPath + '.' + $token + '.backup'; digest_prior = [System.IO.File]::Exists($DigestPath)
    receipt_target = $ReceiptPath; receipt_stage = $ReceiptPath + '.' + $token + '.stage'; receipt_backup = $ReceiptPath + '.' + $token + '.backup'; receipt_prior = [System.IO.File]::Exists($ReceiptPath)
  }
  $journalStage = $journal + '.' + $token + '.stage'
  try {
    [System.IO.File]::WriteAllText($transaction.digest_stage, $DigestText, [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText($transaction.receipt_stage, $ReceiptText, [System.Text.UTF8Encoding]::new($false))
    $digest = [System.IO.File]::ReadAllText($transaction.digest_stage).Trim().ToLowerInvariant()
    $stagedReceipt = [System.IO.File]::ReadAllText($transaction.receipt_stage) | ConvertFrom-Json
    if ($digest -notmatch '^[0-9a-f]{64}$' -or $stagedReceipt.schema_version -ne 1 -or
        $stagedReceipt.command_id -ne 'oracle_existing_store_adoption' -or $stagedReceipt.exit_code -ne 0 -or
        $stagedReceipt.holdout_membership_sha256 -ne $digest) { throw 'staged adoption output validation failed' }

    [System.IO.File]::WriteAllText($journalStage, (($transaction | ConvertTo-Json -Compress) + "`n"), [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::Move($journalStage, $journal)
    if ($transaction.digest_prior) { [System.IO.File]::Copy($DigestPath, $transaction.digest_backup) }
    if ($transaction.receipt_prior) { [System.IO.File]::Copy($ReceiptPath, $transaction.receipt_backup) }
    Remove-AdoptionFile $DigestPath
    [System.IO.File]::Move($transaction.digest_stage, $DigestPath)
    if ($script:adoptionTestFailAfterDigest) { throw 'simulated second publication failure' }
    Remove-AdoptionFile $ReceiptPath
    [System.IO.File]::Move($transaction.receipt_stage, $ReceiptPath)
    if ([System.IO.File]::ReadAllText($DigestPath) -ne $DigestText -or [System.IO.File]::ReadAllText($ReceiptPath) -ne $ReceiptText) {
      throw 'published adoption output validation failed'
    }
    Remove-AdoptionFile $transaction.digest_backup
    Remove-AdoptionFile $transaction.receipt_backup
    Remove-AdoptionFile $journal
    return $true
  } catch {
    if ([System.IO.File]::Exists($journal)) {
      $null = Restore-AdoptionTransaction ([pscustomobject]$transaction) $journal
      return $false
    }
    Remove-AdoptionFile $transaction.digest_stage
    Remove-AdoptionFile $transaction.receipt_stage
    Remove-AdoptionFile $journalStage
    return $false
  }
}

function Invoke-OracleDataAdoptionCore {
  $headSha = Resolve-Commit 'HEAD'
  $cohorts = [ordered]@{}
  foreach ($name in @('smoke', 'tune', 'holdout')) {
    $cohort = Get-CommittedJson $headSha ($adoptionOracleRoot + '/cohorts/' + $name + '.json')
    if (-not (Test-CohortJson $cohort $name)) { return New-AdoptionResult 'INGEST_REQUIRED' ('cohort_schema_' + $name) }
    $cohorts[$name] = $cohort
  }
  if (-not (Test-Disjoint $cohorts.tune.underliers $cohorts.holdout.underliers) -or
      -not (Test-Disjoint $cohorts.tune.buckets_et $cohorts.holdout.buckets_et)) {
    return New-AdoptionResult 'INGEST_REQUIRED' 'cohort_disjointness'
  }

  $dates = @($cohorts.Values | ForEach-Object { @($_.dates) } | ForEach-Object { [string]$_ } | Select-Object -Unique)
  if ($dates.Count -ne 1) { return New-AdoptionResult 'INGEST_REQUIRED' 'cohort_dates' }
  $manifestName = 'oracle_manifest_' + $dates[0] + '.json'
  $manifestPath = Join-Path $adoptionDataRoot $manifestName
  if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { return New-AdoptionResult 'INGEST_REQUIRED' 'manifest_missing' }

  $metadata = Invoke-OracleStoreMetadata $manifestPath $headSha
  $metadataKeys = @('schema_version', 'status', 'manifest_sha256', 'total_rows', 'bucket_count', 'parquet_files', 'schema_sha256', 'cohort_underlier_count')
  if (-not $metadata -or -not (Test-ExactKeys $metadata $metadataKeys) -or $metadata.schema_version -ne 1 -or $metadata.status -ne 'PASS' -or
      $metadata.manifest_sha256 -notmatch '^[0-9a-f]{64}$' -or $metadata.schema_sha256 -notmatch '^[0-9a-f]{64}$' -or
      [long]$metadata.total_rows -le 0 -or [long]$metadata.bucket_count -le 0 -or [long]$metadata.parquet_files -le 0 -or [long]$metadata.cohort_underlier_count -le 0) {
    return New-AdoptionResult 'INGEST_REQUIRED' 'store_validation'
  }
  if (-not (Test-IngestManifest $manifestName ([string]$metadata.manifest_sha256))) {
    return New-AdoptionResult 'INGEST_REQUIRED' 'manifest_validation'
  }

  try { $manifest = [System.IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json }
  catch { return New-AdoptionResult 'INGEST_REQUIRED' 'manifest_validation' }
  $manifestBuckets = @($manifest.buckets.PSObject.Properties.Name)
  foreach ($cohort in $cohorts.Values) {
    if (@($cohort.dates | Where-Object { $_ -ne $manifest.trading_date }).Count -or
        @($cohort.buckets_et | Where-Object { $manifestBuckets -notcontains $_ }).Count) {
      return New-AdoptionResult 'INGEST_REQUIRED' 'cohort_store_coverage'
    }
  }

  $digest = Get-CohortMembershipDigest $cohorts.holdout
  if ($digest -notmatch '^[0-9a-f]{64}$') { return New-AdoptionResult 'INGEST_REQUIRED' 'holdout_digest' }
  $digestRel = $adoptionOracleRoot + '/cohorts/holdout.sha256'
  if (Test-CommittedPath $headSha $digestRel) {
    $committedDigest = (Invoke-GitText @('show', ($headSha + ':' + $digestRel))).Text.ToLowerInvariant()
    if ($committedDigest -ne $digest) { return New-AdoptionResult 'INGEST_REQUIRED' 'holdout_digest_mismatch' }
  }
  $digestPath = Join-Path $adoptionRepoRoot ($digestRel -replace '/', [System.IO.Path]::DirectorySeparatorChar)
  if (Test-Path -LiteralPath $digestPath -PathType Leaf) {
    if (([System.IO.File]::ReadAllText($digestPath).Trim().ToLowerInvariant()) -ne $digest) {
      return New-AdoptionResult 'INGEST_REQUIRED' 'holdout_digest_mismatch'
    }
  }

  $smokeOid = Get-BlobOid $headSha ($adoptionOracleRoot + '/cohorts/smoke.json')
  $tuneOid = Get-BlobOid $headSha ($adoptionOracleRoot + '/cohorts/tune.json')
  $holdoutOid = Get-BlobOid $headSha ($adoptionOracleRoot + '/cohorts/holdout.json')
  if (@(@($smokeOid, $tuneOid, $holdoutOid) | Where-Object { $_ -notmatch '^[0-9a-f]{40}$' }).Count) {
    return New-AdoptionResult 'INGEST_REQUIRED' 'cohort_blob'
  }
  $receipt = [ordered]@{
    schema_version = 1; transition = 'data'; base_sha = $headSha; tested_sha = $headSha
    command_id = 'oracle_existing_store_adoption'; exit_code = 0
    ingest_manifest_name = $manifestName; ingest_manifest_sha256 = [string]$metadata.manifest_sha256
    smoke_blob_oid = $smokeOid; tune_blob_oid = $tuneOid; holdout_blob_oid = $holdoutOid
    holdout_membership_sha256 = $digest
    smoke_schema_valid = $true; tune_schema_valid = $true; holdout_schema_valid = $true
    tune_holdout_underliers_disjoint = $true; tune_holdout_buckets_disjoint = $true
  }
  $receiptPath = Join-Path $adoptionRepoRoot (($adoptionOracleRoot + '/bootstrap/data.json') -replace '/', [System.IO.Path]::DirectorySeparatorChar)
  $digestText = $digest + "`n"
  $receiptText = ($receipt | ConvertTo-Json -Depth 4 -Compress) + "`n"
  if (-not (Publish-AdoptionTransaction $digestPath $receiptPath $digestText $receiptText)) {
    return New-AdoptionResult 'INGEST_REQUIRED' 'publication_transaction'
  }
  return New-AdoptionResult 'ADOPTED' '' ([ordered]@{
    base_sha = $headSha; manifest_sha256 = [string]$metadata.manifest_sha256; holdout_membership_sha256 = $digest
    total_rows = [long]$metadata.total_rows; bucket_count = [long]$metadata.bucket_count; parquet_files = [long]$metadata.parquet_files
    cohort_underlier_count = [long]$metadata.cohort_underlier_count
  })
}

function Invoke-OracleDataAdoption {
  try {
    if (-not (Restore-PendingAdoptionTransaction)) { return New-AdoptionResult 'INGEST_REQUIRED' 'publication_recovery' }
    return Invoke-OracleDataAdoptionCore
  } catch {
    return New-AdoptionResult 'INGEST_REQUIRED' 'validation_exception'
  }
}

if ($MyInvocation.InvocationName -eq '.') { return }
Invoke-OracleDataAdoption | ConvertTo-Json -Compress
