# Follow-up plan — Combine regime-combiner pipeline (deferred from Task 9 / P3.2)

**Status:** NOT STARTED. Split out of pipeline-remediation Task 9 by human decision
(2026-06-20) after a verified architectural blocker. Task 9 landed only 9.1 (alignment
assert) + 9.2 (decorrelate) on branch `feat/pipeline-remediation` (commits 4c4db23,
5b17d10). This plan owns the rest of P3.2: the HMM regime-label pipeline + the per-date
regime combiner.

## Why it was deferred (the blocker — do not re-discover it)
`fit_regime_combiner(pool, regime_labels, n_regimes, fit_begin, fit_end, cfg)`
(`atx-engine/include/atx/engine/combine/regime_combiner.hpp:107`) needs per-period `u32`
`regime_labels` of length `pool.n_periods()`, where label `t` corresponds to combine-panel
period `t`. That join is currently **impossible**:

- The combine stage reads a PRE-SERIALIZED panel via `read_panel`
  (`atx-impl/src/serialize_panel.cpp`). That on-disk format stores only a date COUNT `D`,
  no timestamps. `alpha::Panel` exposes `dates()` as a count; `DateIdx` is a bare 0-based
  `usize` (`atx-engine/include/atx/engine/alpha/panel.hpp`) — **no calendar accessor.**
- The regime macro segment DOES carry a real unix-nanos axis (tsdb
  `SegmentReader::times()`), built on the FRED business-day calendar, forward-filled
  (`atx-impl/.../regime/loader.hpp`, `load_regime_history`).
- The two axes differ in general (FRED business days vs the screened trading-universe axis
  after `--start/--end` + universe compaction) and share NO join key. Any positional map
  (first/last `D` rows, assume-coincident) is PIT-incorrect.
- The `regime` stage is also a standalone subcommand — NOT wired into the `run`/`run_all`
  pipeline (`stage_run.cpp`), so nothing currently emits a regime segment guaranteed to
  match the combine panel.

## Prerequisite (the real first task): persist the panel date axis
Pick ONE (option 1 recommended — see Task-9 report §9.3 for the full options analysis at
`.superpowers/sdd/scratch/task-9-report.md`):

1. **Persist the panel calendar in the `.bin`.** Bump `serialize_panel` `kVersion`; thread
   the `D` unix-nanos dates from `build_history_panel` → `stage_panel` → the `.bin`; add a
   calendar accessor to the reconstructed `alpha::Panel` (or return it alongside). Update
   EVERY panel reader for the new version (format-compat is the main review surface). This
   is a cross-cutting serialization-format change — it deserves its own task + review, NOT
   to be bolted onto a feature task.
2. **Date sidecar** (`panel.bin.dates`) written by `stage_panel`, read by `stage_combine`.
   Smaller blast radius, no format bump, but a parallel artifact to keep in sync.

Either way: combine gains a real per-date calendar to join the regime segment against.

## Then: the regime pipeline (was Task 9.3 / 9.4)
Interfaces are all verified (see the Task-9 brief
`.superpowers/sdd/scratch/task-9-brief.md` §9.3/9.4 for exact signatures + file:line):

- **9.3 labels:** read the regime macro segment (new `--regime-segment` flag / reuse
  `cfg.regime_out`); build `hmm_lin::MatX obs` (T × n_macro_dims) JOINED to the panel
  calendar (forward-fill / PIT-safe), **assert `obs.rows() == pool.n_periods()`**, fail
  closed on misalignment. Fit `learn::baum_welch(obs, HmmCfg{n_states=--n-regimes (default
  3), master_seed=cfg.seed})` — byte-identical for fixed (obs, cfg), seeded init
  (`hmm.hpp:244-252`). `learn::posterior_decode(hmm, obs)` → `vector<u32>` labels.
- **9.4 combiner + per-date blend:** add a `regime` method;
  `fit_regime_combiner(pool, labels, n_regimes, fit_begin, fit_end, cfg)` → `RegimeCombiner
  rc`. The static step-9 blend becomes PER-DATE for the regime method only: per date `t`,
  `w_t = rc.blend(regime_posterior_at(hmm, obs, t))` (PIT posterior, `hmm.hpp:296`), apply
  `w_t[a]` to `streams.positions(a, t)`; assert `w_t.size() == streams.n_alphas()`. Keep
  the static path unchanged for all non-regime methods.

## Constraints (carry from Task 9)
- DEFAULT combine output stays byte-identical (regime path opt-in behind the method flag).
- The whole regime path twice-run byte-identical (drive HMM `master_seed` from `cfg.seed`).
- Works over both the `.dsl` and `--library-dir` (Task-8) pools (same `combine::AlphaStore`).
- `/W4 /WX` clean. Mirror engine tests: `combine_regime_combiner_test.cpp`,
  `combine_crowding_test.cpp`. Note: the regime/MV combiners only pay off once the pool is
  populated (P3.2 note) — value is future-state, not blocking-anything-today.

## Also worth picking up here (Task 9 minor)
- `--capacity-floor` (landed in 9.2) is not yet meaningfully wired: with the constant 1.0
  capacity placeholder, a `floor >= 1` uniformly scales weights by `1/floor`. Either wire a
  real cost-model capacity or document `--capacity-floor` as a no-op-placeholder until then.
