$ErrorActionPreference = 'Stop'
$workspace = 'C:\atx\.superpowers\sdd\tier1-v4'
$auditPath = Join-Path $workspace 'disk-cleanup-audit-v2.json'
$outPath = Join-Path $workspace 'logs\disk-cleanup-applied-v2.json'
$dataRoot = 'C:\atx\atx-db\data\alpha_panel\v1'
if (Test-Path -LiteralPath $outPath) { throw 'Preserve prior cleanup receipt' }
$resolvedRoot = (Resolve-Path -LiteralPath $dataRoot).Path.TrimEnd('\')
$audit = Get-Content -LiteralPath $auditPath -Raw | ConvertFrom-Json
if ($audit.status -ne 'AUDIT_ONLY_NO_DELETION' -or $audit.authenticated_current_preparation_units -ne 144) { throw 'Unexpected audit state' }
$allowed = @(
    (Join-Path $resolvedRoot '_tmp\universe-release-r15_20261003_001'),
    (Join-Path $resolvedRoot '_tmp\fund-release-extract-verify'),
    (Join-Path $resolvedRoot 'fundamentals_release_v1\_spill')
)
$activeWriters = @(Get-CimInstance Win32_Process -Filter "Name = 'python.exe'" | Where-Object {
    $_.CommandLine -like '*-m atx_db.alpha_panel.universe_release*' -or
    $_.CommandLine -like '*-m atx_db.alpha_panel.fund_batch_release*'
})
if ($activeWriters.Count) { throw 'A matching release writer is still active' }
foreach ($run in $audit.ended_runs.PSObject.Properties) {
    $receiptPath = Join-Path $workspace ('logs\' + $run.Name + '.receipt.json')
    if ((Get-FileHash -LiteralPath $receiptPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $run.Value.sha256) { throw 'Ended receipt changed' }
    foreach ($processId in @($run.Value.guard_pid, $run.Value.child_pid)) {
        if (Get-Process -Id $processId -ErrorAction SilentlyContinue) { throw "Original job PID is active: $processId" }
    }
}
$locks = @()
foreach ($entry in $audit.retained_locks.PSObject.Properties) {
    $lock = $entry.Value
    $expectedPath = Join-Path (Join-Path $resolvedRoot $entry.Name) '.writer.lock'
    if ($lock.path -ne $expectedPath -or (Get-FileHash -LiteralPath $expectedPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $lock.sha256) { throw 'Writer lock changed' }
    if ((Get-Item -LiteralPath $expectedPath).Attributes -band [System.IO.FileAttributes]::ReparsePoint) { throw 'Linked writer lock' }
    if (Get-Process -Id $lock.pid -ErrorAction SilentlyContinue) { throw 'Writer PID remains active' }
    $generation = if ($entry.Name -eq 'universe_release_v1') { 'r15_20261003_001' } else { 'r17_20261004_001' }
    $destination = Join-Path (Join-Path (Split-Path -Parent $expectedPath) '_interrupted_locks') ($generation + '-pid' + $lock.pid + '-lowcommit.lock')
    if (-not [System.IO.Path]::GetFullPath($destination).StartsWith($resolvedRoot + '\', [System.StringComparison]::OrdinalIgnoreCase) -or (Test-Path -LiteralPath $destination)) { throw 'Unsafe/existing lock destination' }
    $locks += [pscustomobject]@{ source=$expectedPath; destination=$destination; sha256=$lock.sha256; bytes=$lock.bytes }
}
foreach ($candidate in $audit.candidate_evidence.PSObject.Properties) {
    if ($candidate.Value.manifest_exists) { throw 'Unexpected completed candidate' }
    foreach ($file in $candidate.Value.files.PSObject.Properties) {
        $path = Join-Path $candidate.Value.directory $file.Name
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $file.Value.sha256 -or (Get-Item -LiteralPath $path).Length -ne $file.Value.bytes) { throw 'Candidate evidence changed' }
    }
}
$selected = @($audit.targets | Where-Object { $_.eligible_for_explicit_removal })
if ($selected.Count -ne 3) { throw 'Unexpected cleanup target inventory' }
$verified = @()
foreach ($target in $selected) {
    $absolute = [System.IO.Path]::GetFullPath($target.path)
    if ($allowed -notcontains $absolute -or -not $absolute.StartsWith($resolvedRoot + '\', [System.StringComparison]::OrdinalIgnoreCase)) { throw 'Target escaped explicit whitelist' }
    if ((Resolve-Path -LiteralPath $absolute).Path -ne $absolute) { throw 'Resolved target mismatch' }
    if ($target.dependency_mentions.Count -or $target.reparse_points.Count) { throw 'Protected or linked target' }
    $expected = @{}
    foreach ($row in $target.files) { $expected[$row.path] = $row }
    $observed = @{}
    $pending = [System.Collections.Generic.Stack[string]]::new()
    $pending.Push($absolute)
    while ($pending.Count) {
        $item = Get-Item -LiteralPath $pending.Pop() -Force
        if ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) { throw 'Reparse point appeared after audit' }
        if ($item.FullName -ne $absolute -and -not $item.FullName.StartsWith($absolute + '\', [System.StringComparison]::OrdinalIgnoreCase)) { throw 'Member escaped audited target' }
        if ($item.PSIsContainer) {
            foreach ($child in Get-ChildItem -LiteralPath $item.FullName -Force) { $pending.Push($child.FullName) }
        } else {
            if (-not $expected.ContainsKey($item.FullName)) { throw 'File inventory changed after audit' }
            $mtimeNanoseconds = [int64](($item.LastWriteTimeUtc.Ticks - [int64]621355968000000000) * 100)
            if ($item.Length -ne $expected[$item.FullName].bytes -or $mtimeNanoseconds -ne $expected[$item.FullName].mtime_ns) { throw 'File changed after audit' }
            $observed[$item.FullName] = $true
        }
    }
    if ($observed.Count -ne $expected.Count) { throw 'Audited file is missing' }
    $verified += [pscustomobject]@{ path=$absolute; files=$expected.Count; logical_bytes=$target.logical_bytes; reason=$target.reason }
}
$freeBefore = (Get-PSDrive -Name C).Free
foreach ($lock in $locks) {
    $parent = Split-Path -Parent $lock.destination
    if (-not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent | Out-Null }
    if ((Get-Item -LiteralPath $parent).Attributes -band [System.IO.FileAttributes]::ReparsePoint) { throw 'Linked lock archive directory' }
    Move-Item -LiteralPath $lock.source -Destination $lock.destination
    if ((Get-FileHash -LiteralPath $lock.destination -Algorithm SHA256).Hash.ToLowerInvariant() -ne $lock.sha256) { throw 'Archived lock hash changed' }
}
foreach ($target in $verified) {
    Remove-Item -LiteralPath $target.path -Recurse -Force
    if (Test-Path -LiteralPath $target.path) { throw 'Cleanup target still exists' }
}
$freeAfter = (Get-PSDrive -Name C).Free
$record = [ordered]@{ status='OWNED_ENDED_SCRATCH_REMOVED'; audit_sha256=(Get-FileHash -LiteralPath $auditPath -Algorithm SHA256).Hash.ToLowerInvariant(); deleted=$verified; preserved_orphan_locks=$locks; logical_bytes_removed=($verified | Measure-Object -Property logical_bytes -Sum).Sum; drive_free_before_bytes=$freeBefore; drive_free_after_bytes=$freeAfter; drive_free_delta_bytes=($freeAfter-$freeBefore); protected='Originals, full extraction data/stats/receipts, 144 preparation units, candidate generations and logs retained.' }
[System.IO.File]::WriteAllText($outPath, ($record | ConvertTo-Json -Depth 8), [System.Text.UTF8Encoding]::new($false))
$record | ConvertTo-Json -Depth 8 -Compress
