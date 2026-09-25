# archive21 CompanyFacts resume (OPS-A 2026-09-25, RX3): runs the PINNED git-archive export of c69ef10e
# (atx-db/src identical to 7080a478; 393/393 blobs verified) instead of the live tree other agents are editing.
# Predecessor = archive20 dataset UUID 4c69bc32-60a7-4354-adea-f1ee6c096eec (closed failed 11:59:28.111190Z by
# claude-ctl\close_companyfacts_archive20_headroom_stop.py; its resume_from_run_id = e27c8a4e..., lineage 18).
# cwd MUST be C:\atx\atx-db and --cache-dir MUST stay the default relative data\cache: the chain's
# params_json.companyfacts_zip and every receipt cache_path is the literal 'data\cache\companyfacts.zip', and
# _companyfacts_resume._lineage / verify_companyfacts_resume compare those strings exactly. Only --db-path is absolute.
# Start DETACHED: Start-Process powershell -WindowStyle Hidden -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File',<this>
Set-Location C:\atx\atx-db
$py = 'C:\atx\atx-db\.venv\Scripts\python.exe'
$ctl = 'C:\atx\.superpowers\sdd\tier1-parity'
$exp = "$ctl\exports\c69ef10e\atx-db"
$ua = 'atx-db/0.1 atx-research@example.com'
$launch = "$ctl\activation-companyfacts-archive21-launch.json"
$launcherLog = "$ctl\activation-companyfacts-archive21-launcher.log"
if (Test-Path $launch) { throw "Refusing: $launch exists (use a new run name)" }
$env:PYTHONPATH = "$exp\src"
$importProof = & $py -c "import sys; sys.path.insert(0, r'$exp\src'); import atx_db, atx_db.activation, atx_db.fundamentals, atx_db._companyfacts_resume; print(atx_db.__file__ + '|' + atx_db.fundamentals.__file__ + '|' + atx_db._companyfacts_resume.__file__)"
if ($importProof -notlike "$exp\src\atx_db\__init__.py|*") { throw "Import proof failed: $importProof" }
$meta = [ordered]@{
  launcher_pid = $PID; started_utc = (Get-Date).ToUniversalTime().ToString('o')
  predecessor = '4c69bc32-60a7-4354-adea-f1ee6c096eec'; run_id = 'activation-companyfacts-archive21'
  code_commit = 'c69ef10e033740190f759c435086d8cf2172e60b'; export = $exp; pythonpath = $env:PYTHONPATH
  cwd = (Get-Location).Path; import_proof = $importProof
}
[System.IO.File]::WriteAllText($launch, ($meta | ConvertTo-Json) + "`n", (New-Object System.Text.UTF8Encoding($false)))
& $py "$ctl\run_memory_guarded.py" --job-gb 1.5 --disk-path C:\atx\atx-db\data --min-free-disk-gb 3 `
  --receipt "$ctl\activation-companyfacts-archive21-memory.json" --stdout "$ctl\activation-companyfacts-archive21.log" --stderr "$ctl\activation-companyfacts-archive21.err" `
  -- $py "$exp\scripts\warehouse_activate.py" --db-path C:\atx\atx-db\data\warehouse.duckdb --as-of-date 2026-09-20 --only companyfacts_load `
  --companyfacts-symbol-source archive_members --companyfacts-replace-existing --companyfacts-resume-from-run-id 4c69bc32-60a7-4354-adea-f1ee6c096eec `
  --memory-limit 512MB --threads 1 --backup-keep 100 --force --run-id activation-companyfacts-archive21 --sec-user-agent $ua 2>&1 |
  ForEach-Object { "$_" } | Out-File -FilePath $launcherLog -Encoding utf8
$code = $LASTEXITCODE
"guard_exit=$code finished_utc=$((Get-Date).ToUniversalTime().ToString('o'))" | Out-File -FilePath $launcherLog -Append -Encoding utf8
exit $code
