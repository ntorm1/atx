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
- [ ] S2-2  `stage_metabook` producer (two-pass drive) + R7 pin + PIT causality guard
- [ ] S2-3  netting turnover telemetry
- [ ] S2-4  Euler attribution + effective-bets telemetry
- [ ] S2-5  allocator-method config + close determinism battery

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

## S1 seam (recorded per the cross-sprint contract)

`model_at(period)` prefers the S1 `FactorModelArtifact` when the caller supplies a Factor
`RiskModelConfig`, else falls back to `diag_risk.hpp`'s `diagonal_risk_model` (S2-2). S2 does
NOT hard-depend on S1: Ω is built LOCALLY from sleeve-return P&L (`sleeve_return_cov`), a
SLEEVE-level covariance distinct from the instrument-level `FactorModel` V. `stage_metabook`'s
public entry point in S2 takes only the inert Diagonal path (mirrors S1's own "flag threaded in
Sprint 5" discipline) — a Factor-model `model_at` variant is a straightforward follow-on (same
per-step artifact `stage_optimize.cpp`'s Factor branch already builds) but is NOT wired by S2
to keep the surface minimal; recorded here as the S5/S1 integration seam.

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
