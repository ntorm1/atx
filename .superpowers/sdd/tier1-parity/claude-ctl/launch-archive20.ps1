# archive20 CompanyFacts resume (OPS-A 2026-09-25). Same flags as launch-archive19.ps1; predecessor is
# archive19's dataset UUID e27c8a4e-2d29-47bb-b657-fc1855ea4cec (closed failed 2026-09-25T10:35:44.937122Z by
# claude-ctl\close_companyfacts_archive19_session_kill.py; its params resume_from_run_id = a4942a4b..., lineage 17).
# Meant to be started DETACHED: Start-Process powershell -WindowStyle Hidden -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File',<this>
Set-Location C:\atx\atx-db
$py = 'C:\atx\atx-db\.venv\Scripts\python.exe'
$ctl = 'C:\atx\.superpowers\sdd\tier1-parity'
$ua = 'atx-db/0.1 atx-research@example.com'
$launch = "$ctl\activation-companyfacts-archive20-launch.json"
$launcherLog = "$ctl\activation-companyfacts-archive20-launcher.log"
if (Test-Path $launch) { throw "Refusing: $launch exists (use a new run name)" }
$meta = [ordered]@{ launcher_pid = $PID; started_utc = (Get-Date).ToUniversalTime().ToString('o'); predecessor = 'e27c8a4e-2d29-47bb-b657-fc1855ea4cec'; run_id = 'activation-companyfacts-archive20' }
[System.IO.File]::WriteAllText($launch, ($meta | ConvertTo-Json) + "`n", (New-Object System.Text.UTF8Encoding($false)))
& $py "$ctl\run_memory_guarded.py" --job-gb 1.5 --disk-path C:\atx\atx-db\data --min-free-disk-gb 3 `
  --receipt "$ctl\activation-companyfacts-archive20-memory.json" --stdout "$ctl\activation-companyfacts-archive20.log" --stderr "$ctl\activation-companyfacts-archive20.err" `
  -- $py scripts\warehouse_activate.py --db-path data\warehouse.duckdb --as-of-date 2026-09-20 --only companyfacts_load `
  --companyfacts-symbol-source archive_members --companyfacts-replace-existing --companyfacts-resume-from-run-id e27c8a4e-2d29-47bb-b657-fc1855ea4cec `
  --memory-limit 512MB --threads 1 --backup-keep 100 --force --run-id activation-companyfacts-archive20 --sec-user-agent $ua 2>&1 |
  ForEach-Object { "$_" } | Out-File -FilePath $launcherLog -Encoding utf8
$code = $LASTEXITCODE
"guard_exit=$code finished_utc=$((Get-Date).ToUniversalTime().ToString('o'))" | Out-File -FilePath $launcherLog -Append -Encoding utf8
exit $code
