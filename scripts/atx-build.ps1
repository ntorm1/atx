<#
.SYNOPSIS
  Build/test helper that sources the MSVC environment (vcvars64) and puts the
  VS-bundled Ninja on PATH, then forwards its arguments to cmake (or, with
  -Ctest, to the selected CTest preset).

  Needed because the `ninja` preset uses clang-cl + the Ninja generator, both of
  which require the MSVC dev environment (INCLUDE/LIB/PATH from vcvars64) that a
  plain shell does not have. ninja.exe ships inside the VS install, not on PATH.

.EXAMPLE
  # Configure (data test group only) + build the data tests:
  pwsh scripts/atx-build.ps1 -Preset dev configure -Groups data -Bench
  pwsh scripts/atx-build.ps1 -Preset dev build atx-engine-data-tests

.EXAMPLE
  # Configure and build atx-vol in a worktree:
  pwsh scripts/atx-build.ps1 -Preset dev configure
  pwsh scripts/atx-build.ps1 -Preset dev build atx-vol-tests

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
  [string] $Groups = "",
  [ValidateSet("ninja", "dev", "dev-shared", "rel", "rel-avx2", "hygiene", "vs")]
  [string] $Preset = "ninja"
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
$verb = if ($Args.Count -gt 0) { $Args[0] } else { "" }
$rest = if ($Args.Count -gt 1) { $Args[1..($Args.Count - 1)] } else { @() }

if ($Ctest) {
  # ctest only runs built executables. Resolve no compiler/SDK tools here so a
  # runtime-only machine can execute an existing build.
  $ctestArgs = @("--preset", $Preset, "--output-on-failure") + $Args
  Write-Host "[atx-build] ctest $($ctestArgs -join ' ')" -ForegroundColor Cyan
  Push-Location $RepoRoot
  try {
    & ctest @ctestArgs
    exit $LASTEXITCODE
  }
  finally { Pop-Location }
}

$VsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$VsRoot = if (Test-Path $VsWhere) {
  $foundVs = & $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if ($foundVs) { ($foundVs | Select-Object -First 1).Trim() } else { "" }
} else {
  "C:\Program Files\Microsoft Visual Studio\2022\Community"
}
if (-not $VsRoot) { throw "Visual Studio with C++ tools was not found" }

$VcVars   = Join-Path $VsRoot "VC\Auxiliary\Build\vcvars64.bat"
$NinjaDir = Join-Path $VsRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
$KitsBin  = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin"

$MtExe = Get-ChildItem -Path "$KitsBin\*\x64\mt.exe" -File -ErrorAction SilentlyContinue |
  Sort-Object { [version]$_.Directory.Parent.Name } -Descending |
  Select-Object -First 1
$MtDir = if ($MtExe) { $MtExe.Directory.FullName } else { "" }

if (-not (Test-Path $VcVars))   { throw "vcvars64.bat not found at $VcVars" }
if ($Preset -ne "vs" -and -not (Test-Path $NinjaDir)) { throw "Ninja dir not found at $NinjaDir" }
if (-not $MtExe)                 { throw "x64 mt.exe not found below $KitsBin" }

if ($verb -eq "configure") {
  $cmakeArgs = @("--preset", $Preset, "-DCMAKE_MT=$($MtExe.FullName)")
  if ($Groups) { $cmakeArgs += "-DATX_TEST_GROUPS=$Groups" }
  if ($Bench)  { $cmakeArgs += "-DATX_BUILD_BENCH=ON" }
}
elseif ($verb -eq "build") {
  $cmakeArgs = @("--build", "--preset", $Preset)
  if ($rest.Count -gt 0) { $cmakeArgs += @("--target") + $rest }
}
else {
  # Pass through raw cmake args.
  $cmakeArgs = $Args
}

$ToolPath = if ($Preset -eq "vs") { $MtDir } else { "$NinjaDir;$MtDir" }
# Import vcvars' environment once, then invoke CMake directly with a PowerShell
# argument array. Keeping argv structured preserves spaces and prevents cmd.exe
# metacharacters in caller-supplied CMake values or target names from executing.
$vcvarsCommand = "`"$VcVars`" >nul 2>&1 && set"
$environmentLines = & cmd.exe /d /s /c $vcvarsCommand
if ($LASTEXITCODE -ne 0) { throw "vcvars64.bat failed with exit code $LASTEXITCODE" }
foreach ($line in $environmentLines) {
  $equals = $line.IndexOf('=')
  if ($equals -le 0) { continue }
  $name = $line.Substring(0, $equals)
  $value = $line.Substring($equals + 1)
  Set-Item -LiteralPath ("Env:" + $name) -Value $value
}
$env:PATH = "$ToolPath;$env:PATH"

Write-Host "[atx-build] cmake $($cmakeArgs -join ' ')" -ForegroundColor Cyan
Push-Location $RepoRoot
try {
  & cmake @cmakeArgs
  exit $LASTEXITCODE
}
finally { Pop-Location }
