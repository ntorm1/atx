#!/usr/bin/env bash
# GATE (RESUME 12) build wrapper, copied from m6-logs/tools/build.sh (HOST-2 / HOST-4 / HOST-5), logs dir gate-logs:
# waits until no other build of C:\atx-wt\ runs on the host (tools/busy.ps1 counts ninja / cmake --build / clang-cl /
# lld-link and ignores a foreign build whose command line, or an ancestor's, names a path outside C:\atx-wt\) and the
# research-build memory admission holds (free >= 1000 MiB, commit >= 2500 MiB), polling every 60 s for at most 180
# minutes (HOST-4); then runs scripts/research-build.ps1 once with the given tag, preset and targets, with -Canary when
# the 4th argument is "canary" (G-P8; the gate's canaries, TEST-1). Copies the receipt, build log, cache-stats log and
# canary log into gate-logs/, counts warning lines in the UTF-16 build log (tools/warn_count.py) and appends one line
# to gate-logs/builds.txt. Never edit this file while a build runs (slip M3-S1).
# usage: build.sh TAG PRESET TARGETS [canary]
set -u
tag=$1; preset=$2; targets=$3; canary=${4:-}
root=/c/atx-wt/pool-2
logs=$root/.superpowers/sdd/platform-p9-20261003/gate-logs
cd "$root" || exit 2
t0=$(date +%s); waited=0
while :; do
  busy=$(powershell -NoProfile -ExecutionPolicy Bypass -File "$logs/tools/busy.ps1" | tr -d '\r')
  mem=$(powershell -NoProfile -Command "\$os=Get-CimInstance Win32_OperatingSystem; \$m=Get-CimInstance Win32_PerfFormattedData_PerfOS_Memory; '' + [math]::Floor(\$os.FreePhysicalMemory/1024) + ' ' + [math]::Floor((\$m.CommitLimit-\$m.CommittedBytes)/1MB)" | tr -d '\r')
  free=${mem% *}; commit=${mem#* }
  if [ "$busy" -eq 0 ] && [ "$free" -ge 1100 ] && [ "$commit" -ge 2600 ]; then break; fi
  waited=$(( $(date +%s) - t0 ))
  echo "$(date -u +%FT%TZ) $tag waiting: pool build procs $busy free $free commit $commit (waited ${waited}s)" >> "$logs/builds.txt"
  if [ "$waited" -ge 10800 ]; then echo "$tag GATE TIMEOUT after ${waited}s" | tee -a "$logs/builds.txt"; exit 75; fi
  sleep 60
done
extra=()
[ "$canary" = "canary" ] && extra=(-Canary)
echo "$(date -u +%FT%TZ) $tag start preset $preset ${extra[*]:-} free $free commit $commit head $(git rev-parse --short HEAD)" >> "$logs/builds.txt"
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/research-build.ps1 -Tag "$tag" -Preset "$preset" \
  -Targets "$targets" "${extra[@]}" > "$logs/research-build-$tag.out" 2>&1
rc=$?
for f in receipt.json build.log cache-stats.log canary.log; do
  [ -f "build-equity/mega-$tag-$f" ] && cp "build-equity/mega-$tag-$f" "$logs/"
done
warn=$("C:/Program Files/Python312/python.exe" "$logs/tools/warn_count.py" "build-equity/mega-$tag-build.log")
echo "$(date -u +%FT%TZ) $tag end exit $rc warning-lines ${warn:-?} wall $(( $(date +%s) - t0 ))s" >> "$logs/builds.txt"
tail -3 "$logs/builds.txt"
exit $rc
