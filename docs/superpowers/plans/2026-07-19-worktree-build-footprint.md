# Worktree Build Footprint Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Cut per-worktree build footprint from ~2.1 GB to a fraction of it, and make the reduction automatic for every new worktree.

**Architecture:** Four layers applied in increasing order of risk. L1 reclaims orphaned space, L2 makes ccache hardlink identical objects instead of copying them (the structural fix), L3 NTFS-compresses what remains, L4 shrinks the objects themselves via debug-info, linkage, and target-count changes. Measurement tooling is built first so every later layer proves its own win.

**Tech Stack:** Windows PowerShell 5.1 (ASCII-only scripts), Pester 3.4.0, CMake 3.25+ presets, Ninja, clang-cl, ccache 4.13.6, NTFS.

**Spec:** `docs/superpowers/specs/2026-07-19-worktree-build-footprint-design.md`

## Global Constraints

- All PowerShell must be **ASCII-only** and **Windows PowerShell 5.1 safe** — no `&&`/`||`, no ternary, no `??`, no `-AsHashtable`. Chain with `;` and `if ($?) { }`.
- Pester target version is **3.4.0**. Use `Should Be`, not `Should -Be`. No `-ForEach`, no `BeforeDiscovery`, no `BeforeAll`. Array membership is `($arr -contains 'x') | Should Be $true` (Pester 3's `Should Contain` is a *file-content* assertion).
- Scripts that expose testable functions must guard their pipeline body with `if ($MyInvocation.InvocationName -ne '.')` so tests can dot-source them, matching `scripts/build-tradeable-alphas.ps1`.
- Directory sizing uses `robocopy <dir> NULL /L /S /NJH /BYTES /NFL /NDL /XJ /R:0 /W:0` and parses the `Bytes :` summary line. `Get-ChildItem -Recurse` is too slow on build trees. Note `/BYTES` — `/BS` is not a valid switch and makes robocopy dump usage text.
- robocopy exits non-zero on success (1 = "files would be copied"). Any script calling it must not treat that as failure, and must `exit 0` explicitly where the tool's exit code would otherwise leak.
- **Destructive actions require an explicit opt-in flag and must print exactly what they will delete before deleting it.** No script deletes anything by default.
- Never set `/fp:fast` — fast-math is banned repo-wide.
- Debug info must stay **embedded** (`/Z7`, via `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded` + `CMP0141=NEW`). Separate `/Zi` PDBs are not content-cacheable and would break cross-worktree ccache hits.
- clang-cl is in MSVC-compatible frontend mode: GNU-spelled flags must be forwarded as `/clang:-flag`, guarded by `CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"`. A bare GNU flag is an unknown argument, which `/WX` promotes to a hard error.

## Baseline Measurements (2026-07-19, before any change)

Later tasks compare against these. Re-measure with `scripts/atx-disk.ps1` from Task 1.

| Metric | Value |
|---|---|
| `C:\atx-wt` total | 16.31 GB |
| Live worktrees on disk | 14 |
| Registered worktrees in git | 45 (31 `prunable`) |
| `wt-bt-sota/build/` | 2.10 GB |
| — `.obj` (549 files) | 1.32 GB |
| — `.lib` (9 files) | 0.44 GB |
| Largest object (`solve_test.cpp.obj`) | 47.6 MB |
| ccache size / max | 5.8 / 40.0 GB |
| ccache hit rate | 41.87% |
| ccache `hard_link` | `false` |
| ccache `file_clone` | `false` |
| Object deflate ratio | 7.1x |

## File Structure

| File | Responsibility |
|---|---|
| `scripts/atx-disk.ps1` (new) | Measure and report disk usage; gated reclaim. Pure sizing/formatting functions are dot-sourceable for tests. |
| `scripts/tests/atx-disk.Tests.ps1` (new) | Pester tests for `atx-disk.ps1` pure functions. |
| `scripts/rm-worktree.ps1` (new) | Remove one worktree and prune git metadata so orphans cannot accumulate. |
| `scripts/tests/rm-worktree.Tests.ps1` (new) | Pester tests for `rm-worktree.ps1` argument handling. |
| `scripts/dev-setup.ps1` (modify) | Add ccache `hard_link=true` to the machine-global config (L2). |
| `scripts/new-worktree.ps1` (modify) | Compressed `build/` at creation (L3); `-Groups`/`-AllGroups` (L4c); adopt surviving L4 defaults. |
| `CMakeLists.txt` (modify) | `ATX_SLIM_DEBUG_INFO` option (L4a); fix stale `ATX_SHARED_LIBS` comment. |
| `CMakePresets.json` (modify) | Document the disk-sharing contract; wire L4 defaults that survive gating. |
| `docs/dev/disk-hygiene.md` (new) | What consumes disk, which caches are shared, how to reclaim, how to diagnose a fat worktree. |

---

### Task 1: Disk reporting tool

Measurement must exist before any layer changes, so each later task can prove its own win. This task ships report mode only — reclaim is Task 3.

**Files:**
- Create: `scripts/atx-disk.ps1`
- Test: `scripts/tests/atx-disk.Tests.ps1`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `Get-AtxDirSizeBytes([string]$Path)` -> `[double]` bytes, `0` if path missing.
  - `Format-AtxSize([double]$Bytes)` -> `[string]` like `"2.10 GB"`.
  - `Get-AtxWorktreeReport([string]$WorktreeRoot)` -> array of `[pscustomobject]` with `Name`, `BuildBytes`, `TotalBytes`, `HasBuild`.
  - `Get-AtxReclaimTargets()` -> array of `[pscustomobject]` with `Label`, `Path`, `Bytes`. Task 3 consumes this.

- [ ] **Step 1: Write the failing test**

Create `scripts/tests/atx-disk.Tests.ps1`:

```powershell
#Requires -Modules Pester
<#
.SYNOPSIS
  Pester tests for scripts/atx-disk.ps1 pure functions.
  Target Pester version: 3.4.0 (Windows built-in). Syntax: Describe / It / Should Be.
  AVOIDS v5-only features: -ForEach, BeforeDiscovery, BeforeAll.
#>

$scriptPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'atx-disk.ps1'
. $scriptPath

Describe 'Format-AtxSize' {
    It 'formats gigabytes with two decimals' {
        Format-AtxSize -Bytes 2254857830 | Should Be '2.10 GB'
    }
    It 'formats megabytes below the GB threshold' {
        Format-AtxSize -Bytes 49911726 | Should Be '47.60 MB'
    }
    It 'formats zero as 0.00 MB' {
        Format-AtxSize -Bytes 0 | Should Be '0.00 MB'
    }
}

Describe 'Get-AtxDirSizeBytes' {
    It 'returns 0 for a path that does not exist' {
        Get-AtxDirSizeBytes -Path 'C:\atx-does-not-exist-xyz' | Should Be 0
    }
    It 'returns a positive size for a directory with content' {
        $tmp = Join-Path $env:TEMP ('atxdisk_' + (Get-Random))
        New-Item -ItemType Directory -Force $tmp | Out-Null
        Set-Content (Join-Path $tmp 'a.txt') -Encoding ascii ('x' * 5000)
        $size = Get-AtxDirSizeBytes -Path $tmp
        ($size -gt 0) | Should Be $true
    }
}

Describe 'Get-AtxWorktreeReport' {
    It 'returns empty for a worktree root that does not exist' {
        $r = @(Get-AtxWorktreeReport -WorktreeRoot 'C:\atx-no-wt-xyz')
        $r.Count | Should Be 0
    }
    It 'reports HasBuild false when a worktree has no build dir' {
        $root = Join-Path $env:TEMP ('atxwt_' + (Get-Random))
        New-Item -ItemType Directory -Force (Join-Path $root 'demo') | Out-Null
        $r = @(Get-AtxWorktreeReport -WorktreeRoot $root)
        $r.Count | Should Be 1
        $r[0].Name | Should Be 'demo'
        $r[0].HasBuild | Should Be $false
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `powershell -NoProfile -Command "Import-Module Pester; Invoke-Pester C:\atx\scripts\tests\atx-disk.Tests.ps1"`
Expected: FAIL — the dot-source on line for `atx-disk.ps1` errors because the file does not exist.

- [ ] **Step 3: Write the implementation**

Create `scripts/atx-disk.ps1`:

```powershell
# Disk usage report + gated reclaim for the atx worktree fleet. Idempotent, read-only by
# default. ASCII-only (Windows PowerShell 5.1 safe).
#
# Report:  scripts\atx-disk.ps1
# Reclaim: scripts\atx-disk.ps1 -Reclaim          (see Task 3; prints targets, deletes nothing
#                                                  without -Yes)
#
# Sizing uses robocopy /L (list-only) rather than Get-ChildItem -Recurse: on a 2 GB build
# tree with ~550 objects the latter takes minutes, robocopy takes seconds. robocopy exits
# NON-ZERO on success (1 = "files would be copied"), so callers must ignore its exit code.

param(
  [string]$WorktreeRoot = 'C:\atx-wt',
  [string]$CcacheExe    = 'C:\atx-cache\bin\ccache.exe',
  [switch]$Reclaim,
  # NOTE: deliberately -Yes and not -Confirm. -Confirm is a PowerShell common parameter;
  # declaring our own would shadow it and behave surprisingly if [CmdletBinding()] is ever
  # added to this script.
  [switch]$Yes
)

function Get-AtxDirSizeBytes {
  param([Parameter(Mandatory = $true)][string]$Path)
  if (-not (Test-Path -LiteralPath $Path)) { return [double]0 }
  # /L list-only, /S recurse, /NJH no header, /BYTES raw bytes, /NFL /NDL quiet,
  # /XJ skip junctions (avoids reparse loops), /R:0 /W:0 never retry.
  $out = robocopy $Path NULL /L /S /NJH /BYTES /NFL /NDL /XJ /R:0 /W:0
  $line = $out | Select-String '^\s*Bytes :' | Select-Object -First 1
  if (-not $line) { return [double]0 }
  return [double](($line.ToString() -split '\s+')[3])
}

function Format-AtxSize {
  param([Parameter(Mandatory = $true)][double]$Bytes)
  if ($Bytes -ge 1GB) { return ('{0:N2} GB' -f ($Bytes / 1GB)) }
  return ('{0:N2} MB' -f ($Bytes / 1MB))
}

function Get-AtxWorktreeReport {
  param([Parameter(Mandatory = $true)][string]$WorktreeRoot)
  if (-not (Test-Path -LiteralPath $WorktreeRoot)) { return @() }
  $rows = @()
  foreach ($d in (Get-ChildItem -LiteralPath $WorktreeRoot -Directory -Force -ErrorAction SilentlyContinue)) {
    $build = Join-Path $d.FullName 'build'
    $rows += [pscustomobject]@{
      Name       = $d.Name
      HasBuild   = (Test-Path -LiteralPath $build)
      BuildBytes = (Get-AtxDirSizeBytes -Path $build)
      TotalBytes = (Get-AtxDirSizeBytes -Path $d.FullName)
    }
  }
  return $rows
}

function Get-AtxReclaimTargets {
  $targets = @(
    @{ Label = 'Windows Update download cache'; Path = 'C:\Windows\SoftwareDistribution\Download' },
    @{ Label = 'User temp';                     Path = $env:TEMP },
    @{ Label = 'Windows temp';                  Path = 'C:\Windows\Temp' }
  )
  $rows = @()
  foreach ($t in $targets) {
    $rows += [pscustomobject]@{
      Label = $t.Label
      Path  = $t.Path
      Bytes = (Get-AtxDirSizeBytes -Path $t.Path)
    }
  }
  return $rows
}

function Show-AtxDiskReport {
  param([string]$WorktreeRoot, [string]$CcacheExe)

  $drive = Get-PSDrive C
  Write-Host '== C: =='
  Write-Host ('  used {0}   free {1}' -f (Format-AtxSize ($drive.Used)), (Format-AtxSize ($drive.Free)))

  Write-Host ''
  Write-Host '== worktrees =='
  $rows = @(Get-AtxWorktreeReport -WorktreeRoot $WorktreeRoot)
  $totalBuild = 0
  foreach ($r in ($rows | Sort-Object BuildBytes -Descending)) {
    Write-Host ('  {0,-24} build {1,10}   total {2,10}' -f $r.Name, (Format-AtxSize $r.BuildBytes), (Format-AtxSize $r.TotalBytes))
    $totalBuild += $r.BuildBytes
  }
  Write-Host ('  {0} worktrees, {1} of build output' -f $rows.Count, (Format-AtxSize $totalBuild))

  Write-Host ''
  Write-Host '== git worktree registry =='
  $wtList = @(git worktree list)
  $prunable = @($wtList | Where-Object { $_ -match 'prunable' })
  Write-Host ('  {0} registered, {1} prunable (run scripts\rm-worktree.ps1 -Prune)' -f $wtList.Count, $prunable.Count)

  if (Test-Path -LiteralPath $CcacheExe) {
    Write-Host ''
    Write-Host '== ccache =='
    $cfg = & $CcacheExe -p 2>$null
    foreach ($k in @('hard_link', 'file_clone', 'compression', 'max_size', 'cache_dir')) {
      $l = $cfg | Select-String ('\b' + $k + ' =') | Select-Object -First 1
      if ($l) { Write-Host ('  ' + $l.ToString().Trim()) }
    }
    $stats = & $CcacheExe -s 2>$null
    foreach ($l in ($stats | Select-String 'Hits:|Misses:|Cache size')) {
      Write-Host ('  ' + $l.ToString().Trim())
    }
  }
}

# Pipeline body. The guard lets tests dot-source this file to load the functions
# above without running the report (same pattern as build-tradeable-alphas.ps1).
if ($MyInvocation.InvocationName -ne '.') {
  Show-AtxDiskReport -WorktreeRoot $WorktreeRoot -CcacheExe $CcacheExe
  exit 0
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `powershell -NoProfile -Command "Import-Module Pester; Invoke-Pester C:\atx\scripts\tests\atx-disk.Tests.ps1"`
Expected: PASS — 8 tests, 0 failed.

- [ ] **Step 5: Run the report against the real fleet**

Run: `powershell -NoProfile -File C:\atx\scripts\atx-disk.ps1`
Expected: prints C: usage, ~14 worktrees totalling ~16 GB of build output, 45 registered / 31 prunable, and ccache config showing `hard_link = false`. Record this output — it is the baseline for Tasks 4 through 8.

- [ ] **Step 6: Commit**

```bash
git add scripts/atx-disk.ps1 scripts/tests/atx-disk.Tests.ps1
git commit -m "feat(scripts): atx-disk.ps1 worktree disk usage report"
```

---

### Task 2: Worktree removal with automatic prune

31 of 45 registered worktrees are orphaned `prunable` entries — directories deleted by hand without `git worktree prune`. This task makes correct removal the easy path.

**Files:**
- Create: `scripts/rm-worktree.ps1`
- Test: `scripts/tests/rm-worktree.Tests.ps1`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `Resolve-AtxWorktreePath([string]$Name, [string]$WorktreeRoot)` -> `[string]` full path.
  - `Test-AtxWorktreeSafeToRemove([string]$Path, [string]$WorktreeRoot)` -> `[bool]`; false when the path escapes `$WorktreeRoot`, is the root itself, or does not exist.

- [ ] **Step 1: Write the failing test**

Create `scripts/tests/rm-worktree.Tests.ps1`:

```powershell
#Requires -Modules Pester
<#
.SYNOPSIS
  Pester tests for scripts/rm-worktree.ps1 path-safety functions.
  Target Pester version: 3.4.0. Syntax: Describe / It / Should Be.
#>

$scriptPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'rm-worktree.ps1'
. $scriptPath

Describe 'Resolve-AtxWorktreePath' {
    It 'joins the name onto the worktree root' {
        Resolve-AtxWorktreePath -Name 'wt-demo' -WorktreeRoot 'C:\atx-wt' | Should Be 'C:\atx-wt\wt-demo'
    }
}

Describe 'Test-AtxWorktreeSafeToRemove' {
    It 'rejects the worktree root itself' {
        Test-AtxWorktreeSafeToRemove -Path 'C:\atx-wt' -WorktreeRoot 'C:\atx-wt' | Should Be $false
    }
    It 'rejects a path outside the worktree root' {
        Test-AtxWorktreeSafeToRemove -Path 'C:\atx' -WorktreeRoot 'C:\atx-wt' | Should Be $false
    }
    It 'rejects a parent-traversal escape' {
        Test-AtxWorktreeSafeToRemove -Path 'C:\atx-wt\..\Windows' -WorktreeRoot 'C:\atx-wt' | Should Be $false
    }
    It 'rejects a path that does not exist' {
        Test-AtxWorktreeSafeToRemove -Path 'C:\atx-wt\nope-xyz' -WorktreeRoot 'C:\atx-wt' | Should Be $false
    }
    It 'accepts a real directory under the worktree root' {
        $root = Join-Path $env:TEMP ('atxrm_' + (Get-Random))
        $wt = Join-Path $root 'demo'
        New-Item -ItemType Directory -Force $wt | Out-Null
        Test-AtxWorktreeSafeToRemove -Path $wt -WorktreeRoot $root | Should Be $true
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `powershell -NoProfile -Command "Import-Module Pester; Invoke-Pester C:\atx\scripts\tests\rm-worktree.Tests.ps1"`
Expected: FAIL — dot-source cannot find `rm-worktree.ps1`.

- [ ] **Step 3: Write the implementation**

Create `scripts/rm-worktree.ps1`:

```powershell
# Remove an atx worktree and prune git's registry in one step. ASCII-only (PS 5.1 safe).
#
# The fleet accumulated 31 orphaned "prunable" registry entries because directories were
# deleted by hand. Removing through this script keeps the registry honest.
#
# Remove one:      scripts\rm-worktree.ps1 -Name wt-demo
# Prune orphans:   scripts\rm-worktree.ps1 -Prune
#
# Refuses to touch anything outside $WorktreeRoot.

param(
  [string]$Name = '',
  [string]$WorktreeRoot = 'C:\atx-wt',
  [switch]$Prune,
  [switch]$Force
)

function Resolve-AtxWorktreePath {
  param(
    [Parameter(Mandatory = $true)][string]$Name,
    [Parameter(Mandatory = $true)][string]$WorktreeRoot
  )
  return (Join-Path $WorktreeRoot $Name)
}

function Test-AtxWorktreeSafeToRemove {
  param(
    [Parameter(Mandatory = $true)][string]$Path,
    [Parameter(Mandatory = $true)][string]$WorktreeRoot
  )
  # Normalize BOTH sides before comparing so '..' cannot escape the root. GetFullPath is
  # purely lexical, so it works on paths that do not exist yet.
  $full = [System.IO.Path]::GetFullPath($Path).TrimEnd('\')
  $root = [System.IO.Path]::GetFullPath($WorktreeRoot).TrimEnd('\')
  if ($full -eq $root) { return $false }
  if (-not $full.StartsWith($root + '\', [System.StringComparison]::OrdinalIgnoreCase)) { return $false }
  if (-not (Test-Path -LiteralPath $full)) { return $false }
  return $true
}

if ($MyInvocation.InvocationName -ne '.') {
  $ErrorActionPreference = 'Stop'

  if ($Prune -and -not $Name) {
    Write-Host 'git worktree prune (clearing orphaned registry entries)'
    $before = @(git worktree list).Count
    git worktree prune
    $after = @(git worktree list).Count
    Write-Host ('  registered worktrees: {0} -> {1}' -f $before, $after)
    exit 0
  }

  if (-not $Name) { throw 'Pass -Name <worktree> to remove one, or -Prune to clear orphaned entries.' }

  $path = Resolve-AtxWorktreePath -Name $Name -WorktreeRoot $WorktreeRoot
  if (-not (Test-AtxWorktreeSafeToRemove -Path $path -WorktreeRoot $WorktreeRoot)) {
    throw ('Refusing to remove: ' + $path + ' (not an existing directory under ' + $WorktreeRoot + ')')
  }

  Write-Host ('removing worktree: ' + $path)
  # NOTE: do not name this $args - that is a PowerShell automatic variable and assigning
  # to it inside a script shadows the caller's argument array with confusing results.
  $gitArgs = @('worktree', 'remove', $path)
  if ($Force) { $gitArgs += '--force' }
  git @gitArgs
  if (-not $?) {
    throw ('git worktree remove failed for ' + $path + '. Re-run with -Force if the tree has local changes you intend to discard.')
  }
  git worktree prune
  Write-Host 'done (registry pruned)'
  exit 0
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `powershell -NoProfile -Command "Import-Module Pester; Invoke-Pester C:\atx\scripts\tests\rm-worktree.Tests.ps1"`
Expected: PASS — 6 tests, 0 failed.

- [ ] **Step 5: Prune the real orphaned entries**

Run: `powershell -NoProfile -File C:\atx\scripts\rm-worktree.ps1 -Prune`
Expected: `registered worktrees: 45 -> 14`.

Verify: `git worktree list | Select-String prunable` returns nothing.

- [ ] **Step 6: Commit**

```bash
git add scripts/rm-worktree.ps1 scripts/tests/rm-worktree.Tests.ps1
git commit -m "feat(scripts): rm-worktree.ps1 with automatic registry prune"
```

---

### Task 3: Gated reclaim mode

**Files:**
- Modify: `scripts/atx-disk.ps1` (add reclaim body; `Get-AtxReclaimTargets` already exists from Task 1)
- Modify: `scripts/tests/atx-disk.Tests.ps1` (add reclaim-target tests)

**Interfaces:**
- Consumes: `Get-AtxReclaimTargets()`, `Format-AtxSize()` from Task 1.
- Produces: `Invoke-AtxReclaim([switch]$Yes)` — prints every target with its size; deletes only when `-Yes` is passed.

- [ ] **Step 1: Write the failing test**

Append to `scripts/tests/atx-disk.Tests.ps1`:

```powershell
Describe 'Get-AtxReclaimTargets' {
    It 'returns a target for the Windows Update download cache' {
        $t = @(Get-AtxReclaimTargets)
        $labels = $t | ForEach-Object { $_.Label }
        ($labels -contains 'Windows Update download cache') | Should Be $true
    }
    It 'gives every target a Path and a numeric Bytes' {
        $t = @(Get-AtxReclaimTargets)
        foreach ($row in $t) {
            [string]::IsNullOrEmpty($row.Path) | Should Be $false
            ($row.Bytes -ge 0) | Should Be $true
        }
    }
}

Describe 'Invoke-AtxReclaim' {
    It 'deletes nothing and reports dry-run when -Yes is absent' {
        $probe = Join-Path $env:TEMP ('atxreclaim_keep_' + (Get-Random) + '.txt')
        Set-Content $probe -Encoding ascii 'keep me'
        Invoke-AtxReclaim | Out-Null
        Test-Path $probe | Should Be $true
        Remove-Item $probe -ErrorAction SilentlyContinue
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `powershell -NoProfile -Command "Import-Module Pester; Invoke-Pester C:\atx\scripts\tests\atx-disk.Tests.ps1"`
Expected: FAIL — `Invoke-AtxReclaim` is not recognized as a command.

- [ ] **Step 3: Write the implementation**

In `scripts/atx-disk.ps1`, insert this function immediately before the `if ($MyInvocation.InvocationName -ne '.')` guard:

```powershell
function Invoke-AtxReclaim {
  param([switch]$Yes)

  $targets = @(Get-AtxReclaimTargets)
  $total = 0
  Write-Host '== reclaim targets =='
  foreach ($t in ($targets | Sort-Object Bytes -Descending)) {
    Write-Host ('  {0,10}  {1}' -f (Format-AtxSize $t.Bytes), $t.Path)
    Write-Host ('              ({0})' -f $t.Label)
    $total += $t.Bytes
  }
  Write-Host ('  total reclaimable: {0}' -f (Format-AtxSize $total))
  Write-Host ''
  Write-Host '  NOTE: the Recycle Bin is not touched by this script. Empty it from Explorer'
  Write-Host '        after confirming you do not need anything in it.'

  if (-not $Yes) {
    Write-Host ''
    Write-Host 'DRY RUN - nothing deleted. Re-run with -Reclaim -Yes to delete the above.'
    return
  }

  Write-Host ''
  foreach ($t in $targets) {
    if (-not (Test-Path -LiteralPath $t.Path)) { continue }
    Write-Host ('deleting contents of ' + $t.Path)
    # Delete CONTENTS, not the directory itself - Windows recreates these dirs but is
    # unhappy if they vanish. Files locked by a running process are skipped, not fatal.
    Get-ChildItem -LiteralPath $t.Path -Force -ErrorAction SilentlyContinue |
      Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
  }
  Write-Host 'reclaim complete'
}
```

Then replace the guard body so `-Reclaim` routes correctly:

```powershell
if ($MyInvocation.InvocationName -ne '.') {
  if ($Reclaim) {
    Invoke-AtxReclaim -Yes:$Yes
  } else {
    Show-AtxDiskReport -WorktreeRoot $WorktreeRoot -CcacheExe $CcacheExe
  }
  exit 0
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `powershell -NoProfile -Command "Import-Module Pester; Invoke-Pester C:\atx\scripts\tests\atx-disk.Tests.ps1"`
Expected: PASS — 11 tests, 0 failed.

- [ ] **Step 5: Verify dry-run is genuinely non-destructive**

Run: `powershell -NoProfile -File C:\atx\scripts\atx-disk.ps1 -Reclaim`
Expected: prints targets and `DRY RUN - nothing deleted.` Confirm `C:\Windows\SoftwareDistribution\Download` still has content afterward.

- [ ] **Step 6: Commit**

```bash
git add scripts/atx-disk.ps1 scripts/tests/atx-disk.Tests.ps1
git commit -m "feat(scripts): gated reclaim mode for atx-disk.ps1"
```

---

### Task 4: L2 — ccache hardlinking

The structural fix. The presets already make objects byte-identical across worktrees, but `hard_link=false` means every cache hit writes a fresh physical copy. Verified during design: ccache 4.13.6 hardlinks produce valid AMD64 COFF objects with `compression` left ON.

**Files:**
- Modify: `scripts/dev-setup.ps1:48-50` (the `--set-config` block)

**Interfaces:**
- Consumes: nothing.
- Produces: machine-global ccache config with `hard_link = true`.

- [ ] **Step 1: Record the pre-change baseline**

Run:
```
powershell -NoProfile -Command "C:\atx-cache\bin\ccache.exe -p | Select-String 'hard_link|file_clone'"
```
Expected: `(default) hard_link = false` and `(default) file_clone = false`.

- [ ] **Step 2: Add the config line**

In `scripts/dev-setup.ps1`, immediately after line 50 (`ignore_options`), add:

```powershell
# DISK: hardlink cache hits into the build tree instead of copying them. The hashing keys
# above already make a given TU byte-identical in every worktree, so without this we store
# N physical copies of the same object (14 live worktrees = 16.3 GB of build output on
# 2026-07-19). Verified on ccache 4.13.6: hardlinking yields valid COFF objects with
# compression left ON, so the cache stays compressed.
# Safe because nothing in the build rewrites an object in place - ninja and lld-link only
# read objects after the compiler produces them. Do NOT enable if a build step ever
# patches a .obj, which would corrupt the shared cache entry.
& $ccacheExe --set-config 'hard_link=true'
```

Also extend the block comment at lines 42-47 by appending one line so the "why" stays discoverable:

```powershell
# hard_link is a DISK setting rather than a hashing key, but it belongs here for the same
# reason: it must apply to raw `ninja` builds, not just `cmake --preset` invocations.
```

- [ ] **Step 3: Apply it to this machine**

Run:
```
powershell -NoProfile -Command "C:\atx-cache\bin\ccache.exe --set-config 'hard_link=true'; C:\atx-cache\bin\ccache.exe -p | Select-String 'hard_link'"
```
Expected: `(C:\Users\natha\AppData\Local\ccache\ccache.conf) hard_link = true`

- [ ] **Step 4: Prove hardlinking actually happens on a real build**

Pick a worktree with an existing build and force a rebuild of one object so it comes from cache:

```
cd C:\atx-wt\wt-disp-zc
powershell -NoProfile -Command "Remove-Item build\atx-vol\CMakeFiles\atx-vol-tests.dir\tests\solve_test.cpp.obj -ErrorAction SilentlyContinue"
cmake --build --preset dev --target atx-vol-tests
powershell -NoProfile -Command "fsutil hardlink list build\atx-vol\CMakeFiles\atx-vol-tests.dir\tests\solve_test.cpp.obj"
```

Expected: `fsutil hardlink list` prints **two or more** paths — the build-tree object and its ccache entry. One path only means the object was a cache MISS (recompiled, not linked); re-run the build once more so the second compile hits cache, then re-check.

- [ ] **Step 5: Verify the build still passes**

Run: `ctest --preset dev -R Solve --output-on-failure`
Expected: all matched tests PASS. A hardlinked object that were corrupt would fail to link or crash here.

- [ ] **Step 6: Commit**

```bash
git add scripts/dev-setup.ps1
git commit -m "perf(build): hardlink ccache hits instead of copying them

Objects are already byte-identical across worktrees thanks to the
CCACHE_BASEDIR/NOHASHDIR/IGNOREOPTIONS keys, but hard_link defaulted to
false, so every cache hit wrote a fresh physical copy. 14 live worktrees
held 16.3 GB of build output while the ccache holding a superset of those
objects was 5.8 GB compressed.

Verified on ccache 4.13.6 that hardlinking produces valid COFF objects
with compression left enabled."
```

---

### Task 5: L3 — NTFS-compressed build directories

Hardlinking only helps cache *hits*. A miss writes a real file, and objects deflate 7.1x, so compression pays even after L2.

**Files:**
- Modify: `scripts/new-worktree.ps1` (add compression step before configure)

**Interfaces:**
- Consumes: nothing.
- Produces: `build/` created with the NTFS compression attribute set, inherited by every file CMake and ninja write into it.

- [ ] **Step 1: Measure the compression ratio on a real build dir**

Run:
```
compact /c /s /i /q C:\atx-wt\wt-disp-zc\build
```
Expected: a summary line showing the compression ratio. Record it — anything at or below 1.3:1 means L3 is not worth keeping and this task should be dropped rather than carried as dead config.

- [ ] **Step 2: Add the compression step to worktree creation**

In `scripts/new-worktree.ps1`, insert immediately before the `if ($NoConfigure) {` block (currently line 61):

```powershell
# DISK: create build/ up front with the NTFS compression attribute so every object ninja
# writes into it is compressed on the way to disk. Debug objects are extremely
# compressible (measured 7.1x with deflate on solve_test.cpp.obj; NTFS LZNT1 is weaker but
# still substantial). Setting the attribute on the DIRECTORY makes it inherited - files
# created later are compressed without another pass.
# Cost is write-side CPU only; reads decompress in the filesystem cache. This is invisible
# to CMake, ninja, and the compiler.
$buildDir = Join-Path $wt 'build'
New-Item -ItemType Directory -Force $buildDir | Out-Null
& compact /c /q $buildDir | Out-Null
if ($LASTEXITCODE -ne 0) {
  Write-Warning ('could not set NTFS compression on ' + $buildDir + ' (non-fatal; build proceeds uncompressed)')
}
```

- [ ] **Step 3: Create a throwaway worktree and verify the attribute is inherited**

Run:
```
cd C:\atx
powershell -NoProfile -File scripts\new-worktree.ps1 -Name wt-disktest -Branch feat/disktest -Base main -NoConfigure
powershell -NoProfile -Command "(Get-Item C:\atx-wt\wt-disktest\build).Attributes"
```
Expected: attributes include `Compressed`.

- [ ] **Step 4: Verify inheritance reaches newly written files**

Run:
```
powershell -NoProfile -Command "Set-Content C:\atx-wt\wt-disktest\build\probe.txt -Encoding ascii ('x' * 200000); (Get-Item C:\atx-wt\wt-disktest\build\probe.txt).Attributes"
```
Expected: attributes include `Compressed`, proving inheritance works and CMake/ninja output will land compressed.

- [ ] **Step 5: Remove the throwaway worktree**

Run: `powershell -NoProfile -File C:\atx\scripts\rm-worktree.ps1 -Name wt-disktest -Force`
Expected: `done (registry pruned)`.

- [ ] **Step 6: Commit**

```bash
git add scripts/new-worktree.ps1
git commit -m "perf(build): NTFS-compress worktree build dirs at creation"
```

---

### Task 6: L4a — slim debug info (gated)

Objects carry embedded full debug info. `-gline-tables-only` keeps file:line in stack traces and assertion failures but drops variable/type info. **This task is gated:** if the measured shrink is under 25%, revert and record why.

**Files:**
- Modify: `CMakeLists.txt` (add option near the `-ffile-prefix-map` block, around line 62-78)

**Interfaces:**
- Consumes: nothing.
- Produces: CMake option `ATX_SLIM_DEBUG_INFO` (default `OFF`).

- [ ] **Step 1: Record the baseline object size**

Run:
```
powershell -NoProfile -Command "$d='C:\atx-wt\wt-disp-zc\build'; $o=Get-ChildItem $d -Recurse -Filter *.obj -ErrorAction SilentlyContinue; '{0} objs, {1:N2} GB' -f $o.Count, (($o | Measure-Object Length -Sum).Sum/1GB)"
```
Record the object count and total.

- [ ] **Step 2: Add the option**

In `CMakeLists.txt`, insert immediately after the `-ffile-prefix-map` block (after line 78, before `# ---- Fast linker: LLD`):

```cmake
# ---- Debug info size -------------------------------------------------------
# Embedded debug info (/Z7) is what makes objects content-cacheable, but it is also why a
# single TU can produce a 47 MB object (solve_test.cpp.obj, measured 2026-07-19).
# -gline-tables-only keeps file/line for stack traces and assertion failures and drops
# variable/type DWARF - fine for the test gate, not for stepping through numerics in a
# debugger. OFF by default; the worktree presets opt in.
option(ATX_SLIM_DEBUG_INFO "Emit line-tables-only debug info (smaller objects, no variable inspection)" OFF)
if(ATX_SLIM_DEBUG_INFO AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # clang-cl runs the MSVC-compatible frontend and rejects the GNU spelling as an
    # unknown argument, which /WX turns into a hard error - forward it through the
    # embedded clang driver instead, exactly as the -ffile-prefix-map block above does.
    if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        add_compile_options("/clang:-gline-tables-only")
    else()
        add_compile_options("-gline-tables-only")
    endif()
endif()
```

- [ ] **Step 3: Configure and build a worktree with the option on**

Run:
```
cd C:\atx-wt\wt-disp-zc
cmake --preset dev -DATX_SLIM_DEBUG_INFO=ON
cmake --build --preset dev --target atx-vol-tests
```
Expected: configure and build succeed with no unknown-argument diagnostics. Note this forces a full cache-miss wave — the flag changes the hash, so every TU recompiles once. That is expected and is not a ccache regression.

- [ ] **Step 4: Measure the shrink and apply the gate**

Run:
```
powershell -NoProfile -Command "$d='C:\atx-wt\wt-disp-zc\build'; $o=Get-ChildItem $d -Recurse -Filter *.obj -ErrorAction SilentlyContinue; '{0} objs, {1:N2} GB' -f $o.Count, (($o | Measure-Object Length -Sum).Sum/1GB)"
```

**Gate:** compare to Step 1. If the reduction is **>= 25%**, keep the change and continue to Step 5. If it is **< 25%**, revert `CMakeLists.txt`, and record the measured numbers in the commit message of a `docs:` commit explaining the rejection. Do not leave the option in the tree unused.

- [ ] **Step 5: Verify tests still pass and stack traces still carry line numbers**

Run: `ctest --preset dev -R Solve --output-on-failure`
Expected: PASS. Line information must survive — if a test does fail, its assertion output should still name a source file and line.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt
git commit -m "perf(build): ATX_SLIM_DEBUG_INFO option for line-tables-only debug info"
```

---

### Task 7: L4b — dev-shared as the worktree default (gated)

`dev-shared` already exists and documents "one engine DLL, not ~14 static copies", but it is marked EXPERIMENTAL and `new-worktree.ps1` selects static `dev`. **Gated:** promote to default only if a full build and the full test suite pass.

**Files:**
- Modify: `scripts/new-worktree.ps1:26,45` (`-Shared` switch and preset selection)
- Modify: `CMakeLists.txt:102-107` (fix the stale comment)
- Modify: `CMakePresets.json` (drop EXPERIMENTAL from the `dev-shared` description if promoted)

**Interfaces:**
- Consumes: nothing.
- Produces: `new-worktree.ps1` gains `-Static` (opt back into static libs); `-Shared` becomes a no-op alias retained for compatibility.

- [ ] **Step 1: Fix the stale comment (correct regardless of the gate outcome)**

`CMakeLists.txt:102-103` currently claims `ATX_SHARED_LIBS=ON` is "set by the `dev` preset". That is wrong — `_base` sets it `OFF` and only `dev-shared` turns it on. Replace lines 102-107 with:

```cmake
# STATIC by default (canonical/CI, self-contained exes). ATX_SHARED_LIBS=ON (set by the
# `dev-shared` preset, NOT `dev`) builds the atx libraries as DLLs: each test exe then
# links a thin import lib instead of embedding a full copy of the engine -> much faster
# relinks and far smaller per-worktree build dirs (one engine DLL vs ~14 static copies).
# clang-cl + WINDOWS_EXPORT_ALL_SYMBOLS auto-exports; the DLLs land in bin/ beside the
# exes that load them, so ctest/applocal need no extra wiring.
```

- [ ] **Step 2: Validate a full dev-shared build in a throwaway worktree**

Run:
```
cd C:\atx
powershell -NoProfile -File scripts\new-worktree.ps1 -Name wt-sharedtest -Branch feat/sharedtest -Base main -Shared
cd C:\atx-wt\wt-sharedtest
cmake --build --preset dev-shared
```
Expected: full build succeeds. Watch specifically for unresolved-symbol link errors, which is how `WINDOWS_EXPORT_ALL_SYMBOLS` fails when a symbol needs explicit export.

- [ ] **Step 3: Run the full test suite under dev-shared**

Run: `ctest --preset dev-shared --output-on-failure`
Expected: pass rate equal to `dev`. Compare against a `dev` run on the same commit — pre-existing failures noted in memory (fitting/backtesting modules) do not count against this gate, but any NEW failure does.

- [ ] **Step 4: Measure the build-dir size difference and apply the gate**

Run:
```
powershell -NoProfile -Command ". C:\atx\scripts\atx-disk.ps1; Format-AtxSize (Get-AtxDirSizeBytes -Path 'C:\atx-wt\wt-sharedtest\build')"
```

**Gate:** compare against a fully built `dev` worktree. Promote to default only if the build dir is **meaningfully smaller** and Step 3 introduced no new failures. If either condition fails, keep `dev` as the default, leave `-Shared` opt-in, and record the measurement in the commit message.

- [ ] **Step 5: If promoted, flip the default**

In `scripts/new-worktree.ps1`, replace the `-Shared` param (line 26) and the preset selection (line 45):

```powershell
  [switch]$Shared,   # retained for compatibility; dev-shared is now the default
  [switch]$Static,   # opt back into static atx libs (bigger build dir, self-contained exes)
```

```powershell
# dev-shared (atx libs as DLLs) is the worktree default: one engine DLL instead of a static
# copy embedded in every test exe. Pass -Static for the self-contained-exe layout that CI
# and canonical acceptance runs use.
$preset = if ($Static) { 'dev' } else { 'dev-shared' }
```

Then remove the `EXPERIMENTAL: validate a full build the first time` sentence from the `dev-shared` description in `CMakePresets.json`, replacing it with a note that it is the worktree default as of 2026-07-19.

- [ ] **Step 6: Remove the throwaway worktree and commit**

```bash
powershell -NoProfile -File C:\atx\scripts\rm-worktree.ps1 -Name wt-sharedtest -Force
git add CMakeLists.txt scripts/new-worktree.ps1 CMakePresets.json
git commit -m "perf(build): default worktrees to dev-shared (atx libs as DLLs)"
```

---

### Task 8: L4c — per-worktree test group selection

`ATX_TEST_GROUPS` defaults to `all` in `atx-engine/tests/CMakeLists.txt:18`. `atx-build.ps1:114` already threads a `-Groups` parameter; `new-worktree.ps1` only mentions groups in a help string.

Per the spec, **the default stays `all`** (today's safe behavior) — this task only makes narrowing a first-class, discoverable flag rather than a hint buried in console output.

**Files:**
- Modify: `scripts/new-worktree.ps1` (add `-Groups`, thread into configure, update help output)

**Interfaces:**
- Consumes: `ATX_TEST_GROUPS` cache variable (validated in `atx-engine/tests/CMakeLists.txt`; unknown group names are a hard error).
- Produces: `new-worktree.ps1 -Groups "risk;data"`.

- [ ] **Step 1: Add the parameter**

In `scripts/new-worktree.ps1`, add to the `param(` block:

```powershell
  [string]$Groups = '',  # engine test groups to build, e.g. "risk;data" (default: all groups)
```

- [ ] **Step 2: Thread it into the configure call**

Extend the `$isoArgs` construction (currently lines 51-59) by appending after that block:

```powershell
# DISK/TIME: building only the engine test groups this worktree touches cuts both objects
# and linked exes proportionally. Unknown group names are a hard CMake error by design
# (atx-engine/tests/CMakeLists.txt), so a typo fails fast at configure rather than silently
# building nothing.
if ($Groups) { $isoArgs += ('-DATX_TEST_GROUPS=' + $Groups) }
```

`$isoArgs` is already splatted into the configure call at line 73 (`cmake --preset $preset @isoArgs`), so no change is needed there.

- [ ] **Step 3: Surface it in the ready-output**

Replace the `partial suite` help line (currently line 77) with:

```powershell
  if ($Groups) {
    Write-Host ('  groups: ' + $Groups + '  (reconfigure with -DATX_TEST_GROUPS=... to change)')
  } else {
    Write-Host '  groups: all - pass -Groups "risk;data" at creation to build only what you touch (smaller build dir, faster builds)'
  }
```

- [ ] **Step 4: Verify group selection reaches CMake**

Run:
```
cd C:\atx
powershell -NoProfile -File scripts\new-worktree.ps1 -Name wt-grouptest -Branch feat/grouptest -Base main -Groups "risk;data"
powershell -NoProfile -Command "Select-String -Path C:\atx-wt\wt-grouptest\build\CMakeCache.txt -Pattern 'ATX_TEST_GROUPS'"
```
Expected: `ATX_TEST_GROUPS:STRING=risk;data`

- [ ] **Step 5: Verify a bad group fails fast**

Run:
```
cd C:\atx
powershell -NoProfile -File scripts\new-worktree.ps1 -Name wt-badgroup -Branch feat/badgroup -Base main -Groups "notagroup"
```
Expected: configure FAILS with `ATX_TEST_GROUPS: unknown group 'notagroup'. Valid: ...`

Clean up both:
```
powershell -NoProfile -File C:\atx\scripts\rm-worktree.ps1 -Name wt-grouptest -Force
powershell -NoProfile -File C:\atx\scripts\rm-worktree.ps1 -Name wt-badgroup -Force
```

- [ ] **Step 6: Commit**

```bash
git add scripts/new-worktree.ps1
git commit -m "feat(scripts): -Groups flag on new-worktree.ps1"
```

---

### Task 9: Documentation

**Files:**
- Create: `docs/dev/disk-hygiene.md`
- Modify: `CMakePresets.json` (`_base` description gains the disk contract)

**Interfaces:**
- Consumes: final measured numbers from Tasks 4-8.
- Produces: none (docs only).

- [ ] **Step 1: Re-measure the fleet after all layers**

Run: `powershell -NoProfile -File C:\atx\scripts\atx-disk.ps1`

The doc below is written with the **2026-07-19 pre-change** numbers, which are correct as
stated (they describe the problem this work solved). Do not overwrite them. Instead, add
one line under "The shape of the problem" recording the post-change fleet total from this
run, so the doc shows both the before and the after.

- [ ] **Step 2: Write the doc**

Create `docs/dev/disk-hygiene.md`:

````markdown
# Disk Hygiene

Why the machine fills up, what is shared, and how to get space back.

## The shape of the problem

A worktree's source checkout is ~50 MB. Its `build/` tree is ~2 GB. Fourteen live
worktrees held **16.3 GB** of build output on 2026-07-19 while the sources totalled
under 1 GB. Everything expensive is regenerable.

## What is shared, and what is not

| Thing | Location | Shared across worktrees? |
|---|---|---|
| FetchContent deps | `C:\atx-cache\deps` (`ATX_DEPS_DIR`) | Yes |
| vcpkg payload | `C:\atx-cache\vcpkg_installed` | Yes |
| Compiled objects | `C:\atx-cache\ccache` | Yes — hardlinked into build trees |
| `build/` tree | `<worktree>\build` | No — one per worktree, NTFS-compressed |

The preset environment (`CCACHE_BASEDIR`, `CCACHE_NOHASHDIR`, `CCACHE_IGNOREOPTIONS`)
makes a given source file hash identically in every worktree, so the same object is
reused everywhere. With `hard_link=true` those reuses are hardlinks, not copies — N
worktrees referencing one physical extent.

## Routine commands

```powershell
scripts\atx-disk.ps1                      # report: per-worktree sizes, ccache stats
scripts\atx-disk.ps1 -Reclaim             # dry run: show reclaimable space
scripts\atx-disk.ps1 -Reclaim -Yes        # actually delete
scripts\rm-worktree.ps1 -Name wt-foo      # remove a worktree AND prune the registry
scripts\rm-worktree.ps1 -Prune            # clear orphaned registry entries
```

**Always remove worktrees with `rm-worktree.ps1`.** Deleting the directory by hand
leaves a `prunable` entry in git's registry; 31 of those had accumulated by 2026-07-19.

## Keeping a worktree small

```powershell
scripts\new-worktree.ps1 -Name wt-foo -Branch feat/foo -Groups "risk;data"
```

`-Groups` builds only the engine test groups you are touching, cutting objects and
linked exes proportionally. Unknown group names are a hard configure error, so typos
fail immediately.

## Diagnosing a worktree that grew

```powershell
. scripts\atx-disk.ps1
Format-AtxSize (Get-AtxDirSizeBytes -Path C:\atx-wt\wt-foo\build)
```

Then check, in order:

1. **Multiple build trees.** `build/`, `build-rel/`, `build-rel-avx2/`, `build-hygiene/`
   are separate. A worktree that ran benchmarks has at least two.
2. **Per-worktree deps.** `-Isolated` gives the tree its own `deps/` instead of using
   the shared cache. Intentional for parallel agents, but it costs ~200 MB per preset.
3. **Compression not applied.** `(Get-Item <path>\build).Attributes` should include
   `Compressed`. Worktrees created before 2026-07-19 predate this; fix with
   `compact /c /s <path>\build`.
4. **ccache not hardlinking.** `ccache -p | Select-String hard_link` must say `true`.
   If not, run `scripts\dev-setup.ps1`. Confirm a specific object is shared with
   `fsutil hardlink list <path-to>.obj` — two or more paths means it is linked, not copied.

## Not covered here

A large fraction of C: was unaccounted for at the time of writing — a top-level scan
found 224 GB of 418 GB used. The prime suspect is VSS shadow-copy storage, which
requires an elevated shell to inspect:

```powershell
vssadmin list shadowstorage        # run from an ADMIN prompt
```

That is unrelated to the build system and is likely a larger single win than
everything above.
````

- [ ] **Step 3: Update the preset description**

In `CMakePresets.json`, append to the `_base` `description` string, before the closing quote:

```
 DISK: ccache hard_link=true (set by scripts/dev-setup.ps1) makes these identical objects HARDLINK into each build tree instead of being copied, and scripts/new-worktree.ps1 creates build/ with the NTFS compression attribute. See docs/dev/disk-hygiene.md.
```

- [ ] **Step 4: Verify the JSON is still valid**

Run: `powershell -NoProfile -Command "Get-Content C:\atx\CMakePresets.json -Raw | ConvertFrom-Json | Out-Null; 'JSON OK'"`
Expected: `JSON OK`

- [ ] **Step 5: Verify a configure still works after the edit**

Run: `cd C:\atx; cmake --preset dev -N`
Expected: preset resolves with no error (`-N` lists without generating).

- [ ] **Step 6: Commit**

```bash
git add docs/dev/disk-hygiene.md CMakePresets.json
git commit -m "docs(build): disk hygiene guide and preset disk contract"
```

---

## Final Verification

- [ ] `powershell -NoProfile -Command "Import-Module Pester; Invoke-Pester C:\atx\scripts\tests\"` — all Pester suites pass.
- [ ] `scripts\atx-disk.ps1` reports 14 registered worktrees, 0 prunable.
- [ ] `ccache -p` shows `hard_link = true`.
- [ ] A freshly created worktree's `build/` has the `Compressed` attribute.
- [ ] `fsutil hardlink list` on a cached object shows 2+ paths.
- [ ] `ctest --preset dev` pass rate is unchanged from the pre-change baseline on the same commit.
- [ ] Total `C:\atx-wt` size recorded before and after, and the delta stated in the final commit or PR description.

## Rejected-Layer Protocol

Tasks 6 and 7 are gated on measurement. If a layer fails its gate, **revert the change** and commit a `docs:` note recording the measured numbers and the rejection. Do not leave a disabled option or unused preset in the tree — the spec's standard is that no layer is carried as dead config.
