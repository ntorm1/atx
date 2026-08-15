$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$sourceRoot = Split-Path -Parent (Split-Path -Parent $here)
. (Join-Path $sourceRoot 'scripts/oracle-adopt-existing-data.ps1')

function Write-TestJson([string]$Path, $Value) {
  New-Item -ItemType Directory -Force (Split-Path -Parent $Path) | Out-Null
  [System.IO.File]::WriteAllText($Path, (($Value | ConvertTo-Json -Depth 8) + "`n"), [System.Text.UTF8Encoding]::new($false))
}

function Commit-TestRepo([string]$Repository, [string]$Message) {
  git -C $Repository add -- . | Out-Null
  git -C $Repository commit -m $Message --quiet
  return (git -C $Repository rev-parse HEAD).Trim()
}

function New-TestOracleCase([string]$Name) {
  $caseRoot = Join-Path $TestDrive $Name
  $repo = Join-Path $caseRoot 'repo'
  $data = Join-Path $caseRoot 'data'
  New-Item -ItemType Directory -Force $repo, $data | Out-Null
  git -C $repo init --quiet
  git -C $repo config user.email 'adoption@example.invalid'
  git -C $repo config user.name 'Adoption Test'
  $cohortRoot = Join-Path $repo 'atx-vol/bench/oracle/cohorts'
  Write-TestJson (Join-Path $cohortRoot 'smoke.json') ([ordered]@{ name='smoke'; dates=@('2026-08-14'); underliers=@('SPY'); buckets_et=@('1000'); notes='smoke' })
  Write-TestJson (Join-Path $cohortRoot 'tune.json') ([ordered]@{ name='tune'; dates=@('2026-08-14'); underliers=@('QQQ'); buckets_et=@('1330'); notes='tune' })
  Write-TestJson (Join-Path $cohortRoot 'holdout.json') ([ordered]@{ name='holdout'; dates=@('2026-08-14'); underliers=@('IWM'); buckets_et=@('1500'); notes='holdout secret' })
  $base = Commit-TestRepo $repo 'committed cohorts'
  $manifestPath = Join-Path $data 'oracle_manifest_2026-08-14.json'
  Write-TestJson $manifestPath ([ordered]@{
    trading_date='2026-08-14'; source_tsv_bytes=100; total_rows=6
    buckets=[ordered]@{ '1000'=2; '1330'=2; '1500'=2 }
    top_underliers_by_rows=[ordered]@{ SPY=2; QQQ=2; IWM=2 }
    ingested_at='2026-08-15T12:00:00Z'
  })
  $python = @'
import sys
from pathlib import Path
import pyarrow as pa
import pyarrow.parquet as pq
root=Path(sys.argv[1])
for bucket in ('1000','1330','1500'):
    path=root/'date=2026-08-14'/f'bucket_et={bucket}'/'data.parquet'
    path.parent.mkdir(parents=True,exist_ok=True)
    pq.write_table(pa.table({'x':pa.array([1,2],type=pa.int64())}),path)
'@
  & python -c $python $data
  if ($LASTEXITCODE -ne 0) { throw 'synthetic parquet creation failed' }
  return [pscustomobject]@{ Repo=$repo; Data=$data; Base=$base; CohortRoot=$cohortRoot; Manifest=$manifestPath }
}

function Use-TestOracleCase($Case) {
  $script:repoRoot = $Case.Repo
  $script:dataRoot = $Case.Data
  $script:oracleRoot = 'atx-vol/bench/oracle'
  $script:adoptionRepoRoot = $Case.Repo
  $script:adoptionDataRoot = $Case.Data
  $script:adoptionOracleRoot = 'atx-vol/bench/oracle'
}

Describe 'oracle existing-store receipt adoption' {
  It 'adopts valid aggregate metadata with exact provenance and no membership output or iteration increment' {
    $case = New-TestOracleCase 'valid'
    Use-TestOracleCase $case
    $result = Invoke-OracleDataAdoption
    ($result.status + ':' + [string]$result.reason) | Should Be 'ADOPTED:'
    $result.base_sha | Should Be $case.Base
    $result.total_rows | Should Be 6
    $serialized = $result | ConvertTo-Json -Compress
    foreach ($secret in @('SPY','QQQ','IWM','holdout secret')) { $serialized.Contains($secret) | Should Be $false }

    $receiptPath = Join-Path $case.Repo 'atx-vol/bench/oracle/bootstrap/data.json'
    $digestPath = Join-Path $case.CohortRoot 'holdout.sha256'
    (Test-Path -LiteralPath $receiptPath) | Should Be $true
    (Test-Path -LiteralPath $digestPath) | Should Be $true
    $receipt = [System.IO.File]::ReadAllText($receiptPath) | ConvertFrom-Json
    (Test-ExactKeys $receipt @('schema_version','transition','base_sha','tested_sha','command_id','exit_code','ingest_manifest_name','ingest_manifest_sha256','smoke_blob_oid','tune_blob_oid','holdout_blob_oid','holdout_membership_sha256','smoke_schema_valid','tune_schema_valid','holdout_schema_valid','tune_holdout_underliers_disjoint','tune_holdout_buckets_disjoint')) | Should Be $true
    $receipt.base_sha | Should Be $case.Base
    $receipt.tested_sha | Should Be $case.Base
    $receipt.command_id | Should Be 'oracle_existing_store_adoption'
    $receipt.smoke_blob_oid | Should Be (Get-BlobOid $case.Base 'atx-vol/bench/oracle/cohorts/smoke.json')
    $receipt.tune_blob_oid | Should Be (Get-BlobOid $case.Base 'atx-vol/bench/oracle/cohorts/tune.json')
    $receipt.holdout_blob_oid | Should Be (Get-BlobOid $case.Base 'atx-vol/bench/oracle/cohorts/holdout.json')
    $receipt.ingest_manifest_sha256 | Should Be (Get-FileHash -LiteralPath $case.Manifest -Algorithm SHA256).Hash.ToLowerInvariant()
    $receipt.holdout_membership_sha256 | Should Be ([System.IO.File]::ReadAllText($digestPath).Trim())
    @(Get-ChildItem -LiteralPath (Join-Path $case.Repo 'atx-vol/bench/oracle') -Recurse -Filter 'iter-*.json' -ErrorAction SilentlyContinue).Count | Should Be 0

    $adopted = Commit-TestRepo $case.Repo 'adopt receipt'
    $digest = ''
    (Test-DataReceipt $adopted ([ref]$digest)) | Should Be $true
    $digest | Should Be $receipt.holdout_membership_sha256
  }

  It 'requires ingest on cohort overlap without writing receipts' {
    $case = New-TestOracleCase 'overlap'
    Write-TestJson (Join-Path $case.CohortRoot 'holdout.json') ([ordered]@{ name='holdout'; dates=@('2026-08-14'); underliers=@('QQQ'); buckets_et=@('1500'); notes='overlap' })
    $case.Base = Commit-TestRepo $case.Repo 'overlap cohort'
    Use-TestOracleCase $case
    (Invoke-OracleDataAdoption).status | Should Be 'INGEST_REQUIRED'
    (Test-Path -LiteralPath (Join-Path $case.Repo 'atx-vol/bench/oracle/bootstrap/data.json')) | Should Be $false
  }

  It 'requires ingest when Parquet footer counts disagree with the manifest' {
    $case = New-TestOracleCase 'counts'
    $manifest = [System.IO.File]::ReadAllText($case.Manifest) | ConvertFrom-Json
    $manifest.total_rows = 7
    $manifest.buckets.'1000' = 3
    Write-TestJson $case.Manifest $manifest
    Use-TestOracleCase $case
    (Invoke-OracleDataAdoption).status | Should Be 'INGEST_REQUIRED'
    (Test-Path -LiteralPath (Join-Path $case.Repo 'atx-vol/bench/oracle/bootstrap/data.json')) | Should Be $false
  }

  It 'requires ingest and preserves a conflicting frozen digest' {
    $case = New-TestOracleCase 'digest'
    $digestPath = Join-Path $case.CohortRoot 'holdout.sha256'
    [System.IO.File]::WriteAllText($digestPath, (('e' * 64) + "`n"), [System.Text.Encoding]::ASCII)
    Use-TestOracleCase $case
    (Invoke-OracleDataAdoption).status | Should Be 'INGEST_REQUIRED'
    ([System.IO.File]::ReadAllText($digestPath).Trim()) | Should Be ('e' * 64)
    (Test-Path -LiteralPath (Join-Path $case.Repo 'atx-vol/bench/oracle/bootstrap/data.json')) | Should Be $false
  }
}
