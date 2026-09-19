# Worktree pool BUILD-DIR SWEEPER. ASCII-only (Windows PowerShell 5.1 safe).
#
# WHY: pool trees keep their dev build/ WARM on purpose (see lease-worktree.ps1),
# but the SECONDARY config dirs (build-rel, build-rel-avx2, build-counters,
# build-hygiene, build-lakehouse-off, build-vs, build-relprof) accumulate long
# after the lane that needed them ends -- measured ~30-40 GB across 12 pools.
# Those configs are cheap to regenerate: the shared ccache (C:\atx-cache\ccache)
# is exactly the warm layer that makes a re-configure + rebuild fast, so stale
# secondary dirs are pure disk waste. This sweeper deletes them; the dev build/
# (the pool's warmth contract) is kept unless -IncludeDev.
#
# Usage:
#   scripts\sweep-worktree-builds.ps1                  -> sweep unleased trees under ..\atx-wt
#   scripts\sweep-worktree-builds.ps1 -DryRun          -> report what would be deleted, delete nothing
#   scripts\sweep-worktree-builds.ps1 -IncludeLeased   -> also sweep trees holding a .atx-lease
#   scripts\sweep-worktree-builds.ps1 -IncludeDev      -> also delete the warm dev build/ (next lease pays a cold configure)
#   scripts\sweep-worktree-builds.ps1 -IncludeDeps     -> also delete per-tree deps/ (isolated FETCHCONTENT trees, rel-avx2 lanes re-fetch)
#   scripts\sweep-worktree-builds.ps1 -Path C:\atx     -> sweep an explicit tree instead of the pool root
#
# Safety: leased trees are skipped by default (an active lane may be mid-bench in
# build-rel-avx2). Locked files (a running ninja/clang) make that dir's delete
# fail; the sweeper reports and moves on -- it never kills processes.

param(
  [string]$Root,            # pool root; default: sibling atx-wt of this repo (matches lease-worktree.ps1)
  [string[]]$Path,          # explicit tree(s) to sweep instead of scanning $Root
  [switch]$IncludeLeased,
  [switch]$IncludeDev,
  [switch]$IncludeDeps,
  [switch]$DryRun
)
$ErrorActionPreference = 'Stop'

# Secondary config dirs: every binaryDir in CMakePresets.json except dev's build/.
$sweepDirs = @('build-rel', 'build-rel-avx2', 'build-counters', 'build-hygiene',
               'build-lakehouse-off', 'build-vs', 'build-relprof')
if ($IncludeDev)  { $sweepDirs += 'build' }

function Get-DirGB([string]$p) {
  $s = (Get-ChildItem $p -Recurse -Force -ErrorAction SilentlyContinue |
        Where-Object { -not $_.PSIsContainer } | Measure-Object -Property Length -Sum).Sum
  if ($null -eq $s) { $s = 0 }
  [math]::Round($s / 1GB, 2)
}

# ---- collect candidate trees ----
$trees = @()
if ($Path) {
  foreach ($p in $Path) {
    if (-not (Test-Path $p)) { throw ('no such tree: ' + $p) }
    $trees += Get-Item $p
  }
} else {
  if (-not $Root) {
    $repo = (git rev-parse --show-toplevel 2>$null)
    if ($LASTEXITCODE -ne 0) { throw 'not in a git repo; pass -Root or -Path' }
    $Root = Join-Path (Split-Path $repo.Trim() -Parent) 'atx-wt'
  }
  if (-not (Test-Path $Root)) { throw ('no pool root: ' + $Root) }
  # Any first-level dir that is a git worktree (checkout has a .git file/dir).
  $trees = Get-ChildItem $Root -Directory -ErrorAction SilentlyContinue |
           Where-Object { Test-Path (Join-Path $_.FullName '.git') }
}

$totalFreed = 0.0
$skippedLeases = 0
foreach ($t in $trees) {
  $lease = Join-Path $t.FullName '.atx-lease'
  if ((Test-Path $lease) -and -not $IncludeLeased) {
    $holder = (Get-Content $lease -ErrorAction SilentlyContinue) -join ' | '
    Write-Host ('SKIP (leased)  ' + $t.FullName + '  [' + $holder + ']') -ForegroundColor Yellow
    $skippedLeases++
    continue
  }
  $targets = $sweepDirs
  if ($IncludeDeps) { $targets = $targets + 'deps' }
  foreach ($d in $targets) {
    $dir = Join-Path $t.FullName $d
    if (-not (Test-Path $dir)) { continue }
    $gb = Get-DirGB $dir
    if ($DryRun) {
      Write-Host ('WOULD DELETE   ' + $dir + '  (' + $gb + ' GB)') -ForegroundColor Cyan
      $totalFreed += $gb
      continue
    }
    try {
      Remove-Item $dir -Recurse -Force -Confirm:$false -ErrorAction Stop
      Write-Host ('deleted        ' + $dir + '  (' + $gb + ' GB)') -ForegroundColor Green
      $totalFreed += $gb
    } catch {
      # Locked file (running build) or permissions: report, move on, never kill.
      Write-Host ('FAILED         ' + $dir + '  ' + $_.Exception.Message) -ForegroundColor Red
    }
  }
}

$verb = if ($DryRun) { 'would free' } else { 'freed' }
Write-Host ''
Write-Host ($verb + ' ' + [math]::Round($totalFreed, 1) + ' GB across ' + @($trees).Count + ' trees (' + $skippedLeases + ' leased trees skipped)') -ForegroundColor Green
