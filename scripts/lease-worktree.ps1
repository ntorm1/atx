# Worktree POOL: lease a persistent, warm-build worktree instead of creating a
# fresh one per task. ASCII-only (Windows PowerShell 5.1 safe).
#
# WHY: `git worktree add` is cheap, but the empty build/ dir it implies is not —
# a fresh worktree pays a cold configure + rebuild even with ccache softening it.
# Pool trees keep their build/ dir WARM across tasks: leasing one and switching
# its branch means ninja rebuilds only the TUs that differ between branches.
# Cold-build cost is paid once per pool slot, ever; cache growth stays bounded.
#
# Usage:
#   scripts\lease-worktree.ps1 -Branch feat/x [-Base main] [-Agent name] [-Shared]
#       -> leases a free pool tree (creates pool-N if all busy, cap -MaxPool),
#          switches it to <Branch> (created from <Base> if new), syncs submodules,
#          configures build/ on first use. Prints the tree path to use.
#   scripts\lease-worktree.ps1 -Release pool-2
#       -> detaches the tree from its branch (so the branch can be merged/checked
#          out elsewhere) and clears the lease. build/ stays warm for next lease.
#   scripts\lease-worktree.ps1 -Status
#       -> lists pool trees, lease holders, branches.
#
# The lease marker (.atx-lease in the tree root) is advisory - it serializes
# agents choosing a tree; it does not lock git. One agent per leased tree.
# new-worktree.ps1 remains for deliberately ISOLATED one-off trees (-Isolated).

param(
  [string]$Branch,
  [string]$Base = 'main',
  [string]$Agent = $env:USERNAME,
  [switch]$Shared,       # first-configure uses the dev-shared preset (atx libs as DLLs)
  [string]$Release,
  [switch]$Status,
  [int]$MaxPool = 4
)
$ErrorActionPreference = 'Stop'

$root = (git rev-parse --show-toplevel).Trim()
$wtRoot = Join-Path (Split-Path $root -Parent) 'atx-wt'
New-Item -ItemType Directory -Force $wtRoot | Out-Null

function Get-PoolTrees { Get-ChildItem $wtRoot -Directory -Filter 'pool-*' -ErrorAction SilentlyContinue | Sort-Object Name }
function Get-LeasePath([string]$wt) { Join-Path $wt '.atx-lease' }

if ($Status) {
  $trees = @(Get-PoolTrees)
  if ($trees.Count -eq 0) { Write-Host ('pool empty (' + $wtRoot + '); lease one with -Branch <name>'); return }
  foreach ($t in $trees) {
    $lease = Get-LeasePath $t.FullName
    $br = (git -C $t.FullName rev-parse --abbrev-ref HEAD 2>$null)
    $state = if (Test-Path $lease) { 'LEASED ' + ((Get-Content $lease) -join ' | ') } else { 'free' }
    $warm = if (Test-Path (Join-Path $t.FullName 'build\build.ninja')) { 'warm' } else { 'cold' }
    Write-Host ($t.Name + '  [' + $warm + ']  branch=' + $br + '  ' + $state)
  }
  return
}

if ($Release) {
  $wt = Join-Path $wtRoot $Release
  if (-not (Test-Path $wt)) { throw ('no such pool tree: ' + $wt) }
  $lease = Get-LeasePath $wt
  $dirty = @(git -C $wt status --porcelain) | Where-Object { $_ -notmatch '\.atx-lease$' }
  if ($dirty) {
    throw ('refusing to release ' + $Release + ': tree is dirty (commit or stash first):' + "`n" + ($dirty -join "`n"))
  }
  # Detach so the branch is no longer "checked out in another worktree" - it can
  # then be merged, rebased, or leased again elsewhere. build/ stays warm.
  git -C $wt switch --detach $Base | Out-Null
  if ($LASTEXITCODE -ne 0) { throw ('git switch --detach failed in ' + $wt) }
  Remove-Item $lease -ErrorAction SilentlyContinue
  Write-Host ('released ' + $Release + ' (detached at ' + $Base + ', build/ kept warm)') -ForegroundColor Green
  return
}

if (-not $Branch) { throw 'usage: lease-worktree.ps1 -Branch <branch> [-Base main] | -Release <pool-N> | -Status' }

# ---- pick a free tree (or grow the pool) ----
$free = @(Get-PoolTrees | Where-Object { -not (Test-Path (Get-LeasePath $_.FullName)) }) | Select-Object -First 1
if ($free) {
  $wt = $free.FullName
  $poolName = $free.Name
} else {
  $n = @(Get-PoolTrees).Count + 1
  if ($n -gt $MaxPool) {
    throw ('pool exhausted (' + $MaxPool + ' trees, all leased). Release one (-Release pool-N) or raise -MaxPool.')
  }
  $poolName = 'pool-' + $n
  $wt = Join-Path $wtRoot $poolName
  Write-Host ('growing pool: git worktree add --detach ' + $wt + ' ' + $Base) -ForegroundColor Cyan
  git worktree add --detach $wt $Base
  if ($LASTEXITCODE -ne 0) { throw 'git worktree add failed' }
}

# ---- point the tree at the branch ----
# Existing branch: plain switch (never -C: that would silently reset the branch
# to Base). New branch: create from Base. Either way ninja later rebuilds only
# the TUs whose inputs differ from whatever this tree last built.
git show-ref --verify --quiet ('refs/heads/' + $Branch)
if ($LASTEXITCODE -eq 0) {
  git -C $wt switch $Branch
} else {
  git -C $wt switch -c $Branch $Base
}
if ($LASTEXITCODE -ne 0) {
  throw ('git switch failed (branch checked out in another worktree, or conflicts). Tree NOT leased: ' + $wt)
}
# worktree add/switch does not populate submodules (databento-cpp arrives empty).
git -C $wt submodule update --init --recursive
if ($LASTEXITCODE -ne 0) { throw 'git submodule update failed' }

Set-Content -Path (Get-LeasePath $wt) -Encoding Ascii -Value @(
  ('agent=' + $Agent),
  ('branch=' + $Branch),
  ('since=' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
)

# ---- first-use configure (build/ absent) ----
$preset = if ($Shared) { 'dev-shared' } else { 'dev' }
if (-not (Test-Path (Join-Path $wt 'build\build.ninja'))) {
  Write-Host ('cold tree: configuring preset ' + $preset + ' (one-time for this slot)') -ForegroundColor Cyan
  Push-Location $wt
  try { & (Join-Path $wt 'scripts\atx-build.ps1') configure -Preset $preset }
  finally { Pop-Location }
  if ($LASTEXITCODE -ne 0) { throw 'configure failed (lease kept; fix and re-run configure inside the tree)' }
} elseif ($Shared) {
  Write-Host 'NOTE: tree already configured; -Shared ignored (reconfigure inside the tree to switch preset).' -ForegroundColor Yellow
}

Write-Host ''
Write-Host ('leased ' + $poolName + ' -> ' + $wt + '  (branch ' + $Branch + ')') -ForegroundColor Green
Write-Host ('  cd ' + $wt)
Write-Host ('  build : powershell scripts\atx-build.ps1 build <target>        (target-scoped; never bare `ninja all`)')
Write-Host ('  check : powershell scripts\atx-build.ps1 check <file.cpp>      (single-TU compile, no link)')
Write-Host ('  test  : powershell scripts\atx-build.ps1 -Ctest -R <Suite>')
Write-Host ('  done  : powershell scripts\lease-worktree.ps1 -Release ' + $poolName)
