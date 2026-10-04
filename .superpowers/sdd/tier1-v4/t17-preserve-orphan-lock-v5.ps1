$ErrorActionPreference = 'Stop'
if (-not $env:ATX_GUARD_JOB) { throw 'External guard required' }
$stage = 'C:\atx\atx-db\data\alpha_panel\v1\fundamentals_release_v1'
$generation = Join-Path $stage 'generations\r17_20261004_002'
$workspace = 'C:\atx\.superpowers\sdd\tier1-v4'
$guardPath = Join-Path $workspace 'logs\t17-runtime-b0007-b0014-v1.receipt.json'
$inspectionPath = Join-Path $workspace 't17-interruption-inspection-v5.json'
$outPath = Join-Path $workspace 'logs\t17-preserved-orphan-lock-v5.json'
if (Test-Path -LiteralPath $outPath) { throw 'Preserve prior recovery evidence' }
$guard = Get-Content -LiteralPath $guardPath -Raw | ConvertFrom-Json
$guardSha = '87bf8ab1a24f313da18b8bc008525fd125348675b4995616ed3adb2b0a41b4db'
if ($guard.status -ne 'stopped_low_commit' -or $guard.returncode -ne 137 -or $guard.guard_pid -ne 24904 -or $guard.child_pid -ne 20900 -or $guard.cap_hit) { throw 'Unexpected stopped owner' }
if ($guard.command -notcontains 'atx_db.alpha_panel.fund_release_runtime' -or $guard.command -notcontains 'r17_20261004_002') { throw 'Wrong native command' }
function Assert-OwnersAbsent {
    foreach ($processIdentifier in @(24904,20900,23988)) {
        if (Get-Process -Id $processIdentifier -ErrorAction SilentlyContinue) { throw 'An owner PID remains; inspect before recovery' }
    }
    $writers = @(Get-CimInstance Win32_Process -Filter "Name = 'python.exe'" | Where-Object { $_.CommandLine -like '*-m atx_db.alpha_panel.fund_release_runtime*' -or $_.CommandLine -like '*-m atx_db.alpha_panel.fund_batch_release*' -or $_.CommandLine -like '*t17-financial-unit-*' })
    if ($writers.Count) { throw 'Matching native writer active' }
}
Assert-OwnersAbsent
$inspectionSha = '6090f38683aadb68a5409cf850ff339447a2a26db87ac9f0b4a69616cd4aaf95'
if ((Get-Item -LiteralPath $inspectionPath).Length -ne 34026 -or (Get-FileHash -LiteralPath $inspectionPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $inspectionSha) { throw 'Independently pinned interruption inspection changed' }
$inspection = Get-Content -LiteralPath $inspectionPath -Raw | ConvertFrom-Json
if ((Get-FileHash -LiteralPath $guardPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $guardSha -or $inspection.guard.sha256 -ne $guardSha) { throw 'Stopped-owner receipt differs from independent inspection' }
if ($inspection.status -ne 'HOST_STOPPED_FINANCIAL_CONTINUATION_INSPECTED' -or $inspection.lock_text -ne '23988' -or ($inspection.retained_admission_batch_ids -join ',') -ne '0,1,2,3,4,5,6,7' -or $inspection.final_manifest_exists -or $inspection.current_pointer_exists) { throw 'Unexpected candidate checkpoint' }
$lockPath = Join-Path $stage '.writer.lock'
$archive = Join-Path $stage '_interrupted_locks'
$destination = Join-Path $archive 'r17_20261004_002-pid23988-runtime-b0007-b0014-v1-lowcommit.lock'
if ((Resolve-Path -LiteralPath $stage).Path -ne $stage -or (Resolve-Path -LiteralPath $lockPath).Path -ne $lockPath) { throw 'Unexpected resolved owner path' }
if ([IO.Path]::GetFullPath($destination) -ne $destination -or -not $destination.StartsWith($stage+'\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Archive escaped owner directory' }
function Assert-NoReparse([string]$path) {
    $existingPath = [IO.Path]::GetFullPath($path)
    while (-not (Test-Path -LiteralPath $existingPath)) {
        $parentInfo = [IO.Directory]::GetParent($existingPath)
        if ($null -eq $parentInfo) { throw 'Cannot locate an existing path ancestor' }
        $existingPath = $parentInfo.FullName
    }
    $item = Get-Item -LiteralPath $existingPath -Force
    while ($null -ne $item) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Linked owner/archive/member path' }
        if ($item -is [IO.FileInfo]) { $item = $item.Directory }
        elseif ($item -is [IO.DirectoryInfo]) { $item = $item.Parent }
        else { throw 'Unsupported filesystem member type' }
    }
}
Assert-NoReparse $lockPath
Assert-NoReparse $archive
Assert-NoReparse $destination
Assert-NoReparse $guardPath
Assert-NoReparse $inspectionPath
if (Test-Path -LiteralPath $destination) { throw 'Preserve prior archive' }
if ((Get-Content -LiteralPath $lockPath -Raw) -ne '23988') { throw 'Lock owner changed' }
$lockSha = '0ab4580b4fac8de5c6528dfbd63a64110bcd9742c19c80a5b6247ca8307af11a'
if ((Get-Item -LiteralPath $lockPath).Length -ne 5 -or $inspection.lock_identity.bytes -ne 5 -or $inspection.lock_identity.sha256 -ne $lockSha) { throw 'Independent five-byte lock identity mismatch' }
if ((Get-FileHash -LiteralPath $lockPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $lockSha) { throw 'Lock bytes changed' }
$snapshotPath = Join-Path $generation 'snapshot.json'
if ((Get-FileHash -LiteralPath $snapshotPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne 'c73f6293a5785c185567b01c443747aa2c7294614b45bfdaf2b8016ec9807a0c') { throw 'Source snapshot changed' }
if ((Test-Path -LiteralPath (Join-Path $stage 'current.json')) -or (Test-Path -LiteralPath (Join-Path $generation 'manifest.json'))) { throw 'Candidate unexpectedly published' }
foreach ($bid in 0..84) {
    $admissionPath = Join-Path $generation ('units\batch-{0:D4}\admission.json' -f $bid)
    if ((Test-Path -LiteralPath $admissionPath) -ne ($bid -le 7)) { throw 'Retained admission frontier changed' }
}
foreach ($entry in $inspection.native_members.PSObject.Properties) {
    $memberPath = [IO.Path]::GetFullPath($entry.Name)
    if (-not $memberPath.StartsWith($generation+'\units\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Inspected member escaped generation units' }
    Assert-NoReparse $memberPath
    if ((Get-Item -LiteralPath $memberPath).Length -ne $entry.Value.bytes -or (Get-FileHash -LiteralPath $memberPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.Value.sha256) { throw 'Inspected native member changed' }
}
Assert-OwnersAbsent
if ((Get-FileHash -LiteralPath $inspectionPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $inspectionSha -or (Get-FileHash -LiteralPath $guardPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $guardSha) { throw 'Recovery evidence changed during member checks' }
if ((Get-Content -LiteralPath $lockPath -Raw) -ne '23988' -or (Get-FileHash -LiteralPath $lockPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $lockSha) { throw 'Lock changed before preservation' }
$null = New-Item -ItemType Directory -Path $archive -Force
Move-Item -LiteralPath $lockPath -Destination $destination
if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant() -ne $lockSha) { throw 'Archived lock mismatch' }
$record = [pscustomobject]@{
    status='ORPHAN_LOCK_PRESERVED_AFTER_DEAD_OWNER_CHECK'; source=$lockPath; destination=$destination;
    bytes=5; sha256=$lockSha; guard_receipt_sha256=$guardSha;
    candidate_inspection_sha256=$inspectionSha;
    retained_admission_batch_ids=@(0,1,2,3,4,5,6,7);
    qualification='Only five-byte dead-owner lock archived. Failed attempts and sources remain unchanged. No unit admitted; next native build must perform complete ordinary source/code/unit validation.'
}
[IO.File]::WriteAllText($outPath, ($record | ConvertTo-Json -Depth 4), [Text.UTF8Encoding]::new($false))
$record | ConvertTo-Json -Compress
