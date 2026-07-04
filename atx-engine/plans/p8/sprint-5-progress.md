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

## S5-2 — cumulative-N selection column + blocking PBO

Two DISTINCT sub-seams (kept separate per the architecture note):

**(1) Selection column.** `factory/search_driver.hpp`'s `SearchConfig` gets an appended
`atx::usize prior_trial_count{0}` field: the CROSS-RUN cumulative trial count from a
persistent library opened before this search. `search_driver.cpp`'s `evaluate_generation`
changes `gen_fit.trial_count = std::max<usize>(1U, canon.size())` to
`cfg.prior_trial_count + std::max<usize>(1U, canon.size())` (only inside the existing
`if (cfg.deflate_selection)` guard — untouched off-path). `factory.cpp`'s four persistent-
library admit paths (`mine_into` serial/MultiProcess, `mine_into_oos` serial/parallel) now read
`lib_lib.cumulative_trials()` BEFORE calling `driver.run()` (moved up from its previous
post-search read site, reused for both the search-time wire and the existing admission-time
`admit_fit.trial_count` computation) and thread it into a local `SearchConfig` copy's
`prior_trial_count`. The cascade skip-bound (`cascade_gate_passes`) is UNCHANGED — confirmed
still the byte-safe LOOSENING fold from `main`, per the kickoff note; this is a genuinely
separate mechanism from the NSGA `dsr` selection column.

**(2) Blocking PBO.** `FactoryConfig.blocking_pbo` (new field, default false) + a new
`detail::check_blocking_pbo(cfg, rep)` helper (factory.hpp/.cpp, alongside `finalize_run_pbo`):
returns `Err` iff `cfg.blocking_pbo` is set AND `rep.pbo_gate_passed` is false (a real breach);
`Ok()` otherwise (byte-identical control flow at `blocking_pbo=false` or `max_pbo>=1.0`, since
`pbo_gate_passed` fail-opens by `finalize_run_pbo`'s own pre-existing contract in both cases).
Called via `ATX_TRY_VOID` right before each of the four persistent-library admit paths' final
`return Ok(rep)` — a FAIL-CLOSED escalation of the Factory call itself, distinct from
`--pbo-hard-block` (which only flips the STAGE's exit code in `stage_discover.cpp`, never
touching `Factory::mine_into`'s return value). `stage_discover.cpp` threads
`cfg.blocking_pbo -> fcfg.blocking_pbo`.

Accept evidence (new `atx-engine/tests/factory/deflate_selection_running_n_test.cpp` +
`blocking_pbo_test.cpp`): `DeflateSelection.PriorTrialCountDeflatesSearchSelection` (gen-0's
best fitness — the ONE apples-to-apples comparable generation, since gen 0's population is
seed/grammar-determined independent of fitness — is strictly lower at `prior_trial_count=100000`
vs `=0`, same seed), `DeflateSelection.OffPathByteIdenticalWhenDeflateSelectionOff`,
`DeflateSelection.SeqEqualsParallel`, `BlockingPbo.UnadmitsOnBreach` (hand-built i.i.d.-noise
`admitted_pnls` via `finalize_run_pbo`'s own sanctioned hand-built-fixture test doorway — pure
noise reliably yields `pbo>0`, unlike the `real_signal_panel` fixture's deliberately-stationary
edge which empirically gives `pbo==0`; advisory-only stays Ok, `blocking_pbo=true` on the
identical breach returns Err), `BlockingPbo.InertAtMaxPboOffDefault` (real end-to-end
`Factory::mine_into` call, `blocking_pbo=true` + the `max_pbo=1.0` default -> still Ok). All
GREEN. Pinned goldens + full `factory`/`library`/`cascade`/`nsga` groups: 187/187 green (2
pre-existing unrelated `RobustPipelineE2E` engine-risk-group failures, predate this session —
confirmed present before any S5 edit). `AtxImplDiscover`/`AtxImplSweep` impl suites: 42/42 +
prior counts green; `atx-impl`/`atx-impl-tests` rebuild clean.

**Deviation note (why `DeflateSelection.PriorTrialCountDeflatesSearchSelection` only compares
generation 0):** an earlier draft asserted "no generation regresses" across the WHOLE run, which
is unsound — from generation 1 onward the two configs' tournament/elitism selection reads
different `raw` values (the dsr haircut), so the two runs' populations genuinely DIVERGE into
different genome trajectories; nothing orders "best of population A" vs "best of population B"
once they differ. Generation 0's population is identical between both configs (grammar/seed-
determined, computed BEFORE any fitness is known), so it is the one rigorous apples-to-apples
comparison point, and it is what the test asserts.

**Bench table (p8 final-wave ledger reconciliation, ANALYTICAL — NOT a live discover run).**
The sprint-5-wire-deflate-validate.md acceptance checklist asked for "the admitted-set size and
mean `dsr` for {undeflated, cumulative-N at N=1, at N=100}" — this was never actually recorded.
An executed measurement needs a real `Factory::mine_into` run over a real candidate population
(`-Profile prod`'s pop 300 / gen 15 is an hour-long, operator-driven exercise the harness's own
`.NOTES` explicitly forbid gating a sprint on); reproducing it here would mean either fabricating
that run's numbers or actually spending the wall-clock, neither of which this final-wave pass
does. Instead, the table below evaluates `eval::deflated_sharpe` DIRECTLY (the exact formula in
`deflated_sharpe.hpp`, computed by hand off the header's own equations — not a mock) at `T=120`
(the `real_signal_panel` fixture's date count), `skew=0`, `exkurt=0` (Gaussian baseline), over a
representative synthetic population of 8 per-period Sharpes spanning weak to exceptional, against
`min_dsr=0.5` (`kMinDsr`, the convention `factory_oos_test.cpp` already uses):

| per-period SR | annualized SR | dsr @ N=1 | dsr @ N=100 | admit @ N=1 | admit @ N=100 |
|---:|---:|---:|---:|:---:|:---:|
| 0.04 | 0.63 | 0.6686 | 0.0186 | yes | no |
| 0.08 | 1.27 | 0.8082 | 0.0496 | yes | no |
| 0.12 | 1.90 | 0.9039 | 0.1121 | yes | no |
| 0.16 | 2.54 | 0.9586 | 0.2160 | yes | no |
| 0.20 | 3.17 | 0.9846 | 0.3595 | yes | no |
| 0.25 | 3.97 | 0.9964 | 0.5657 | yes | yes |
| 0.30 | 4.76 | 0.9993 | 0.7522 | yes | yes |
| 0.40 | 6.35 | 1.0000 | 0.9534 | yes | yes |

Admitted-set size + mean `dsr` (of the same 8-candidate population, `min_dsr=0.5`):

| regime | admitted | mean dsr (admitted) | mean dsr (whole population) |
|---|---:|---:|---:|
| undeflated (no dsr floor, `min_dsr=0`) | 8 / 8 | n/a (bar off) | n/a |
| cumulative-N at N=1 | 8 / 8 | 0.9150 | 0.9150 |
| cumulative-N at N=100 | 3 / 8 | 0.7571 | 0.3784 |

This is the concrete quantified S5-2 claim the sprint plan asked for: at `N=100` the SAME
population that fully clears the bar at `N=1` DROPS 5 of 8 marginal candidates (only the
annualized-SR 3.97/4.76/6.35 survivors clear `SR*_100 ≈ 0.19–0.34`), exactly the multiple-testing
anti-snooping bite `prior_trial_count` is for — measured directly from the formula this session,
honestly labeled as analytical rather than claiming an unexecuted live run's numbers.

**Item 1 note (p8 final-wave):** at the time of the final whole-branch integration wave (all 5
sprints committed), the default `dev` preset (`ATX_UNITY_BUILD=ON`) FAILED to build
`atx-engine-factory-tests` — ~14 ODR redefinition/ambiguous-call errors: `factory_behavior_test.cpp`,
`factory_cost_aware_fitness_test.cpp`, and `factory_nsga_search_test.cpp` each wrap file-local test
helpers (`frictionless_sim`, `Lcg`, `make_panel`, ...) in an anonymous `namespace { ... }`, which
gives each FILE internal linkage under normal one-TU-per-file compilation but — per
[namespace.unnamed] — collapses into the SAME compiler-synthesized namespace once Unity batching
concatenates the three files into one translation unit. The final wave fixed this by excluding
those 3 files from Unity batching (`SKIP_UNITY_BUILD_INCLUSION`, `atx-engine/tests/CMakeLists.txt`)
rather than touching any test's logic. This note does not assert how earlier S5 sub-sprint sessions
built/tested (not verified retroactively); it records only that the DEFAULT preset was red at the
final-wave checkpoint and is green after the fix, confirmed by a full rebuild of
`atx-engine-factory-tests` + `atx-engine-library-tests` + `atx-impl`/`atx-impl-tests` under the
unmodified default preset.

## S5-3 — NEW `eval/robustness_battery`: the admission-time robustness subsystem

Greenfield: NEW `atx-engine/include/atx/engine/eval/robustness_battery.hpp` + `src/eval/
robustness_battery.cpp` (registered in `atx-engine/CMakeLists.txt`'s eval source list, needing a
reconfigure — done). Design choice: the battery is ENGINE-AGNOSTIC — it never touches a
Genome/Panel/VM. It takes a caller-supplied `Reevaluator` (`std::function<Result<f64>(const
RobustnessScenario&)>`) and only (a) builds each check's perturbed `RobustnessScenario`
(a sub-universe instrument mask, a seeded-PERMUTED alternate group map, a seeded-PERMUTED
"noise" input, or a jittered param scale) and (b) asks `Reevaluator` for the resulting edge. A
real caller (a future factory/discover integration) wraps its own compile+eval+extract_streams
path; the unit tests here use hand-built synthetic `Reevaluator` lambdas — the correct,
DIRECT way to test the battery's own orchestration/threshold logic in isolation, exactly as
`finalize_run_pbo`'s own doc pattern (hand-built fixtures) does for PBO.

Four checks, `BatteryConfig{sub_universe, alt_neutralization, noise_control, param_perturbation,
min_survival_ratio, seed}` (all-false = no-op, `Reevaluator` never called):
- `sub_universe` / `alt_neutralization`: SURVIVAL checks (`PASS iff scenario_edge >=
  min_survival_ratio * base_edge`). `alt_neutralization`'s "alternate group_map" is a SEEDED
  PERMUTATION of the candidate's own `group_id` (same label multiset, shuffled assignment) —
  self-contained, no second caller-supplied map needed.
- `noise_control`: the NEGATIVE control, INVERTED polarity (`PASS iff scenario_edge <
  min_survival_ratio * base_edge` — the edge must COLLAPSE). The "seeded random draw of matched
  marginal" is a Fisher-Yates PERMUTATION of the candidate's own input values — a permutation
  trivially preserves the exact empirical marginal while destroying genuine structure, so no
  distribution-fitting machinery is needed.
- `param_perturbation`: `param_perturbation_draws` seeded multiplicative jitters of the
  candidate's param scale; PASS iff the coefficient of variation of the resulting edges is
  `<= param_perturbation_max_cv`.

Each of the three randomized checks seeds an INDEPENDENT `atx::core::Xoshiro256pp` from
`cfg.seed XOR <check-specific salt>` — never thread/time, and never shared across checks (so no
check's result depends on which OTHER checks are enabled). No internal `parallel_for` exists
anywhere in the battery (every check is a small, fixed, sequential reduction), so seq==parallel
holds trivially by construction; `Deterministic_TwiceRun` is the load-bearing reproducibility
proof (using a `Reevaluator` that reads the RNG-derived scenario contents, not a constant mock).

**Collateral fix (unrelated to the battery's own logic, needed to build):** `ATX_UNITY_BUILD`
(the `dev` preset's batched-TU test build) newly co-batched `eval_regime_slice_test.cpp` and
`eval_lockbox_test.cpp` once this file was added to the `eval` test group (unity batch
boundaries shift when a group gains a file). Both pre-existing files independently define a
`struct Lcg` inside an anonymous namespace — safe across ordinary TUs, but an ODR redefinition
WITHIN one unity TU (unnamed namespaces merge per-TU). Renamed `eval_regime_slice_test.cpp`'s to
`RegimeSliceLcg` (2-line, behavior-preserving rename) to unblock the build; this is a latent,
pre-existing defect the S5-3 file merely exposed, not something S5-3's own logic caused.

Accept evidence (new `atx-engine/tests/eval/robustness_battery_test.cpp`, 8 tests):
`AllChecksOff_NoOp` (Reevaluator never called), `NoiseControlRejectsArtifact` (a constructed
dimensional-artifact edge that survives noise is REJECTED; a genuine edge that collapses PASSES
— the central S5-3 claim), `NoiseControlInapplicableWithoutInputValues`,
`SubUniverseCollapseRejected`, `AltNeutralizationRemovesTilt` +
`AltNeutralizationPermutesGroupIdPreservingMultiset`, `ParamPerturbationStable`,
`Deterministic_TwiceRun`. All GREEN. Full `eval` engine test group: 99/99 green. `atx-impl`/
`atx-impl-tests` rebuild clean (no consumer wired yet — see the deferred-integration note below).

**Deferred (recorded, not fabricated):** S5-3 ships the battery itself; wiring it into the LIVE
admission path (a `--robustness-battery` CLI flag + a real `Reevaluator` built from
`factory::Genome` + `alpha::Engine` + `extract_streams`, called from `Factory::mine_into`/
`mine_into_oos`) is NOT part of this unit — the spec's S5-3 task scope is "an automated battery
that rejects..." as a standalone eval/ subsystem with its own tests, and S5-4's stage-graph
wiring is where a real caller would eventually invoke it. Given the remaining sprint scope
(S5-4/S5-5) and time budget, this integration is left as an explicit gap for a future unit
(the `Reevaluator` seam was designed specifically so that integration is a thin adapter, not a
redesign).

## S5-4 — stage graph + build-megaalpha-book.ps1 (commit TBD)

**Stage graph (metabook is the new node; everything else was already wired pre-S5):**
- `atx-impl/src/config.hpp`: `kSubcommands` grown 9->10, appending `"metabook"`.
- `atx-impl/src/stages.hpp`: declared the 1-arg `run_metabook(const RunConfig&)` overload
  (co-exists with `stage_metabook.hpp`'s 2-arg `run_metabook(cfg, scfg)`; both in `atx::impl`).
- `atx-impl/src/dispatch.cpp`: usage line + `if (sub == "metabook") return run_metabook(cfg);`.
- `atx-impl/src/stage_run.cpp`: `sleeve_method_from_string` (erc/hrp/invvol -> S2
  `fund::RiskBudgetMethod`, invvol/default -> InverseVol); the 1-arg `run_metabook(cfg)` wrapper
  delegating to the S2-owned 2-arg body; and, in `run_all`'s stage 5, `cfg.metabook` SUBSTITUTES
  metabook for optimize (both write `books.bin` in the same shape `stage_report.cpp` reads) —
  the kv key becomes `cfg.metabook ? "metabook" : "optimize"` (byte-identical key text at the
  off-path default).

Accept evidence: new `atx-impl/tests/stage_run_megabook_test.cpp` — `MegaBookGraph_InertByteIdentical`
(every new S5-0..S5-4 field explicitly asserted at its inert value vs. untouched defaults ->
identical digest, identical 6-entry kvs, in order load/panel/discover/combine/optimize/report) and
`MetabookStage_SkippedAtDefault` (kvs[4].first == "optimize" at default, == "metabook" with
`--metabook`, digests differ). Both GREEN (1.27s, 1.45s). Broader regression:
`AtxImplE2E|AtxImplCli|Config` 60/60 GREEN; full `atx-impl-tests` suite rebuilt clean, 0 NEW
failures — the only FAILED lines are the two pre-existing (confirmed present before this session
touched any file) `RobustPipelineE2E.NoiseGrowsRobustLibraryByZero` /
`SyntheticPanelAdmitsRobustSurvivors`, plus the expected `_NOT_BUILT` groups
(`atx-core-tests`, `atx-tsdb-tests`, `atx-engine-parallel-tests`, `atx-engine-book-tests`,
`atx-engine-regime-tests`, `atx-engine-store-tests`) never built in this tree. `atx-impl` binary
target rebuilt explicitly (`cmake --build --preset dev --target atx-impl`, exit=0, relink only).

**Deviation (recorded): stage vocabulary.** The spec's literal `-Stage` list is
`augment|discover|riskmodel|combine|metabook|optimize|report|pipeline|all`. This codebase has NO
standalone `augment` or `riskmodel` CLI subcommand: FINRA short-interest augment
(`stage_augment.hpp`) is explicitly documented as deferred pending ORATS-seg/symbology
infrastructure (pre-existing, not S5 scope); `--risk-model`/`--dead-alpha-factors`/
`--group-neutralize` reach the engine through the EXISTING zero-arg `run_optimize(cfg)` (S5-0),
not a separate stage. `build-megaalpha-book.ps1`'s `-Stage` `ValidateSet` is therefore
`discover|combine|metabook|optimize|report|pipeline|all` — riskmodel knobs are folded into
`New-OptimizeArgv`, augment is out of scope. Likewise `--combine-method` does not exist; S3's
existing `--method` already accepts `stack`/`regime-stack` end to end, so the prod profile passes
`--method stack`. Both substitutions are commented in the script header and covered by dedicated
Pester `It`s (`New-CombineArgv - --method deviation`, `New-OptimizeArgv - --risk-model deviation`).

**`atx-impl/scripts/build-megaalpha-book.ps1`** (new): modeled on `scripts/build-tradeable-alphas.ps1`
— `New-DiscoverArgv`/`New-CombineArgv`/`New-MetabookArgv`/`New-OptimizeArgv`/`New-ReportArgv`
testable argv functions, `-DryRun`, `if ($MyInvocation.InvocationName -ne '.')` guard,
`-Profile prod|smoke` resolving population/generations (300/15 vs 40/4) and the opt-in set
(prod: `--risk-model factor --dead-alpha-factors --group-neutralize --metabook --sleeve-method hrp
--method stack --impact-in-selection --require-split-stable --blocking-pbo --min-dsr 0.5
--max-pbo 0.5`, all via `--capacity-curve` on report; smoke: every new VALUE flag passed
explicitly at its inert default, every new BOOLEAN flag OMITTED since absence is its inert value).
`-Stage all|pipeline` expansion auto-excludes metabook/optimize's non-selected alternative so a
run never writes `books.bin` twice. `AtxExe` defaults to the p8 WORKTREE's own build
(`C:\atx-wt\p8\build\bin\atx-impl.exe`) — deliberately NOT copying
`build-tradeable-alphas.ps1`'s `C:\atx\...` default, so an operator who omits `-AtxExe` never
silently invokes the main repo's (pre-S5) binary.

**`atx-impl/scripts/tests/build-megaalpha-book.Tests.ps1`** (new, Pester 3.4.0): 28 `It`s across
9 `Describe` blocks (core-flags, inert-value smoke, prod opt-in, `--workers` conditional,
population/generations tiers, `--method` deviation, the new metabook stage, `--risk-model`
deviation, `--capacity-curve` opt-in). 28/28 GREEN (`Invoke-Pester ... -PassThru`:
`TotalCount=28 PassedCount=28 FailedCount=0`). Real (non-dot-sourced) `-DryRun` invocation of both
profiles also exercised directly (not just via Pester's dot-source) — confirmed correct 4-stage
(smoke: discover/combine/optimize/report) and 4-stage (prod: discover/combine/metabook/report,
optimize excluded) argv sequences, no PowerShell errors.

**Dev-panel live smoke — OUTCOME (honest, not fabricated):** `dev-panel.bin` is confirmed ABSENT
from this worktree and no panel generator exists in it. Best-effort attempt made: ran the real
built `atx-impl.exe` (not DryRun) with the exact smoke-profile discover argv against
`work/dev/dev-panel.bin` —
```
$ ./build/bin/atx-impl.exe discover --panel work/dev/dev-panel.bin --seed-file atx-impl/tests/fixtures/alpha101.txt --gated ...
read_panel: cannot open 'work/dev/dev-panel.bin'
exit=1
```
— the expected, real failure (no fabricated wall time or admit count). DEFERRED: a genuine
dev-panel live smoke is an operator step (build a real panel via `load`+`panel` on production
ORATS data, or a synthetic one) alongside V1; it is not blocking S5-4, which is proven instead via
(a) the C++ `stage_run_megabook_test.cpp`'s two real, passing `run_all` invocations against a
synthetic 10x100 ORATS-zip fixture (genuinely exercises load->panel->discover->combine->
metabook/optimize->report end to end, including the metabook branch), (b) 28/28 Pester argv
coverage, and (c) a clean `atx-impl` binary rebuild.

## S5-5 — V1 operator scorecard template (commit TBD)

**`atx-impl/research/2026-07-03-megaalpha-book-results.md`** (new): a TEMPLATE, not a placeholder
— all 10 spec-required sections present (run provenance; book-level net-of-10bps OOS Sharpe; DSR
under cumulative-N; PBO/CSCV; CPCV; walk-forward OOS Sharpe; capacity curve; N_eff/IR breadth; the
robustness-battery pass/fail matrix; the reject-histogram/battery-failure dominant bucket), every
numeric row marked `<TBD — filled at V1>`. V1 (the full-panel prod run) is explicitly NOT executed
this sprint — it is the single out-of-loop operator milestone, per the ROADMAP's validation
discipline ("no hour-long run is a sprint gate").

**V1 command, adapted + verified composable (deviation recorded again here for visibility):** the
spec's literal V1 command uses `-Stage augment,discover` then
`-Stage riskmodel,combine,metabook,optimize,report` — this harness's `-Stage` vocabulary (S5-4's
documented deviation) has no `augment`/`riskmodel` stage, so the equivalent two-command form is:
```powershell
.\atx-impl\scripts\build-megaalpha-book.ps1 -Profile prod -Stage discover -WorkDir work\megaalpha
.\atx-impl\scripts\build-megaalpha-book.ps1 -Profile prod -Stage pipeline -WorkDir work\megaalpha
```
Both verified via a REAL (non-Pester, non-dot-sourced) `-DryRun` invocation — confirmed correct
argv and, in the second command, that `optimize` is auto-excluded (metabook substitutes it in the
prod profile) so `books.bin` is written exactly once:
```
=== V1 cmd 1: -Stage discover ===
[profile=prod] population=300 generations=15 panel=work\accept\panel.bin workdir=work\megaalpha
=== [DryRun] discover ===
  ...\atx-impl.exe discover --panel work\accept\panel.bin --seed-file atx-impl\tests\fixtures\alpha101.txt --gated --library-dir ...\_library --max-pbo 0.5 --min-dsr 0.5 --min-sharpe 0.25 --min-fitness 1.0 --max-turnover 0.50 --oos-fraction 0.25 --alpha-out ...\alphas --population 300 --generations 15 --impact-in-selection --require-split-stable --blocking-pbo

=== V1 cmd 2: -Stage pipeline ===
=== [DryRun] combine ===   ...--method stack
=== [DryRun] metabook ===  ...--sleeve-method hrp
=== [DryRun] report ===    ...--capacity-curve
(optimize absent - metabook substituted it, as designed)
```

**Accept evidence:** the existing 28/28 GREEN Pester suite already covers `-DryRun -Profile prod`
composability (the default `-Stage all` test path resolves to exactly this same
discover/combine/metabook/report sequence); the two-command form above was additionally spot-run
directly to confirm the split invocation composes identically. No binary was invoked in either
case — V1 remains DryRun-verified only, never executed.

**Deferred (recorded, not fabricated):** every numeric row in the template is unmeasured. The
robustness battery has no live `Reevaluator` wired into `Factory::mine_into`/`mine_into_oos` (S5-3's
own deferred-integration note) — row 9 of the template can only be filled once that adapter exists
or via an out-of-band battery pass over the V1 book's admitted candidates.

## Unit checklist
- [x] S5-0 CLI flag surface
- [x] S5-1 library::verdict_for deflation screens
- [x] S5-2 cumulative-N selection column + blocking PBO
- [x] S5-3 eval/robustness_battery
- [x] S5-4 stage graph + build-megaalpha-book.ps1
- [x] S5-5 V1 scorecard template
