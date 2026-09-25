# Read-only liveness evidence for activation-companyfacts-archive19 (killed with controller session 1).
# Writes .superpowers\sdd\tier1-parity\activation-companyfacts-archive19-process-check.json (refuses overwrite).
$ErrorActionPreference = 'Stop'
$ctl = 'C:\atx\.superpowers\sdd\tier1-parity'
$out = "$ctl\activation-companyfacts-archive19-process-check.json"
if (Test-Path $out) { throw "Refusing to overwrite $out" }
$db = 'C:\atx\atx-db\data\warehouse.duckdb'
$procs = @(Get-CimInstance Win32_Process -Filter "Name like 'python%'" | Select-Object ProcessId, ParentProcessId, CommandLine)
$matching = @($procs | Where-Object { $_.CommandLine -match 'warehouse_activate|run_memory_guarded|companyfacts-archive19|close_companyfacts' })
$original = @(Get-Process -Id 10272 -ErrorAction SilentlyContinue)
$exclusive = $false; $exclusiveError = $null
try { $fs = [System.IO.File]::Open($db, 'Open', 'Read', 'None'); $fs.Close(); $exclusive = $true } catch { $exclusiveError = $_.Exception.Message }
$wal = Get-Item "$db.wal" -ErrorAction SilentlyContinue
$dbItem = Get-Item $db
$result = [ordered]@{
  observed_at_utc = (Get-Date).ToUniversalTime().ToString('o')
  run_id = 'activation-companyfacts-archive19'
  matching_process_count = $matching.Count
  original_pid_count = $original.Count
  original_child_pid = 10272
  session_exit_code = $null
  termination_cause = 'controller session 1 closed; guard and child killed without guard terminal status (memory receipt stale status=running)'
  matching_pids = @($matching | ForEach-Object { $_.ProcessId })
  python_processes = @($procs | ForEach-Object { [ordered]@{ pid = $_.ProcessId; ppid = $_.ParentProcessId; cmd_head = ([string]$_.CommandLine).Substring(0, [Math]::Min(120, ([string]$_.CommandLine).Length)) } })
  warehouse_exclusive_open_ok = $exclusive
  warehouse_exclusive_open_error = $exclusiveError
  warehouse_bytes = $dbItem.Length
  warehouse_mtime_utc = $dbItem.LastWriteTimeUtc.ToString('o')
  wal_bytes = if ($wal) { $wal.Length } else { $null }
  wal_mtime_utc = if ($wal) { $wal.LastWriteTimeUtc.ToString('o') } else { $null }
}
$json = $result | ConvertTo-Json -Depth 5
[System.IO.File]::WriteAllText($out, $json + "`n", (New-Object System.Text.UTF8Encoding($false)))
Write-Output $json
