#!/usr/bin/env bash
# M2f pytest runner: pyt.sh <Debug|Release> <log-name> <basetemp-tag> <test paths...>
# explicit paths, -p no:cacheprovider, own --basetemp in the session scratchpad, PYTHONDONTWRITEBYTECODE=1,
# ATX_EQUITY_BIN = the build type's bin (the engine verb atx-research-eval), ATX_ENGINE_STATS_REQUIRED=1 (as the lane),
# the build type's vcpkg DLL dir first on PATH.
set -u
BT=$1; LOG=$2; TAG=$3; shift 3
ROOT=C:/atx-wt/pool-2
SCR=C:/Users/natha/AppData/Local/Temp/claude/c--atx/6ea15be9-c1e8-4316-99d4-4854dfd7c03c/scratchpad
VC=/c/atx-cache/vcpkg_installed/x64-windows
if [ "$BT" = Debug ]; then BIN=$ROOT/build-equity/bin; FIRST=$VC/debug/bin; SECOND=$VC/bin
else BIN=$ROOT/build-equity-rel/bin; FIRST=$VC/bin; SECOND=$VC/debug/bin; fi
OUT=$ROOT/.superpowers/sdd/platform-p9-20261003/m2f-logs/$LOG
cd "$ROOT" || exit 9
rm -rf "$SCR/pyt-$TAG"
PATH="$FIRST:$SECOND:$PATH" PYTHONDONTWRITEBYTECODE=1 ATX_EQUITY_BIN="$BIN" ATX_ENGINE_STATS_REQUIRED=1 \
  "C:/Program Files/Python312/python.exe" -m pytest -q -rs -p no:cacheprovider --basetemp "$SCR/pyt-$TAG" "$@" > "$OUT" 2>&1
rc=$?
echo "$BT $LOG exit=$rc :: $(tail -1 "$OUT" | tr -d '\r')"
rm -rf "$SCR/pyt-$TAG"
