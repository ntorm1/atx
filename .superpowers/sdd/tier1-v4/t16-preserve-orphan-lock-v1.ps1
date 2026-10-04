$ErrorActionPreference = 'Stop'
$stagePath = 'C:\atx\atx-db\data\alpha_panel\v1\universe_release_v1'
$guardReceiptPath = 'C:\atx\.superpowers\sdd\tier1-v4\logs\t16-universe-year2019-v1.receipt.json'
$outPath = 'C:\atx\.superpowers\sdd\tier1-v4\logs\t16-preserved-orphan-lock-v1.json'
if (Test-Path -LiteralPath $outPath) { throw 'Preserve previous operation receipt' }
$guard = Get-Content -LiteralPath $guardReceiptPath -Raw | ConvertFrom-Json
if ($guard.status -ne 'stopped_low_commit' -or $guard.returncode -ne 137 -or $guard.guard_pid -ne 20924 -or $guard.child_pid -ne 24780) { throw 'Unexpected stopped-job identity' }
if ($guard.command -notcontains 'r15_20261003_001') { throw 'Wrong stopped generation' }
foreach ($processIdentifier in @(20924,24780,2664)) {
    if (Get-Process -Id $processIdentifier -ErrorAction SilentlyContinue) { throw "Owner process still exists: $processIdentifier" }
}
$resolvedStage = (Resolve-Path -LiteralPath $stagePath).Path.TrimEnd('\')
$lockPath = Join-Path $resolvedStage '.writer.lock'
$resolvedLock = (Resolve-Path -LiteralPath $lockPath).Path
$destinationDirectory = [System.IO.Path]::GetFullPath((Join-Path $resolvedStage '_interrupted_locks'))
$destination = [System.IO.Path]::GetFullPath((Join-Path $destinationDirectory 'r15_20261003_001-pid2664-lowcommit.lock'))
if ($resolvedLock -ne ($resolvedStage + '\.writer.lock')) { throw 'Unexpected lock path' }
if (-not $destination.StartsWith($resolvedStage + '\', [System.StringComparison]::OrdinalIgnoreCase)) { throw 'Destination escaped owned release stage' }
if (Test-Path -LiteralPath $destination) { throw 'Preserve previous lock archive' }
if ((Get-Content -LiteralPath $resolvedLock -Raw) -ne '2664') { throw 'Lock owner changed' }
if ((Get-FileHash -LiteralPath $resolvedLock -Algorithm SHA256).Hash.ToLowerInvariant() -ne '141dc5ae59a81abaaeeb12e46e5e9a173dd6530e671bf9462300c867303ffe65') { throw 'Lock identity changed' }
if (Test-Path -LiteralPath (Join-Path $resolvedStage 'current.json')) { throw 'Unexpected publication' }
if (Test-Path -LiteralPath (Join-Path $resolvedStage 'generations\r15_20261003_001\manifest.json')) { throw 'Unexpected complete generation' }
if (Test-Path -LiteralPath (Join-Path $resolvedStage 'generations\r15_20261003_001\year=2019')) { throw 'New unit needs separate inspection' }
$lockBytes = (Get-Item -LiteralPath $resolvedLock).Length
$null = New-Item -ItemType Directory -Path $destinationDirectory -Force
Move-Item -LiteralPath $resolvedLock -Destination $destination
$sha = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
if ($sha -ne '141dc5ae59a81abaaeeb12e46e5e9a173dd6530e671bf9462300c867303ffe65') { throw 'Preserved lock mismatch' }
$record = [ordered]@{ status='ORPHAN_LOCK_PRESERVED_AFTER_DEAD_OWNER_CHECK'; source=$resolvedLock; destination=$destination; bytes=$lockBytes; sha256=$sha; guard_receipt_sha256=(Get-FileHash -LiteralPath $guardReceiptPath -Algorithm SHA256).Hash.ToLowerInvariant(); qualification='No data unit admitted or publication changed. Next build must perform ordinary full source/code/unit validation.' }
$json = $record | ConvertTo-Json -Depth 4
[System.IO.File]::WriteAllText($outPath, $json, [System.Text.UTF8Encoding]::new($false))
$record | ConvertTo-Json -Compress
