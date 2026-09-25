# Read-only liveness evidence for activation-companyfacts-archive20 after its guard stopped (stopped_low_headroom).
# Writes .superpowers\sdd\tier1-parity\activation-companyfacts-archive20-process-check.json (refuses overwrite).
$ErrorActionPreference = 'Stop'
$ctl = 'C:\atx\.superpowers\sdd\tier1-parity'
$out = "$ctl\activation-companyfacts-archive20-process-check.json"
if (Test-Path $out) { throw "Refusing to overwrite $out" }
$db = 'C:\atx\atx-db\data\warehouse.duckdb'
$pids = 11544, 16160, 9060, 14644, 19636   # launcher, venv shim, guard, guard child_pid (shim), worker
$procs = @(Get-CimInstance Win32_Process -Filter "Name like 'python%'" | Select-Object ProcessId, ParentProcessId, CommandLine)
$matching = @($procs | Where-Object { $_.CommandLine -match 'warehouse_activate|run_memory_guarded|close_companyfacts' })
$original = @($pids | ForEach-Object { Get-Process -Id $_ -ErrorAction SilentlyContinue })
$launcherTail = (Get-Content "$ctl\activation-companyfacts-archive20-launcher.log" -Tail 1)
$exit = if ($launcherTail -match 'guard_exit=(-?\d+)') { [int]$Matches[1] } else { $null }
$exclusive = $false; $exclusiveError = $null
try { $fs = [System.IO.File]::Open($db, 'Open', 'Read', 'None'); $fs.Close(); $exclusive = $true } catch { $exclusiveError = $_.Exception.Message }
$wal = Get-Item "$db.wal" -ErrorAction SilentlyContinue
$dbItem = Get-Item $db
$result = [ordered]@{
  observed_at_utc = (Get-Date).ToUniversalTime().ToString('o')
  run_id = 'activation-companyfacts-archive20'
  matching_process_count = $matching.Count
  original_pid_count = $original.Count
  original_pids = $pids
  session_exit_code = $exit
  launcher_log_tail = $launcherTail
  matching_pids = @($matching | ForEach-Object { $_.ProcessId })
  warehouse_exclusive_open_ok = $exclusive
  warehouse_exclusive_open_error = $exclusiveError
  warehouse_bytes = $dbItem.Length
  warehouse_mtime_utc = $dbItem.LastWriteTimeUtc.ToString('o')
  wal_bytes = if ($wal) { $wal.Length } else { $null }
  concurrent_python = @($procs | Where-Object { $_.CommandLine -match 'pytest|bench' } | ForEach-Object { ([string]$_.CommandLine).Substring(0, [Math]::Min(140, ([string]$_.CommandLine).Length)) })
}
$json = $result | ConvertTo-Json -Depth 5
[System.IO.File]::WriteAllText($out, $json + "`n", (New-Object System.Text.UTF8Encoding($false)))
Write-Output $json
