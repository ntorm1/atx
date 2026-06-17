# Spin up a fresh git worktree wired for fast iterative dev. ASCII-only (Windows PowerShell 5.1 safe).
#
# Creates a worktree, configures the `dev` CMake preset (sccache compiler cache + shared
# FetchContent deps), and leaves it ready for clangd (the committed .clangd reads each
# worktree's own build/compile_commands.json, so no symlink is needed).
#
# Cross-worktree speed comes from two SHARED caches, not from copying build trees:
#   * sccache       -> global object cache (set SCCACHE_DIR to relocate; default %LOCALAPPDATA%)
#   * ATX_DEPS_DIR  -> shared FetchContent clone/build dir (deps fetched once)
# The first worktree primes the caches; every later worktree is mostly cache hits.
#
# Prereqs: run from a Visual Studio Developer PowerShell (MSVC env), with sccache on PATH,
# VCPKG_ROOT set, and ATX_DEPS_DIR set. Run scripts\dev-setup.ps1 once first.
#
# Example: scripts\new-worktree.ps1 -Name s8 -Branch feat/s8 -Base main

param(
  [Parameter(Mandatory = $true)][string]$Name,
  [string]$Branch = $Name,
  [string]$Base = 'main',
  [switch]$NoConfigure
)
$ErrorActionPreference = 'Stop'

$root = (git rev-parse --show-toplevel).Trim()
$wtRoot = Join-Path (Split-Path $root -Parent) 'atx-wt'
$wt = Join-Path $wtRoot $Name
New-Item -ItemType Directory -Force $wtRoot | Out-Null
if (Test-Path $wt) { throw ('Worktree path already exists: ' + $wt) }

Write-Host ('git worktree add ' + $wt + '  (' + $Branch + ' from ' + $Base + ')') -ForegroundColor Cyan
git worktree add $wt -b $Branch $Base

if (-not $env:ATX_DEPS_DIR) {
  Write-Warning 'ATX_DEPS_DIR is not set in this shell - the dev preset falls back to a per-worktree _deps. Run scripts\dev-setup.ps1 and open a new shell.'
}

if ($NoConfigure) {
  Write-Host ('skipped configure (-NoConfigure). Run: cd ' + $wt + '; cmake --preset dev') -ForegroundColor Yellow
  return
}

Push-Location $wt
try {
  Write-Host 'cmake --preset dev  (sccache + shared deps)' -ForegroundColor Cyan
  cmake --preset dev
  Write-Host ''
  Write-Host ('ready: ' + $wt) -ForegroundColor Green
  Write-Host '  build : cmake --build --preset dev --target atx-engine-<group>-tests   (groups: alpha risk data factory parallel learn eval library combine fund book core)'
  Write-Host '  partial suite (faster worktree builds): reconfigure with  cmake --preset dev -DATX_TEST_GROUPS="risk;data"  to drop the groups you are not touching'
  Write-Host '  test  : ctest --preset dev -R <Suite>'
  Write-Host '  clangd: auto-loads build/compile_commands.json (no setup)'
  if (Get-Command sccache -ErrorAction SilentlyContinue) { sccache --show-stats | Select-Object -First 12 }
}
finally { Pop-Location }
