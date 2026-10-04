$ErrorActionPreference = 'Stop'
$auditPath = 'C:\atx\.superpowers\sdd\tier1-v4\disk-cleanup-audit-v1.json'
$outPath = 'C:\atx\.superpowers\sdd\tier1-v4\logs\disk-cleanup-applied-v1.json'
$dataRoot = 'C:\atx\atx-db\data\alpha_panel\v1'
if (Test-Path -LiteralPath $outPath) { throw 'Preserve prior cleanup receipt' }
$resolvedRoot = (Resolve-Path -LiteralPath $dataRoot).Path.TrimEnd('\')
$audit = Get-Content -LiteralPath $auditPath -Raw | ConvertFrom-Json
if ($audit.status -ne 'AUDIT_ONLY_NO_DELETION' -or $audit.authenticated_current_preparation_units -ne 144) { throw 'Unexpected audit state' }
$allowed = @(
    (Join-Path $resolvedRoot '_tmp\universe-release-r15_20261003_001'),
    (Join-Path $resolvedRoot '_tmp\release-universe-load-diagnostic-v1.duckdb'),
    (Join-Path $resolvedRoot '_tmp\release-universe-year-diagnostic-v1.duckdb'),
    (Join-Path $resolvedRoot 'fundamentals_v11\_interrupted_work-r2-20261003T211353Z')
)
$activeWriters = @(Get-CimInstance Win32_Process -Filter "Name = 'python.exe'" | Where-Object { $_.CommandLine -like '*atx_db.alpha_panel.universe_release*' -and $_.CommandLine -like '*r15_20261003_001*' })
if ($activeWriters.Count) { throw 'A matching release writer is still active' }
foreach ($receiptName in @('t16-universe-year2018-v1.receipt.json','t16-universe-year2019-v1.receipt.json','t16-universe-year2019-v2.receipt.json','universe-load-diagnostic-v1.receipt.json','universe-year-diagnostic-v1.receipt.json')) {
    $receipt = Get-Content -LiteralPath (Join-Path 'C:\atx\.superpowers\sdd\tier1-v4\logs' $receiptName) -Raw | ConvertFrom-Json
    if ($receipt.status -notin @('completed','stopped_low_commit')) { throw "Run not ended: $receiptName" }
}
$selected = @($audit.targets | Where-Object { $_.eligible_for_explicit_removal })
if ($selected.Count -ne 4) { throw 'Unexpected cleanup target inventory' }
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
        if (-not $item.FullName.StartsWith($absolute, [System.StringComparison]::OrdinalIgnoreCase)) { throw 'Member escaped audited target' }
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
    $verified += [pscustomobject]@{ path=$absolute; files=$expected.Count; logical_bytes=$target.logical_bytes; reason=$target.reason; is_directory=(Get-Item -LiteralPath $absolute).PSIsContainer }
}
$freeBefore = (Get-PSDrive -Name C).Free
foreach ($target in $verified) {
    if ($target.is_directory) { Remove-Item -LiteralPath $target.path -Recurse -Force }
    else { Remove-Item -LiteralPath $target.path -Force }
    if (Test-Path -LiteralPath $target.path) { throw 'Cleanup target still exists' }
}
$freeAfter = (Get-PSDrive -Name C).Free
$record = [ordered]@{ status='OWNED_OBSOLETE_SCRATCH_REMOVED'; audit_sha256=(Get-FileHash -LiteralPath $auditPath -Algorithm SHA256).Hash.ToLowerInvariant(); deleted=$verified; logical_bytes_removed=($verified | Measure-Object -Property logical_bytes -Sum).Sum; drive_free_before_bytes=$freeBefore; drive_free_after_bytes=$freeAfter; drive_free_delta_bytes=($freeAfter-$freeBefore); protected='Raw originals, archives, ledgers, current144preparation units, extraction inputs, completed batches, candidate generations, evidence reports and foreign/active caches retained.' }
[System.IO.File]::WriteAllText($outPath, ($record | ConvertTo-Json -Depth 8), [System.Text.UTF8Encoding]::new($false))
$record | ConvertTo-Json -Depth 8 -Compress
