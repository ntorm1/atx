$ErrorActionPreference = 'Stop'
if (-not $env:ATX_GUARD_JOB) { throw 'External guard required' }
$workspace = 'C:\atx\.superpowers\sdd\tier1-v4'
$dataRoot = 'C:\atx\atx-db\data\alpha_panel\v1'
$outPath = Join-Path $workspace 'logs\disk-cleanup-applied-v4.json'
if (Test-Path -LiteralPath $outPath) { throw 'Preserve prior cleanup receipt' }
$nativePath = Join-Path $workspace 'logs\t16-universe-r15-resume-20261004-v2.receipt.json'
$native = Get-Content -LiteralPath $nativePath -Raw | ConvertFrom-Json
if ($native.status -ne 'completed' -or $native.returncode -ne 0 -or $native.cap_hit) { throw 'Universe continuation has not completed successfully' }
$expectedCommand = @('.venv/Scripts/python.exe', '-m', 'atx_db.alpha_panel.universe_release', 'build', '--bindings', 'C:/atx/.superpowers/sdd/tier1-v4/task16-reader-binding-v1.json', '--generation', 'r15_20261003_001', '--max-new-years', '2')
if (($native.command | ConvertTo-Json -Compress) -ne ($expectedCommand | ConvertTo-Json -Compress)) { throw 'Unexpected universe command' }
foreach ($oldPid in @($native.guard_pid, $native.child_pid, 1792)) {
    if (Get-Process -Id $oldPid -ErrorAction SilentlyContinue) { throw 'Prior native PID present; inspect before cleanup' }
}
$writers = @(Get-CimInstance Win32_Process -Filter "Name = 'python.exe'" | Where-Object { $_.CommandLine -like '*-m atx_db.alpha_panel.universe_release*' })
if ($writers.Count) { throw 'Matching universe writer active' }
$auditPath = Join-Path $workspace 't16-universe-progress-audit-v2.json'
$audit = Get-Content -LiteralPath $auditPath -Raw | ConvertFrom-Json
if ($audit.status -ne 'FIVE_BOUNDED_NATIVE_YEARS_IDENTITIES_VERIFIED' -or $audit.years.Count -ne 5) { throw 'Independent year audit missing' }
$protectedPins = @{}
foreach ($entry in $audit.evidence.PSObject.Properties) {
    $actual = (Get-FileHash -LiteralPath $entry.Name -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $entry.Value.sha256 -or (Get-Item -LiteralPath $entry.Name).Length -ne $entry.Value.bytes) { throw 'Admitted universe evidence changed' }
    $protectedPins[$entry.Name] = $actual
}
foreach ($path in @($auditPath, (Join-Path $workspace 'task16-reader-binding-v1.json'))) {
    $protectedPins[$path] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
}
$snapshotPath = Join-Path $dataRoot 'universe_release_v1\generations\r15_20261003_001\snapshot.json'
$normalizedSnapshot = (Get-Content -LiteralPath $snapshotPath -Raw).Replace('\\', '/').ToLowerInvariant()
$target = 'C:\atx\atx-db\data\alpha_panel\v1\_tmp\universe-release-r15_20261003_001'
$absolute = [IO.Path]::GetFullPath($target)
if ($absolute -ne $target -or -not $absolute.StartsWith($dataRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Target escaped exact owned whitelist' }
if (-not (Test-Path -LiteralPath $absolute) -or (Resolve-Path -LiteralPath $absolute).Path -ne $absolute) { throw 'Missing or mismatched scratch target' }
if ($normalizedSnapshot.Contains($absolute.Replace('\', '/').ToLowerInvariant())) { throw 'Frozen source snapshot references scratch' }
$ancestor = Get-Item -LiteralPath $absolute -Force
while ($null -ne $ancestor) {
    if ($ancestor.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Linked target or ancestor' }
    $ancestor = $ancestor.Parent
}
$files = @()
$pending = [Collections.Generic.Stack[string]]::new()
$pending.Push($absolute)
while ($pending.Count) {
    $item = Get-Item -LiteralPath $pending.Pop() -Force
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Linked scratch member' }
    if ($item.FullName -ne $absolute -and -not $item.FullName.StartsWith($absolute + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Scratch member escaped target' }
    if ($item.PSIsContainer) {
        foreach ($child in Get-ChildItem -LiteralPath $item.FullName -Force) { $pending.Push($child.FullName) }
    } else {
        $files += [pscustomobject]@{ path=$item.FullName; bytes=$item.Length; modified_utc=$item.LastWriteTimeUtc.ToString('o') }
        if ($files.Count -gt 20000) { throw 'Scratch inventory exceeds bound' }
    }
}
$logicalBytes = ($files | Measure-Object bytes -Sum).Sum
$freeBefore = (Get-PSDrive -Name C).Free
Remove-Item -LiteralPath $absolute -Recurse -Force
if (Test-Path -LiteralPath $absolute) { throw 'Owned scratch target remains' }
foreach ($path in $protectedPins.Keys) {
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $protectedPins[$path]) { throw 'Protected evidence changed during cleanup' }
}
$freeAfter = (Get-PSDrive -Name C).Free
$record = [pscustomobject]@{
    status='OWNED_ENDED_UNIVERSE_SCRATCH_REMOVED'; target=$absolute; files=$files;
    logical_bytes_removed=$logicalBytes; drive_free_before_bytes=$freeBefore;
    drive_free_after_bytes=$freeAfter; drive_free_delta_bytes=($freeAfter-$freeBefore);
    protected_sha256=$protectedPins;
    qualification='Only the ended named universe generation scratch database/spill directory removed. All source and admitted output/receipt bytes retained and checked before/after. No readiness or economic gate change.'
}
[IO.File]::WriteAllText($outPath, ($record | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
$record | Select-Object status,logical_bytes_removed,drive_free_before_bytes,drive_free_after_bytes,drive_free_delta_bytes,qualification | ConvertTo-Json -Compress
