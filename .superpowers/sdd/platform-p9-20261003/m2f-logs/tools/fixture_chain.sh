#!/usr/bin/env bash
# M2e SQL2-CLS fixture chain (as M2a-M2d): atx-research-store init / catalog / digest / verify --pins and
# research_store_identity.py on the committed fixture tree, with a relative and an absolute --root, into scratch
# catalogs. usage: fixture_chain.sh <scratch-dir> <tag>
set -u
SCR=$1; TAG=$2
ROOT=C:/atx-wt/pool-2
EXE=$ROOT/build-equity/bin/atx-research-store.exe
VC=/c/atx-cache/vcpkg_installed/x64-windows
export PATH="$VC/debug/bin:$VC/bin:$PATH" PYTHONDONTWRITEBYTECODE=1
PY="C:/Program Files/Python312/python.exe"
cd "$ROOT" || exit 9
mkdir -p "$SCR"
echo "exe sha256 $(sha256sum "$EXE" | cut -c1-16) head $(git rev-parse --short=8 HEAD)"
for spell in rel abs; do
  if [ $spell = rel ]; then R=atx-engine/tests/fixtures/research_store/tree; else R=$ROOT/atx-engine/tests/fixtures/research_store/tree; fi
  DB=$SCR/$TAG-cat-$spell.sqlite
  rm -f "$DB" "$DB-wal" "$DB-shm"
  echo "== $spell root=$R"
  "$EXE" init --catalog "$DB"; echo "init $?"
  "$EXE" catalog --catalog "$DB" --root "$R" --classes atx-engine/schemas/research_store/classes.json; echo "catalog $?"
  "$EXE" digest --catalog "$DB"; echo "digest $?"
  "$EXE" verify --catalog "$DB" --pins > "$SCR/$TAG-verify-$spell.txt" 2>&1; echo "verify $?"
  "$PY" atx-engine/tools/research_store_identity.py --catalog "$DB" --root "$R" | tail -1; echo "identity ${PIPESTATUS[0]}"
done
