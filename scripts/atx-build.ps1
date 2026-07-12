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

# vcvars64 can fail silently in shells with a huge/odd PATH (its vswhere lookup
# breaks), leaving mt.exe (Windows SDK manifest tool) unresolved -> CMake's
# compiler check dies with "MT: command CMAKE_MT-NOTFOUND ... failed" in every
# FRESH build dir (cached CMAKE_MT hides it in existing ones). clang-cl and
# lld-link self-locate MSVC/SDK, so pinning the newest SDK bin dir onto PATH is
# the only extra piece a fresh worktree needs.
$MtDir = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.*\x64\mt.exe" -ErrorAction SilentlyContinue |
  Sort-Object { [version]($_.Directory.Parent.Name) } | Select-Object -Last 1 | ForEach-Object { $_.DirectoryName }
if (-not $MtDir) { Write-Warning "mt.exe not found under Windows Kits; relying on vcvars64 PATH" }

# Build the inner command. Everything runs inside one cmd.exe session so the env
# vcvars64 sets (INCLUDE/LIB/PATH) is live for the cmake/ctest invocation.
$verb = if ($Args.Count -gt 0) { $Args[0] } else { "" }
$rest = if ($Args.Count -gt 1) { $Args[1..($Args.Count - 1)] } else { @() }

if ($Ctest) {
  # ctest only runs the built exes (DLLs are applocal-staged beside them), so it
  # needs neither vcvars nor Ninja — invoke it directly to avoid cmd.exe parsing
  # of regex metacharacters like '|' in -R patterns.
  $ctestArgs = @("--test-dir", "$RepoRoot\build", "--output-on-failure", "-j", "16") + $Args
  Write-Host "[atx-build] ctest $($ctestArgs -join ' ')" -ForegroundColor Cyan
  & ctest @ctestArgs
  exit $LASTEXITCODE
}
elseif ($verb -eq "configure") {
  # `dev` is the canonical iterate preset (same binaryDir build/ as `ninja`).
  # Using it here keeps configure consistent with scripts\new-worktree.ps1 —
  # previously this line said `ninja`, silently dropping the shared-deps setup.
  $cfg = "cmake --preset dev"
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

$PathPrefix = if ($MtDir) { "$NinjaDir;$MtDir" } else { $NinjaDir }
# CCACHE_BASEDIR is the one per-worktree ccache key (relativizes this tree's paths in
# the hash -> cross-worktree cache hits). The preset `environment` block only applies
# to `cmake --preset`/`cmake --build --preset` invocations, and the build verb below
# calls `cmake --build <dir>` directly - so export it here. The worktree-invariant
# keys (hash_dir/sloppiness/ignore_options) live in the global ccache config
# (scripts/dev-setup.ps1).
$full = "`"$VcVars`" >nul 2>&1 && set `"PATH=$PathPrefix;%PATH%`" && set `"CCACHE_BASEDIR=$RepoRoot`" && cd /d `"$RepoRoot`" && $inner"
Write-Host "[atx-build] $inner" -ForegroundColor Cyan
& cmd.exe /c $full
exit $LASTEXITCODE
