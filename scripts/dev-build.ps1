<#
.SYNOPSIS
  One-shot build/test helper that imports the VS 2022 Developer environment
  (clang-cl + ninja + MSVC) into THIS process, then runs CMake.

  PowerShell shell state does not persist across separate tool invocations, so
  the VS dev env must be imported in the same process that runs the build. This
  script does both. Always run the whole pipeline you need in one call, e.g.:

    pwsh -File scripts/dev-build.ps1 -Configure -Build -Test
    pwsh -File scripts/dev-build.ps1 -Build -Test -TestRegex ring_buffer_test
    pwsh -File scripts/dev-build.ps1 -Build -Bench

.NOTES
  Exits non-zero on the first failing stage. Prints "DEV-BUILD OK" on success.
#>
param(
  [switch]$Configure,
  [switch]$Build,
  [switch]$Bench,
  [switch]$Test,
  [string]$TestRegex = ""
)

$ErrorActionPreference = "Stop"

$vs = "C:\Program Files\Microsoft Visual Studio\2022\Community"
Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null

$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

function Invoke-Stage([scriptblock]$cmd, [string]$name) {
  Write-Output "=== $name ==="
  & $cmd
  if ($LASTEXITCODE -ne 0) { Write-Output "STAGE FAILED: $name (exit $LASTEXITCODE)"; exit $LASTEXITCODE }
}

if ($Configure) { Invoke-Stage { cmake --preset ninja } "configure" }
if ($Build)     { Invoke-Stage { cmake --build --preset ninja } "build" }
if ($Bench)     { Invoke-Stage { cmake --build --preset ninja --target atx-core-bench } "bench-build" }
if ($Test) {
  if ($TestRegex) {
    Invoke-Stage { ctest --preset ninja -R $TestRegex --output-on-failure } "test ($TestRegex)"
  } else {
    Invoke-Stage { ctest --preset ninja --output-on-failure } "test"
  }
}

Write-Output "DEV-BUILD OK"
