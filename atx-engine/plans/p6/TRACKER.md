# p6 — Tradeable-Alpha Uplift · TRACKER

Living status for the seven p6 sprints. See [ROADMAP.md](ROADMAP.md) for scope/contracts.
**Last updated:** 2026-06-27 (after S2 merge).

## Sprint status

| Sprint | Theme | Branch | State | Merged? |
|---|---|---|---|---|
| **S1** | Eval/VM performance | `feat/p6-sprint1-eval-vm` | in progress (7 commits ahead) — not verified here | no |
| **S2** | **Factory admission** | (merged) | ✅ **DONE** — 5 commits, 109/109 factory green, whole-branch review READY | **yes → main @ `e0b4152`** |
| **S3** | Search net-cost selection | `feat/p6-s3-search-netcost` | in progress (8 commits ahead) — not verified here | no |
| **S4** | Cost-aware gates | — | not started (no worktree) | no |
| **S5** | Panel augmentation | `feat/p6-s5-panel-augment` | worktree created, 0 commits — not started | no |
| **S6** | Downstream portfolio | — | not started (no worktree) | no |
| **S7** | Wire + build tradeable alphas | — | not started — runs LAST, composes S1–S6 | no |

> S1/S3 commit counts are observed from their branches only; their tests/reviews were **not** run by
> the S2 session. Confirm independently before treating them as done.

---

## S2 — Factory Admission (COMPLETE, merged)

Commits on main: `8f95b3f` `33cb268` (S2-0) · `eec7eaf` (S2-1) · `4472769` (S2-2) · `e0b4152` (S2-3).
Touched only `factory.cpp` / `factory.hpp` / `factory_oos_test.cpp`. Determinism contract held:
default/off path byte-identical, `oracle.hpp` untouched, all goldens green.

- **S2-0** Hoisted ONE holdout `Engine` in `mine_into_oos` (no-leaked-state proven vs `vm.hpp`).
  Removed the `mean_cse_pct` report-only recompile and the `cse_pct` telemetry (field retained at
  default 0.0 — its bench/python consumers are out of S2 scope).
- **S2-1** Opt-in `FactoryConfig::cascade_gate_factor` (default **0.0 = inert**) train→holdout pre-gate.
  Bound: skip iff per-period train Sharpe `< min_dsr / factor` (factor 3.0) — conservative true upper
  bound, proven gate-on==gate-off (digest+admitted+histogram, `n_cascade_skipped>0`). Wired
  symmetrically in serial + parallel. New telemetry `FactoryReport::n_cascade_skipped`.
- **S2-2** Replaced K-per-candidate sub-window `compute_metrics` with `subwindow_sharpe()` reusing
  `combine::detail::pnl_moments` — **bit-identical by construction** (not a re-derivation). Active only
  when `dsr_subwindows >= 2`; default path untouched.
- **S2-3** Collapsed the **2** (plan said 4) holdout admission ladders into one
  `Factory::admit_on_holdout(...)` — verbatim extraction, so seq==parallel holds by construction.

### S7 wiring surface S2 exposes (inert until S7 threads a CLI flag)
- `FactoryConfig::cascade_gate_factor` (f64, default 0.0) — turn the cascade pre-gate on.
- `FactoryReport::n_cascade_skipped` (usize) — holdout-evals skipped by the gate (telemetry/report).

---

## Gaps / follow-ups uncovered during S2

**Build / dev-env (now documented in [`.agents/cpp/agent.md`](../../../.agents/cpp/agent.md) → "First-build gotchas"):**
- A fresh worktree does NOT auto-checkout the `databento-cpp` submodule → configure fails. Must
  `git submodule update --init --recursive atx-core/third-party/databento-cpp`.
- **`scripts/atx-build.ps1` is buggy** in a plain shell: its NinjaDir PATH prepend (redundant — vcvars
  already adds VS's Ninja) was observed to drop the SDK `mt.exe` → `CMAKE_MT-NOTFOUND` → clang-cl
  "broken". **Next step: delete that prepend in atx-build.ps1.** Workaround used: source vcvars + run
  cmake directly.
- `dev` preset **unity build collides on the `factory` test group** — `noisy_close` / `fixture_panel` /
  `make_panel` are redefined across `factory_*_test.cpp`. **Next step: namespace/uniquify those helpers**
  (or put shared ones in a single header) so the factory group builds under unity. Worked around with
  `-DATX_UNITY_BUILD=OFF`.
- ProcessExecutor (`*SeqParallel`) tests need `atx-shm-worker` built beside the test exe, else
  `gather_mine_scores: mine shard reported a fault` (NotFound → `0xc000001d`). Easy to miss when
  building a single test target.

**S2 functional follow-ups (non-blocking):**
- **Cascade gate is serial-only in practice.** `mine_into_oos_parallel` runs `gather_mine_scores`
  (holdout eval for ALL candidates) BEFORE the loop, so the pre-gate there saves only post-gather admit
  work — the real eval-skip win is serial-only. To realize it in parallel, gate BEFORE gather (filter the
  candidate list passed to `gather_mine_scores`) — carefully, to preserve the seq==parallel digest.
- **Cascade factor calibration is panel-specific.** factor 3.0 upper-bound proven on the seed-17 test
  panel only. The gate is OFF by default so zero default-path risk, but any config (S7 build profile)
  that turns it ON must re-run `AdmittedSetUnchanged_AfterCascadeGate`-style proof on its panel before
  trusting it.
- `cse_pct` is now write-never but still in `FactoryReport` + read by `bench/factory_bench.cpp` and the
  python binding (they silently get 0.0). Out of S2 scope; remove the field + both consumers in a
  cross-cutting cleanup (or whoever owns bench/python).
- Cosmetic: `subwindow_sharpe` doc comment cites `metrics.hpp:184-194`; the Sharpe stmt is 192-194.

**Plan-accuracy note for remaining sprints:** the S2 plan's `factory.cpp` line numbers and site counts
were STALE (it assumed ~2100 lines / 4 ladders / 3 fresh-Engine sites; real file was 1780 lines / 2
ladders / 1 fresh-Engine site, and the "seed pre-pass" functions it referenced don't exist). **Treat
every sprint plan's line numbers as approximate — locate by function/comment anchors and verify the
real structure before editing.**

---

## Next steps

1. **S1, S3** — verify their branches (build + full group tests + review) and merge, OR continue if WIP.
2. **S4, S6** — not started; per ROADMAP "minimum tradeable" path, **S6 → S4+S3** is the priority order
   (the book is wrong; net-of-cost is the binding constraint).
3. **S5** — worktree empty; start panel augmentation when ready (unlocks the catalog).
4. **S7 (last)** — thread the CLI flags for every sprint's opt-in surface (incl. S2's
   `cascade_gate_factor`), then run the real-panel hunt against the [north-star acceptance](ROADMAP.md#north-star-s7-acceptance).
5. **Dev-infra cleanup** (cheap, unblocks everyone): fix `atx-build.ps1` NinjaDir prepend; uniquify the
   factory unity-colliding test helpers; consider having `new-worktree.ps1` run `git submodule update
   --init` automatically.
