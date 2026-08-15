# Exact stage-1 verifier targets used by vol-oracle-iter.
param(
  [Parameter(Mandatory = $true)]
  [ValidateSet('disk', 'ingest_manifest', 'cohort_manifests', 'holdout_digest')]
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
  $observations = 1
  $rawEvidence = $headSha + ':' + $digest
}

$bytes = [System.Text.UTF8Encoding]::new($false).GetBytes($rawEvidence)
try {
  $sha = [System.Security.Cryptography.SHA256]::Create()
  $rawOutputSha256 = ([System.BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
} finally { if ($sha) { $sha.Dispose() } }
[ordered]@{ schema_version = 1; status = 'PASS'; observations = $observations; command_id = $Gate; raw_output_sha256 = $rawOutputSha256 } |
  ConvertTo-Json -Compress
