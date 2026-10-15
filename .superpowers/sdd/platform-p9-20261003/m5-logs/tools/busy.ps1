# Root M5 (RESUME 11; copied from m4-logs/tools) build-wait helper, ruling HOST-5: prints the number of build processes (ninja, cmake,
# clang-cl, lld-link) that count toward the one-build rule. A process is FOREIGN, and is not counted, when its own
# command line names a path outside C:\atx-wt\ (e.g. `cmake --build C:\atx\build-vol`), or when one of its ancestors
# is such a process (the foreign ninja's own command line names no path; its compilers inherit from it).
$names = @('ninja.exe', 'cmake.exe', 'clang-cl.exe', 'lld-link.exe')
$all = @(Get-CimInstance Win32_Process)
$byId = @{}
foreach ($p in $all) { $byId[[int]$p.ProcessId] = $p }
function Test-Outside([string] $cmd) {
  if (-not $cmd) { return $false }
  return [regex]::IsMatch($cmd, '(?i)[a-z]:[\\/]atx[\\/]')
}
function Test-Foreign($p) {
  $seen = @{}
  $cur = $p
  for ($i = 0; $i -lt 64 -and $cur -ne $null; $i++) {
    $id = [int]$cur.ProcessId
    if ($seen.ContainsKey($id)) { break }
    $seen[$id] = $true
    if ($names -contains $cur.Name.ToLower() -and (Test-Outside $cur.CommandLine)) { return $true }
    $parent = [int]$cur.ParentProcessId
    if (-not $byId.ContainsKey($parent)) { break }
    $cur = $byId[$parent]
  }
  return $false
}
$count = 0
foreach ($p in $all) {
  if ($names -contains $p.Name.ToLower()) {
    if ($p.Name.ToLower() -eq 'cmake.exe' -and $p.CommandLine -notmatch '--build') { continue }
    if (-not (Test-Foreign $p)) { $count++ }
  }
}
Write-Output $count
