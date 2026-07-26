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

Write-Output "atx-build dry-run argv assertions passed"
