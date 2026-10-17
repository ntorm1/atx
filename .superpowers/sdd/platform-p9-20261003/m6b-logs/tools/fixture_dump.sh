#!/usr/bin/env bash
# M6b (RESUME 11) SQL4-S S-1: the fixture-chain catalog (copy of gate0-logs/tools/fixture_chain.sh, rel root only)
# plus `dump --table` of every catalog table into <scratch>/<tag>-dump/, so the pre-merge and post-merge exes' rows can
# be compared table by table (the cause of a digest change). usage: fixture_dump.sh <scratch-dir> <tag> <logs-dir>
set -u
SCR=$1; TAG=$2; LOGS=$3
ROOT=C:/atx-wt/pool-2
EXE=$ROOT/build-equity/bin/atx-research-store.exe
VC=/c/atx-cache/vcpkg_installed/x64-windows
export PATH="$VC/debug/bin:$VC/bin:$PATH" PYTHONDONTWRITEBYTECODE=1
PY="C:/Program Files/Python312/python.exe"
cd "$ROOT" || exit 9
mkdir -p "$SCR/$TAG-dump" "$LOGS/$TAG-dump"
echo "exe sha256 $(sha256sum "$EXE" | cut -c1-16) head $(git rev-parse --short=8 HEAD)"
R=atx-engine/tests/fixtures/research_store/tree
DB=$SCR/$TAG-cat.sqlite
rm -f "$DB" "$DB-wal" "$DB-shm"
"$EXE" init --catalog "$DB" > /dev/null; echo "init $?"
"$EXE" catalog --catalog "$DB" --root "$R" --classes atx-engine/schemas/research_store/classes.json | grep -E "files_seen|catalog_digest"; echo "catalog ${PIPESTATUS[0]}"
"$EXE" verify --catalog "$DB" --pins > "$SCR/$TAG-verify.txt" 2>&1; echo "verify --pins $?"
"$PY" atx-engine/tools/research_store_identity.py --catalog "$DB" --root "$R" | tail -1; echo "identity ${PIPESTATUS[0]}"
for t in store_info catalog_run artifact artifact_seen skipped_path producer run run_command run_binding run_start \
         stage_receipt cycle_binding cycle_verdict wave_result wave_timing spec_doc pin trial_line ledger_state \
         field_manifest field_entry candidate candidate_event build_receipt build_exe; do
  "$EXE" dump --catalog "$DB" --table "$t" > "$LOGS/$TAG-dump/$t.txt" 2>&1 || echo "dump $t exit $?"
done
echo "dumped $(ls "$LOGS/$TAG-dump" | wc -l) tables"
