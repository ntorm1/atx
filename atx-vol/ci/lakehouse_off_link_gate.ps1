<#
.SYNOPSIS
  CI gate (Task E3, sprint Step 2): ATX_VOL_LAKEHOUSE=OFF still configures and
  LINKS cleanly (the minimal-install configuration: no research Parquet track
  store, no track_compact CLI).

.DESCRIPTION
  `ATX_VOL_LAKEHOUSE` (root CMakeLists.txt, default ON) gates
  src/track_store.cpp, src/catalog.cpp, src/sweep_driver.cpp,
  src/track_compact_reconcile.cpp, src/track_gc.cpp (and their gtest files,
  atx-vol/tests/CMakeLists.txt) out of the build entirely when OFF -- their
  headers (track_store.hpp, catalog.hpp, sweep_driver.hpp) stay declared
  unconditionally (Tier-B surface since the E2 promotion), so a caller that
  reaches for one of those symbols in an OFF build would fail to LINK, never
  to compile. D2 verified this leg once by hand (task-D2-report.md);this gate
  makes it a standing, re-runnable check instead of tribal knowledge.

  Uses the dedicated `dev-lakehouse-off` preset (CMakePresets.json, added by
  this task) -- its own binaryDir (build-lakehouse-off/) so this NEVER
  disturbs whatever is warm in the default `dev` build/ directory. Same
  toolchain/deps as `dev` (the flag touches no compiler flags or third-party
  deps), so no FETCHCONTENT_BASE_DIR isolation is needed.

  Builds BOTH `atx-vol` (the library) and `atx-vol-tests` (the actual
  consumer that would surface a missing-symbol link error from an
  accidentally-unconditional test file) and then runs one no-op-cost sanity
  test from the resulting binary to prove it is not just linked but
  executable.
#>
$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$Preset = "dev-lakehouse-off"

Write-Host "[lakehouse-off-gate] configuring -Preset $Preset ..." -ForegroundColor Cyan
& powershell.exe -NoProfile -File (Join-Path $RepoRoot "scripts\atx-build.ps1") configure -Preset $Preset
if ($LASTEXITCODE -ne 0) {
  Write-Host "[lakehouse-off-gate] FAIL: configure failed (exit $LASTEXITCODE)." -ForegroundColor Red
  exit 1
}

foreach ($target in @("atx-vol", "atx-vol-tests")) {
  Write-Host "[lakehouse-off-gate] building $target ..." -ForegroundColor Cyan
  & powershell.exe -NoProfile -File (Join-Path $RepoRoot "scripts\atx-build.ps1") build -Preset $Preset $target
  if ($LASTEXITCODE -ne 0) {
    Write-Host "[lakehouse-off-gate] FAIL: $target failed to build/link under ATX_VOL_LAKEHOUSE=OFF (exit $LASTEXITCODE)." -ForegroundColor Red
    exit 1
  }
}

# track_compact must not even exist as a ninja target when the flag is OFF
# (atx-vol/CMakeLists.txt gates its add_executable the same way).
$ninjaGraph = Join-Path $RepoRoot "build-lakehouse-off\build.ninja"
if (Test-Path $ninjaGraph) {
  $hasTrackCompact = Select-String -Path $ninjaGraph -Pattern "track_compact" -Quiet
  if ($hasTrackCompact) {
    Write-Host "[lakehouse-off-gate] FAIL: track_compact target present in the OFF-config ninja graph -- ATX_VOL_LAKEHOUSE guard regressed." -ForegroundColor Red
    exit 1
  }
}

$exe = Join-Path $RepoRoot "build-lakehouse-off\bin\atx-vol-tests.exe"
if (-not (Test-Path $exe)) {
  Write-Host "[lakehouse-off-gate] FAIL: atx-vol-tests.exe not found after a reported-successful build." -ForegroundColor Red
  exit 1
}
Write-Host "[lakehouse-off-gate] sanity-running one unconditional (Arrow-free) test..." -ForegroundColor Cyan
$output = & $exe "--gtest_filter=TrackKeyTest.SameConfigTwiceIsIdentical" 2>&1
$output | ForEach-Object { Write-Host "  $_" }
if ($LASTEXITCODE -ne 0) {
  Write-Host "[lakehouse-off-gate] FAIL: the linked OFF-config test binary did not run cleanly (exit $LASTEXITCODE)." -ForegroundColor Red
  exit 1
}

Write-Host ""
Write-Host "[lakehouse-off-gate] PASS: atx-vol + atx-vol-tests configure, link, and run cleanly with ATX_VOL_LAKEHOUSE=OFF." -ForegroundColor Green
exit 0
