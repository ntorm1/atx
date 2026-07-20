# Worktree Build Footprint — Design

**Date:** 2026-07-19
**Status:** Approved (design), pending implementation plan
**Scope:** `atx` + `atx-vol` build infrastructure, worktree lifecycle scripts

## Problem

Every git worktree materializes its own multi-GB build tree. Measured on
`C:\atx-wt\wt-bt-sota`:

| Component | Size |
|---|---|
| `build/` | 2.10 GB |
| — `.obj` (549 files) | 1.32 GB |
| — `.lib` (9 files) | 0.44 GB |
| — `.pdb` (3) | 0.13 GB |
| — `.pch` (3) | 0.10 GB |
| — `.dll` / `.exe` | 0.10 GB |
| `build-rel-avx2/` | 0.29 GB |
| `deps/` | 0.21 GB |
| source checkout | 0.04 GB |

14 live worktrees consume **16.3 GB**. The source checkout is ~50 MB; the rest is
regenerable build output. C: is at 418 GB used / 458 GB, ~40 GB free.

Worst single object: `solve_test.cpp.obj` at **47.6 MB** — embedded debug info
(`/Z7`) plus heavy template instantiation.

### Root cause

The build infra is already well tuned for **speed** and not at all for **disk**:

- `CCACHE_BASEDIR` / `CCACHE_NOHASHDIR` / `CCACHE_IGNOREOPTIONS` make objects
  hash identically across worktrees (byte-identity verified 2026-07-12).
- `ATX_DEPS_DIR` shares the FetchContent tree; `VCPKG_INSTALLED_DIR` shares the
  vcpkg payload.

But ccache is configured `hard_link = false` and `file_clone = false`, so **every
cache hit writes a fresh physical copy** into the worktree. The infra proves the
objects are identical, then stores N physical copies of them.

The asymmetry is stark: ccache's own cache holds ~24,000 objects in **5.8 GB**
(it compresses); the 14 build dirs hold a *subset* of those same objects in
**16.3 GB** uncompressed. Build dirs are a redundant re-expansion of data the
cache already stores deduped and compressed.

Secondary: `git worktree list` reports **45** registered worktrees but only 14
exist on disk — 31 orphaned `prunable` entries.

Tertiary (tracked, not solved here): ccache hit rate is **41.87%**, well below
what the cross-worktree hashing setup should deliver.

## Measured facts driving the design

- Objects compress **7.1x** (47.6 MB → 6.7 MB, zip/deflate).
- ccache 4.13.6 `hard_link=1` was empirically verified to:
  - produce valid AMD64 COFF objects (header `64 86 05 00`),
  - work with `compression=true` left ON (contradicting the initial assumption
    that hardlinking requires an uncompressed cache),
  - collapse repeated compiles to a single physical extent (3 hardlink entries:
    cache entry + both output objects).

This last point removes the only significant risk from the core layer.

## Design — four layers

Ordered by leverage-to-risk. L1–L3 are reversible and invisible to build output.
L4 changes artifacts and is gated per-item on a passing build.

### L1 — Reclaim (zero risk, one-time)

- `git worktree prune` — clears 31 orphaned entries.
- Recycle Bin (10.3 GB), `SoftwareDistribution\Download` (4.1 GB), user Temp
  (3.3 GB).

Expected: ~18 GB. No build impact.

### L2 — Stop copying objects (transparent, primary structural win)

Set ccache `hard_link = true`. Cache hits link instead of copy, so identical
objects across N worktrees collapse to one physical extent.

Applies to hits only — a miss still writes a real file, which L3 covers.
Verified safe above. Ninja and the linker do not modify objects in place, which
is the documented hazard for hardlinked cache entries; the plan adds an
explicit check that no build step rewrites an object.

`file_clone` (block cloning) is **not** used: it requires ReFS or NTFS/DevDrive
and C: is plain NTFS.

### L3 — Compress what remains (transparent)

Apply the NTFS compression attribute to `build/` at worktree creation, so misses
and link outputs land compressed. NTFS LZNT1 will not reach deflate's 7.1x
(it is weaker and works on 64 KB chunks; expect ~2x on objects) but it compounds
with L2 and costs only write-side CPU.

Applied at directory creation so it is inherited by all files written into it.

### L4 — Shrink the objects themselves (approved fidelity/behavior changes)

Each item independently gated on a full passing build; any that fails to pay for
itself is dropped rather than carried as dead config.

1. **`-gline-tables-only`** for debug builds. Keeps file:line in stack traces and
   assertion failures; drops full variable/type info in the debugger. Expected
   50–70% object shrink. Must stay embedded (`/Z7`) to preserve ccache
   content-cacheability.
   Spelling matters: under `clang-cl` this is passed as
   `/clang:-gline-tables-only` (the driver is in cl-compatible mode), not as a
   bare `-gline-tables-only`. The existing `CCACHE_IGNOREOPTIONS` entry covers
   `-ffile-prefix-map` only, so this new flag participates in the ccache hash —
   intended, since it changes object content.
2. **`dev-shared` (DLLs) as the worktree default.** `ATX_SHARED_LIBS=ON` so test
   exes link a thin import lib instead of embedding the whole engine. The preset
   exists and documents "one engine DLL, not ~14 static copies" but is marked
   EXPERIMENTAL and is not what `new-worktree.ps1` selects. Promoting it to the
   default requires validating a full build first.
3. **Narrower `ATX_TEST_GROUPS` default.** A worktree compiles only the groups
   its task touches. Already supported as an opt-in hint in `new-worktree.ps1`;
   this makes it the default with an explicit opt-out. Concretely:
   `new-worktree.ps1` gains a `-Groups` parameter, and an `-AllGroups` switch
   restores today's build-everything behavior. The default when neither is
   passed must be decided during planning — building all groups (today's
   behavior, safe) versus a curated minimal set (smaller, but an agent hitting
   an unlisted group must reconfigure). This is the one L4 item whose default is
   not yet settled.

## Deliverables

The user's requirement is that this be **easy by default** — no per-worktree
discipline required.

### Scripts

- **`scripts/dev-setup.ps1`** (modify) — write ccache `hard_link = true` into the
  ccache config as part of one-time setup, so L2 applies to every worktree
  without per-shell env vars.
- **`scripts/new-worktree.ps1`** (modify) — create `build/` up front with the NTFS
  compression attribute set (L3); adopt the L4 defaults that survive gating.
- **`scripts/rm-worktree.ps1`** (new) — remove a worktree and run
  `git worktree prune`, so orphaned entries cannot accumulate again.
- **`scripts/atx-disk.ps1`** (new) — two modes:
  - report: per-worktree build sizes, ccache stats/hit rate, reclaimable totals;
  - reclaim: L1 actions, with stale-build-dir reaping for worktrees whose branch
    is merged. Destructive actions require an explicit flag and print what they
    will delete before doing it.

### Docs

- **`docs/dev/disk-hygiene.md`** — what consumes disk, which caches are shared vs
  per-worktree, how to run the reclaim script, and how to diagnose a worktree
  that has grown unexpectedly.
- Update the `_base` preset description in `CMakePresets.json` to state the
  disk-sharing contract alongside the existing speed contract.

## Verification

- Per-layer before/after measurement of `C:\atx-wt` total and a single worktree's
  `build/`.
- L2: confirm hardlink counts > 1 on real build objects (`fsutil hardlink list`).
- L2/L4: full `ctest` pass on at least one worktree before any change is adopted
  as default.
- Confirm ccache hit rate does not regress (L4's flag changes alter the hash and
  will force a one-time cache miss wave — expected, must not be mistaken for a
  regression).

## Out of scope

- **~194 GB unaccounted.** Top-level scan measured 224 GB of 418 GB used. Prime
  suspect is VSS shadow-copy storage (`vssadmin list shadowstorage` requires an
  elevated shell and was not run). Tracked separately; unrelated to worktrees.
- ccache's 41.87% hit rate. Real problem, separate investigation.
- Moving build output to another volume — C: is the only fixed drive.
