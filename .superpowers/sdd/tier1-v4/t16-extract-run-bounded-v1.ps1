param([Parameter(Mandatory=$true)][int]$FirstBatch, [Parameter(Mandatory=$true)][int]$LastBatch, [int]$Attempt=1)
$ErrorActionPreference = 'Stop'
if ($FirstBatch -lt 1 -or $LastBatch -gt 84 -or $LastBatch -lt $FirstBatch -or ($LastBatch-$FirstBatch+1) -gt 8) { throw 'One bounded serial chunk of at most eight remaining batches required' }
if ($Attempt -lt 1 -or $Attempt -gt 10 -or ($Attempt -gt 1 -and $FirstBatch -ne $LastBatch)) { throw 'A retry requires one explicitly inspected batch' }
$work = 'C:\atx\.superpowers\sdd\tier1-v4'
$python = '.venv/Scripts/python.exe'
$guard = '../.superpowers/sdd/tier1-parity/run_memory_guarded.py'
$env:PYTHONPATH = 'C:/atx/atx-db/src'
$env:OPENBLAS_NUM_THREADS = '1'
if ((Get-Location).Path -ne 'C:\atx\atx-db') { throw 'Unexpected working directory' }
for ($batchNumber=$FirstBatch; $batchNumber -le $LastBatch; $batchNumber++) {
    $batchLabel = '{0:d4}' -f $batchNumber
    $run = 't16-extract-r19-b' + $batchLabel + '-v' + $Attempt
    $auditRun = 't16-extract-r19-b' + $batchLabel + '-audit-v1'
    $receiptPath = 'C:/atx/atx-db/data/alpha_panel/v1/fund_extract_validation_v1/r19_20261004_001/batch-' + $batchLabel + '.json'
    $guardReceipt = Join-Path $work ('logs/' + $run + '.receipt.json')
    $auditReceipt = Join-Path $work ('logs/' + $auditRun + '.receipt.json')
    if ($Attempt -gt 1) {
        $previousPath = Join-Path $work ('logs/t16-extract-r19-b' + $batchLabel + '-v' + ($Attempt-1) + '.receipt.json')
        $previous = Get-Content -LiteralPath $previousPath -Raw | ConvertFrom-Json
        if ($previous.status -ne 'stopped_low_commit' -or $previous.returncode -ne 137 -or $previous.cap_hit) { throw 'Prior attempt needs a separate cause-specific recovery' }
        foreach ($oldPid in @($previous.guard_pid, $previous.child_pid)) {
            if (Get-Process -Id $oldPid -ErrorAction SilentlyContinue) { throw 'Prior PID is still present; inspect before retry' }
        }
    }
    if ((Test-Path -LiteralPath $receiptPath) -or (Test-Path -LiteralPath $guardReceipt) -or (Test-Path -LiteralPath $auditReceipt)) { throw 'Existing attempt/receipt requires separate inspected continuation' }
    & $python $guard --job-gb 0.8 --wait-minutes 10 --quiet --receipt $guardReceipt --stdout (Join-Path $work ('logs/' + $run + '.out.log')) --stderr (Join-Path $work ('logs/' + $run + '.err.log')) -- $python -m atx_db.alpha_panel.fund_batch_release validate-extract --bindings C:/atx/.superpowers/sdd/tier1-v4/task16-fund-reader-binding-v1.json --batch $batchNumber --receipt $receiptPath
    if ($LASTEXITCODE -ne 0) { throw ('Native guarded batch ended without success: ' + $batchLabel) }
    & $python $guard --job-gb 0.3 --wait-minutes 10 --quiet --receipt $auditReceipt --stdout (Join-Path $work ('logs/' + $auditRun + '.out.log')) --stderr (Join-Path $work ('logs/' + $auditRun + '.err.log')) -- $python ../.superpowers/sdd/tier1-v4/t16-extract-receipt-audit-v1.py --batch $batchNumber --run $run
    if ($LASTEXITCODE -ne 0) { throw ('Independent receipt audit failed: ' + $batchLabel) }
    $evidence = Get-Content -LiteralPath (Join-Path $work ('logs/t16-extract-r19-b' + $batchLabel + '-admission-v1.json')) -Raw | ConvertFrom-Json
    if ($evidence.status -ne 'ONE_GUARDED_SOURCE_REPLAY_INDEPENDENTLY_PINNED' -or $evidence.batch -ne $batchNumber) { throw 'Unexpected audit completion' }
    [pscustomobject]@{ batch=$batchNumber; status=$evidence.status; rows=$evidence.rows; native_seconds=$evidence.native_run_seconds; peak_bytes=$evidence.native_peak_bytes; receipt_sha256=$evidence.native_receipt.identity.sha256 } | ConvertTo-Json -Compress
}
