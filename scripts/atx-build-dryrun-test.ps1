$ErrorActionPreference = "Stop"

$helper = Join-Path $PSScriptRoot "atx-build.ps1"
$shell = Join-Path $PSHOME "powershell.exe"

$configureJson = & $shell -NoProfile -File $helper -DryRun -Preset dev -Groups atx_vol `
  configure "-DATX_VOL_COUNTERS=ON" "-DSPACE_VALUE=A B"
if ($LASTEXITCODE -ne 0) {
  throw "configure dry-run failed: $LASTEXITCODE"
}
$configure = $configureJson | ConvertFrom-Json
$expectedConfigure = @(
  "--preset",
  "dev",
  "-DATX_TEST_GROUPS=atx_vol",
  "-DATX_VOL_COUNTERS=ON",
  "-DSPACE_VALUE=A B"
)
if ($configure.executable -ne "cmake" -or
    (Compare-Object @($configure.arguments) $expectedConfigure -SyncWindow 0)) {
  throw "configure argv drifted: $($configureJson -join [Environment]::NewLine)"
}

$ctestJson = & $shell -NoProfile -File $helper -DryRun -Ctest -Jobs 1 -L atx_vol
if ($LASTEXITCODE -ne 0) {
  throw "ctest dry-run failed: $LASTEXITCODE"
}
$ctest = $ctestJson | ConvertFrom-Json
$expectedCtest = @(
  "--test-dir",
  (Join-Path (Split-Path -Parent $PSScriptRoot) "build"),
  "--output-on-failure",
  "-j",
  "1",
  "-L",
  "atx_vol"
)
if ($ctest.executable -ne "ctest" -or $ctest.ctest_jobs -ne 1 -or
    (Compare-Object @($ctest.arguments) $expectedCtest -SyncWindow 0)) {
  throw "ctest argv drifted: $($ctestJson -join [Environment]::NewLine)"
}

# ── FIX-I-4: -Preset must pick the binaryDir, for `build` as well as -Ctest ──
#
# `Get-PresetBinaryDir` is the root-cause fix (9457562) for the defect that cost
# this sprint its entire perf baseline: `build` and `-Ctest` hard-coded
# "$RepoRoot\build" and ignored -Preset, so `configure -Preset rel` wrote
# build-rel\ while `build <tgt>` rebuilt and handed back the DEBUG binary -- no
# error, because the target exists in both -- and the surface-db pilots were
# benchmarked ~15x slow on a fully unoptimized exe.
#
# Everything above this line exercises ONLY the default preset, where the answer
# is "build" whether Get-PresetBinaryDir works or is reverted to a string
# literal, and it never exercises the `build` verb at all. These three cases are
# the actual regression gate: the first two fail if -Preset stops resolving,
# the third pins the historical default so a fix cannot drift it either.
$repoRoot = Split-Path -Parent $PSScriptRoot
$cases = @(
  # Byte-for-byte the FIRST ctest case above except for `-Preset rel`, so the
  # only thing that can move the expected binaryDir is the preset resolution.
  # (`-L atx_vol` is carried over rather than dropped because `-Ctest` with no
  # trailing args leaves a stray $null in the argv -- pre-existing, cosmetic
  # under -DryRun, and not this fix's business.)
  @{ Name = "ctest -Preset rel";  Argv = @("-DryRun", "-Preset", "rel", "-Ctest", "-Jobs", "1", "-L", "atx_vol")
     Exe = "ctest";  Expected = @("--test-dir", (Join-Path $repoRoot "build-rel"), "--output-on-failure", "-j", "1", "-L", "atx_vol") }
  @{ Name = "build -Preset rel";  Argv = @("-DryRun", "-Preset", "rel", "build", "atx-vol-tests")
     Exe = "cmake";  Expected = @("--build", (Join-Path $repoRoot "build-rel"), "--target", "atx-vol-tests") }
  @{ Name = "build -Preset dev";  Argv = @("-DryRun", "-Preset", "dev", "build", "atx-vol-tests")
     Exe = "cmake";  Expected = @("--build", (Join-Path $repoRoot "build"), "--target", "atx-vol-tests") }
)
foreach ($case in $cases) {
  $caseArgv = @($case.Argv)   # bind to a variable so @caseArgv SPLATS (PS 5.1)
  $json = & $shell -NoProfile -File $helper @caseArgv
  if ($LASTEXITCODE -ne 0) {
    throw "$($case.Name) dry-run failed: $LASTEXITCODE"
  }
  $parsed = $json | ConvertFrom-Json
  if ($parsed.executable -ne $case.Exe -or
      (Compare-Object @($parsed.arguments) $case.Expected -SyncWindow 0)) {
    throw "$($case.Name) argv drifted (a -Preset regression sends the build to the wrong binaryDir): $($json -join [Environment]::NewLine)"
  }
}

Write-Output "atx-build dry-run argv assertions passed"
