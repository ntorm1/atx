$ErrorActionPreference = 'Stop'
$workspace = 'C:\atx\.superpowers\sdd\tier1-v4'
$auditScript = Join-Path $workspace 't17-financial-unit-validate-v1.py'
$auditSha = (Get-FileHash -LiteralPath $auditScript -Algorithm SHA256).Hash.ToLowerInvariant()
$auditBytes = (Get-Item -LiteralPath $auditScript).Length
$env:PYTHONPATH = 'C:/atx/atx-db/src'
$env:OPENBLAS_NUM_THREADS = '1'
foreach ($bid in 3..6) {
    $tag = 't17-financial-native-validation-b{0:D4}-v1' -f $bid
    $receipt = Join-Path $workspace ('logs\' + $tag + '.receipt.json')
    $stdout = Join-Path $workspace ('logs\' + $tag + '.out.log')
    $stderr = Join-Path $workspace ('logs\' + $tag + '.err.log')
    $validation = Join-Path $workspace ($tag + '.json')
    foreach ($path in @($receipt,$stdout,$stderr,$validation)) {
        if (Test-Path -LiteralPath $path) { throw 'Preserve prior exact batch attempt' }
    }
    & .venv/Scripts/python.exe C:/atx/.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.8 --heavy --disk-path C:/atx/atx-db/data --min-free-disk-gb 40 --wait-minutes 10 --receipt $receipt --stdout $stdout --stderr $stderr --quiet -- .venv/Scripts/python.exe C:/atx/.superpowers/sdd/tier1-v4/t17-financial-unit-validate-v1.py --batch $bid --attempt-tag v1
    if ($LASTEXITCODE -ne 0) { throw ('Guarded batch validation failed: ' + $bid) }
    $guard = Get-Content -LiteralPath $receipt -Raw | ConvertFrom-Json
    $result = Get-Content -LiteralPath $validation -Raw | ConvertFrom-Json
    foreach ($field in @('guard_version','command','receipt','job_limit_gb','job_cap_bytes','heavy','status','returncode','cap_hit','disk','thresholds')) {
        if ($field -cnotin $guard.PSObject.Properties.Name) { throw ('Missing mandatory guard field: ' + $field) }
    }
    $expectedCommand = @('.venv/Scripts/python.exe','C:/atx/.superpowers/sdd/tier1-v4/t17-financial-unit-validate-v1.py','--batch',[string]$bid,'--attempt-tag','v1')
    if ($guard.command -isnot [array] -or ($guard.command | ConvertTo-Json -Compress) -cne ($expectedCommand | ConvertTo-Json -Compress)) { throw 'Exact reviewed native validation command required' }
    if ($guard.guard_version -cne 'c58-m0-r1' -or $guard.receipt -cne $receipt -or $guard.status -cne 'completed' -or -not ($guard.returncode -is [int] -or $guard.returncode -is [long]) -or $guard.returncode -ne 0 -or $guard.cap_hit -isnot [bool] -or $guard.cap_hit -or $guard.stop_reason) { throw 'Exact successful guard identity and typed terminal fields required' }
    if ($guard.job_limit_gb -isnot [double] -or $guard.job_limit_gb -ne 0.8 -or $guard.job_cap_bytes -ne 858992640 -or $guard.heavy -isnot [bool] -or -not $guard.heavy -or $guard.disk.path -cne 'C:\atx\atx-db\data' -or $guard.disk.min_free_gb -ne 40 -or $guard.thresholds.test_override -isnot [bool] -or $guard.thresholds.test_override -or $guard.thresholds.test_slot_dir -isnot [bool] -or $guard.thresholds.test_slot_dir) { throw 'Original guard resource contract required' }
    foreach ($field in @('batch','status','external_guard_receipt','successful_external_guard_required','new_financial_units_created','publication','source_quality_certified','gate_promotion','controller_code')) {
        if ($field -cnotin $result.PSObject.Properties.Name) { throw ('Missing mandatory result field: ' + $field) }
    }
    if (-not ($result.batch -is [int] -or $result.batch -is [long]) -or $result.batch -ne $bid -or $result.status -cne 'ONE_RETAINED_FINANCIAL_UNIT_NATIVE_TYPED_REPLAY_VERIFIED' -or $result.external_guard_receipt -cne $receipt -or $result.successful_external_guard_required -isnot [bool] -or -not $result.successful_external_guard_required -or -not ($result.new_financial_units_created -is [int] -or $result.new_financial_units_created -is [long]) -or $result.new_financial_units_created -ne 0) { throw 'Exact native validation outcome mismatch' }
    foreach ($field in @('publication','source_quality_certified','gate_promotion')) {
        if ($result.$field -isnot [bool] -or $result.$field) { throw ('Non-promoting result required: ' + $field) }
    }
    if ($result.controller_code.sha256 -cne $auditSha -or $result.controller_code.bytes -ne $auditBytes -or (Get-FileHash -LiteralPath $auditScript -Algorithm SHA256).Hash.ToLowerInvariant() -cne $auditSha -or (Get-Item -LiteralPath $auditScript).Length -ne $auditBytes) { throw 'Reviewed audit controller bytes changed' }
    [pscustomobject]@{batch=$bid;status=$result.status;seconds=$guard.run_seconds;peak_bytes=$guard.native_peak_job_memory_bytes;new_units_created=$result.new_financial_units_created;gate_promotion=$result.gate_promotion} | ConvertTo-Json -Compress
}
