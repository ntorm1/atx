$ErrorActionPreference = 'Stop'
$workspace = 'C:\atx\.superpowers\sdd\tier1-v4'
$auditScript = Join-Path $workspace 't17-financial-unit-create-v1.py'
$auditSha = (Get-FileHash -LiteralPath $auditScript -Algorithm SHA256).Hash.ToLowerInvariant()
$auditBytes = (Get-Item -LiteralPath $auditScript).Length
$env:PYTHONPATH = 'C:/atx/atx-db/src'
$env:OPENBLAS_NUM_THREADS = '1'
# Root must first bind successful audit7 and creator8 receipts. This controller
# schedules only9..15, stops on the first failed attempt, and never retries.
if ((Get-Location).Path -cne 'C:\atx\atx-db') { throw 'Fixed working directory required' }
foreach ($bid in 9..15) {
    $tag = 't17-financial-native-create-b{0:D4}-v1' -f $bid
    $receipt = Join-Path $workspace ('logs\' + $tag + '.receipt.json')
    $stdout = Join-Path $workspace ('logs\' + $tag + '.out.log')
    $stderr = Join-Path $workspace ('logs\' + $tag + '.err.log')
    $validation = Join-Path $workspace ($tag + '.json')
    $startPath = Join-Path $workspace ('logs\' + $tag + '.start.json')
    foreach ($path in @($receipt,$stdout,$stderr,$validation,$startPath)) {
        if (Test-Path -LiteralPath $path) { throw 'Preserve prior exact batch attempt' }
    }
    & .venv/Scripts/python.exe C:/atx/.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.8 --heavy --disk-path C:/atx/atx-db/data --min-free-disk-gb 40 --wait-minutes 10 --receipt $receipt --stdout $stdout --stderr $stderr --quiet -- .venv/Scripts/python.exe C:/atx/.superpowers/sdd/tier1-v4/t17-financial-unit-create-v1.py --batch $bid --attempt-tag v1
    if ($LASTEXITCODE -ne 0) { throw ('Guarded batch creation failed: ' + $bid) }
    $guard = Get-Content -LiteralPath $receipt -Raw | ConvertFrom-Json
    $result = Get-Content -LiteralPath $validation -Raw | ConvertFrom-Json
    foreach ($field in @('guard_version','command','receipt','job_limit_gb','job_cap_bytes','heavy','status','returncode','cap_hit','disk','thresholds')) {
        if ($field -cnotin $guard.PSObject.Properties.Name) { throw ('Missing mandatory guard field: ' + $field) }
    }
    $expectedCommand = @('.venv/Scripts/python.exe','C:/atx/.superpowers/sdd/tier1-v4/t17-financial-unit-create-v1.py','--batch',[string]$bid,'--attempt-tag','v1')
    if ($guard.command -isnot [array] -or ($guard.command | ConvertTo-Json -Compress) -cne ($expectedCommand | ConvertTo-Json -Compress)) { throw 'Exact reviewed native creation command required' }
    if ($guard.guard_version -cne 'c58-m0-r1' -or $guard.receipt -cne $receipt -or $guard.status -cne 'completed' -or -not ($guard.returncode -is [int] -or $guard.returncode -is [long]) -or $guard.returncode -ne 0 -or $guard.cap_hit -isnot [bool] -or $guard.cap_hit -or $guard.stop_reason) { throw 'Exact successful guard identity and typed terminal fields required' }
    if (-not ($guard.job_limit_gb -is [double] -or $guard.job_limit_gb -is [decimal]) -or $guard.job_limit_gb -ne 0.8 -or $guard.job_cap_bytes -ne 858992640 -or $guard.heavy -isnot [bool] -or -not $guard.heavy -or $guard.disk.path -cne 'C:\atx\atx-db\data' -or $guard.disk.min_free_gb -ne 40 -or $guard.thresholds.test_override -isnot [bool] -or $guard.thresholds.test_override -or $guard.thresholds.test_slot_dir -isnot [bool] -or $guard.thresholds.test_slot_dir) { throw 'Original guard resource contract required' }
    foreach ($field in @('batch','status','external_guard_receipt','successful_external_guard_required','new_financial_units_created','publication','source_quality_certified','gate_promotion','controller_code','start','observed_frontier','prior_units_replayed_or_newly_qualified')) {
        if ($field -cnotin $result.PSObject.Properties.Name) { throw ('Missing mandatory result field: ' + $field) }
    }
    if (-not ($result.batch -is [int] -or $result.batch -is [long]) -or $result.batch -ne $bid -or $result.status -cne 'ONE_NATIVE_FINANCIAL_UNIT_DOUBLE_REPLAY_ADMITTED' -or $result.external_guard_receipt -cne $receipt -or $result.successful_external_guard_required -isnot [bool] -or -not $result.successful_external_guard_required -or -not ($result.new_financial_units_created -is [int] -or $result.new_financial_units_created -is [long]) -or $result.new_financial_units_created -ne 1) { throw 'Exact native creation outcome mismatch' }
    foreach ($field in @('publication','source_quality_certified','gate_promotion')) {
        if ($result.$field -isnot [bool] -or $result.$field) { throw ('Non-promoting result required: ' + $field) }
    }
    if ($result.prior_units_replayed_or_newly_qualified -isnot [bool] -or $result.prior_units_replayed_or_newly_qualified) { throw 'Prior units must receive no new semantic credit' }
    if ($result.observed_frontier -isnot [array] -or ($result.observed_frontier | ConvertTo-Json -Compress) -cne (@(0..$bid) | ConvertTo-Json -Compress)) { throw 'Exact next-unit frontier required' }
    $start = Get-Content -LiteralPath $startPath -Raw | ConvertFrom-Json
    $startSha = (Get-FileHash -LiteralPath $startPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $startBytes = (Get-Item -LiteralPath $startPath).Length
    if ($result.start.path -cne $startPath -or $result.start.sha256 -cne $startSha -or $result.start.bytes -ne $startBytes -or $start.batch -ne $bid -or $start.status -cne 'ONE_MISSING_NATIVE_UNIT_STARTED' -or $start.controller_code.sha256 -cne $auditSha -or $start.controller_code.bytes -ne $auditBytes) { throw 'Exact creator start identity required' }
    if ($result.controller_code.sha256 -cne $auditSha -or $result.controller_code.bytes -ne $auditBytes -or (Get-FileHash -LiteralPath $auditScript -Algorithm SHA256).Hash.ToLowerInvariant() -cne $auditSha -or (Get-Item -LiteralPath $auditScript).Length -ne $auditBytes) { throw 'Reviewed audit controller bytes changed' }
    [pscustomobject]@{batch=$bid;status=$result.status;seconds=$guard.run_seconds;peak_bytes=$guard.native_peak_job_memory_bytes;new_units_created=$result.new_financial_units_created;gate_promotion=$result.gate_promotion} | ConvertTo-Json -Compress
}
