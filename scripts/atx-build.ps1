<#
.SYNOPSIS
  Build/test helper that sources the MSVC environment (vcvars64) and puts the
  VS-bundled Ninja on PATH, then forwards its arguments to cmake (or, with
  -Ctest, to ctest in build/).

  Needed because the `ninja` preset uses clang-cl + the Ninja generator, both of
  which require the MSVC dev environment (INCLUDE/LIB/PATH from vcvars64) that a
  plain shell does not have. ninja.exe ships inside the VS install, not on PATH.

.EXAMPLE
  # Configure (data test group only) + build the data tests:
  pwsh scripts/atx-build.ps1 configure -Groups data -Bench
  pwsh scripts/atx-build.ps1 build atx-engine-data-tests

.EXAMPLE
  # Run the ORATS tests:
  pwsh scripts/atx-build.ps1 -Ctest -R DataOratsHistory
#>
[CmdletBinding(PositionalBinding = $false)]
param(
  [Parameter(ValueFromRemainingArguments = $true)]
  [string[]] $Args,
  [switch] $Ctest,
  [switch] $Bench,
  [string] $Groups = ""
)

$ErrorActionPreference = "Stop"

$VsRoot   = "C:\Program Files\Microsoft Visual Studio\2022\Community"
$VcVars   = Join-Path $VsRoot "VC\Auxiliary\Build\vcvars64.bat"
$NinjaDir = Join-Path $VsRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
$RepoRoot = Split-Path -Parent $PSScriptRoot

if (-not (Test-Path $VcVars))   { throw "vcvars64.bat not found at $VcVars" }
if (-not (Test-Path $NinjaDir)) { throw "Ninja dir not found at $NinjaDir" }

# Build the inner command. Everything runs inside one cmd.exe session so the env
# vcvars64 sets (INCLUDE/LIB/PATH) is live for the cmake/ctest invocation.
$verb = if ($Args.Count -gt 0) { $Args[0] } else { "" }
$rest = if ($Args.Count -gt 1) { $Args[1..($Args.Count - 1)] } else { @() }

if ($Ctest) {
  # ctest only runs the built exes (DLLs are applocal-staged beside them), so it
  # needs neither vcvars nor Ninja — invoke it directly to avoid cmd.exe parsing
  # of regex metacharacters like '|' in -R patterns.
  $ctestArgs = @("--test-dir", "$RepoRoot\build", "--output-on-failure") + $Args
  Write-Host "[atx-build] ctest $($ctestArgs -join ' ')" -ForegroundColor Cyan
  & ctest @ctestArgs
  exit $LASTEXITCODE
}
elseif ($verb -eq "configure") {
  $cfg = "cmake --preset ninja"
  if ($Groups) { $cfg += " -DATX_TEST_GROUPS=$Groups" }
  if ($Bench)  { $cfg += " -DATX_BUILD_BENCH=ON" }
  $inner = $cfg
}
elseif ($verb -eq "build") {
  $inner = "cmake --build `"$RepoRoot\build`" --target " + ($rest -join " ")
}
else {
  # Pass through raw cmake args.
  $inner = "cmake " + ($Args -join " ")
}

$full = "`"$VcVars`" >nul 2>&1 && set `"PATH=$NinjaDir;%PATH%`" && cd /d `"$RepoRoot`" && $inner"
Write-Host "[atx-build] $inner" -ForegroundColor Cyan
& cmd.exe /c $full
exit $LASTEXITCODE
