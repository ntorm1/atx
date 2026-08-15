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
} else {
  . (Join-Path $PSScriptRoot 'oracle-capability.ps1')
  $headSha = Resolve-Commit 'HEAD'
  $digest = ''
  if (-not (Test-DataReceipt $headSha ([ref]$digest))) { throw ($Gate + ' gate failed closed aggregate data receipt validation') }
  $observations = 1
}

[ordered]@{ schema_version = 1; status = 'PASS'; observations = $observations } |
  ConvertTo-Json -Compress
