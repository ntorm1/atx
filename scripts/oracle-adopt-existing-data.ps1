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

function Write-AtomicUtf8([string]$Path, [string]$Text) {
  $parent = Split-Path -Parent $Path
  New-Item -ItemType Directory -Force $parent | Out-Null
  $temp = $Path + '.' + [guid]::NewGuid().ToString('N') + '.tmp'
  try {
    [System.IO.File]::WriteAllText($temp, $Text, [System.Text.UTF8Encoding]::new($false))
    Move-Item -LiteralPath $temp -Destination $Path -Force
  } finally {
    if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Force }
  }
}

function New-AdoptionResult([string]$Status, [string]$Reason, $Fields = $null) {
  $result = [ordered]@{ schema_version = 1; status = $Status; command_id = 'oracle_existing_store_adoption' }
  if ($Reason) { $result.reason = $Reason }
  if ($Fields) { foreach ($entry in $Fields.GetEnumerator()) { $result[$entry.Key] = $entry.Value } }
  return [pscustomobject]$result
}

function Invoke-OracleStoreMetadata([string]$ManifestPath) {
  $raw = @(& python $adoptionMetadataTool --data-root $adoptionDataRoot --manifest $ManifestPath 2>$null)
  if ($LASTEXITCODE -ne 0 -or @($raw).Count -ne 1) { return $null }
  try { return ([string]$raw[0]) | ConvertFrom-Json } catch { return $null }
}

function Invoke-OracleDataAdoption {
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

  $metadata = Invoke-OracleStoreMetadata $manifestPath
  $metadataKeys = @('schema_version', 'status', 'manifest_sha256', 'total_rows', 'bucket_count', 'parquet_files', 'schema_sha256')
  if (-not $metadata -or -not (Test-ExactKeys $metadata $metadataKeys) -or $metadata.schema_version -ne 1 -or $metadata.status -ne 'PASS' -or
      $metadata.manifest_sha256 -notmatch '^[0-9a-f]{64}$' -or $metadata.schema_sha256 -notmatch '^[0-9a-f]{64}$' -or
      [long]$metadata.total_rows -le 0 -or [long]$metadata.bucket_count -le 0 -or [long]$metadata.parquet_files -le 0) {
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
  Write-AtomicUtf8 $digestPath ($digest + "`n")
  Write-AtomicUtf8 $receiptPath (($receipt | ConvertTo-Json -Depth 4 -Compress) + "`n")
  return New-AdoptionResult 'ADOPTED' '' ([ordered]@{
    base_sha = $headSha; manifest_sha256 = [string]$metadata.manifest_sha256; holdout_membership_sha256 = $digest
    total_rows = [long]$metadata.total_rows; bucket_count = [long]$metadata.bucket_count; parquet_files = [long]$metadata.parquet_files
  })
}

if ($MyInvocation.InvocationName -eq '.') { return }
Invoke-OracleDataAdoption | ConvertTo-Json -Compress
