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
    trading_date='2026-08-14'; source_tsv_bytes=100; total_rows=9
    buckets=[ordered]@{ '1000'=3; '1330'=3; '1500'=3 }
    top_underliers_by_rows=[ordered]@{ SPY=3; QQQ=3; IWM=3 }
    ingested_at='2026-08-15T12:00:00Z'
  })
  if (-not $script:oracleFixtureParquet) {
    $python = @'
import datetime as dt
import importlib.util
import sys
from pathlib import Path
import pyarrow as pa
import pyarrow.parquet as pq
root=Path(sys.argv[1])
tool=Path(sys.argv[2])
spec=importlib.util.spec_from_file_location('oracle_store_metadata',tool)
module=importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
for bucket in ('1000','1330','1500'):
    path=root/'date=2026-08-14'/f'bucket_et={bucket}'/'data.parquet'
    path.parent.mkdir(parents=True,exist_ok=True)
    values={}
    for name,type_name in module.REQUIRED_SCHEMA.items():
        if type_name == 'large_string':
            values[name]=pa.array(['x','x','x'],type=pa.large_string())
        elif type_name == 'double':
            values[name]=pa.array([1.0,1.0,1.0],type=pa.float64())
        elif type_name == 'int64':
            values[name]=pa.array([1,1,1],type=pa.int64())
        elif type_name == 'timestamp[us]':
            values[name]=pa.array([dt.datetime(2026,8,14)]*3,type=pa.timestamp('us'))
    values['undSecKey_tk']=pa.array(['SPY','QQQ','IWM'],type=pa.large_string())
    values['tradingDate']=pa.array(['2026-08-14']*3,type=pa.large_string())
    values['bucket_et']=pa.array([bucket]*3,type=pa.large_string())
    values['okey_cp']=pa.array(['C','P','C'],type=pa.large_string())
    pq.write_table(pa.table(values),path)
'@
    & python -c $python $data $adoptionMetadataTool
    if ($LASTEXITCODE -ne 0) { throw 'synthetic parquet creation failed' }
    $script:oracleFixtureParquet = @{}
    foreach ($bucket in @('1000','1330','1500')) {
      $path = Join-Path $data ('date=2026-08-14\bucket_et=' + $bucket + '\data.parquet')
      $script:oracleFixtureParquet[$bucket] = [System.IO.File]::ReadAllBytes($path)
    }
  } else {
    foreach ($bucket in @('1000','1330','1500')) {
      $path = Join-Path $data ('date=2026-08-14\bucket_et=' + $bucket + '\data.parquet')
      New-Item -ItemType Directory -Force (Split-Path -Parent $path) | Out-Null
      [System.IO.File]::WriteAllBytes($path, $script:oracleFixtureParquet[$bucket])
    }
  }
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
    $result.total_rows | Should Be 9
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
    $manifest.total_rows = 10
    $manifest.buckets.'1000' = 4
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

  It 'rejects a uniform arbitrary schema even when every required field name is present' {
    $case = New-TestOracleCase 'uniform-schema'
    $python = @'
import importlib.util
import sys
from pathlib import Path
import pyarrow as pa
import pyarrow.parquet as pq
root=Path(sys.argv[1]); tool=Path(sys.argv[2])
spec=importlib.util.spec_from_file_location('oracle_store_metadata',tool)
module=importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
for path in root.glob('date=*/bucket_et=*/*.parquet'):
    pq.write_table(pa.table({name:pa.array([1,1,1],type=pa.int64()) for name in module.REQUIRED_SCHEMA}),path)
'@
    & python -c $python $case.Data $adoptionMetadataTool
    if ($LASTEXITCODE -ne 0) { throw 'uniform-schema fixture failed' }
    Use-TestOracleCase $case
    (Invoke-OracleDataAdoption).status | Should Be 'INGEST_REQUIRED'
    (Test-Path -LiteralPath (Join-Path $case.Repo 'atx-vol/bench/oracle/bootstrap/data.json')) | Should Be $false
  }

  It 'requires ingest when a committed cohort underlier is absent from the aggregate store' {
    $case = New-TestOracleCase 'underlier-missing'
    Write-TestJson (Join-Path $case.CohortRoot 'holdout.json') ([ordered]@{ name='holdout'; dates=@('2026-08-14'); underliers=@('DIA'); buckets_et=@('1500'); notes='absent key' })
    $case.Base = Commit-TestRepo $case.Repo 'absent cohort underlier'
    Use-TestOracleCase $case
    (Invoke-OracleDataAdoption).status | Should Be 'INGEST_REQUIRED'
    (Test-Path -LiteralPath (Join-Path $case.Repo 'atx-vol/bench/oracle/bootstrap/data.json')) | Should Be $false
  }

  It 'rolls back both outputs byte-exactly when the second publication fails' {
    $case = New-TestOracleCase 'rollback'
    Use-TestOracleCase $case
    $digestPath = Join-Path $case.CohortRoot 'holdout.sha256'
    $receiptPath = Join-Path $case.Repo 'atx-vol/bench/oracle/bootstrap/data.json'
    New-Item -ItemType Directory -Force (Split-Path -Parent $receiptPath) | Out-Null
    $oldDigest = "prior-digest-bytes`r`n"
    $oldReceipt = "prior-receipt-bytes`n"
    [System.IO.File]::WriteAllText($digestPath, $oldDigest, [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText($receiptPath, $oldReceipt, [System.Text.UTF8Encoding]::new($false))
    $newDigest = ('a' * 64) + "`n"
    $newReceipt = ([ordered]@{ schema_version=1; command_id='oracle_existing_store_adoption'; exit_code=0; holdout_membership_sha256=('a' * 64) } | ConvertTo-Json -Compress) + "`n"
    $script:adoptionTestFailAfterDigest = $true
    try { (Publish-AdoptionTransaction $digestPath $receiptPath $newDigest $newReceipt) | Should Be $false }
    finally { $script:adoptionTestFailAfterDigest = $false }
    [System.IO.File]::ReadAllText($digestPath) | Should Be $oldDigest
    [System.IO.File]::ReadAllText($receiptPath) | Should Be $oldReceipt
    @(Get-ChildItem -LiteralPath (Join-Path $case.Repo 'atx-vol/bench/oracle') -Recurse -File | Where-Object { $_.Name -match '\.(?:stage|backup)$|\.txn\.json$' }).Count | Should Be 0
  }
}
