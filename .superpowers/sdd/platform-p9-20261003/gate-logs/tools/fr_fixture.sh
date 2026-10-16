#!/usr/bin/env bash
# root FLAGS agent (RESUME 14; FLAGS-ROUTE step "the fixture chain digest"): the form of gate agent 4's
# fixture-chain-{4,5,6}.txt (m6b-logs/tools/fixture_dump.sh's catalog / verify / identity lines, without the table
# dumps): the Release store exe p9-3f, two fresh catalogs of the store fixture tree
# atx-engine/tests/fixtures/research_store/tree, both DBs OUTSIDE the work tree (SQL3-2), `verify --pins` and the
# render identity on the second. Prints exit codes, files_seen / catalog_digest lines and the identity summary only.
# usage: fr_fixture.sh <scratch-dir outside the tree> <header text>
set -u
SCR=$1; HDR=$2
ROOT=C:/atx-wt/pool-2
EXE=$ROOT/build-equity-rel/bin/atx-research-store.exe
VC=/c/atx-cache/vcpkg_installed/x64-windows
export PATH="$VC/bin:$PATH" PYTHONDONTWRITEBYTECODE=1
PY="C:/Program Files/Python312/python.exe"
cd "$ROOT" || exit 9
R=atx-engine/tests/fixtures/research_store/tree
echo "$HDR $(date -u +%Y-%m-%dT%H:%M:%SZ) head $(git rev-parse --short=8 HEAD)"
for n in 1 2; do
  DB=$SCR/fx-$n.sqlite
  if [ -e "$DB" ]; then echo "catalog $n: $DB exists: stop"; exit 8; fi
  echo "store exe $(sha256sum "$EXE" | cut -c1-16) catalog $n:"
  "$EXE" init --catalog "$DB" > /dev/null; echo "  init $?"
  "$EXE" catalog --catalog "$DB" --root "$R" --classes atx-engine/schemas/research_store/classes.json \
    | grep -E "files_seen|catalog_digest" | sed 's/^/  /'
  echo "  catalog exit ${PIPESTATUS[0]}"
done
"$EXE" verify --catalog "$SCR/fx-2.sqlite" --pins > "$SCR/fx-2-verify.txt" 2>&1; echo "  verify --pins $?"
echo "  identity: $("$PY" atx-engine/tools/research_store_identity.py --catalog "$SCR/fx-2.sqlite" --root "$R" | tail -1)"
