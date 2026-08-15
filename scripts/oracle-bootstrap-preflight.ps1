# Exact stage-1 verifier targets used by vol-oracle-iter.
param(
  [Parameter(Mandatory = $true)]
  [ValidateSet('disk', 'aggregate_store', 'ingest_manifest', 'cohort_manifests', 'holdout_digest')]
  [string]$Gate
)
$ErrorActionPreference = 'Stop'

if ($Gate -eq 'disk') {
  $root = [System.IO.Path]::GetPathRoot((Resolve-Path $PSScriptRoot).Path)
  $drive = New-Object System.IO.DriveInfo($root)
  if ($drive.AvailableFreeSpace -lt 15GB) { throw 'disk gate requires at least 15 GiB free' }
  $observations = [int][Math]::Max(1, [Math]::Floor($drive.AvailableFreeSpace / 1GB))
  $rawEvidence = [string]$drive.AvailableFreeSpace
} else {
  . (Join-Path $PSScriptRoot 'oracle-capability.ps1')
  $headSha = Resolve-Commit 'HEAD'
  $digest = ''
  if (-not (Test-DataReceipt $headSha ([ref]$digest))) { throw ($Gate + ' gate failed closed aggregate data receipt validation') }
  if ($Gate -eq 'aggregate_store') {
    $receipt = Get-CommittedJson $headSha ($oracleRoot + '/bootstrap/data.json')
    $tool = Join-Path $repoRoot 'atx-vol/scripts/oracle_store_metadata.py'
    $manifestPath = Join-Path $dataRoot ([string]$receipt.ingest_manifest_name)
    $raw = @(& python $tool --data-root $dataRoot --manifest $manifestPath 2>$null)
    if ($LASTEXITCODE -ne 0 -or @($raw).Count -ne 1) { throw 'aggregate_store gate failed closed metadata invocation' }
    try { $metadata = ([string]$raw[0]) | ConvertFrom-Json } catch { throw 'aggregate_store gate failed closed metadata receipt' }
    if (-not (Test-ExactKeys $metadata @('schema_version', 'status', 'manifest_sha256', 'total_rows', 'bucket_count', 'parquet_files', 'schema_sha256')) -or
        $metadata.schema_version -ne 1 -or $metadata.status -ne 'PASS' -or $metadata.manifest_sha256 -ne $receipt.ingest_manifest_sha256 -or
        [long]$metadata.total_rows -le 0 -or [long]$metadata.bucket_count -le 0 -or [long]$metadata.parquet_files -le 0 -or
        $metadata.schema_sha256 -notmatch '^[0-9a-f]{64}$') { throw 'aggregate_store gate failed closed metadata validation' }
    $rawEvidence = $headSha + ':' + $digest + ':' + $metadata.manifest_sha256 + ':' + $metadata.total_rows
  } else {
    $rawEvidence = $headSha + ':' + $digest
  }
  $observations = 1
}

$bytes = [System.Text.UTF8Encoding]::new($false).GetBytes($rawEvidence)
try {
  $sha = [System.Security.Cryptography.SHA256]::Create()
  $rawOutputSha256 = ([System.BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
} finally { if ($sha) { $sha.Dispose() } }
[ordered]@{ schema_version = 1; status = 'PASS'; observations = $observations; command_id = $Gate; raw_output_sha256 = $rawOutputSha256 } |
  ConvertTo-Json -Compress
