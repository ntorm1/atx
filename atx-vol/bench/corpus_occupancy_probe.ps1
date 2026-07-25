<#
.SYNOPSIS
  T1 (BT-T1) — corpus fan-out worker-occupancy probe. Produces the "phase-split log"
  exit criterion 7 names, plus the MEAN WORKER OCCUPANCY that criterion's ">= 14/16"
  threshold is stated in.

.DESCRIPTION
  WHY AN EXTERNAL PROBE AND NOT A COUNTER.

  The shipped corpus instrumentation (CorpusPhaseTimings, corpus.hpp) records the inner
  fit-worker budget the scheduler OFFERS -- `inner_worker_slots` is a raw sum over every
  budget resolution and `reclaimed_inner_boards` a distinct-board count. corpus.hpp:417-419
  says so explicitly: "It is a raw sum, NOT a per-board mean; dividing it by `boards_fitted`
  does not give a mean inner width." The PHASE line carries no resolution count either, so
  no mean can be formed from those fields at all.

  More importantly, OFFERED width is the wrong quantity even if it were available. The
  elastic-budget design's own correctness proof (corpus.cpp:670-681) is that at any instant
  the m running boards' inner slices "sum to at most outer_budget" -- i.e. offered width is
  ~constant at the budget BY CONSTRUCTION. A board offered 4 inner workers only USES them
  if its fit actually reaches a fan-out with >= 4 independent tasks. Reading offered width
  would therefore report ~16/16 on a run that is in fact running one core hot, which is the
  precise failure mode BT-T1 exists to detect.

  So this probe measures REALIZED occupancy: mean busy cores = (process CPU time consumed)
  / (wall time elapsed), sampled from outside the process. That is what "mean workers" means
  physically and it is the same quantity the cited ~9/16 baseline is in.

  CONTENTION SENSITIVITY (stated, because it decides whether a number is citable). This
  quantity is NOT contention-immune. Foreign load deschedules our threads: our CPU time
  falls AND our wall time rises, so both terms move the ratio down. A contended reading is
  therefore a LOWER BOUND -- it can only understate occupancy, never flatter it. Run it on a
  proven-quiet box; if it must be run under load, a reading at or above target is still
  sound while a reading below target is inconclusive.

  PHASE DECOMPOSITION. The run is not all fan-out: parquet ingest, archive write and
  checkpoint I/O are essentially serial. The PHASE line splits them, so the fan-out's own
  occupancy is recovered by charging the serial phases one core:

      occ_fanout ~= (cpu_total_s - serial_wall_s) / fit_fanout_s
      serial_wall_s = ingest_s + archive_write_s + checkpoint_s + other_s

  Both the whole-run occupancy and this fan-out-restricted estimate are reported; they
  bracket the answer and the gap between them is itself diagnostic.

.EXAMPLE
  pwsh atx-vol/bench/corpus_occupancy_probe.ps1 `
      -Exe build-rel-avx2/bin/atxvol_spy_dispersion_backtest.exe `
      -Spec scratch-bench/t1-in/run_spec.tsv -OutDir scratch-bench/t1-20d -Reps 3
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)][string] $Exe,
  [Parameter(Mandatory = $true)][string] $Spec,
  [Parameter(Mandatory = $true)][string] $OutDir,
  [int] $Reps = 3,
  [int] $DateBatch = 0,          # 0 = leave ATX_VOL_CORPUS_DATE_BATCH unset (shipped default)
  [int] $FitWorkers = -1,        # -1 = leave ATX_VOL_FIT_WORKERS unset
  [int] $SampleMs = 100,
  [string] $OutTsv = ""
)

$ErrorActionPreference = "Stop"
$exePath  = (Resolve-Path $Exe).Path
$specPath = (Resolve-Path $Spec).Path
$specDir  = Split-Path -Parent $specPath
$outRoot  = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $OutDir))
$cores = [int]$env:NUMBER_OF_PROCESSORS

$rows = @()
foreach ($rep in 1..$Reps) {
  # Cold build every rep: an existing run directory makes the next invocation a
  # resume rather than the full fan-out this probe exists to measure.
  if (Test-Path $outRoot) { Remove-Item $outRoot -Recurse -Force }

  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = $exePath
  $psi.Arguments = "build-corpus --spec `"$specPath`" --out `"$outRoot`""
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $psi.WorkingDirectory = $specDir
  $psi.EnvironmentVariables["ATX_VOL_CORPUS_PHASE_TIMING"] = "1"
  if ($DateBatch -gt 0)  { $psi.EnvironmentVariables["ATX_VOL_CORPUS_DATE_BATCH"] = "$DateBatch" }
  if ($FitWorkers -ge 0) { $psi.EnvironmentVariables["ATX_VOL_FIT_WORKERS"] = "$FitWorkers" }

  $proc = New-Object System.Diagnostics.Process
  $proc.StartInfo = $psi
  # Async drain: a full stdout pipe would deadlock the child.
  $sbOut = New-Object System.Text.StringBuilder
  $sbErr = New-Object System.Text.StringBuilder
  $onOut = Register-ObjectEvent -InputObject $proc -EventName OutputDataReceived -Action {
    if ($EventArgs.Data) { [void]$Event.MessageData.AppendLine($EventArgs.Data) }
  } -MessageData $sbOut
  $onErr = Register-ObjectEvent -InputObject $proc -EventName ErrorDataReceived -Action {
    if ($EventArgs.Data) { [void]$Event.MessageData.AppendLine($EventArgs.Data) }
  } -MessageData $sbErr

  # ── Per-rep contention evidence ────────────────────────────────────────────
  # This probe's quantity is NOT contention-immune (see .DESCRIPTION), so every rep
  # carries its own proof rather than relying on a single check taken before the
  # batch. `foreign_cpu_s` is the CPU time every OTHER process on the box consumed
  # while this rep ran; `foreign_cores` expresses it as mean busy cores. A rep with
  # foreign_cores near zero was measured on a quiet box and is citable on its own
  # evidence; a rep with foreign load is reported, not silently averaged in.
  $cpuOfOthers = {
    param($selfId)
    $t = 0.0
    foreach ($p in (Get-Process -ErrorAction SilentlyContinue)) {
      if ($p.Id -eq $selfId -or $p.Id -eq $PID) { continue }
      try { if ($p.CPU) { $t += $p.CPU } } catch { }
    }
    return $t
  }

  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  [void]$proc.Start()
  $foreignStart = & $cpuOfOthers $proc.Id
  $proc.BeginOutputReadLine()
  $proc.BeginErrorReadLine()

  # Occupancy time series: instantaneous busy-core count between consecutive samples.
  $series = @()
  $prevCpu = [TimeSpan]::Zero
  $prevEl  = [TimeSpan]::Zero
  $peakWs  = 0L
  while (-not $proc.HasExited) {
    Start-Sleep -Milliseconds $SampleMs
    try {
      $proc.Refresh()
      $cpu = $proc.TotalProcessorTime
      $el  = $sw.Elapsed
      $dC  = ($cpu - $prevCpu).TotalSeconds
      $dW  = ($el  - $prevEl ).TotalSeconds
      if ($dW -gt 0) { $series += [math]::Round($dC / $dW, 3) }
      $prevCpu = $cpu; $prevEl = $el
      if ($proc.WorkingSet64 -gt $peakWs) { $peakWs = $proc.WorkingSet64 }
    } catch { }   # process may exit between HasExited and Refresh
  }
  $cpuTotal = $proc.TotalProcessorTime.TotalSeconds
  $foreignEnd = & $cpuOfOthers $proc.Id
  $proc.WaitForExit()
  $sw.Stop()
  $wall = $sw.Elapsed.TotalSeconds
  $foreignCpu = [math]::Max(0.0, $foreignEnd - $foreignStart)
  Unregister-Event -SourceIdentifier $onOut.Name; Unregister-Event -SourceIdentifier $onErr.Name
  $stdout = $sbOut.ToString(); $stderr = $sbErr.ToString()
  if ($proc.ExitCode -ne 0) {
    Write-Host "rep $rep FAILED exit=$($proc.ExitCode)" -ForegroundColor Red
    Write-Host $stdout; Write-Host $stderr
    throw "build-corpus failed on rep $rep"
  }

  # Parse the PHASE line (dispersion_run.cpp:848-858; every field is name=value).
  $phase = ($stdout -split "`n" | Where-Object { $_ -match '^PHASE ' } | Select-Object -First 1)
  if (-not $phase) { throw "no PHASE line in rep $rep output (is ATX_VOL_CORPUS_PHASE_TIMING honored?)" }
  $f = @{}
  foreach ($kv in ($phase.Trim() -replace '^PHASE ', '') -split '\s+') {
    $p = $kv -split '=', 2
    if ($p.Count -eq 2) { $f[$p[0]] = [double]$p[1] }
  }

  $serialWall = $f['ingest_s'] + $f['archive_write_s'] + $f['checkpoint_s'] + $f['other_s']
  $occWhole   = if ($wall -gt 0) { $cpuTotal / $wall } else { 0 }
  $occFanout  = if ($f['fit_fanout_s'] -gt 0) { ($cpuTotal - $serialWall) / $f['fit_fanout_s'] } else { 0 }

  $rows += [pscustomobject]@{
    rep                = $rep
    wall_s             = [math]::Round($wall, 3)
    cpu_s              = [math]::Round($cpuTotal, 3)
    occ_whole_run      = [math]::Round($occWhole, 3)
    occ_fanout         = [math]::Round($occFanout, 3)
    occ_sampled_mean   = if ($series.Count) { [math]::Round((($series | Measure-Object -Average).Average), 3) } else { 0 }
    occ_sampled_p95    = if ($series.Count) { [math]::Round((($series | Sort-Object)[[int][math]::Floor(0.95 * ($series.Count - 1))]), 3) } else { 0 }
    samples            = $series.Count
    cores              = $cores
    ingest_s           = $f['ingest_s']
    build_s            = $f['build_s']
    fit_fanout_s       = $f['fit_fanout_s']
    archive_write_s    = $f['archive_write_s']
    checkpoint_s       = $f['checkpoint_s']
    other_s            = $f['other_s']
    fanout_calls       = $f['fanout_calls']
    boards             = $f['boards']
    date_batch         = $f['date_batch']
    reclaimed          = $f['reclaimed']
    inner_slots        = $f['inner_slots']
    peak_ws_mb         = [math]::Round($peakWs / 1MB, 1)
    foreign_cpu_s      = [math]::Round($foreignCpu, 2)
    foreign_cores      = if ($wall -gt 0) { [math]::Round($foreignCpu / $wall, 3) } else { 0 }
    quiet              = ($foreignCpu / [math]::Max($wall, 1e-9)) -lt 0.25
    phase_line         = $phase.Trim()
  }
  Write-Host ("rep {0}: wall={1:N1}s cpu={2:N1}s occ_whole={3:N2}/{4} occ_fanout={5:N2}/{4} boards={6} fanout_calls={7} reclaimed={8} foreign={9:N2} cores" -f `
    $rep, $wall, $cpuTotal, $occWhole, $cores, $occFanout, $f['boards'], $f['fanout_calls'], $f['reclaimed'], ($foreignCpu / [math]::Max($wall,1e-9))) -ForegroundColor Cyan
}

$rows | Format-Table rep, wall_s, cpu_s, occ_whole_run, occ_fanout, occ_sampled_mean, fit_fanout_s, boards, fanout_calls, reclaimed, inner_slots, foreign_cores, quiet -AutoSize
$occ = $rows.occ_fanout
"SPREAD occ_fanout (ALL reps): min={0:N2} median={1:N2} max={2:N2} (n={3}, cores={4})" -f `
  ($occ | Measure-Object -Minimum).Minimum, (($occ | Sort-Object)[[int][math]::Floor($occ.Count/2)]), `
  ($occ | Measure-Object -Maximum).Maximum, $occ.Count, $cores
$q = @($rows | Where-Object { $_.quiet })
if ($q.Count -gt 0 -and $q.Count -lt $rows.Count) {
  $qo = $q.occ_fanout
  "SPREAD occ_fanout (QUIET reps only): min={0:N2} median={1:N2} max={2:N2} (n={3})" -f `
    ($qo | Measure-Object -Minimum).Minimum, (($qo | Sort-Object)[[int][math]::Floor($qo.Count/2)]), `
    ($qo | Measure-Object -Maximum).Maximum, $qo.Count
}
"QUIET REPS: {0} of {1}" -f $q.Count, $rows.Count
if ($OutTsv) { $rows | Export-Csv -Path $OutTsv -Delimiter "`t" -NoTypeInformation -Encoding utf8; "wrote $OutTsv" }
