<#
.SYNOPSIS
  Out-of-tree smoke-consumer gate for the INSTALLED atx-vol package (plan 5.1).

  Installs the already-configured monorepo build into a FRESH prefix, then
  configures + builds + runs atx-vol/test-package -- a standalone CMake project
  that only ever says `find_package(atx-vol REQUIRED)`. It has no
  add_subdirectory of the atx tree and no FetchContent of anything, so a pass
  proves the installed prefix is genuinely self-sufficient (headers, the
  tl::expected the public `Result<T>` resolves to, and the link interface).

  The consumer binary is run TWICE and its stdout byte-compared: the packaging
  gate doubles as a determinism gate, per the task's determinism constraint.

.NOTES
  ABSOLUTE-PATH BANNER: like scripts/atx-build.ps1 (which this delegates every
  cmake invocation to, for the MSVC environment), this script always drives the
  worktree that physically CONTAINS it. Stand in that worktree and invoke it by
  absolute path.

  The consumer must be built with the SAME CMAKE_BUILD_TYPE and compiler as the
  installed libraries -- a Release consumer linking Debug static libs trips
  _ITERATOR_DEBUG_LEVEL. Both are read back out of the library build's cache
  rather than hard-coded, so `-LibraryBuildDir build-rel` just works.

.EXAMPLE
  Set-Location C:\atx-wt\pool-3
  powershell -File scripts\atx-vol-test-package.ps1
#>
[CmdletBinding()]
param(
  # Build dir of the already-configured monorepo to install FROM.
  [string] $LibraryBuildDir = "",
  # Install prefix to create (wiped first). Under build-*/ so .gitignore covers it.
  [string] $Prefix = "",
  # Build dir for the consumer project (wiped first).
  [string] $BuildDir = ""
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
$AtxBuild = Join-Path $PSScriptRoot "atx-build.ps1"
$Source   = Join-Path $RepoRoot "atx-vol\test-package"

if (-not $LibraryBuildDir) { $LibraryBuildDir = Join-Path $RepoRoot "build" }
if (-not $Prefix)          { $Prefix          = Join-Path $RepoRoot "build-install" }
if (-not $BuildDir)        { $BuildDir        = Join-Path $RepoRoot "build-test-package" }

$LibraryBuildDir = [System.IO.Path]::GetFullPath($LibraryBuildDir)
$Prefix          = [System.IO.Path]::GetFullPath($Prefix)
$BuildDir        = [System.IO.Path]::GetFullPath($BuildDir)

if (-not (Test-Path (Join-Path $LibraryBuildDir "CMakeCache.txt"))) {
  throw "[test-package] no CMakeCache.txt in $LibraryBuildDir - configure it first"
}

function Invoke-Cmake {
  param([string[]] $CmakeArgs, [string] $What)
  & $AtxBuild @CmakeArgs
  if ($LASTEXITCODE -ne 0) { throw "[test-package] $What failed (exit $LASTEXITCODE)" }
}

# Cache entries are read back so the consumer's toolchain matches the library's.
$cacheDump = & cmake -N -LA -B $LibraryBuildDir
function Get-CacheVar {
  param([string] $Name, [string] $Default = "")
  $hit = $cacheDump | Select-String -Pattern ("^" + [regex]::Escape($Name) + ":[^=]+=(.*)$")
  if ($hit) { return $hit.Matches[0].Groups[1].Value }
  return $Default
}

$buildType   = Get-CacheVar "CMAKE_BUILD_TYPE"   "Debug"
$cxxCompiler = Get-CacheVar "CMAKE_CXX_COMPILER" "clang-cl"
$vcpkgDir    = Get-CacheVar "VCPKG_INSTALLED_DIR"
$vcpkgTriple = Get-CacheVar "VCPKG_TARGET_TRIPLET" "x64-windows"

Write-Host "[test-package] library build : $LibraryBuildDir ($buildType)" -ForegroundColor Cyan
Write-Host "[test-package] install prefix: $Prefix" -ForegroundColor Cyan

# ---- 1. Fresh install prefix ------------------------------------------------
if (Test-Path $Prefix)   { Remove-Item -Recurse -Force $Prefix }
if (Test-Path $BuildDir) { Remove-Item -Recurse -Force $BuildDir }
Invoke-Cmake @("--install", $LibraryBuildDir, "--prefix", $Prefix) "cmake --install"

# ---- 2. Configure the out-of-tree consumer ---------------------------------
$configureArgs = @(
  "-S", $Source,
  "-B", $BuildDir,
  "-G", "Ninja",
  "-DCMAKE_BUILD_TYPE=$buildType",
  # CXX only: the consumer project declares LANGUAGES CXX, so passing
  # CMAKE_C_COMPILER too would only earn an "unused variable" warning.
  "-DCMAKE_CXX_COMPILER=$cxxCompiler",
  "-DCMAKE_PREFIX_PATH=$Prefix",
  "-DVCPKG_TARGET_TRIPLET=$vcpkgTriple",
  "-DVCPKG_MANIFEST_MODE=OFF"
)
if ($vcpkgDir) { $configureArgs += "-DVCPKG_INSTALLED_DIR=$vcpkgDir" }
Invoke-Cmake $configureArgs "consumer configure"

# ---- 3. Build ---------------------------------------------------------------
Invoke-Cmake @("--build", $BuildDir) "consumer build"

# ---- 4. Run twice and byte-compare -----------------------------------------
$exe = Join-Path $BuildDir "atx-vol-smoke.exe"
if (-not (Test-Path $exe)) { throw "[test-package] consumer exe not found at $exe" }

# <prefix>/bin carries the tool exes and any applocal-deployed vcpkg DLLs. It is
# never an atx DLL: atx-vol is distributed static-only (plan 5.2) and
# cmake/atx-vol-install.cmake refuses to install an ATX_SHARED_LIBS=ON build, so
# step 1 above would have failed before reaching here.
$env:PATH = (Join-Path $Prefix "bin") + ";" + $env:PATH

$run1 = & $exe
if ($LASTEXITCODE -ne 0) { $run1 | Write-Host; throw "[test-package] smoke run 1 failed" }
$run2 = & $exe
if ($LASTEXITCODE -ne 0) { $run2 | Write-Host; throw "[test-package] smoke run 2 failed" }

$text1 = ($run1 -join "`n")
$text2 = ($run2 -join "`n")
Write-Host "---- smoke output ----" -ForegroundColor Cyan
Write-Host $text1
Write-Host "----------------------" -ForegroundColor Cyan
if ($text1 -ne $text2) {
  Write-Host "run 2:" -ForegroundColor Red
  Write-Host $text2
  throw "[test-package] NON-DETERMINISTIC: the two runs differ"
}

Write-Host "[test-package] PASS (find_package + link + run, deterministic over 2 runs)" `
  -ForegroundColor Green
exit 0
