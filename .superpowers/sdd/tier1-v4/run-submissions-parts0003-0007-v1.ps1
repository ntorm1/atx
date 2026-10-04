# Root controller: five sequential bounded native commands, each with its own guard.
# A failure stops the chain. Completed partitions persist; no source gate is inferred.
$ErrorActionPreference = 'Stop'
if ((Get-Location).Path -ne 'C:\atx\atx-db') { throw 'Run from the explicit native workspace' }
$controllerRoot = 'C:\atx\.superpowers\sdd\tier1-v4'
$prior = Get-Content -LiteralPath (Join-Path $controllerRoot 'task11-submissions-progress-v1.json') -Raw | ConvertFrom-Json
if ($prior.status -ne 'BOUNDED_INVENTORY_CHAIN_BYTES_AND_EXECUTION_VERIFIED' -or $prior.last_main_member -ne 'CIK0000900817.json' -or $prior.counts.main_members -ne 30000 -or $prior.terminal_exhaustion) { throw 'Unexpected prior bounded chain' }
$inventoryCursor = 'CIK0000900817.json'
$env:PYTHONPATH = 'C:\atx\atx-db\src'
foreach ($partitionNumber in 3..7) {
    $tag = 'part{0:D4}-v1' -f $partitionNumber
    $outputFile = (Join-Path $controllerRoot ('task11-submissions-inventory-'+$tag+'.jsonl')).Replace('\','/')
    $nativeReceipt = $outputFile + '.receipt.json'
    $guardReceipt = Join-Path $controllerRoot ('logs\t11-submissions-inventory-'+$tag+'.receipt.json')
    $stdoutPath = Join-Path $controllerRoot ('logs\t11-submissions-inventory-'+$tag+'.out.log')
    $stderrPath = Join-Path $controllerRoot ('logs\t11-submissions-inventory-'+$tag+'.err.log')
    foreach ($candidate in @($outputFile,$nativeReceipt,$guardReceipt,$stdoutPath,$stderrPath)) {
        if (Test-Path -LiteralPath $candidate) { throw 'Preserve existing bounded partition evidence' }
    }
    $nativeArgs = @('.venv/Scripts/python.exe','-m','atx_db.alpha_panel.thirteenf_quality','inventory-submissions',
        '--archive','C:/atx/atx-db/data/cache/submissions.zip','--after',$inventoryCursor,
        '--through','CIK9999999999.json','--max-main','10000','--first','2019-01-01','--output',$outputFile)
    & '.venv/Scripts/python.exe' '../.superpowers/sdd/tier1-parity/run_memory_guarded.py' --job-gb 0.2 --wait-minutes 10 --quiet --receipt $guardReceipt --stdout $stdoutPath --stderr $stderrPath -- @nativeArgs
    $guardResult = Get-Content -LiteralPath $guardReceipt -Raw | ConvertFrom-Json
    if ($guardResult.status -ne 'failed' -or $guardResult.returncode -ne 2 -or $guardResult.cap_hit -or $guardResult.stop_reason) { throw 'Native command did not complete intentional unqualified exit2; chain stopped' }
    $partition = Get-Content -LiteralPath $nativeReceipt -Raw | ConvertFrom-Json
    if ($partition.schema -ne 'atx.thirteenf-submissions-inventory-partition/v1' -or
        $partition.source_completeness -ne 'UNQUALIFIED_PARTITION' -or
        $partition.after -ne $inventoryCursor -or $partition.through -ne 'CIK9999999999.json' -or
        $partition.archive_sha256 -ne '702fbcd8b4335bc649e9e4eab3a202f3effc314b43421664bfecb59365767165' -or
        $partition.code_sha256.thirteenf_quality -ne '02a04d7e4dfe8221cb0d65c076ecd506fa049ff2dc3e75628aee6702e7dbd5c5' -or
        $partition.code_sha256.submissions_archive -ne '12d48c0334fa0013726200967809f9fb0cb2eee75113debd8cd78d0eba89f09f' -or
        $partition.first_filing_date -ne '2019-01-01' -or
        $partition.counts.main_members -lt 1 -or $partition.counts.main_members -gt 10000 -or
        $partition.last_main_member -le $inventoryCursor) { throw 'Bounded partition contract differs; chain stopped' }
    [pscustomobject]@{status='COMPLETED_UNQUALIFIED_NATIVE_PARTITION';part=$partitionNumber;after=$inventoryCursor;last=$partition.last_main_member;counts=$partition.counts;exceptions=$partition.exceptions.Count;run_seconds=$guardResult.run_seconds;peak_bytes=$guardResult.native_peak_job_memory_bytes} | ConvertTo-Json -Compress
    $inventoryCursor = $partition.last_main_member
    if (-not $partition.bound_reached) { break }
}
