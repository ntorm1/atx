# Controller executes only after granting the exclusive data slot. No HTTP.
$ErrorActionPreference='Stop'
Set-Location 'C:/atx/atx-db'
$env:PYTHONPATH='C:/atx/atx-db/src'
$env:OPENBLAS_NUM_THREADS='1'
$taskStem='t9-v4-cache-20261003'
$taskLog='../.superpowers/sdd/tier1-v4/logs'
foreach ($taskPhase in @('parse100','resume100','audit200')) {
    foreach ($taskEnding in @('receipt.json','out.log','err.log')) {
        if (Test-Path -LiteralPath "$taskLog/$taskStem-$taskPhase.$taskEnding") { throw 'Unique output path already exists; choose a new stem.' }
    }
}
$taskMap='../.superpowers/sdd/tier1-v4/t9-v4-frozen200-map-20261003.json'
if (Test-Path -LiteralPath $taskMap) { throw 'Audit map already exists; choose a new output path.' }
# 1. New v4 code/input pin; same original ordering. Expected ~20-40 seconds.
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.6 --wait-minutes 60 --quiet --receipt "$taskLog/$taskStem-parse100.receipt.json" --stdout "$taskLog/$taskStem-parse100.out.log" --stderr "$taskLog/$taskStem-parse100.err.log" -- .venv/Scripts/python.exe -m atx_db.alpha_panel.ea_actuals --phase parse --limit-new-documents 100
if ($LASTEXITCODE -ne 0) { throw 'Bounded parse failed; no retry.' }
$taskFirst=Get-Content -LiteralPath "$taskLog/$taskStem-parse100.out.log" -Raw | ConvertFrom-Json
if ($taskFirst.status -ne 'partial_cache_only' -or $taskFirst.published -ne $false -or $taskFirst.cache_counts.new_units -ne 100) { throw 'Unexpected parse state; stop for controller reconciliation.' }
# 2. True same-input resume, no new parsing. Expected ~10-25 seconds.
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.6 --wait-minutes 60 --quiet --receipt "$taskLog/$taskStem-resume100.receipt.json" --stdout "$taskLog/$taskStem-resume100.out.log" --stderr "$taskLog/$taskStem-resume100.err.log" -- .venv/Scripts/python.exe -m atx_db.alpha_panel.ea_actuals --phase parse --limit-new-documents 0
if ($LASTEXITCODE -ne 0) { throw 'Resume failed; no retry.' }
$taskResume=Get-Content -LiteralPath "$taskLog/$taskStem-resume100.out.log" -Raw | ConvertFrom-Json
if ($taskResume.run_key -ne $taskFirst.run_key -or $taskResume.published -ne $false -or $taskResume.cache_counts.cache_hits -lt 100 -or $taskResume.cache_counts.new_units -ne 0) { throw 'Unexpected resume reuse or drift; stop.' }
# 3. Metadata-only fixed 200 denominator and retained sample membership check.
# No raw-body reads/parsing; expected ~10-30 seconds, same 0.6 GiB cap.
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.6 --wait-minutes 60 --quiet --receipt "$taskLog/$taskStem-audit200.receipt.json" --stdout "$taskLog/$taskStem-audit200.out.log" --stderr "$taskLog/$taskStem-audit200.err.log" -- .venv/Scripts/python.exe ../.superpowers/sdd/tier1-v4/t9-frozen-audit-map.py --parse-receipt "$taskLog/$taskStem-parse100.out.log" --output $taskMap
if ($LASTEXITCODE -ne 0) { throw 'Frozen audit mapping failed; no retry or denominator replacement.' }
Get-Content -LiteralPath "$taskLog/$taskStem-audit200.out.log"
