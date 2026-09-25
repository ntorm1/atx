param([string] $CheckPreset = "")
$ErrorActionPreference = "Stop"

$helper = Join-Path $PSScriptRoot "atx-build.ps1"
$shell = Join-Path $PSHOME "powershell.exe"
$savedBuildParallelLevel = $env:CMAKE_BUILD_PARALLEL_LEVEL
try {
$env:CMAKE_BUILD_PARALLEL_LEVEL = $null

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
     Exe = "cmake";  Expected = @("--build", (Join-Path $repoRoot "build-rel"), "--parallel", "1", "--target", "atx-vol-tests") }
  @{ Name = "build -Preset dev";  Argv = @("-DryRun", "-Preset", "dev", "build", "atx-vol-tests")
     Exe = "cmake";  Expected = @("--build", (Join-Path $repoRoot "build"), "--parallel", "1", "--target", "atx-vol-tests") }
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

# Build/check use the same bounded worker policy, but ctest deliberately ignores
# the build environment. These invoke the real script without compiling.
foreach ($case in @(
  @{ Env = "3"; Explicit = @(); Expected = 3 },
  @{ Env = "7"; Explicit = @("-Jobs", "2"); Expected = 2 },
  @{ Env = "invalid"; Explicit = @("-Jobs", "4"); Expected = 4 },
  @{ Env = ""; Explicit = @(); Expected = 1 }
)) {
  $env:CMAKE_BUILD_PARALLEL_LEVEL = $case.Env
  $argv = @("-DryRun", "-Preset", "equity-hygiene") + $case.Explicit + @("build", "atx-impl-tests")
  $json = & $shell -NoProfile -File $helper @argv
  if ($LASTEXITCODE -ne 0) { throw "build concurrency dry-run failed" }
  $parsed = $json | ConvertFrom-Json
  $expected = @("--build", (Join-Path $repoRoot "build-equity-hygiene"), "--parallel", "$($case.Expected)", "--target", "atx-impl-tests")
  if ($parsed.build_jobs -ne $case.Expected -or (Compare-Object @($parsed.arguments) $expected -SyncWindow 0)) {
    throw "build worker precedence/argv drifted: $json"
  }
}
$env:CMAKE_BUILD_PARALLEL_LEVEL = "invalid"
$ctestJson = & $shell -NoProfile -File $helper -DryRun -Ctest -L atx_vol
if ($LASTEXITCODE -ne 0 -or ($ctestJson | ConvertFrom-Json).ctest_jobs -ne 1) {
  throw "ctest must keep its default worker count independently of build environment"
}

foreach ($bad in @("0", "-1", "3.5", "garbage", "257", "99999999999999999999")) {
  $env:CMAKE_BUILD_PARALLEL_LEVEL = $bad
  $ErrorActionPreference = "Continue"
  $null = & $shell -NoProfile -File $helper -DryRun build atx-impl-tests 2>&1
  $code = $LASTEXITCODE
  $ErrorActionPreference = "Stop"
  if ($code -eq 0) { throw "invalid worker limit '$bad' was accepted" }
}
$env:CMAKE_BUILD_PARALLEL_LEVEL = $null
foreach ($argv in @(
  @("-DryRun", "build"),
  @("-DryRun", "-Jobs", "1", "build", "atx-impl-tests", "--parallel", "8"),
  @("-DryRun", "build", "atx-impl-tests", "-j8"),
  @("-DryRun", "build", "atx-impl-tests", "-j+4"),
  @("-DryRun", "build", "atx-impl-tests", "-j=4"),
  @("-DryRun", "build", "atx-impl-tests", "--jobs=8")
)) {
  $ErrorActionPreference = "Continue"
  $null = & $shell -NoProfile -File $helper @argv 2>&1
  $code = $LASTEXITCODE
  $ErrorActionPreference = "Stop"
  if ($code -eq 0) { throw "bare build or overriding worker flag was accepted" }
}

if ($CheckPreset) {
  foreach ($case in @(
    @{ Env = "2"; Explicit = @(); Expected = 2 },
    @{ Env = "bad"; Explicit = @("-Jobs", "1"); Expected = 1 }
  )) {
    $env:CMAKE_BUILD_PARALLEL_LEVEL = $case.Env
    $argv = @("-DryRun", "-Preset", $CheckPreset) + $case.Explicit + @("check",
      "atx-impl/src/stage_equity_baseline.cpp", "atx-impl/src/stage_equity_book.cpp")
    $json = & $shell -NoProfile -File $helper @argv
    if ($LASTEXITCODE -ne 0) { throw "multi-TU check dry-run failed" }
    $parsed = $json | ConvertFrom-Json
    if ($parsed.executable -ne "ninja" -or $parsed.build_jobs -ne $case.Expected -or
        $parsed.arguments[2] -ne "-j" -or $parsed.arguments[3] -ne "$($case.Expected)" -or
        $parsed.arguments.Count -ne 6) {
      throw "check must cap both selected objects and their dependency closure: $json"
    }
  }
  Write-Output "atx-build multi-TU check worker assertions passed ($CheckPreset)"
}
Write-Output "atx-build dry-run argv assertions passed"
}
finally {
  $env:CMAKE_BUILD_PARALLEL_LEVEL = $savedBuildParallelLevel
}
