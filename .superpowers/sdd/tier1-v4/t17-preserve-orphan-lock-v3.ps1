$ErrorActionPreference = 'Stop'
if (-not $env:ATX_GUARD_JOB) { throw 'External guard required' }
$stage = 'C:\atx\atx-db\data\alpha_panel\v1\fundamentals_release_v1'
$workspace = 'C:\atx\.superpowers\sdd\tier1-v4'
$guardPath = Join-Path $workspace 'logs\t17-runtime-pilot-b0000-v3.receipt.json'
$inspectionPath = Join-Path $workspace 't17-interruption-inspection-v3.json'
$outPath = Join-Path $workspace 'logs\t17-preserved-orphan-lock-v3.json'
if (Test-Path -LiteralPath $outPath) { throw 'Preserve prior recovery evidence' }
$guard = Get-Content -LiteralPath $guardPath -Raw | ConvertFrom-Json
if ($guard.status -ne 'stopped_low_commit' -or $guard.returncode -ne 137 -or $guard.guard_pid -ne 6424 -or $guard.child_pid -ne 10692 -or $guard.cap_hit) { throw 'Unexpected stopped owner' }
if ($guard.command -notcontains 'atx_db.alpha_panel.fund_release_runtime' -or $guard.command -notcontains 'r17_20261004_002') { throw 'Wrong native command' }
foreach ($processIdentifier in @(6424,10692,7732)) {
    if (Get-Process -Id $processIdentifier -ErrorAction SilentlyContinue) { throw 'An owner PID remains; inspect before recovery' }
}
$writers = @(Get-CimInstance Win32_Process -Filter "Name = 'python.exe'" | Where-Object { $_.CommandLine -like '*-m atx_db.alpha_panel.fund_release_runtime*' -or $_.CommandLine -like '*-m atx_db.alpha_panel.fund_batch_release*' })
if ($writers.Count) { throw 'Matching native writer active' }
$inspection = Get-Content -LiteralPath $inspectionPath -Raw | ConvertFrom-Json
if ((Get-FileHash -LiteralPath $guardPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne '1dbbdb8458874f24b829f9495edf8393f62d9c9d0ae150a65ab1e3fea2d448c4' -or $inspection.guard.sha256 -ne '1dbbdb8458874f24b829f9495edf8393f62d9c9d0ae150a65ab1e3fea2d448c4') { throw 'Stopped-owner receipt differs from independent inspection' }
if ($inspection.status -ne 'HOST_STOPPED_FINANCIAL_CANDIDATE_INSPECTED' -or $inspection.lock_text -ne '7732' -or $null -ne $inspection.admission -or $inspection.final_manifest_exists -or $inspection.current_pointer_exists) { throw 'Unexpected candidate checkpoint' }
$lockPath = Join-Path $stage '.writer.lock'
$archive = Join-Path $stage '_interrupted_locks'
$destination = Join-Path $archive 'r17_20261004_002-pid7732-runtime-pilot-v3-lowcommit.lock'
if ((Resolve-Path -LiteralPath $stage).Path -ne $stage -or (Resolve-Path -LiteralPath $lockPath).Path -ne $lockPath) { throw 'Unexpected resolved owner path' }
if ([IO.Path]::GetFullPath($destination) -ne $destination -or -not $destination.StartsWith($stage+'\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Archive escaped owner directory' }
foreach ($path in @($lockPath,$archive)) {
    if (Test-Path -LiteralPath $path) {
        $item = Get-Item -LiteralPath $path -Force
        while ($null -ne $item) {
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Linked owner/archive path' }
            $item = $item.Parent
        }
    }
}
if (Test-Path -LiteralPath $destination) { throw 'Preserve prior archive' }
if ((Get-Content -LiteralPath $lockPath -Raw) -ne '7732') { throw 'Lock owner changed' }
$lockSha = '61f4a24677e07919033f02398ced57278bae7c0a475a54af9128d142577536cb'
if ((Get-Item -LiteralPath $lockPath).Length -ne 4 -or $inspection.lock_identity.bytes -ne 4 -or $inspection.lock_identity.sha256 -ne $lockSha) { throw 'Independent four-byte lock identity mismatch' }
if ((Get-FileHash -LiteralPath $lockPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $lockSha) { throw 'Lock bytes changed' }
$snapshotPath = Join-Path $stage 'generations\r17_20261004_002\snapshot.json'
if ((Get-FileHash -LiteralPath $snapshotPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne 'c73f6293a5785c185567b01c443747aa2c7294614b45bfdaf2b8016ec9807a0c') { throw 'Source snapshot changed' }
if ((Test-Path -LiteralPath (Join-Path $stage 'current.json')) -or (Test-Path -LiteralPath (Join-Path $stage 'generations\r17_20261004_002\manifest.json')) -or (Test-Path -LiteralPath (Join-Path $stage 'generations\r17_20261004_002\units\batch-0000\admission.json'))) { throw 'Candidate changed after inspection' }
foreach ($entry in $inspection.native_members.PSObject.Properties) {
    if ((Get-FileHash -LiteralPath $entry.Name -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.Value.sha256) { throw 'Inspected native member changed' }
}
$null = New-Item -ItemType Directory -Path $archive -Force
Move-Item -LiteralPath $lockPath -Destination $destination
if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant() -ne $lockSha) { throw 'Archived lock mismatch' }
$record = [pscustomobject]@{
    status='ORPHAN_LOCK_PRESERVED_AFTER_DEAD_OWNER_CHECK'; source=$lockPath; destination=$destination;
    bytes=4; sha256=$lockSha; guard_receipt_sha256=(Get-FileHash -LiteralPath $guardPath -Algorithm SHA256).Hash.ToLowerInvariant();
    candidate_inspection_sha256=(Get-FileHash -LiteralPath $inspectionPath -Algorithm SHA256).Hash.ToLowerInvariant();
    qualification='Only four-byte dead-owner lock archived. Failed attempts and sources remain unchanged. No unit admitted; next native build must perform complete ordinary source/code/unit validation.'
}
[IO.File]::WriteAllText($outPath, ($record | ConvertTo-Json -Depth 4), [Text.UTF8Encoding]::new($false))
$record | ConvertTo-Json -Compress
