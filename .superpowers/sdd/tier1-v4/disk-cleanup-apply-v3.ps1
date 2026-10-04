$ErrorActionPreference = 'Stop'
if (-not $env:ATX_GUARD_JOB) { throw 'External guard required' }
$workspace = 'C:\atx\.superpowers\sdd\tier1-v4'
$dataRoot = 'C:\atx\atx-db\data\alpha_panel\v1'
$outPath = Join-Path $workspace 'logs\disk-cleanup-applied-v3.json'
if (Test-Path -LiteralPath $outPath) { throw 'Preserve prior cleanup receipt' }
$freezePath = Join-Path $workspace 'logs\t16-fund-r19-freeze-v1.receipt.json'
$freeze = Get-Content -LiteralPath $freezePath -Raw | ConvertFrom-Json
if ($freeze.status -ne 'completed' -or $freeze.returncode -ne 0 -or $freeze.cap_hit) { throw 'Native source freeze has not completed' }
$expectedCommand = @('.venv/Scripts/python.exe', '-m', 'atx_db.alpha_panel.fund_batch_release', 'build', '--bindings', 'C:/atx/.superpowers/sdd/tier1-v4/task16-fund-reader-binding-v1.json', '--generation', 'r17_20261004_002', '--max-new-batches', '0', '--extract-admission', 'C:/atx/.superpowers/sdd/tier1-v4/task16-extract-admission-r19-v1.json', '--expected-admission-sha256', '168b3ff61f4580066db74624bf58efb803993e8d2a0f1060daf4dfec1024ab1f')
if (($freeze.command | ConvertTo-Json -Compress) -ne ($expectedCommand | ConvertTo-Json -Compress)) { throw 'Unexpected source-freeze command' }
foreach ($oldPid in @($freeze.guard_pid, $freeze.child_pid)) {
    if (Get-Process -Id $oldPid -ErrorAction SilentlyContinue) { throw 'Source-freeze PID still present; inspect before cleanup' }
}
$writers = @(Get-CimInstance Win32_Process -Filter "Name = 'python.exe'" | Where-Object {
    $_.CommandLine -like '*-m atx_db.alpha_panel.fund_batch_release*' -or
    $_.CommandLine -like '*-m atx_db.alpha_panel.fundamentals*'
})
if ($writers.Count) { throw 'A matching writer is still active' }
$snapshotPath = Join-Path $dataRoot 'fundamentals_release_v1\generations\r17_20261004_002\snapshot.json'
$snapshot = Get-Content -LiteralPath $snapshotPath -Raw
$normalizedSnapshot = $snapshot.Replace('\\', '/').ToLowerInvariant()
$protectedPaths = @(
    $snapshotPath,
    (Join-Path $workspace 'task16-fund-reader-binding-v1.json'),
    (Join-Path $workspace 'task16-extract-admission-r19-v1.json'),
    (Join-Path $workspace 'task16-extract-admission-r19-v1.audit.json')
)
$protectedPins = @{}
foreach ($protectedPath in $protectedPaths) {
    $protectedPins[$protectedPath] = (Get-FileHash -LiteralPath $protectedPath -Algorithm SHA256).Hash.ToLowerInvariant()
}
if ($protectedPins[(Join-Path $workspace 'task16-extract-admission-r19-v1.json')] -ne '168b3ff61f4580066db74624bf58efb803993e8d2a0f1060daf4dfec1024ab1f') { throw 'Independent admission changed' }
$allowed = @(
    'C:\atx\atx-db\data\alpha_panel\v1\fundamentals_release_v1\_spill',
    'C:\atx\atx-db\data\alpha_panel\v1\_tmp\fund-release-extract-verify'
)
$verified = @()
foreach ($target in $allowed) {
    $absolute = [IO.Path]::GetFullPath($target)
    if ($absolute -ne $target -or -not $absolute.StartsWith($dataRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Target escaped exact whitelist' }
    if (-not (Test-Path -LiteralPath $absolute)) { continue }
    if ((Resolve-Path -LiteralPath $absolute).Path -ne $absolute) { throw 'Resolved target mismatch' }
    $ancestor = Get-Item -LiteralPath $absolute -Force
    while ($null -ne $ancestor) {
        if ($ancestor.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Linked ancestor or target' }
        $ancestor = $ancestor.Parent
    }
    if ($normalizedSnapshot.Contains($absolute.Replace('\', '/').ToLowerInvariant())) { throw 'Frozen source snapshot references scratch target' }
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
    $verified += [pscustomobject]@{ path=$absolute; files=$files; logical_bytes=($files | Measure-Object bytes -Sum).Sum }
}
if ($verified.Count -ne 2) { throw 'Expected two existing owned scratch directories' }
$freeBefore = (Get-PSDrive -Name C).Free
foreach ($target in $verified) {
    Remove-Item -LiteralPath $target.path -Recurse -Force
    if (Test-Path -LiteralPath $target.path) { throw 'Scratch target remains' }
}
foreach ($protectedPath in $protectedPaths) {
    if ((Get-FileHash -LiteralPath $protectedPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $protectedPins[$protectedPath]) { throw 'Protected evidence changed' }
}
$freeAfter = (Get-PSDrive -Name C).Free
$record = [ordered]@{
    status='OWNED_ENDED_SCRATCH_REMOVED'; deleted=$verified;
    logical_bytes_removed=($verified | Measure-Object logical_bytes -Sum).Sum;
    drive_free_before_bytes=$freeBefore; drive_free_after_bytes=$freeAfter; drive_free_delta_bytes=($freeAfter-$freeBefore);
    native_source_freeze_receipt_sha256=(Get-FileHash -LiteralPath $freezePath -Algorithm SHA256).Hash.ToLowerInvariant();
    protected_sha256=$protectedPins;
    qualification='Cleanup only. All source/receipt/candidate paths and foreign caches retained. Repeat native source freeze must pass before financial construction.'
}
[IO.File]::WriteAllText($outPath, ($record | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
$record | Select-Object status,logical_bytes_removed,drive_free_before_bytes,drive_free_after_bytes,drive_free_delta_bytes,qualification | ConvertTo-Json -Compress
