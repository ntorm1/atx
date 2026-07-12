# Spin up a fresh git worktree wired for fast iterative dev. ASCII-only (Windows PowerShell 5.1 safe).
#
# Creates a worktree, configures the `dev` CMake preset (sccache compiler cache + shared
# FetchContent deps), and leaves it ready for clangd (the committed .clangd reads each
# worktree's own build/compile_commands.json, so no symlink is needed).
#
# Cross-worktree speed comes from two SHARED caches, not from copying build trees:
#   * sccache       -> global cache for cacheable compiler calls; clang-cl PCH
#                      dependent calls currently remain non-cacheable (/Fp)
#   * ATX_DEPS_DIR  -> shared FetchContent clone/build dir (deps fetched once)
# Shared dependencies are the reliable win; inspect sccache stats rather than
# assuming a fresh first-party build will be mostly hits.
#
# Prereqs: sccache on PATH, VCPKG_ROOT set, and ATX_DEPS_DIR set. The worktree's
# atx-build helper sources the MSVC environment and resolves Ninja + mt.exe.
# Run scripts\dev-setup.ps1 once first.
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

$commonGitDir = (git rev-parse --path-format=absolute --git-common-dir).Trim()
if (-not $commonGitDir) { throw 'Unable to resolve the shared git directory' }
$mainRoot = Split-Path $commonGitDir -Parent
$wtRoot = Join-Path (Split-Path $mainRoot -Parent) 'atx-wt'
$wt = Join-Path $wtRoot $Name
New-Item -ItemType Directory -Force $wtRoot | Out-Null
if (Test-Path $wt) { throw ('Worktree path already exists: ' + $wt) }

Write-Host ('git worktree add ' + $wt + '  (' + $Branch + ' from ' + $Base + ')') -ForegroundColor Cyan
git worktree add $wt -b $Branch $Base
if ($LASTEXITCODE -ne 0) { throw 'git worktree add failed' }

Push-Location $wt
try {
  Write-Host 'initializing required submodules' -ForegroundColor Cyan
  git submodule update --init --recursive atx-core/third-party/databento-cpp
  if ($LASTEXITCODE -ne 0) { throw 'git submodule update failed' }
}
finally { Pop-Location }

if (-not $env:ATX_DEPS_DIR) {
  Write-Warning 'ATX_DEPS_DIR is not set in this shell - the dev preset falls back to a per-worktree _deps. Run scripts\dev-setup.ps1 and open a new shell.'
}

$preset = if ($Shared) { 'dev-shared' } else { 'dev' }

if ($NoConfigure) {
  Write-Host ('skipped configure (-NoConfigure). Run: cd ' + $wt + '; scripts\atx-build.ps1 -Preset ' + $preset + ' configure') -ForegroundColor Yellow
  return
}

Push-Location $wt
try {
  $note = if ($Shared) { 'sccache + shared deps + atx DLLs' } else { 'sccache + shared deps' }
  Write-Host ('scripts\atx-build.ps1 -Preset ' + $preset + ' configure  (' + $note + ')') -ForegroundColor Cyan
  & .\scripts\atx-build.ps1 -Preset $preset configure
  if ($LASTEXITCODE -ne 0) { throw ('configure failed for preset ' + $preset) }
  Write-Host ''
  Write-Host ('ready: ' + $wt) -ForegroundColor Green
  Write-Host ('  build : scripts\atx-build.ps1 -Preset ' + $preset + ' build atx-engine-<group>-tests   (groups: alpha risk data factory parallel learn eval library combine fund book core regime store)')
  Write-Host ('  vol   : scripts\atx-build.ps1 -Preset ' + $preset + ' build atx-vol-tests')
  Write-Host ('  partial suite: scripts\atx-build.ps1 -Preset ' + $preset + ' configure -Groups "risk;data"')
  Write-Host ('  test  : scripts\atx-build.ps1 -Preset ' + $preset + ' -Ctest -R <Suite>')
  Write-Host '  clangd: auto-loads build/compile_commands.json (no setup)'
  if (-not $Shared) { Write-Host '  smaller artifacts / faster relinks: scripts\atx-build.ps1 -Preset dev-shared configure' }
  if (Get-Command sccache -ErrorAction SilentlyContinue) { sccache --show-stats | Select-Object -First 12 }
}
finally { Pop-Location }
