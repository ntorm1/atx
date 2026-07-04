# p8 Sprint 2 — Mega-Alpha Meta-Book — Progress Ledger

Base: feat/p8 @ 992cc03 (S4-5b, last commit before S2 kickoff).

Branch: feat/p8  Worktree: C:\atx-wt\p8

## Kickoff anchor re-confirmation (per the brief's ambiguity resolutions)

Confirmed (read-only) the four FROZEN call surfaces S2 CALLS but does not re-derive:

- `fund::MetaBook::run` (`meta_book.hpp:170-175`):
  `Result<MetaBookResult> run(const risk::RebalanceSchedule& sched,
   const std::function<risk::HorizonSources(usize sleeve, usize period)>& sources_at,
   const std::function<const risk::FactorModel&(usize period)>& model_at,
   const std::function<std::span<const f64>(usize period)>& returns_at,
   const book::CostInputs& cost) const`. PASS 1 walks `sleeves` independently
  (`Sleeve::run`); PASS 2 builds the trailing Ω (`meta_book.hpp:41-49`, STRICTLY
  `p < s`), allocates via `MetaAllocator`, nets via `net_fund_book`.
- `fund::MetaAllocator::allocate` (`meta_allocator.hpp:125-127`):
  `Result<CapitalWeights> allocate(const MatX& Omega, span<const f64> sleeve_vol,
   span<const f64> caps) const`.
- `fund::net_fund_book` (`netting.hpp:104-107`):
  `Result<NetResult> net_fund_book(span<const span<const f64>> sleeve_books,
   span<const span<const f64>> sleeve_prev, span<const f64> c,
   const book::CostInputs& cost)`.
- `fund::sleeve_return_cov` / `fund::fund_risk` (`cross_sleeve_risk.hpp:101-120`):
  `Result<MatX> sleeve_return_cov(span<const span<const f64>> sleeve_pnl)`;
  `Result<FundRisk> fund_risk(span<const span<const f64>> sleeve_books, span<const f64> c,
   const risk::FactorModel& V, const MatX& Omega)`.
- `fund::Sleeve` / `fund::SleeveConfig` (`sleeve.hpp:56-85`): `SleeveConfig{mh, members,
  tag, capacity_gross=1e9}`; `Sleeve::run` is a transparent one-liner delegating to
  `risk::MultiHorizonOptimizer{cfg.mh}.run(...)` — the STRUCTURAL R7 pin source.

All four signatures match the brief's guardrail table exactly (`meta_book.hpp`,
`meta_allocator.hpp`, `netting.hpp`, `cross_sleeve_risk.hpp`) — no divergence, no
re-derivation needed in S2.

## R7 pin — pre-flight analysis (before writing any wiring code)

The brief flags a risk that `stage_optimize`'s book (built via `risk::MultiPeriodOptimizer`)
might NOT reduce byte-identically from `risk::MultiHorizonOptimizer` (H=1, identity source,
minimal constraints) — the engine used by `fund::Sleeve`. Investigation before S2-2:

- `atx-engine/tests/risk/risk_multi_horizon_integration_test.cpp`,
  `MultiHorizonIntegration.R7_DegenerateReducesToMultiPeriodByteIdentical` (lines 411-488,
  ALREADY LANDED, GREEN, Sprint-1-owned, untouched by S2) proves EXACTLY this reduction at
  the engine level: `MultiHorizonOptimizer{H=1, ONE SignalHorizon::identity() source,
  GrossNet+PositionCap minimal constraints}.run(...)` is `std::bit_cast<u64>` element-wise
  byte-identical to `MultiPeriodOptimizer.run(...)` fed the SAME per-period alpha/model/cost —
  including the whole-result FNV digest.
- `atx-engine/tests/fund/fund_meta_book_integration_test.cpp`,
  `FundMetaBook.R7_OneSleeveReducesToMultiHorizonByteIdentical` (lines 481-574, ALREADY
  LANDED, GREEN, frozen fund/ layer) proves the OTHER half: one `Sleeve` + a
  `MetaAllocatorConfig` yielding `c==[1.0]` every period (single sleeve, `fractional_kelly=1`,
  `target_vol=0`, `max_gross=4` — never binds at S=1) + one-sleeve netting (net==gross, no
  crossing) ⇒ `MetaBook::run`'s `fund_books` is byte-identical to that sleeve's
  `MultiHorizonResult.books` AND to a standalone `MultiHorizonOptimizer::run` over the same
  fixture.
- `atx-impl/src/stage_riskmodel.hpp`'s doc block (lines 12-15, 93-96) + `data/adapt_factor.hpp`
  (`artifact_to_factor_model`, lines 39-49) confirm `build_risk_model(research,
  RiskModelConfig{})` (the Diagonal, inert path `stage_optimize.cpp` calls) LOWERS
  `diag_risk.hpp`'s `diagonal_risk_model(research)`'s own `(X,F,D)` into an artifact and
  `artifact_to_factor_model` forwards those SAME matrices (copied, not moved) straight into
  `risk::FactorModel::create` — bit-for-bit the SAME `FactorModel::create` call
  `diagonal_risk_model` itself makes. So `stage_optimize`'s `single_model` IS
  `diagonal_risk_model(research)`'s output, byte-for-byte, not merely "the same kind of model."

**Conclusion (composes, not merely plausible):** IF `stage_metabook`'s SingleSleeve path
feeds `MultiHorizonOptimizer` (via `Sleeve::run`) the IDENTICAL sched / ONE-identity-source
alpha / `diagonal_risk_model` / `CostInputs` that `stage_optimize.cpp`'s MVO branch builds,
THEN by composing the two ALREADY-PROVEN reductions above, `stage_metabook`'s SingleSleeve
fund book is byte-identical to `stage_optimize`'s book — provided the STAGE-LEVEL plumbing
(schedule construction, the alpha source, the resolved gross/name_cap/risk_aversion, the cost
model) is assembled identically. This is `stage_metabook`'s actual job (a wiring-fidelity
concern, not a numerics concern) and is verified empirically by a dedicated stage-boundary
test in S2-2/S2-5 (see below for the outcome).

**Design decision (recorded because it resolves the brief's "per-member vs combo" ambiguity):**
For `SleeveAssignment::SingleSleeve`, `sources_at(0, p)` returns ONE `HorizonSource` = the
ALREADY-COMBINED `combo` panel's `"alpha"` field cross-section at `p` (the SAME panel + SAME
field `stage_optimize.cpp`'s `alpha_at` reads) with `SignalHorizon::identity()` — NOT N raw
per-member alpha signals. Rationale: `stage_combine.cpp`'s calibrated per-alpha blend weights
(`AlphaCombiner`, OOS-fit) are Sprint-3-owned and are NOT re-derived by S2 (out-of-scope,
"Re-deriving any src/fund/*.cpp... FROZEN" + "Editing stage_combine.cpp — Sprint 3"); feeding
the whole admitted set as N raw per-member `HorizonSource` pairs to `Sleeve::run` would
gp_aim-AVERAGE them unweighted, which is NOT the calibrated blend `stage_optimize` deploys —
that WOULD break the "== stage_optimize book" claim for a reason unrelated to the H=1
reduction. Reading the pre-existing combo panel directly (one identity source) sidesteps that
mismatch entirely and keeps `stage_metabook` a pure ALTERNATIVE CONSUMER of the same combo
artifact `stage_optimize` consumes — never touching `stage_combine.cpp`. Multi-sleeve modes
(S2-1 `ByLibraryGroup`/`ByCorrCluster`/`BySignalFamily`) instead evaluate each sleeve's OWN
member subset and combine it LOCALLY with a documented (non-calibrated) equal-weight blend —
a distinct, S2-owned "mega-alpha per sleeve" combination, never claimed byte-identical to
anything (multi-sleeve is the on-path RED→GREEN case, not the byte-identity pin).

## MetaBookStageConfig (S2-owned stage config seam)

`atx-impl/src/stage_metabook.hpp` — NOT threaded onto `RunConfig` (Sprint-5-owned hub file,
untouched). Carries its OWN copies of the gross/name-cap/risk-aversion knobs the single-sleeve
`mh` needs (mirrors `stage_optimize`'s resolution exactly); `run_metabook` re-resolves these
from `RunConfig` using the SAME formula `stage_optimize.cpp` uses (`gross_val`/`name_cap_val`
defaulting to 1.0, `risk_aversion` gated by `set_flags.count("risk-aversion")`) so the
byte-identity pin does not depend on the caller hand-populating `MetaBookStageConfig`
correctly — `run_metabook` overwrites those three fields from `RunConfig` before calling
`assign_sleeves`. `SleeveAssignment::SingleSleeve == 0` (`static_assert`-pinned).

## Unit checklist
- [x] S2-0  ledger + `MetaBookStageConfig` + FROZEN-signature confirmation
- [x] S2-1  `assign_sleeves` (SleeveSpec seam)
- [x] S2-2  `stage_metabook` producer (two-pass drive) + R7 pin + PIT causality guard
- [x] S2-3  netting turnover telemetry
- [x] S2-4  Euler attribution + effective-bets telemetry
- [x] S2-5  allocator-method config + close determinism battery

## S2-5 wiring fix (found while writing the close battery)

`build_metabook_result` looked up `combo.field_id("alpha")` UNCONDITIONALLY, even on the
multi-sleeve/library path which never reads combo's field contents (it re-evaluates each
sleeve's own members from the library instead). Moved the lookup inside the `single_no_lib`
branch so a multi-sleeve invocation no longer spuriously requires an "alpha" field it never
uses. Caught by writing `MultiSleeveByCorrClusterReachesTheStageEndToEnd` (a combo panel with
only a "close"-shaped field would otherwise have failed field lookup); no test regression.

## S2-5 close determinism battery — all four classes proven end-to-end through the stage

- **off-path byte-identity**: the pinned S2-2 test (`MetabookStageBoundary.
  SingleSleeveByteIdenticalToStageOptimizeBook`) IS this gate — re-affirmed by name
  (`MetabookCloseBattery.OffPathByteIdenticalIsThePinnedS22Test`), not duplicated.
- **on-path RED->GREEN, end-to-end through the REAL stage** (new for S2-5 — every earlier
  multi-sleeve test used a hand-scripted engine fixture, not the actual library/DSL path):
  `MetabookCloseBattery.MultiSleeveByCorrClusterReachesTheStageEndToEnd` builds a REAL
  `library::Library` with two admit-time-PnL-correlated clusters (3 alphas each, real
  parseable DSL `"rank(close)"` / `"delta(close,2)"`), flushes it to disk (`flush_all()` --
  required: `run_metabook` re-opens the library at `cfg.library_dir` as a fresh instance,
  which only sees SEALED segments), and drives the FULL `compile_batch -> Engine::evaluate ->
  extract_streams -> per-sleeve equal-weight combine -> MetaBook::run` pipeline through
  `run_metabook(cfg, {ByCorrCluster})`. Result: `sleeves` kvs == "2" (the two clusters
  survived), a valid books panel with 6 instruments across every rebalance period. THE
  concrete proof that `evaluate_sleeve_signal` (S2-2's multi-sleeve seam, never previously
  exercised end-to-end) actually compiles/evaluates real DSL and reaches the driver.
- **twice-run (stage level)**: `MetabookCloseBattery.TwiceRunStageKvsByteIdentical` --
  identical `RunConfig` + `MetaBookStageConfig` -> identical `StageResult::digest` AND every
  `kvs` key/value pair byte-identical across two separate `run_metabook` calls (not just the
  engine-level `MetaBook::run` twice-run already proven in `fund_metabook_wire_test.cpp`).
- **seq==parallel**: the pinned S2-2 structural proof (`FundMetabookWire.
  PassOneSleeveIndependence`) IS this gate — re-affirmed by name
  (`MetabookCloseBattery.SeqEqParallelIsThePinnedS22Test`).
- **allocator method + HRP singular-Omega safety**: `MetabookAllocMethod.
  ErcDefaultAndHrpNeverTrapsOnSingularOmega` confirms the default is
  `EqualRiskContribution` and that `HierarchicalRiskParity` on an EXACTLY singular Omega
  (`det==0`, the perfectly-correlated two-sleeve fixture) returns finite, non-negative
  capital weights with `Sigma|c| <= max_gross` — never traps/NaNs.

## NCO future-work stretch (recorded verbatim per the sprint's Out-of-scope section)

**NCO (Nested Clustered Optimization, Lopez de Prado 2019a)** as the meta-allocation
successor to HRP/ERC — inner-cluster weights x cross-validated outer-cluster weights,
reducing estimation error further than HRP. It would consume Sprint 1's cleaned covariance +
clustering. Explicit future-work stretch, deferred to a p8-S2 stretch unit or the next module
(ROADMAP Future-work backlog); HRP/ERC ship first, honestly measured. S2 ships nothing NCO in
the critical path — `RiskBudgetMethod` has exactly three enumerators (InverseVol, ERC, HRP);
adding NCO is a pure-addition follow-on (`meta_allocator.hpp` is S2-owned but its estimation
math is frozen, so an NCO kernel would land in `src/fund/meta_allocator.cpp` as new code, not
a rewrite — out of this sprint's critical path per the brief).

## Sprint close — full regression

134 tests green across `atx-engine-fund-tests` + `atx-impl-tests` (fund/*, metabook_*,
optimize/pit/riskmodel suites) after S2-5; zero regressions introduced across all five units.

## S2-4 measured Euler exactness + effective-bets (EXACT, not merely within tolerance)

Fixture: two disjoint-support sleeves over 4 names (sleeve A trades {0,1}, sleeve B trades
{2,3}; diagonal V, dollar-neutral MVO) with returns_at driven by Walsh/Hadamard ±1 sequences
on each pair, giving an EXACT (not sampled/approximate) 0.0 or 1.0 correlation between the two
sleeves' realized P&L. `MetabookReport.EffectiveBetsGauge` measured: decorrelated equal-
capital sleeves -> `effective_bets == 2.0` EXACTLY; perfectly correlated sleeves ->
`effective_bets == 1.0` EXACTLY (both landed on the theoretical boundary, not merely inside
the ±0.2 tolerance band the test asserts) — the diversification gauge behaves correctly at
both boundaries, the direct counter-mechanism to Phase-D's measured crowding (`N_eff=8.76`
over 30 collapsed alphas). `MetabookReport.EulerAttributionSums`: `Sigma_s return_contrib[s]`
matches an independently-recomputed `R_fund` to 1e-6; `Sigma_s risk_contrib[s]` is finite and
non-negative; `Sigma_s crossing_credit[s]` matches an independently-recomputed total crossing
benefit to 1e-6. `MetabookReport.SingleSleeveReportDegenerateAndSharpeMatches`: one sleeve's
`effective_bets` hits the documented degenerate contract (0 or 1, never NaN/garbage) and
`fund_metrics.sharpe` matches `combine::compute_metrics` called independently on the SAME
r_fund + flattened book schedule to 1e-9. `run_metabook`'s kvs now also carries
`sleeve_return_contrib` / `sleeve_risk_contrib` / `sleeve_crossing_credit` (comma-joined) +
`fund_effective_bets` + `fund_sharpe` + the three `*_contrib_sum` cross-check fields.

## S2-3 measured netting win (the offsetting-sleeve fixture, GUARANTEED by construction)

`MetabookNetting.ReducesTurnoverOnOffsettingSleeves`: sleeve B's alpha == -sleeve A's alpha
every period (4 instruments, 4 periods) ⇒ sleeve B's optimized book is the additive inverse of
sleeve A's book (the optimizer's demean+gross-normalize is odd-symmetric under negation) ⇒
`net = |c_A − c_B|·Σ|w_i| < gross = (c_A + c_B)·Σ|w_i|` STRICTLY whenever the book is nonzero
(capital weights are always ≥ 0 by construction) — a mathematically guaranteed crossing
benefit, not an empirically-hoped-for one. Measured: `turnover_net_total < turnover_gross_total`
holds; `crossing_benefit_bps` sums to a strictly positive total; the R3 triangle
(`turnover_net[s] <= turnover_gross[s]`) and non-negativity (`crossing_benefit_bps[s] >= 0`)
hold on every period for all three allocator methods (InverseVol/ERC/HRP).
`run_metabook`'s `StageResult::kvs` now carries `fund_turnover_net` / `fund_turnover_gross` /
`crossing_benefit_bps` / `crossed_fraction` (aggregate sums over the schedule); verified on the
SingleSleeve stage path (no crossing possible): `fund_turnover_net == fund_turnover_gross`,
`crossing_benefit_bps == 0`, `crossed_fraction == 0`, parsed back from the kvs STRING surface
(not just the internal report struct) — and the digest is UNCHANGED across two runs even
though kvs strings are recomputed (telemetry never enters the fund-book digest, mirroring the
combine breadth/capacity convention).

## S2-2 RESULT — the R7 pin holds EXACTLY, both claims, no divergence

The pre-flight composed-proof (above) predicted stage-level byte-identity IF the plumbing
matched exactly. Built it (SingleSleeve reads the combo panel's "alpha" field directly as ONE
identity-horizon source when `cfg.library_dir` is empty; schedule/gross/name_cap/risk_aversion/
cost resolved with the IDENTICAL formulas `stage_optimize.cpp` uses; `model_at` =
`diagonal_risk_model(research)`, the same call `stage_optimize`'s Diagonal path makes) and it
holds EXACTLY on the first real attempt:
- Engine level (`fund_metabook_wire_test.cpp`,
  `FundMetabookWire.SingleSleeveByteIdenticalToMultiPeriodOptimizer`): one-sleeve `MetaBook`
  (c==[1] boundary config) == a standalone `risk::MultiPeriodOptimizer::run` over the SAME raw
  alpha/model/cost, `std::bit_cast<u64>` element-wise, EVERY period, EVERY name. PASS.
- Stage level (`atx-impl/tests/metabook_test.cpp`,
  `MetabookStageBoundary.SingleSleeveByteIdenticalToStageOptimizeBook`): `run_metabook`
  (`MetaBookStageConfig{}` default, no `--library-dir`) on a 6-instrument/20-date fixture ==
  `run_optimize`'s book, byte-for-byte, AND the two `StageResult::digest` values are EQUAL.
  PASS. Both claim (a) (== standalone MultiHorizonOptimizer, already proven by the frozen
  fund/ test) and claim (b) (== stage_optimize's actual deployed book) hold WITHOUT
  weakening, WITHOUT a documented divergence — the risk flagged in the dispatch brief did not
  materialize once the stage-level plumbing was assembled identically.

Design choice that made this possible (see "Design decision" above): SingleSleeve-with-no-
library reads the PRE-EXISTING combo panel directly rather than re-evaluating per-member
alphas and re-blending them locally. Had S2 instead fed the whole admitted set as N raw
per-member `HorizonSource` pairs into one sleeve (an UNWEIGHTED gp_aim average), claim (b)
would NOT have held (stage_combine's calibrated blend weights differ from an unweighted mean)
— this was the actual failure mode the risk was warning about, sidestepped by construction,
not discovered-and-patched after the fact.

## S2-2 PIT causality guard (mandatory, verified GREEN)

`FundMetabookWire.PitCausalityGuard`: (1) truncating a 6-period schedule after t=3 leaves
every fund book AND every capital-weight vector at p<=3 byte-identical to the untruncated run
(6/6 periods, 2 sleeves each). (2) perturbing sleeve returns at p>=4 by +5.0 (a large,
unmistakable perturbation) leaves c[0..3] COMPLETELY unchanged — no look-ahead leak. Both
assertions pass.

## S2-2 measured netting + diversification (preview; full S2-3/S2-4 telemetry lands next)

`FundMetabookWire.TwoSleeveComposesWithMeasuredCrossingWin` (momentum vs anti-correlated
reversal sleeve, 6-period schedule): `turnover_net < turnover_gross` strictly on at least one
period (the crossing bites), `turnover_net <= turnover_gross` holds on EVERY period (R3
triangle inequality), and the running totals show `net_total < gross_total` overall.
`AllocatorMethodDispatch`: InverseVol / EqualRiskContribution / HierarchicalRiskParity each
route correctly and `Sigma|c| <= max_gross` holds for all three.

## S2-1 deviation (recorded — ownership-driven, not a design change)

The spec's wiring note cites `library.hpp:461-475` (`Library::segment_crc_per_alpha`) for
`ByLibraryGroup`'s per-alpha segment key. That accessor is **private** on `Library`, and
`library/library.hpp` is not an S2-owned file (S2 owns `fund/*.hpp`, not `library/*.hpp`) —
S2 cannot make it public. Substituted the public `Library::kFlushBatch` (1024) as the grouping
key (`AlphaId.value / kFlushBatch`): constant within a sealed segment and steps at the same
admit-count boundaries a real per-alpha segment CRC would, so it is a faithful (if coarser —
it cannot distinguish two segments that happen to collide on CRC, an unmodeled edge case)
public substitute. A library smaller than `kFlushBatch` (every test fixture in this sprint)
collapses to one group either way, so `ByLibraryGroup`'s only currently-tested behavior (the
degenerate SingleSleeve fallback) is identical under either key.

## S1 seam (recorded per the cross-sprint contract; corrected in the p8 final-wave — see below)

**Plainly, as actually built:** `model_at(period)` ALWAYS returns `diag_risk.hpp`'s
`diagonal_risk_model(research)` (one whole-panel model, applied at every period) — there is no
Factor-model path in `stage_metabook.cpp` today, and `build_metabook_result`/`run_metabook` take
no `RiskModelConfig` at all. S2 does NOT hard-depend on S1: Ω is built LOCALLY from sleeve-return
P&L (`sleeve_return_cov`), a SLEEVE-level covariance distinct from the instrument-level
`FactorModel` V, so the diag-only `model_at` is not a missing dependency, just an unexploited
option. (An earlier draft of this note incorrectly described `model_at` as already preferring an
S1 `FactorModelArtifact` when the caller supplies a Factor `RiskModelConfig` — that sentence
contradicted this same paragraph's own next sentence, which correctly says the Factor path is
NOT wired; corrected here rather than left standing.)

A Factor-model `model_at` variant is a straightforward follow-on (the SAME per-step artifact
`stage_optimize.cpp`'s Factor branch already builds via `build_risk_model` +
`data::artifact_to_factor_model`) but was not attempted by S2 (to keep the surface minimal).
The p8 final-wave brief listed this as Item 4 — OPTIONAL, "only if a small safe additive
overload; else defer with a note." The sketch (a `const risk::RiskModelConfig &risk_cfg = {}`
default parameter on `build_metabook_result`/`run_metabook`, branching `model_at`'s single
whole-panel build between `diagonal_risk_model(research)` at the Diagonal default and
`build_risk_model(research, risk_cfg)` + `data::artifact_to_factor_model` at Factor) IS a small,
additive, default-preserving overload in principle — the default argument keeps every existing
call site (including the R7 stage-boundary pin) byte-identical. It was deferred in this
final-wave session on priority/budget grounds (items 1/2/3/5 were required; this one was
explicitly optional and lowest-priority), not because it was found infeasible. Recorded here as
the S5/S1/final-wave integration seam for whoever picks it up next.

## `run_all` / CLI seam (S5, per the brief's binding note)

`stage_metabook` is NOT inserted into `stage_run.cpp`'s `run_all` orchestration and no CLI flag
is added (`config.hpp`/`config.cpp`/`stage_run.cpp` are Sprint-5-owned hub files, untouched).
`run_metabook(const RunConfig&, const MetaBookStageConfig&)` is reachable only via its own
direct-call signature and its tests (mirrors S1's `run_optimize(cfg, risk_cfg)` overload
discipline). S5 must: (a) add `--metabook`/`--sleeve-assignment`/`--allocator-method`/
`--risk-lookback` CLI flags mapping onto `MetaBookStageConfig`, (b) thread a `metabook` step
into `run_all` between `combine` and `optimize` (or as an alternative terminal stage — S5's
call), (c) decide whether `stage_report` needs a metabook-aware kvs reader (the books panel
`stage_metabook` writes is a drop-in `stage_optimize`-shaped panel + `.meta.txt` sidecar, so
`stage_report` should consume it unchanged, but this is unverified end-to-end since S2 does not
wire `run_all`).
