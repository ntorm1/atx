$ErrorActionPreference = 'Stop'
if (-not $env:ATX_GUARD_JOB) { throw 'External guard required' }
$workspace = 'C:\atx\.superpowers\sdd\tier1-v4'
$stage = 'C:\atx\atx-db\data\alpha_panel\v1\fundamentals_release_v1'
$scratch = Join-Path $stage '_spill'
$out = Join-Path $workspace 'logs\disk-cleanup-fund-audit-v1.json'
if (Test-Path -LiteralPath $out) { throw 'Preserve previous cleanup evidence' }
function Assert-NoReparse([string]$path) {
    $cursor = [IO.Path]::GetFullPath($path)
    while (-not (Test-Path -LiteralPath $cursor)) {
        $parent = [IO.Directory]::GetParent($cursor)
        if ($null -eq $parent) { throw 'No existing ancestor' }
        $cursor = $parent.FullName
    }
    $item = Get-Item -LiteralPath $cursor -Force
    while ($null -ne $item) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Linked cleanup or protected path' }
        if ($item -is [IO.FileInfo]) { $item = $item.Directory }
        elseif ($item -is [IO.DirectoryInfo]) { $item = $item.Parent }
        else { throw 'Unexpected path type' }
    }
}
function Assert-NoFinancialWriter {
    $writers = @(Get-CimInstance Win32_Process -Filter "Name = 'python.exe'" | Where-Object { $_.CommandLine -like '*-m atx_db.alpha_panel.fund_release_runtime*' -or $_.CommandLine -like '*-m atx_db.alpha_panel.fund_batch_release*' -or $_.CommandLine -like '*t17-financial-unit-*' })
    if ($writers.Count) { throw 'Financial writer or validator is active' }
}
Assert-NoFinancialWriter
$matrixPath = Join-Path $workspace 'readiness-matrix-v8.json'
$matrix = Get-Content -LiteralPath $matrixPath -Raw | ConvertFrom-Json
if (($matrix.checkpoint.financial_batches_independently_checked -join ',') -ne '0,1,2,3,4,5,6') { throw 'Expected checked-audit checkpoint required' }
$record = @($matrix.requirements | Where-Object id -eq 'D01')[0].observations | Where-Object checkpoint_version -eq 8
if (@($record).Count -ne 1 -or $record.gate_promotion -ne $false) { throw 'Exact non-promoting audit record required' }
$protected = @{}
$targets = @()
foreach ($bid in 3..6) {
    $tag = 't17-financial-native-validation-b{0:D4}-v1' -f $bid
    $resultPath = Join-Path $workspace ($tag + '.json')
    $guardPath = Join-Path $workspace ('logs\' + $tag + '.receipt.json')
    foreach ($path in @($resultPath,$guardPath)) {
        Assert-NoReparse $path
        $pin = $record.evidence_identities.PSObject.Properties[$path].Value
        if ($null -eq $pin -or (Get-Item -LiteralPath $path).Length -ne $pin.bytes -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $pin.sha256) { throw 'Independently recorded audit evidence changed' }
        $protected[$path] = $pin
    }
    $guard = Get-Content -LiteralPath $guardPath -Raw | ConvertFrom-Json
    $audit = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
    $expected = @('.venv/Scripts/python.exe','C:/atx/.superpowers/sdd/tier1-v4/t17-financial-unit-validate-v1.py','--batch',[string]$bid,'--attempt-tag','v1')
    if ($guard.status -cne 'completed' -or $guard.returncode -ne 0 -or $guard.cap_hit -isnot [bool] -or $guard.cap_hit -or $guard.stop_reason -or ($guard.command | ConvertTo-Json -Compress) -cne ($expected | ConvertTo-Json -Compress)) { throw 'Exact successful audit guard required' }
    foreach ($owner in @($guard.guard_pid,$guard.child_pid)) {
        if (Get-Process -Id $owner -ErrorAction SilentlyContinue) { throw 'Ended audit PID present; inspect before cleanup' }
    }
    if ($audit.batch -ne $bid -or $audit.status -cne 'ONE_RETAINED_FINANCIAL_UNIT_NATIVE_TYPED_REPLAY_VERIFIED' -or $audit.resources.guard.job -cne $guard.child_env_forced.ATX_GUARD_JOB) { throw 'Audit database owner evidence mismatch' }
    $target = [IO.Path]::GetFullPath($audit.resources.database)
    if ([IO.Path]::GetDirectoryName($target) -cne $scratch -or [IO.Path]::GetFileName($target) -cnotmatch '^fund-runtime-[0-9a-f]{32}\.duckdb$') { throw 'Target is not an exact ended R23 scratch database' }
    Assert-NoReparse $target
    if (-not (Test-Path -LiteralPath $target) -or (Test-Path -LiteralPath ($target+'.wal'))) { throw 'Scratch absent or unclosed WAL; preserve and inspect' }
    $item = Get-Item -LiteralPath $target
    if ($item.PSIsContainer -or $item.Length -gt 2GB) { throw 'Unexpected scratch type or size' }
    $targets += [pscustomobject]@{path=$target;bytes=$item.Length;sha256=(Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant();audit=$resultPath;guard=$guardPath}
}
if (@($targets.path | Select-Object -Unique).Count -ne 4) { throw 'Four distinct successful audit databases required' }
$inspectionPath = Join-Path $workspace 't17-interruption-inspection-v5.json'
if ((Get-Item -LiteralPath $inspectionPath).Length -ne 34026 -or (Get-FileHash -LiteralPath $inspectionPath -Algorithm SHA256).Hash.ToLowerInvariant() -cne '6090f38683aadb68a5409cf850ff339447a2a26db87ac9f0b4a69616cd4aaf95') { throw 'Independently pinned current candidate inspection changed' }
$inspection = Get-Content -LiteralPath $inspectionPath -Raw | ConvertFrom-Json
foreach ($entry in $inspection.native_members.PSObject.Properties) { $protected[$entry.Name] = $entry.Value }
$snapshotPath = Join-Path $stage 'generations\r17_20261004_002\snapshot.json'
$protected[$snapshotPath] = [pscustomobject]@{bytes=10411360;sha256='c73f6293a5785c185567b01c443747aa2c7294614b45bfdaf2b8016ec9807a0c'}
$lockPath = Join-Path $stage '.writer.lock'
$protected[$lockPath] = $inspection.lock_identity
$snapshotText = (Get-Content -LiteralPath $snapshotPath -Raw).Replace('\\','/').ToLowerInvariant()
foreach ($target in $targets) {
    if ($snapshotText.Contains($target.path.Replace('\','/').ToLowerInvariant())) { throw 'Frozen source snapshot references candidate scratch' }
}
foreach ($path in @($matrixPath,$inspectionPath)) {
    $protected[$path] = [pscustomobject]@{bytes=(Get-Item -LiteralPath $path).Length;sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()}
}
function Assert-Protected {
    foreach ($path in $protected.Keys) {
        Assert-NoReparse $path
        if ((Get-Item -LiteralPath $path).Length -ne $protected[$path].bytes -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $protected[$path].sha256) { throw 'Protected source/checkpoint/evidence changed' }
    }
}
Assert-Protected
Assert-NoFinancialWriter
$freeBefore = (Get-PSDrive -Name C).Free
foreach ($target in $targets) {
    Assert-NoReparse $target.path
    if ((Get-Item -LiteralPath $target.path).Length -ne $target.bytes -or (Get-FileHash -LiteralPath $target.path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $target.sha256) { throw 'Owned scratch changed before deletion' }
    $stream = [IO.File]::Open($target.path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::None)
    $stream.Dispose()
    Remove-Item -LiteralPath $target.path -Force
    if (Test-Path -LiteralPath $target.path) { throw 'Owned scratch remains' }
}
Assert-Protected
$freeAfter = (Get-PSDrive -Name C).Free
$result = [pscustomobject]@{status='FOUR_ENDED_AUDIT_RUNTIME_DATABASES_REMOVED';targets=$targets;logical_bytes_removed=($targets | Measure-Object bytes -Sum).Sum;drive_free_before_bytes=$freeBefore;drive_free_after_bytes=$freeAfter;drive_free_delta_bytes=($freeAfter-$freeBefore);protected_identities=$protected;qualification='Only four exact closed successful-audit scratch database files removed. Native units0..7, batch8 partial start, source snapshot, dead-owner lock and audit evidence checked before/after; original sources and all failed-attempt scratch retained. No financial/source/gold/release gate change.'}
[IO.File]::WriteAllText($out,($result | ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false))
$result | Select-Object status,logical_bytes_removed,drive_free_delta_bytes,qualification | ConvertTo-Json -Compress
