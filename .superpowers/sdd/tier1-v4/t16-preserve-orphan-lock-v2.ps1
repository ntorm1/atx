$ErrorActionPreference = 'Stop'
$stagePath = 'C:\atx\atx-db\data\alpha_panel\v1\universe_release_v1'
$guardReceiptPath = 'C:\atx\.superpowers\sdd\tier1-v4\logs\t16-universe-year2019-v2.receipt.json'
$outPath = 'C:\atx\.superpowers\sdd\tier1-v4\logs\t16-preserved-orphan-lock-v2.json'
$inspectionPath = 'C:\atx\.superpowers\sdd\tier1-v4\logs\disk-cleanup-verification-v1.json'
if (Test-Path -LiteralPath $outPath) { throw 'Preserve previous operation receipt' }
$guard = Get-Content -LiteralPath $guardReceiptPath -Raw | ConvertFrom-Json
if ($guard.status -ne 'stopped_low_commit' -or $guard.returncode -ne 137 -or $guard.guard_pid -ne 11020 -or $guard.child_pid -ne 4316) { throw 'Unexpected stopped-job identity' }
if ($guard.command -notcontains 'r15_20261003_001') { throw 'Wrong stopped generation' }
foreach ($processIdentifier in @(11020,4316,17684)) {
    if (Get-Process -Id $processIdentifier -ErrorAction SilentlyContinue) { throw "Owner process still exists: $processIdentifier" }
}
$inspection = Get-Content -LiteralPath $inspectionPath -Raw | ConvertFrom-Json
if ($inspection.status -ne 'CURRENT_PREPARATION_AND_CANDIDATE_BYTES_VERIFIED' -or $inspection.retained_writer_lock -ne '17684' -or $inspection.publication_exists -ne $false -or $inspection.unsealed_candidate_directories.Count -ne 0) { throw 'Candidate inspection changed' }
$resolvedStage = (Resolve-Path -LiteralPath $stagePath).Path.TrimEnd('\')
$resolvedLock = (Resolve-Path -LiteralPath (Join-Path $resolvedStage '.writer.lock')).Path
$destinationDirectory = [System.IO.Path]::GetFullPath((Join-Path $resolvedStage '_interrupted_locks'))
$destination = [System.IO.Path]::GetFullPath((Join-Path $destinationDirectory 'r15_20261003_001-pid17684-lowcommit.lock'))
if ($resolvedStage -ne $stagePath -or $resolvedLock -ne ($stagePath + '\.writer.lock')) { throw 'Unexpected absolute owned path' }
if (-not $destination.StartsWith($resolvedStage + '\', [System.StringComparison]::OrdinalIgnoreCase)) { throw 'Destination escaped owned release stage' }
foreach ($checkedPath in @($resolvedStage,$resolvedLock,$destinationDirectory)) {
    if ((Test-Path -LiteralPath $checkedPath) -and ((Get-Item -LiteralPath $checkedPath -Force).Attributes -band [System.IO.FileAttributes]::ReparsePoint)) { throw 'Reparse path refused' }
}
if (Test-Path -LiteralPath $destination) { throw 'Preserve previous lock archive' }
if ((Get-Content -LiteralPath $resolvedLock -Raw) -ne '17684') { throw 'Lock owner changed' }
$expectedHash = '856b9705dbec4a9e18627d0b8fe8d0119ce617f1da16df462c362f459568f049'
if ((Get-FileHash -LiteralPath $resolvedLock -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expectedHash) { throw 'Lock identity changed' }
if (Test-Path -LiteralPath (Join-Path $resolvedStage 'current.json')) { throw 'Unexpected publication' }
if (Test-Path -LiteralPath (Join-Path $resolvedStage 'generations\r15_20261003_001\manifest.json')) { throw 'Unexpected complete generation' }
foreach ($year in @('2018','2019')) {
    $receiptPath = Join-Path $resolvedStage ('generations\r15_20261003_001\year=' + $year + '\receipt.json')
    if ((Get-FileHash -LiteralPath $receiptPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $inspection.candidate_units.$year.receipt_sha256) { throw 'Candidate receipt changed after inspection' }
}
$lockBytes = (Get-Item -LiteralPath $resolvedLock).Length
$null = New-Item -ItemType Directory -Path $destinationDirectory -Force
Move-Item -LiteralPath $resolvedLock -Destination $destination
$sha = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
if ($sha -ne $expectedHash) { throw 'Preserved lock mismatch' }
$record = [ordered]@{ status='ORPHAN_LOCK_PRESERVED_AFTER_DEAD_OWNER_CHECK'; source=$resolvedLock; destination=$destination; bytes=$lockBytes; sha256=$sha; guard_receipt_sha256=(Get-FileHash -LiteralPath $guardReceiptPath -Algorithm SHA256).Hash.ToLowerInvariant(); candidate_inspection_sha256=(Get-FileHash -LiteralPath $inspectionPath -Algorithm SHA256).Hash.ToLowerInvariant(); qualification='No data unit admitted or publication changed. Next build must perform ordinary full source/code/unit validation.' }
[System.IO.File]::WriteAllText($outPath, ($record | ConvertTo-Json -Depth 4), [System.Text.UTF8Encoding]::new($false))
$record | ConvertTo-Json -Compress
