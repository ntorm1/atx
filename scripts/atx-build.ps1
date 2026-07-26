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

.NOTES
  ABSOLUTE-PATH BANNER (parallel-worktree agents). This script ALWAYS builds the
  worktree that physically contains it (`$RepoRoot`, from `$PSScriptRoot`) — never
  your shell's cwd. Invoke it by its ABSOLUTE path
  (`& C:\atx-wt\<wt>\scripts\atx-build.ps1 ...`) and stand in that same worktree.
  A relative `.\scripts\...` resolves against the cwd, which defaults to the live
  C:\atx tree — that reconfigured the live tree twice last sprint. The wrong-tree
  guard below now refuses when your cwd's git worktree != `$RepoRoot`.

  DEPS ISOLATION (parallel agents). Pass a per-worktree, per-preset
  `-DFETCHCONTENT_BASE_DIR=C:/atx-wt/<wt>/deps/<preset>` at configure so N agents
  (and Debug vs Release in one tree) never share a `spdlog-build` object tree —
  that shared tree races on `_ITERATOR_DEBUG_LEVEL`. `new-worktree.ps1 -Isolated`
  wires this automatically.

  P-CORE BENCH-LEASE. Benchmarks are only citable on a quiet host: pin to the
  P-cores (`configure_pricing_executor(PerformanceCores)`) and, when several
  agents share the box, LEASE the P-cores to one bench at a time and cap fit
  fan-out with `ATX_VOL_FIT_WORKERS` (e.g. `$env:ATX_VOL_FIT_WORKERS=1` for a
  single-op latency bench, or a small cap so a background fit does not oversubscribe
  the leased cores). Correctness gates run on Debug/`rel`; perf on `rel-avx2`.
#>
[CmdletBinding(PositionalBinding = $false)]
param(
  [Parameter(ValueFromRemainingArguments = $true)]
  [string[]] $Args,
  [switch] $Ctest,
  [switch] $Bench,
  [string] $Groups = "",
  [string] $Preset = "dev",
  [ValidateRange(1, 256)]
  [int] $Jobs = 1,
  [switch] $DryRun
)

$ErrorActionPreference = "Stop"

$VsRoot   = "C:\Program Files\Microsoft Visual Studio\2022\Community"
$VcVars   = Join-Path $VsRoot "VC\Auxiliary\Build\vcvars64.bat"
$NinjaDir = Join-Path $VsRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
$RepoRoot = Split-Path -Parent $PSScriptRoot

# ── Wrong-tree guard (M6) ──────────────────────────────────────────────────
# This script builds $RepoRoot — the worktree that physically CONTAINS it (via
# $PSScriptRoot), NOT your shell's cwd. To keep that honest and kill the cwd-trap
# that silently reconfigured the live C:\atx tree twice last sprint, refuse to run
# unless your shell is standing in the SAME git worktree you are about to build.
# The safe pattern becomes: `Set-Location <worktree>` then invoke that tree's
# scripts\atx-build.ps1. Override once (CI / deliberate cross-tree) by setting
# $env:ATX_BUILD_ALLOW_ANY_TREE=1.
$pwdToplevel = (& git -C "$PWD" rev-parse --show-toplevel 2>$null)
if ($LASTEXITCODE -eq 0 -and $pwdToplevel -and -not $env:ATX_BUILD_ALLOW_ANY_TREE) {
  $normRepo = ([System.IO.Path]::GetFullPath($RepoRoot)).TrimEnd('\', '/').Replace('\', '/')
  $normPwd  = ([System.IO.Path]::GetFullPath($pwdToplevel.Trim())).TrimEnd('\', '/').Replace('\', '/')
  if ($normRepo -ne $normPwd) {
    throw @"
[atx-build] WRONG-TREE GUARD: refusing to build.
  this script builds : $normRepo   (the worktree that contains it)
  your shell is in   : $normPwd
These differ — the cwd-trap that silently reconfigured the live tree last sprint.
Fix: Set-Location '$RepoRoot'  then re-run (or invoke that tree's own
scripts\atx-build.ps1 from inside it). Deliberate override: `$env:ATX_BUILD_ALLOW_ANY_TREE=1.
"@
  }
}

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

# Build the executable and argument array. Commands that need the compiler run
# after vcvars64's environment has been imported into this PowerShell process.
$verb = if ($Args.Count -gt 0) { $Args[0] } else { "" }
$rest = if ($Args.Count -gt 1) { $Args[1..($Args.Count - 1)] } else { @() }

if ($Ctest) {
  # Serial is the evidence default. Parallelism is an explicit operator choice
  # (`-Jobs N`) so a supposedly serial attribution gate cannot silently run 16
  # tests at once.
  $innerExe = "ctest"
  $innerArgs = @("--test-dir", "$RepoRoot\build", "--output-on-failure", "-j", "$Jobs") + $Args
  $requiresMsvc = $false
}
elseif ($verb -eq "configure") {
  # `dev` is the canonical iterate preset (same binaryDir build/ as `ninja`).
  # Using it here keeps configure consistent with scripts\new-worktree.ps1 —
  # previously this line said `ninja`, silently dropping the shared-deps setup.
  # -Preset dev-shared flips to the DLL build (same build/ dir).
  $innerExe = "cmake"
  $innerArgs = @("--preset", $Preset)
  if ($Groups) { $innerArgs += "-DATX_TEST_GROUPS=$Groups" }
  if ($Bench)  { $innerArgs += "-DATX_BUILD_BENCH=ON" }
  # F-7: configure's caller-supplied -D arguments are part of the argv. The old
  # string builder computed `$rest` and then silently discarded it here.
  $innerArgs += $rest
  $requiresMsvc = $true
}
elseif ($verb -eq "build") {
  $innerExe = "cmake"
  $innerArgs = @("--build", "$RepoRoot\build", "--target") + $rest
  $requiresMsvc = $true
}
elseif ($verb -eq "check") {
  # Single-TU type-check loop: compile ONLY the named source's object (no link,
  # no downstream targets) — seconds per iteration instead of a target build.
  # Resolves each source to its .obj via ninja's target list, so it works for any
  # first-party TU without knowing the owning CMake target.
  if ($rest.Count -eq 0) { throw "usage: atx-build.ps1 check <source.cpp> [more.cpp ...]" }
  $buildDir = Join-Path $RepoRoot "build"
  if (-not (Test-Path (Join-Path $buildDir "build.ninja"))) {
    throw "no build/build.ninja - run: atx-build.ps1 configure"
  }
  $ninjaExe = Join-Path $NinjaDir "ninja.exe"
  # `-t targets all` needs no MSVC env; lines look like "path/to/foo.cpp.obj: CXX_COMPILER__..."
  $targetLines = & $ninjaExe -C $buildDir -t targets all
  $objs = @()
  foreach ($src in $rest) {
    # Normalize to a repo-relative path fragment. The full source path is not
    # contiguous in the obj path (CMakeFiles/<tgt>.dir is inserted), so match on
    # "<parentDir>/<basename>.obj" — the source path relative to its CMakeLists.
    $rel = ($src -replace '/', '\') -replace '^\.\\', ''
    $rootPrefix = ([System.IO.Path]::GetFullPath($RepoRoot)).TrimEnd('\') + '\'
    $abs = [System.IO.Path]::GetFullPath((Join-Path $RepoRoot $rel))
    if ($abs.StartsWith($rootPrefix)) { $rel = $abs.Substring($rootPrefix.Length) }
    $leaf = Split-Path $rel -Leaf
    $parent = Split-Path (Split-Path $rel -Parent) -Leaf
    $needle = if ($parent) { "\$parent\$leaf.obj" } else { "\$leaf.obj" }
    $hits = @($targetLines | ForEach-Object { ($_ -split ': ')[0] } |
      Where-Object { ('\' + ($_ -replace '/', '\')) -like ('*' + $needle) })
    if ($hits.Count -eq 0) {
      # Fall back to basename-only (source may sit directly beside its CMakeLists).
      $hits = @($targetLines | ForEach-Object { ($_ -split ': ')[0] } |
        Where-Object { ('\' + ($_ -replace '/', '\')) -like ('*\' + $leaf + '.obj') })
    }
    if ($hits.Count -eq 0) { throw "check: no object target found for '$src' (is its target configured?)" }
    if ($hits.Count -gt 1) {
      Write-Host ("[atx-build] check: '" + $src + "' matches " + $hits.Count + " objects (building all):") -ForegroundColor Yellow
      $hits | ForEach-Object { Write-Host ("  " + $_) }
    }
    $objs += $hits
  }
  $innerExe = "ninja"
  $innerArgs = @("-C", "$buildDir") + $objs
  $requiresMsvc = $true
}
else {
  # Pass through raw cmake args.
  $innerExe = "cmake"
  $innerArgs = @($Args)
  $requiresMsvc = $true
}

if ($DryRun) {
  # Machine-readable argv pin for script tests and operator inspection. JSON
  # preserves argument boundaries that a display string cannot.
  [ordered]@{
    executable = $innerExe
    arguments = @($innerArgs)
    ctest_jobs = if ($Ctest) { $Jobs } else { $null }
    requires_msvc = $requiresMsvc
  } | ConvertTo-Json -Depth 3
  exit 0
}

Write-Host ("[atx-build] " + $innerExe + " " + ($innerArgs -join " ")) -ForegroundColor Cyan
if (-not $requiresMsvc) {
  & $innerExe @innerArgs
  exit $LASTEXITCODE
}

# Import vcvars into THIS PowerShell process, then invoke the executable with a
# real argument array. This avoids reparsing a composed shell string and
# preserves spaces/metacharacters in -D values and test regexes.
$vcvarsCommand = "`"$VcVars`" >nul 2>&1 && set"
$vcvarsEnvironment = & cmd.exe /d /s /c $vcvarsCommand
if ($LASTEXITCODE -ne 0) {
  throw "vcvars64.bat failed with exit code $LASTEXITCODE"
}
foreach ($line in $vcvarsEnvironment) {
  $separator = $line.IndexOf("=")
  if ($separator -le 0) { continue }
  $name = $line.Substring(0, $separator)
  $value = $line.Substring($separator + 1)
  [Environment]::SetEnvironmentVariable($name, $value, "Process")
}

$PathPrefix = if ($MtDir) { "$NinjaDir;$MtDir" } else { $NinjaDir }
$env:PATH = "$PathPrefix;$env:PATH"
# CCACHE_BASEDIR is the one per-worktree ccache key (relativizes this tree's
# paths in the hash -> cross-worktree cache hits).
$env:CCACHE_BASEDIR = $RepoRoot
Push-Location $RepoRoot
try {
  & $innerExe @innerArgs
  $exitCode = $LASTEXITCODE
}
finally {
  Pop-Location
}
exit $exitCode
