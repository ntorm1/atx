# SIGN (HOST-4 / HOST-5): wait until no POOL C++ build runs and no higher-priority pool lane is waiting for one, bound
# 180 minutes. HOST-5: only builds under C:\atx-wt\ count (a ninja / cmake / clang-cl / lld-link process whose command
# line names a C:\atx-wt\ path, or a research-build.ps1 run); a build of another session (a command line naming
# C:\atx\ or any path outside C:\atx-wt\) is foreign: logged, never counted, never touched. A wait script of a
# higher-priority lane (root, B2, AL-CLOCK, COV-MV, D3W-PB: HOST-4) also holds SIGN back, so SIGN never jumps the
# queue. Quiet = two polls 60 s apart with nothing counted. Exit 0 when quiet, 1 at the bound.
param([Parameter(Mandatory = $true)][string]$Log, [int]$BoundMinutes = 180)
$build = '^(ninja|cmake|cl|clang-cl|clang|lld-link|link)\.exe$'
$higher = 'p9-(2[a-z]|b2|alclock|covmv|d3wpb)-|pool-(2|14|30|31|23)[\\/]'
$start = Get-Date
$quiet = 0
while (((Get-Date) - $start).TotalMinutes -lt $BoundMinutes) {
    $all = @(Get-CimInstance Win32_Process)
    $procs = @($all | Where-Object { $_.Name -match $build })
    $pool = @($procs | Where-Object { $_.CommandLine -match 'atx-wt' })
    $foreign = @($procs | Where-Object { $_.CommandLine -notmatch 'atx-wt' })
    $rb = @($all | Where-Object { $_.Name -eq 'powershell.exe' -and $_.CommandLine -match 'research-build\.ps1' })
    $waiters = @($all | Where-Object { $_.Name -match '^(powershell|python)\.exe$' -and
                       $_.CommandLine -match '(wait|build|teeth)' -and $_.CommandLine -match $higher -and
                       $_.CommandLine -notmatch 'pool-29' })
    $free = [math]::Round((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory / 1024)
    $n = $pool.Count + $rb.Count + $waiters.Count
    if ($n -eq 0) { $quiet += 1 } else { $quiet = 0 }
    $fmt = { param($xs) ($xs | ForEach-Object { "$($_.Name):$($_.ProcessId)" }) -join ' ' }
    $line = "{0} pool builds [{1}] research-build [{2}] higher waiters [{3}] foreign (ignored) [{4}] free MB {5} quiet {6}"
    Add-Content -Path $Log -Value ($line -f (Get-Date -Format s), (& $fmt $pool), (& $fmt $rb), (& $fmt $waiters),
        (& $fmt $foreign), $free, $quiet)
    if ($quiet -ge 2) { Add-Content -Path $Log -Value "QUIET"; exit 0 }
    Start-Sleep -Seconds 60
}
Add-Content -Path $Log -Value "BOUND reached ($BoundMinutes min)"
exit 1
