<#
.SYNOPSIS
  Build a SurfaceDb one session per process, enforcing the per-session time cap.

.DESCRIPTION
  ONE PROCESS PER DAY, DELIBERATELY. A single `--from A --to B` invocation would
  be simpler and is wrong here for three reasons:

    * The operator cap is per SESSION ("cap a day fit at 10 minutes; above that
      something is wrong and should be killed and investigated"). One process
      spanning ten days can only be capped in aggregate, so the day that blew
      the budget is invisible and killing it destroys nine good days' work.
    * Discovery mode RETAINS every date's decoded parquet table for the panel
      pass (`opra_hive.cpp` moves each `tbl` into `di.table`), so a ten-day
      window holds ten boards resident. One day per process bounds that to one.
    * A cap breach leaves the offending day visibly ABSENT rather than
      half-written, and the nine others still land.

  Measured on the 2026-08 full-OPRA corpus: 120-142 s per session for
  5,197-5,246 loaded cells, so no day comes within four times the cap.

.PARAMETER CarryLoo
  Overrides `DeAmOptions::max_carry_leave_one_out` (ATX_VOL_CARRY_LOO, default
  0.005 in annualized borrow-RATE units).

.PARAMETER CarryMoneynessShift
  Overrides `DeAmOptions::max_carry_moneyness_shift` (ATX_VOL_CARRY_MNY_SHIFT,
  default 0.01 in MONEYNESS units).

  Both are the tier-1 and tier-2 carry-anchor gates and are the measured binding
  constraint on thin-chain coverage. Left unset they are not exported at all, so
  a run is bit-identical to the shipped defaults; a wrong value would otherwise
  be indistinguishable from a fit change.

.EXAMPLE
  powershell atx-vol\tools\build_surface_days.ps1 -DbRoot C:/atx-data/surface-db/prodv1 `
      -Hive C:/atx-data/opra-all -Log C:/atx-data/logs/prodv1

.EXAMPLE
  # the widened-carry arm, everything else identical so a delta is attributable
  powershell atx-vol\tools\build_surface_days.ps1 -DbRoot C:/atx-data/surface-db/prodv1-carry `
      -Hive C:/atx-data/opra-all -Log C:/atx-data/logs/prodv1c `
      -CarryLoo 0.02 -CarryMoneynessShift 0.04
#>
param(
  # NOT named `Db`: that is the built-in alias for the -Debug common
  # parameter, and PowerShell refuses the whole script with
  # `ParameterNameConflictsWithAlias` before running a line of it.
  [Parameter(Mandatory = $true)][string]$DbRoot,
  [Parameter(Mandatory = $true)][string]$Hive,
  [Parameter(Mandatory = $true)][string]$Log,
  [string]$Exe = "C:/atx/build-rel/bin/atx-vol-surface-db-build.exe",
  [string]$Preset = "populate",
  [string]$Rate = "0.043",
  [string]$SnapshotSuffix = "T19:55:00Z",
  [string]$Spots = "",
  [int]$CapSeconds = 600,
  [string]$From = "",
  [string]$To = "",
  [double]$CarryLoo = 0,
  [double]$CarryMoneynessShift = 0
)

# Windows command-line quoting, as CommandLineToArgvW parses it. Needed because
# `ProcessStartInfo.Arguments` is a single string on .NET Framework: a database
# or hive path containing a space would otherwise split into two arguments and
# the build would be handed a truncated path.
function Quote-Arg([string]$Value) {
  if ($Value -eq '') { return '""' }
  if ($Value -notmatch '[ \t"]') { return $Value }
  # Backslashes are literal EXCEPT immediately before a quote, where each must be
  # doubled; the run that precedes the closing quote is doubled for the same
  # reason. This is the rule the CRT documents, not an approximation of it.
  $sb = New-Object System.Text.StringBuilder
  $null = $sb.Append('"')
  $slashes = 0
  foreach ($ch in $Value.ToCharArray()) {
    if ($ch -eq '\') { $slashes++; continue }
    if ($ch -eq '"') {
      $null = $sb.Append('\', $slashes * 2 + 1).Append('"')
    } else {
      $null = $sb.Append('\', $slashes).Append($ch)
    }
    $slashes = 0
  }
  $null = $sb.Append('\', $slashes * 2).Append('"')
  return $sb.ToString()
}

# Exported ONLY when set: an unset gate must leave the process environment
# untouched so the run is bit-identical to the shipped defaults.
if ($CarryLoo -gt 0) { $env:ATX_VOL_CARRY_LOO = "$CarryLoo" }
if ($CarryMoneynessShift -gt 0) { $env:ATX_VOL_CARRY_MNY_SHIFT = "$CarryMoneynessShift" }

New-Item -ItemType Directory -Force $Log | Out-Null

$dates = Get-ChildItem $Hive -Directory -Filter "date=*" |
  ForEach-Object { $_.Name -replace '^date=', '' } | Sort-Object
if ($From) { $dates = $dates | Where-Object { $_ -ge $From } }
if ($To)   { $dates = $dates | Where-Object { $_ -le $To } }
if (-not $dates) { Write-Error "no date= partitions under $Hive in the requested window"; exit 2 }

$summary = @()
foreach ($d in $dates) {
  $rep = Join-Path $Log "report_$d.csv"
  $out = Join-Path $Log "build_$d.log"
  $argList = @("--db", $DbRoot, "--hive", $Hive, "--from", $d, "--to", $d,
               "--preset", $Preset, "--r", $Rate,
               "--snapshot-suffix", $SnapshotSuffix, "--fit-workers", "0", "--report", $rep)
  if ($Spots) { $argList += @("--spots", $Spots) }

  # NOT Start-Process. `Start-Process -PassThru` combined with output
  # redirection returns a Process object whose ExitCode is NEVER populated on
  # Windows PowerShell 5.1 -- verified directly: `cmd /c exit 7` launched that
  # way reports an EMPTY exit code after WaitForExit(ms) AND after the
  # argument-less WaitForExit(), while the same command through
  # System.Diagnostics.Process reports 7. A blank exit status makes a failing
  # session look exactly like a passing one, which is the whole point of running
  # day by day, so the launcher uses .NET directly.
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = $Exe
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  # `.Arguments`, not `.ArgumentList`: the latter is .NET Core / .NET 5+ only and
  # Windows PowerShell 5.1 runs on .NET Framework 4.x, where the property does
  # not exist -- verified on this host (CLR 4.0.30319). So the list has to be
  # joined into ONE command-line string with Windows quoting applied by hand.
  $psi.Arguments = ($argList | ForEach-Object { Quote-Arg $_ }) -join ' '

  $sw = [Diagnostics.Stopwatch]::StartNew()
  $p = [System.Diagnostics.Process]::Start($psi)
  # Drain BOTH pipes concurrently before waiting. A build writes ~315 KB of
  # stdout; leaving either pipe unread deadlocks the child on a full buffer long
  # before the cap would fire, and the cap would then report a spurious breach.
  $stdout = $p.StandardOutput.ReadToEndAsync()
  $stderr = $p.StandardError.ReadToEndAsync()
  if (-not $p.WaitForExit($CapSeconds * 1000)) {
    # A cap breach is an INVESTIGATION trigger, not a retry: kill the tree and
    # record it, so the day is visibly absent instead of silently half-written.
    Write-Output "CAP-BREACH $d killed at ${CapSeconds}s"
    taskkill /PID $p.Id /T /F | Out-Null
    $summary += [pscustomobject]@{ date = $d; secs = $CapSeconds; exit = "CAP-BREACH" }
    continue
  }
  $p.WaitForExit()
  $sw.Stop()
  [System.IO.File]::WriteAllText($out, $stdout.Result)
  [System.IO.File]::WriteAllText("$out.err", $stderr.Result)
  $secs = [math]::Round($sw.Elapsed.TotalSeconds, 1)
  Write-Output "$d exit=$($p.ExitCode) secs=$secs"
  $summary += [pscustomobject]@{ date = $d; secs = $secs; exit = $p.ExitCode }
}

$summary | Export-Csv -NoTypeInformation -Encoding utf8 (Join-Path $Log "timing.csv")
$breaches = @($summary | Where-Object { $_.exit -eq "CAP-BREACH" }).Count
$failures = @($summary | Where-Object { $_.exit -is [int] -and $_.exit -ne 0 }).Count
Write-Output ""
Write-Output "$($summary.Count) session(s): $breaches cap-breach, $failures non-zero exit"
if ($breaches -gt 0 -or $failures -gt 0) { exit 1 }
