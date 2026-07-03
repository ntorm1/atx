# p8 Sprint 5 — Wire, Deflate & Validate (the CAPSTONE) — Progress Ledger

Base: feat/p8 @ 36c8c7a (S1-S4 complete: risk-model covariance, mega-book metabook,
nonlinear/regime combine, cost/capacity/execution correctness).

Branch: feat/p8  Worktree: C:\atx-wt\p8

## Kickoff — base-state re-confirmation (2026-07-03)

Per the dispatch brief: this worktree is cut from `main` (not the stale `feat/warehouse-parity`
the spec's own warnings target). Confirmed at kickoff:
- `factory.cpp` `cascade_gate_passes` (~line 1001-1030) ALREADY has the `S1-4` `SR*_N` LOOSENING
  fold on the keep side (`sr_tr*cfg.cascade_gate_factor + sr_star_n >= cfg.min_dsr`) — there is
  NO stale `static_cast<void>(trial_count)` anywhere in `factory.cpp`. S5-2's cascade sub-seam is
  therefore a CONFIRM (verified byte-safe, unmodified), not a re-add.
- `atx-engine/tests/factory/cascade_trial_count_test.cpp` (pre-existing, p7-S1-4 work) already
  covers `MonotoneLoosensWithN`, `KeeperNeverSkippedAtAnyN`, `RealRunAdmittedSetUnchangedAtRealN`,
  `SeqEqualsParallelAtRealN` — i.e. the spec's `CascadeTrialCount.SkipThresholdMonotoneInN` accept
  criterion is ALREADY GREEN pre-S5 for the cascade skip-bound sub-seam. S5-2 adds NEW tests only
  for the genuinely-new sub-seam: the running-N fed into the NSGA `dsr` SELECTION column (a
  DIFFERENT mechanism from the cascade skip-bound; see the architecture note).
- `AdmitKind` (`library.hpp:116-125`) is `{Accept, Duplicate, RejectSharpe, RejectFitness,
  RejectTurnover, RejectCorrelated, RejectPriceScale, RejectDsrSubwindow}` — 8 enumerators
  (indices 0-7), confirming the spec's frozen-prefix claim exactly.
- `library::verdict_for` (`library.hpp:408-447`) ends at the corr screen; no DSR/PBO/split branch
  — confirmed dead-code gap.
- `combine::GateConfig`/`GateDeflation` (`gate.hpp:72-143`) exist and are ALREADY used by
  `AlphaGate::admit` (S1, merged) but ARE NOT threaded into the library facade — confirmed.
- `stage_discover.cpp`'s `gc` (GateConfig) block sets `min_sharpe/min_fitness/max_turnover/
  max_pool_corr/rt_cost_bps/min_holding_days` from `cfg.*`, but NEVER `gc.min_dsr`/`gc.max_pbo`
  (those CLI values only feed the separate `FactoryConfig.min_dsr`/`max_pbo` factory-side
  pre-check) — confirmed; S5-0 wires `gc.require_split_stable` only (see S5-0 section below for
  why `gc.min_dsr`/`gc.max_pbo` are deliberately NOT also wired here).

## Deviation from the dispatch brief (recorded, not silently absorbed)

The dispatch brief states, for `factory/fitness.cpp`: **"the impact-in-selection BODY is
S4-owned... S4's body already reads it."** Repo inspection (grep across `factory.cpp`/
`fitness.cpp`/`fitness.hpp` for `cost_selection`) shows this is **not yet true**: S4's own
progress ledger (`sprint-4-progress.md`, "SEAM (binding, for Sprint 5)") explicitly documents
that `cost::CostSelectionConfig` + `factory::apply_selection_cost` (the pure S4-4 [B7] function)
are SHIPPED but NOT YET consumed — Sprint 5 was expected to add the `FitnessCfg.cost_selection`
field AND the one call site in `fitness.cpp`'s `finish_report`. Given the dispatch brief's HARD,
binding rule is "NEVER edit factory/fitness.cpp", S5 honors the hard rule over the (apparently
stale) assumption: `--impact-in-selection`/`--selection-aum` are threaded as CLI flags
(`RunConfig` fields + parse arms, S5-0) but are **not yet connected to any engine field or
behavior** — there is no fitness.hpp field for them to populate without a matching fitness.cpp
consumer, and shipping an unused field would be exactly the "unused API" anti-pattern the
implementation-quality standard forbids. This is a genuine, honestly-recorded gap: the flags
parse and validate but do not yet change search selection. Whoever completes the S4-4 seam
(adding `cost_selection` to `FitnessCfg` + the `finish_report` call site) should also delete this
note.

## S5-0 — CLI flag surface (config.hpp / config.cpp / stage_discover.cpp / stage_optimize.cpp /
## stage_panel.cpp)

New `RunConfig` fields (appended at struct end, all inert by default):
`risk_model="diagonal"`, `dead_alpha_factors=false`, `group_neutralize=false`, `metabook=false`,
`sleeve_method="invvol"`, `impact_in_selection=false`, `selection_aum=0.0`,
`capacity_curve=false`, `require_split_stable=false`, `blocking_pbo=false`, `short_interest=""`,
`augment_out=""`, `si_publication_lag=2`, `incremental_panel=false`. (`kelly_fraction`/
`kelly_max_gross` already existed as fields since a prior sprint; S5-0 adds only their CLI
parse arms, which were missing.)

Wiring landed THIS unit (thin, at existing construction blocks):
- `stage_discover.cpp`: `gc.require_split_stable = cfg.require_split_stable`.
- `stage_optimize.cpp`: the zero-arg `run_optimize(cfg)` entry point now builds a
  `risk::RiskModelConfig` from `cfg.risk_model`/`cfg.dead_alpha_factors`/`cfg.group_neutralize`
  instead of a hardcoded `RiskModelConfig{}` — at the defaults the constructed config is
  IDENTICAL to `RiskModelConfig{}`, so the no-flag path is byte-identical by construction.
- `stage_panel.cpp`: `acquire_history_panel`'s incremental-append branch is now gated by the
  RUNTIME `cfg.incremental_panel` flag instead of the `ATX_PANEL_INCREMENTAL` compile macro
  (which no registered build ever defined, so the p7/S6 incremental path was dead code on every
  CI/dev build). Default false ⇒ unconditional full rebuild, byte-identical to pre-S5.

Deliberately NOT wired this unit (documented, not silently dropped):
- `--combine-method`: NOT a new field. `--method stack|regime-stack` (S3's existing
  `method_from_string`) already routes end-to-end from the CLI today. Adding a second,
  redundant `combine_method` flag would be pure duplication.
- `--metabook`/`--sleeve-method`: the CLI fields + validation land in S5-0; the actual
  `run_metabook` CALL from `run_all` (and the `metabook` dispatch/kSubcommands arm) is S5-4's
  job (the stage-graph assembly unit), per the sprint's own sequencing.
- `--blocking-pbo` consumption: `FactoryConfig` has no `blocking_pbo` field yet; it is added in
  S5-2 (alongside the blocking-PBO un-admit logic it gates), which is also where
  `stage_discover.cpp` threads `cfg.blocking_pbo` into it.
- `--impact-in-selection`/`--selection-aum` engine consumption: see the Deviation note above.
- `--short-interest`/`--augment-out`/`--si-publication-lag`: the RunConfig fields + parse arms
  exist, but there is no "augment" CLI subcommand — `stage_augment.hpp`'s own doc comment defers
  the CLI stage (it needs NEW infrastructure: reconstructing the panel date axis + a
  FINRA-ticker -> instrument map from the ORATS seg partition + symbology, well beyond flag
  threading). The pure engine core (`augment_panel_with_finra`) is fully built/tested; only the
  CLI stage is deferred, exactly as `stage_augment.hpp`'s own comment already documented pre-S5.
  Recorded honestly (never a hard block; never fabricated as "done").

Accept evidence: `ConfigParse.MegaBookFlags_RoundTrip`, `ConfigParse.MegaBookFlags_OmittedAreInert`,
`ConfigParse.RiskModelRejectsUnknownValue`, `ConfigParse.SleeveMethodRejectsUnknownValue`,
`ConfigFile.MegaBookFlags_RoundTrip` (new, `atx-impl/tests/config_megabook_flags_test.cpp`) — all
GREEN. Full `atx-impl-tests` suite: 254/255 green (1 unrelated pre-existing engine-side failure,
`RobustPipelineE2E.SyntheticPanelAdmitsRobustSurvivors` in `atx-engine-risk-tests`, predates this
session — no file S5 touched is in that test's dependency graph).

## S5-1 — `GateDeflation` -> `library::verdict_for` (close the p7-S1 dead-code carry-forward)

`library.hpp`: `AlphaCandidate` gets an APPENDED `combine::GateDeflation defl =
combine::kInertDeflation;` field (8th member; every existing 6/7-arg brace-init call site keeps
constructing it from the default member initializer — proven by
`LibraryVerdict.InertDeflation_ByteIdentical`, which explicitly constructs the LEGACY 6-argument
`AlphaCandidate` brace-init and asserts `.defl == kInertDeflation`). `AdmitKind` gets
`RejectDsr(8)`, `RejectPbo(9)`, `RejectSplitUnstable(10)` APPENDED (pinned by
`LibraryVerdict.AdmitKindEnumFrozenPrefix`, a `static_assert` over indices 0..10).
`verdict_for` gets the three screens inserted AFTER the existing corr check (an intentional
ordering choice — see the in-code comment — distinct from `AlphaGate::admit`'s cheap-first
order, to avoid perturbing the pre-S5-1 lazy-corr evaluation position any goldens might pin).

`factory/factory.hpp`: `FactoryReport::reject_histogram` grown from `std::array<usize,8>` to
`std::array<usize,11>` (AdmitKind now has 11 values) — a latent OOB-write landmine closed
preemptively; harmless today since no factory call site's `GateConfig` ever sets
`min_dsr>0`/`max_pbo<1`/`require_split_stable` (see below), so `kind` never actually reaches
8..10 in this sprint. Fixed two pre-existing test call sites that hardcoded the old size-8 array
(`factory_oos_test.cpp:712`) and an exhaustive `switch(AdmitKind)` with no `default`
(`library_integration_test.cpp`'s `map_kind`, extended with the new `RejectDsr/RejectPbo/
RejectSplitUnstable -> GateVerdict::RejectDsr/RejectPbo/RejectSplitUnstable` arms).

`factory/factory.cpp`: the three `library::AlphaCandidate` construction sites (`mine_into`
serial ~line 419, `mine_into` MultiProcess-gathered ~line 690, the shared `admit_on_holdout`
ladder ~line 1054) now populate `.defl.dsr` from the SAME per-candidate `dsr`/`hold_dsr` value
that call site's own factory-side pre-check already uses (`dsr >= cfg.min_dsr` / `hold_dsr >=
min_dsr`), and `.defl.split_stable` from the already-computed `split_ok`. `.defl.pbo` is left at
the inert default (0.0) — PBO is a RUN-level statistic (`finalize_run_pbo`, computed AFTER the
whole admit loop over the admitted SET), so no per-candidate value exists to populate; that is
the DISTINCT blocking-PBO seam (S5-2), never conflated with this one.

**Why this is safe today (and honestly, currently redundant) at the factory call sites:** none
of the three call sites' `GateConfig` (`gc` in `stage_discover.cpp`) ever sets `min_dsr>0.0` or
`max_pbo<1.0` — S5-0 deliberately left `gc.min_dsr`/`gc.max_pbo` at their `GateConfig{}` inert
defaults (see the S5-0 section: wiring the CLI's `--min-dsr`/`--max-pbo` into `gc` would be
redundant with the factory's OWN existing `dsr >= cfg.min_dsr` pre-check and was out of the
S5-0 wiring sketch's literal scope). So today, `verdict_for`'s new S5-1 screens are inert on
every REAL invocation via the factory/CLI pipeline; they are load-bearing for (a) any OTHER
direct caller of `Library::admit`/`try_admit` that constructs a non-inert `GateConfig` without
its own pre-check, and (b) the dedicated `LibraryVerdict.*` engine unit tests, which construct
`GateConfig{.min_dsr=0.5}` etc. directly and prove the screens fire correctly in isolation.

Accept evidence (new `atx-engine/tests/library/library_verdict_deflation_test.cpp`):
`LibraryVerdict.LowDsrRejectedWhenMinDsrSet`, `HighDsrAdmittedWhenMinDsrSet`,
`InertDeflation_ByteIdentical`, `PboRejectAndSplitReject`, `AdmitKindEnumFrozenPrefix`,
`MatchesAlphaGateAdmitAcrossDeflationBranches` — all GREEN. Full `library` + `factory` engine
test groups: 180/180 (2 pre-existing disabled tests, unrelated) + `cascade_trial_count_test.cpp`
all green. Pinned goldens re-confirmed: `FactoryOos.*` (all), `NsgaSearch.ScalarRaw_
ReproducesGoldenDigest`. `atx-impl`/`atx-impl-tests` rebuild clean (engine header change
propagates through `library.hpp`/`factory.hpp` includes).

## Unit checklist
- [x] S5-0 CLI flag surface
- [x] S5-1 library::verdict_for deflation screens
- [ ] S5-2 cumulative-N selection column + blocking PBO
- [ ] S5-3 eval/robustness_battery
- [ ] S5-4 stage graph + build-megaalpha-book.ps1
- [ ] S5-5 V1 scorecard template
