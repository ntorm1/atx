#!/usr/bin/env bash
# AL-CLOCK build wrapper (HOST-2; copy of root's m2j-logs/tools/build.sh): waits until no other build runs on the
# host (no ninja.exe / clang-cl.exe / lld-link.exe process) and the research-build memory admission holds (free
# >= 1100 MiB, commit >= 2600 MiB), polling every 60 s for at most 90 minutes; then runs scripts/research-build.ps1
# once with the given tag, preset and targets (-Canary when CANARY=1). CCACHE_RECACHE=1 is exported when RECACHE=1
# (the lane-named stale-PCH remedy). Copies the receipt, build log, canary log and cache-stats log into
# alclock-logs/ and appends one line to alclock-logs/builds.txt.
# usage: [RECACHE=1] [CANARY=1] build.sh TAG PRESET TARGETS
set -u
tag=$1; preset=$2; targets=$3
root=/c/atx-wt/pool-30
logs=$root/.superpowers/sdd/platform-p9-20261003/alclock-logs
cd "$root" || exit 2
t0=$(date +%s); waited=0
while :; do
  busy=$(tasklist //FO CSV 2>/dev/null | grep -i -c -E '"(ninja|clang-cl|lld-link)\.exe"')
  mem=$(powershell -NoProfile -Command "\$os=Get-CimInstance Win32_OperatingSystem; \$m=Get-CimInstance Win32_PerfFormattedData_PerfOS_Memory; '' + [math]::Floor(\$os.FreePhysicalMemory/1024) + ' ' + [math]::Floor((\$m.CommitLimit-\$m.CommittedBytes)/1MB)" | tr -d '\r')
  free=${mem% *}; commit=${mem#* }
  if [ "$busy" -eq 0 ] && [ "$free" -ge 1100 ] && [ "$commit" -ge 2600 ]; then break; fi
  waited=$(( $(date +%s) - t0 ))
  echo "$(date -u +%FT%TZ) $tag waiting: other build procs $busy free $free commit $commit (waited ${waited}s)" >> "$logs/builds.txt"
  if [ "$waited" -ge 5400 ]; then echo "$tag GATE TIMEOUT after ${waited}s" | tee -a "$logs/builds.txt"; exit 75; fi
  sleep 60
done
echo "$(date -u +%FT%TZ) $tag start preset $preset free $free commit $commit recache ${RECACHE:-0} canary ${CANARY:-0} head $(git rev-parse --short HEAD) dirty $(git status --porcelain | wc -l)" >> "$logs/builds.txt"
if [ "${RECACHE:-0}" = 1 ]; then export CCACHE_RECACHE=1; fi
extra=""
if [ "${CANARY:-0}" = 1 ]; then extra="-Canary"; fi
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/research-build.ps1 -Tag "$tag" -Preset "$preset" \
  -Targets "$targets" $extra > "$logs/research-build-$tag.out" 2>&1
rc=$?
bdir=build-equity
for f in receipt.json build.log canary.log cache-stats.log; do
  [ -f "$bdir/mega-$tag-$f" ] && cp "$bdir/mega-$tag-$f" "$logs/"
done
warn=$(grep -c -i -E 'warning[: ]' "$bdir/mega-$tag-build.log" 2>/dev/null)
echo "$(date -u +%FT%TZ) $tag end exit $rc warning-lines ${warn:-?} wall $(( $(date +%s) - t0 ))s" >> "$logs/builds.txt"
tail -3 "$logs/builds.txt"
exit $rc
