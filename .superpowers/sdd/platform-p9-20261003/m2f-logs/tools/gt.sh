#!/usr/bin/env bash
# M2f gtest runner: gt.sh <Debug|Release> <exe-name> <log-name> [gtest filter]
# Runs <tree>/bin/<exe-name>.exe with the vcpkg DLL dir of the build type first on PATH, output to m2f-logs/<log-name>.
set -u
BT=$1; EXE=$2; LOG=$3; FILTER=${4:-}
ROOT=C:/atx-wt/pool-2
VC=/c/atx-cache/vcpkg_installed/x64-windows
if [ "$BT" = Debug ]; then TREE=$ROOT/build-equity; FIRST=$VC/debug/bin; SECOND=$VC/bin
else TREE=$ROOT/build-equity-rel; FIRST=$VC/bin; SECOND=$VC/debug/bin; fi
OUT=$ROOT/.superpowers/sdd/platform-p9-20261003/m2f-logs/$LOG
cd "$ROOT" || exit 9
if [ -n "$FILTER" ]; then
  PATH="$FIRST:$SECOND:$PATH" "$TREE/bin/$EXE.exe" --gtest_filter="$FILTER" > "$OUT" 2>&1
else
  PATH="$FIRST:$SECOND:$PATH" "$TREE/bin/$EXE.exe" > "$OUT" 2>&1
fi
rc=$?
echo "$BT $EXE ${FILTER:-<whole>} exit=$rc :: $(grep -a -E '^\[  PASSED  \]|^\[  FAILED  \]|^\[  SKIPPED \]|tests? from .* ran' "$OUT" | tr '\n' ' ')"
