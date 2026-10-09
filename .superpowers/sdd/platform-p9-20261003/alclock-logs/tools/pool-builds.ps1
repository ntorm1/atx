# AL-CLOCK (fix round 1; rulings HOST-4 / HOST-5): prints one line "<count> <free MiB> <commit MiB> <who>" where
# count is the number of build processes (ninja, cmake, cl, clang-cl, lld-link) whose command line names a path under
# C:\atx-wt\ (a pool build; a build of another session outside C:\atx-wt\ is foreign and not counted, HOST-5), and
# who lists the pools those processes name. Read-only: it touches no process.
$names = '^(ninja|cmake|cl|clang-cl|lld-link)\.exe$'
$procs = @(Get-CimInstance Win32_Process | Where-Object {
    $_.Name -match $names -and $_.CommandLine -and $_.CommandLine -match '(?i)c:[\\/]atx-wt[\\/]' })
$pools = @($procs | ForEach-Object {
    if ($_.CommandLine -match '(?i)atx-wt[\\/](pool-[0-9]+)') { $Matches[1] } else { 'atx-wt' } } |
  Sort-Object -Unique)
$os = Get-CimInstance Win32_OperatingSystem
$m = Get-CimInstance Win32_PerfFormattedData_PerfOS_Memory
$free = [math]::Floor($os.FreePhysicalMemory / 1024)
$commit = [math]::Floor(($m.CommitLimit - $m.CommittedBytes) / 1MB)
$who = if ($pools.Count) { $pools -join ',' } else { '-' }
Write-Output ("{0} {1} {2} {3}" -f $procs.Count, $free, $commit, $who)
