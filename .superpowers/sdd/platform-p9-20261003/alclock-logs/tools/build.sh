#!/usr/bin/env bash
# AL-CLOCK build wrapper (HOST-2; copy of root's m2j-logs/tools/build.sh; fix round 1: rulings HOST-4 / HOST-5).
# Waits until no POOL build runs on the host (tools/pool-builds.ps1: ninja / cmake / cl / clang-cl / lld-link whose
# command line names a path under C:\atx-wt\; another session's build outside C:\atx-wt\ is foreign and not counted,
# HOST-5) for 60 s (two polls 30 s apart, both quiet) and research-build's own memory admission holds (free >= 1000
# MiB, commit >= 2500 MiB: its thresholds, never lowered); bound 180 minutes in all (HOST-4). Then runs
# scripts/research-build.ps1 once with the given tag, preset and targets (-Canary when CANARY=1); a "memory
# admission" refusal of the script is waited out the same way inside the bound, never bypassed. CCACHE_RECACHE=1 is
# exported when RECACHE=1 (the lane-named stale-PCH remedy). Copies the receipt, build log, canary log and
# cache-stats log into alclock-logs/ and appends lines to alclock-logs/builds.txt.
# usage: [RECACHE=1] [CANARY=1] build.sh TAG PRESET TARGETS
set -u
tag=$1; preset=$2; targets=$3
root=/c/atx-wt/pool-30
logs=$root/.superpowers/sdd/platform-p9-20261003/alclock-logs
probe="$logs/tools/pool-builds.ps1"
bound=10800
cd "$root" || exit 2
t0=$(date +%s)
wait_quiet() {
  local quiet=0 line busy free commit who waited
  while :; do
    line=$(powershell -NoProfile -ExecutionPolicy Bypass -File "$probe" | tr -d '\r')
    read -r busy free commit who <<< "$line"
    if [ "$busy" -eq 0 ] && [ "$free" -ge 1000 ] && [ "$commit" -ge 2500 ]; then
      quiet=$((quiet + 1))
      [ "$quiet" -ge 2 ] && { echo "$free $commit"; return 0; }
    else
      quiet=0
      waited=$(( $(date +%s) - t0 ))
      echo "$(date -u +%FT%TZ) $tag waiting: pool build procs $busy ($who) free $free commit $commit (waited ${waited}s)" >> "$logs/builds.txt"
    fi
    if [ $(( $(date +%s) - t0 )) -ge "$bound" ]; then return 75; fi
    sleep 30
  done
}
if [ "${RECACHE:-0}" = 1 ]; then export CCACHE_RECACHE=1; fi
extra=""
if [ "${CANARY:-0}" = 1 ]; then extra="-Canary"; fi
while :; do
  mem=$(wait_quiet) || { echo "$(date -u +%FT%TZ) $tag GATE TIMEOUT after $(( $(date +%s) - t0 ))s" | tee -a "$logs/builds.txt"; exit 75; }
  echo "$(date -u +%FT%TZ) $tag start preset $preset free/commit $mem recache ${RECACHE:-0} canary ${CANARY:-0} head $(git rev-parse --short HEAD) dirty $(git status --porcelain | wc -l)" >> "$logs/builds.txt"
  powershell -NoProfile -ExecutionPolicy Bypass -File scripts/research-build.ps1 -Tag "$tag" -Preset "$preset" \
    -Targets "$targets" $extra > "$logs/research-build-$tag.out" 2>&1
  rc=$?
  if [ "$rc" -ne 0 ] && grep -q "memory admission" "$logs/research-build-$tag.out"; then
    echo "$(date -u +%FT%TZ) $tag research-build refused: $(grep -o 'memory admission[^"]*' "$logs/research-build-$tag.out" | head -1); waiting" >> "$logs/builds.txt"
    sleep 60
    continue
  fi
  break
done
bdir=build-equity
for f in receipt.json build.log canary.log cache-stats.log; do
  [ -f "$bdir/mega-$tag-$f" ] && cp "$bdir/mega-$tag-$f" "$logs/"
done
warn=$(grep -c -i -E 'warning[: ]' "$bdir/mega-$tag-build.log" 2>/dev/null)
echo "$(date -u +%FT%TZ) $tag end exit $rc warning-lines ${warn:-?} wall $(( $(date +%s) - t0 ))s" >> "$logs/builds.txt"
tail -3 "$logs/builds.txt"
exit $rc
