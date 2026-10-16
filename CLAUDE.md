# atx monorepo — agent quickstart

C++20 monorepo, layered: `atx-core` (vocab + IO) → `atx-tsdb` (shm store) → `atx-engine` (alpha/factory/learn/risk) → `atx-impl` (pipeline binary). `atx-vol` = vol surface / vol-derivatives library (see `atx-vol/CLAUDE.md`). Toolchain: clang-cl 18 + CMake presets + Ninja + LLD + ccache; deps via vcpkg manifest + pinned FetchContent. Tests: GoogleTest.

## Build & test — always via the wrapper

Ninja lives inside the VS install, not on PATH. Use `scripts/atx-build.ps1` (sources vcvars, pins Ninja + `mt.exe`, exports `CCACHE_BASEDIR`, has a wrong-tree guard). `pwsh` may be absent — use `powershell` (5.1).

```powershell
# atx-vol lanes: ALWAYS -Preset vol-rel (Release+AVX2, no ThinLTO, vol-only closure)
powershell scripts\atx-build.ps1 configure -Preset vol-rel                  # build-vol-rel/
powershell scripts\atx-build.ps1 check atx-vol\src\foo.cpp -Preset vol-rel  # single-TU compile, seconds — the shaping loop
powershell scripts\atx-build.ps1 build atx-vol-quote-slice-tests -Preset vol-rel -Jobs 2  # focused target, NEVER atx-vol-tests in a lane
powershell scripts\atx-build.ps1 -Ctest -Preset vol-rel -R "^atx-vol-quote-slice$"         # anchored
powershell scripts\atx-build.ps1 -Bench configure -Preset vol-rel-avx2      # perf pins only (ThinLTO)
powershell scripts\atx-build.ps1 -Ctest -L atx_vol_fast     # release qualification only; forbidden in oracle loop
```

Never raw `cmake --build`/`ninja` outside the wrapper or `--preset` (loses `CCACHE_BASEDIR`). Iterate with `check <file>` → `build <owning-test-target>` → anchored `-Ctest -R <Suite>`. Full labels belong only to release qualification outside `vol-oracle-iter`; the oracle loop never runs them. atx-vol lane rules (focused targets, `-Jobs` for N concurrent lanes): `atx-vol/CLAUDE.md` "Build lanes".

## Parallel work — worktree POOL, never raw `git worktree add`

```powershell
powershell scripts\lease-worktree.ps1 -Branch feat/x-<run-slug> -Base <frozen-sha> -Agent <owner> -RunId <run-id> -HeartbeatId <run-unique-heartbeat> -MaxPool 20
powershell scripts\lease-worktree.ps1 -Status           # who holds what
powershell scripts\lease-worktree.ps1 -Release pool-1 -RunId <same-run-id>
```

Warm tree: 5 s no-op / 27 s branch flip. Fresh worktree: 132 s at 38% cache — and manual `git worktree add` skips submodules. The lease configures `-ConfigurePreset auto` (vol branch → `vol-rel`, else `dev`/`equity-dev`; `none` skips): a slot already warm for that preset leases in ~7 s (measured 6.6 s), a cold one pays one configure (~42–55 s, deps cloned from local mirrors). Lease v3 publication is atomic and records run_id, branch, frozen base SHA, time, and an explicit durable process or heartbeat owner. Heartbeat acquisition starts an independent continuously renewing keeper; foreground commands do not own liveness. Production callers must never use the short-lived launcher PID. Release requires the acquiring run_id and stops the keeper; investigate owner state before explicit stale recovery.

## House style — mandatory reading before C++

Before writing or reviewing any C++: read `.agents/cpp/agent.md` (authoritative — build presets, TDD, safety rules, review checklist). Other roles: `.agents/pm/agent.md` (dispatch + gates), `.agents/cio/agent.md` (direction), `.agents/research/agent.md` (cited research).

## Memory

- **Durable facts**: `atx-vol/docs/LEDGER.md` — append-only, one line per fact, grep it BEFORE re-deriving anything (build traps, measured numbers, decisions). Append on gate-pass or hard-won discovery.
- **Sprint working state**: `.superpowers/sdd/<sprint>/` — `task-N-brief.md` / `task-N-report.md` / `task-N-review.md` + `progress.md`. Contracts: `.agents/harness/TEMPLATES.md`.
- Long specs/plans: `docs/plans/`, `docs/superpowers/specs/`; per-module docs in `<module>/docs/`.

## DAG harness

Multi-lane atx-vol work: invoke the `vol-dag` skill (decision rules + orchestration), which runs the `vol-sprint` workflow — plan → parallel pool-leased build lanes → adversarial review → gate. Single-lane work: use `vol-builder` agent directly, or just work inline; fan out only when ≥2 genuinely disjoint lanes exist (multi-agent ≈ 15× tokens).
