#!/usr/bin/env bash
# R9 helper: wait (bounded) for a quieter host -- no clang-cl / lld-link / ninja process and mean CPU < 65 % over
# three samples -- then run one c3b-pyt.sh invocation. Appends the host state at launch to r9-host.txt.
# usage: r9-quiet-run.sh MAX_MINUTES LABEL SEED BASETEMP LOG -- pytest-args...   (env passes through)
set -u
max=$1; shift
logs=/c/atx-wt/pool-15/.superpowers/sdd/platform-p9-20261003/c3-logs
probe() {
  powershell -NoProfile -Command '$c=(1..3|%{(Get-CimInstance Win32_Processor|Measure-Object LoadPercentage -Average).Average;Start-Sleep 2}|Measure-Object -Average).Average; $b=@(Get-Process|?{$_.ProcessName -match "^(clang-cl|lld-link|ninja)$"}).Count; $f=[math]::Round((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory/1KB); "{0} {1} {2}" -f [math]::Round($c),$b,$f' | tr -d '\r'
}
t0=$(date +%s); state=""; quiet=no
while :; do
  state=$(probe); read -r cpu builds free <<< "$state"
  if [ "$builds" -eq 0 ] && [ "$cpu" -lt 65 ]; then quiet=yes; break; fi
  [ $(( $(date +%s) - t0 )) -ge $(( max * 60 )) ] && break
  sleep 30
done
waited=$(( $(date +%s) - t0 ))
echo "$1 launch $(date +%FT%T) waited ${waited}s quiet=$quiet cpu% ${cpu} build-procs ${builds} free-MiB ${free}" >> "$logs/r9-host.txt"
exec bash "$logs/c3b-pyt.sh" "$@"
