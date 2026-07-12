# Spin up a fresh git worktree wired for fast iterative dev. ASCII-only (Windows PowerShell 5.1 safe).
#
# Creates a worktree, configures the `dev` CMake preset (sccache compiler cache + shared
# FetchContent deps), and leaves it ready for clangd (the committed .clangd reads each
# worktree's own build/compile_commands.json, so no symlink is needed).
#
# Cross-worktree speed comes from three SHARED caches, not from copying build trees:
#   * ccache        -> global object cache (C:\atx-cache\ccache); the preset env keys
#                      make the SAME source hash identically in every worktree
#   * ATX_DEPS_DIR  -> shared FetchContent clone/build dir (deps fetched once)
#   * VCPKG_INSTALLED_DIR (preset) -> one shared vcpkg payload instead of 1.1 GB per worktree
# The first worktree primes the caches; every later worktree is mostly cache hits.
#
# Prereqs: run from a Visual Studio Developer PowerShell (MSVC env), with ccache on PATH,
# VCPKG_ROOT set, and ATX_DEPS_DIR set. Run scripts\dev-setup.ps1 once first.
#
# Example: scripts\new-worktree.ps1 -Name s8 -Branch feat/s8 -Base main

param(
  [Parameter(Mandatory = $true)][string]$Name,
  [string]$Branch = $Name,
  [string]$Base = 'main',
  [switch]$NoConfigure,
  [switch]$Shared   # configure the `dev-shared` preset (atx libs as DLLs: smaller artifacts, faster relinks)
)
$ErrorActionPreference = 'Stop'

$root = (git rev-parse --show-toplevel).Trim()
$wtRoot = Join-Path (Split-Path $root -Parent) 'atx-wt'
$wt = Join-Path $wtRoot $Name
New-Item -ItemType Directory -Force $wtRoot | Out-Null
if (Test-Path $wt) { throw ('Worktree path already exists: ' + $wt) }

Write-Host ('git worktree add ' + $wt + '  (' + $Branch + ' from ' + $Base + ')') -ForegroundColor Cyan
git worktree add $wt -b $Branch $Base

# git worktree add does NOT populate submodules; without this the configure dies at
# atx-core/third-party/databento-cpp ("does not contain a CMakeLists.txt").
Push-Location $wt
try { git submodule update --init --recursive } finally { Pop-Location }

if (-not $env:ATX_DEPS_DIR) {
  Write-Warning 'ATX_DEPS_DIR is not set in this shell - the dev preset falls back to a per-worktree _deps. Run scripts\dev-setup.ps1 and open a new shell.'
}

$preset = if ($Shared) { 'dev-shared' } else { 'dev' }

if ($NoConfigure) {
  Write-Host ('skipped configure (-NoConfigure). Run: cd ' + $wt + '; cmake --preset ' + $preset) -ForegroundColor Yellow
  return
}

Push-Location $wt
try {
  $note = if ($Shared) { 'sccache + shared deps + atx DLLs' } else { 'sccache + shared deps' }
  Write-Host ('cmake --preset ' + $preset + '  (' + $note + ')') -ForegroundColor Cyan
  cmake --preset $preset
  Write-Host ''
  Write-Host ('ready: ' + $wt) -ForegroundColor Green
  Write-Host ('  build : cmake --build --preset ' + $preset + ' --target atx-engine-<group>-tests   (groups: alpha risk data factory parallel learn eval library combine fund book core regime store)')
  Write-Host ('  partial suite (faster worktree builds): reconfigure with  cmake --preset ' + $preset + ' -DATX_TEST_GROUPS="risk;data"  to drop the groups you are not touching')
  Write-Host ('  test  : ctest --preset ' + $preset + ' -R <Suite>')
  Write-Host '  clangd: auto-loads build/compile_commands.json (no setup)'
  if (-not $Shared) { Write-Host '  smaller artifacts / faster relinks: re-run with -Shared (or cmake --preset dev-shared) to build atx libs as DLLs' }
  if (Get-Command ccache -ErrorAction SilentlyContinue) { ccache -s | Select-Object -First 10 }
  elseif (Get-Command sccache -ErrorAction SilentlyContinue) { sccache --show-stats | Select-Object -First 12 }
}
finally { Pop-Location }
