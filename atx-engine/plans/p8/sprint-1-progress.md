# p8 Sprint 1 — Risk-Model Covariance Spine — Progress Ledger

Base: main @ 8e5f0e9 (p8 ROADMAP + 5 sprint plans added; no p8 code yet).

Branch: feat/p8  Worktree: C:\atx-wt\p8

## Kickoff anchor re-confirmation (per the brief's ambiguity resolutions)

- `FactorModelBuilder::build_components` confirmed at `risk/factor_model.hpp:253-255`,
  returns `FactorComponents{X, F, D, fit_end}` (`factor_model.hpp:217-222`).
- `diagonal_risk_model` confirmed at `atx-impl/src/diag_risk.hpp:29-70` (`X=M×1 zeros`,
  `F=Identity(1,1)`, `D`=per-instrument TRI-return population variance floored at 1e-4).
- `stage_optimize.cpp:202` confirmed: `ATX_TRY(auto model, diagonal_risk_model(research));`
  is the sole risk-model call on the MVO path; `model_at` (`:225-227`) returns it for
  every period.
- `data/factor_model_artifact.hpp` confirmed EXISTING (S6.6, `atx::engine::data`
  namespace): `FactorModelArtifact{MatX X, MatX F, VecX D, usize fit_begin, fit_end}`,
  lowered via `data::artifact_to_factor_model` (`data/adapt_factor.hpp/.cpp`) which
  forwards to `risk::FactorModel::create` (single validation point). EXTENDED, not
  forked, per the brief.
- `multi_horizon.hpp:154-157` does NOT carry the `n_dead_factors` `NotImplemented`
  guard the brief's gap table cites (that region is `MultiHorizonConfig`/`Result`
  fields). The REAL guard is `FactorModelBuilder::build()` at
  `src/risk/factor_model.cpp:354-358` (dead rung, precedence over stat) and
  `build_components()` at `factor_model.cpp:374-378` (rejects stat/dead — dispatched
  by `build()`, not `build_components`). Both are in the FROZEN estimation-body file
  (`src/risk/factor_model.cpp`) S1 must NOT edit. S1-4/S1-5 do not need to touch
  either guard: `stage_riskmodel` calls `build_components` directly (never routes
  through `build()`), so the dead/stat dispatch in `build()` is simply never on our
  call path — augmentation happens via the separate free function
  `risk::augment_factor_model` (`risk/dead_factor.hpp`), which is NOT gated by those
  guards. Neutralization is `FactorModel::neutralize` (`factor_model.hpp:164`), a
  method on the ALREADY-ASSEMBLED model — also untouched by the `build()` guards.
  **Ledger note (deviation from brief's file:line, kept because the actual call path
  bypasses the cited guards entirely — no code in the guarded functions needs to
  change for S1-4/S1-5 to work).**
- **Architecture wrinkle not anticipated by the brief:** `FactorModelBuilder::build_components`
  consumes `atx::engine::PanelView` (`loop/panel_types.hpp`), a NON-OWNING zero-copy
  ring-buffer accessor over `RollingPanel` storage — NOT `atx::engine::alpha::Panel`
  (the flat, file-backed, date-major research panel `atx-impl` actually reads/writes).
  No existing bridge converts one to the other (`RollingPanel` is a live-loop
  construct; grep of `atx-impl` for `PanelView(` returns zero hits). Existing engine
  tests (`risk_factor_builder_test.cpp`'s `PanelFixture`) build a `PanelView` by
  owning a small `(fields_, mask_)` buffer and constructing a `PanelView` over it —
  this is the precedented, cold-path-acceptable pattern `stage_riskmodel.cpp` follows
  (own a per-fit-window buffer sized to the lookback, populate it by reversing
  `alpha::Panel`'s date-major order into `PanelView`'s newest-first order, construct
  the view, call `build_components`, discard the buffer). Documented here since it
  is a real design decision the brief's dependency map did not spell out.
- `sector_groups.hpp` (`atx-impl/src`) already builds a dense `group_map` from a
  panel's "sector" field for `S1-5`'s group_map need — reused, not re-derived.
- `RunConfig`/`stages.hpp`'s `run_optimize(const RunConfig&)` signature is UNCHANGED.
  `config.hpp`/`config.cpp` are Sprint-5-owned hub files; the brief's own "Out of
  scope" section confirms `--risk-model`/CLI threading is S5's job and S1 "proves the
  engine path via a direct-call integration test, not the CLI." `RiskModelConfig` is
  therefore a pure engine/impl-internal struct, never added to `RunConfig`. The
  no-flag `run_optimize` call is edited to route through a DEFAULT-CONSTRUCTED
  `RiskModelConfig` (inert ⇒ `Diagonal`), preserving byte-identical output; the
  Factor path is exercised by calling the new `stage_riskmodel`/`stage_optimize`
  helpers directly from tests, exactly as the brief's bench section prescribes.

## Unit checklist
- [x] S1-0  RiskModelConfig + FactorModelArtifact POD + ledger (this file)
- [x] S1-1  stage_riskmodel producer (build_risk_model)
- [x] S1-2  stage_optimize covariance-source swap
- [x] S1-3  cleaned_alpha_cov accessor (combine seam; Sprint-3 handoff)
- [x] S1-4  dead-alpha crowding factors
- [ ] S1-5  factor/industry neutralization reachable

## Determinism contract (every unit)
- `RiskModelKind::Diagonal == 0` (frozen enum index); inert default routes to the
  EXACT existing `diagonal_risk_model` path — byte-identical books/digests.
- `dead_alpha_factors=false` / `group_neutralize=false` ⇒ no augmentation / neutralize
  path unchanged.
- No RNG anywhere on S1's path (FactorModelBuilder estimation, dead-factor
  eigen-extraction, neutralize are all order-fixed deterministic).
- `oracle.hpp` untouched; no golden re-baseline.

## Log
S1-0: complete. RiskModelConfig (kind/dead_alpha_factors/group_neutralize/
fit_lookback_days/style toggles) added to risk/factor_model.hpp next to the
FactorModel apply-math it configures (no estimation-body edit); RiskModelKind::
Diagonal pinned at enum value 0 via static_assert + runtime test.
FactorModelArtifact (data/factor_model_artifact.hpp, EXTENDED not forked) gained
serialize_artifact/deserialize_artifact (flat little-endian header + column-major
f64 payload) + digest_artifact (atx::core::hash_bytes, wyhash). New suite
RiskModelConfigSpine (atx-engine/tests/risk/risk_model_config_test.cpp) 5/5 green:
DefaultsAreInert, DiagonalIsFrozenAtZero, ArtifactRoundTripByteIdentical (bit-exact
X/F/D/fit_begin/fit_end + re-lowers through the existing S6.6
artifact_to_factor_model seam), ArtifactDigestStableTwiceRun, ArtifactDigestChangesOnMutation.
RED verified first (17 compile errors: RiskModelConfig/RiskModelKind/
serialize_artifact/deserialize_artifact/digest_artifact all undeclared) before
any implementation was written.
Full gate: atx-engine-risk-tests 256/258 pass (2 pre-existing RobustPipelineE2E
failures, confirmed independent of this change — same two tests fail with
risk_model_config_test.cpp removed from the build; documented in p8 ROADMAP.md as
a pre-p8 backlog item); atx-engine-data-tests 118/121 pass (3 skipped, real-data/
operator-only tests, pre-existing); atx-impl-tests 202/206 pass (4 skipped,
pre-existing). Zero new failures, zero touched files outside the owned set.

S1-1: complete. NEW atx-impl/src/stage_riskmodel.{hpp,cpp} (registered in
atx-impl/CMakeLists.txt's explicit stage_*.cpp list, which is NOT globbed —
confirmed by reading the file before editing) implements
build_risk_model(research, cfg, group_id={}) -> Result<FactorModelArtifact>.
Diagonal path delegates to diag_risk.hpp's diagonal_risk_model and lowers its
(X,F,D) into the artifact (byte-identical drop-in). Factor path: a NEW
PanelWindowView bridge owns a small (fields_, mask_) buffer and exposes an
atx::engine::PanelView over it for FactorModelBuilder::build_components — no
existing Panel(atx-impl's flat, date-major, file-backed research panel)
->PanelView(engine's non-owning ring-buffer accessor, PanelView ctor confirmed
public but consumer-built-only; RollingPanel confirmed a live-loop construct
unrelated to this batch pipeline) adapter existed; this is a genuine new seam,
documented in the header/impl and the ledger anchor-reconfirmation section above.
style_size maps to StyleFactor::Liquidity (log dollar-ADV), NOT StyleFactor::Size
(the research panel carries no market_cap field at all -- confirmed via
risk/exposures.hpp's own header note -- so true Size is structurally unreachable
regardless of the mask; Liquidity is the panel-derivable proxy the brief's own
architecture note names ("log(dollar-ADV) or log-cap proxy")).

Sizing bug found + fixed mid-unit (documented for transparency): the PanelView
buffer must carry MORE rows than the estimation window passed to
build_components, because build_components regresses one cross-section PER
DATE in [0, window) and each date's own style lookback (Beta needs row+253,
Momentum row+252, Volatility row+60) reads forward from that date -- so the
OLDEST estimation date needs `window + deepest_lookback` total rows available,
not just `window`. Missing this (RED failure: "too few usable dates (M_s < K
everywhere)") was the first GREEN attempt's failure; fixed by computing
`total_rows = estimation_window + deepest_lookback(cfg)` for the buffer while
still passing `estimation_window` (not the buffer's row count) to
build_components. A second bug (X.rows()==0 despite a correctly-sized buffer)
was PanelView's physical-row addressing: physical rows must be filled
OLDEST-first ascending (phys = n_rows_-1-r for newest-first r), matching
PanelView::physical_row's own `(head_+cap_-row_from_newest)&(cap_-1)` formula
with head_ = n_rows_-1 -- NOT phys=r, which silently wrapped every row past the
first to the far end of the ring (uninitialized NaN). Both bugs were caught by
the RED->GREEN loop itself (test 2's own acceptance criteria), not discovered
after the fact.

New suite AtxImplRiskModel (atx-impl/tests/stage_riskmodel_test.cpp) 4/4 green:
- DiagonalEquivalentToDiagonalRiskModel: cfg.kind==Diagonal's artifact lowers to
  a FactorModel with byte-identical risk(w) vs diagonal_risk_model directly, and
  X=Mx1 zeros / F=[[1]] exactly.
- FactorNondegenerateDelevers: a fixture where every instrument shares a common
  shock (market factor) plus small idiosyncratic noise, with 2 industry groups +
  Volatility style column (K=3, confirmed non-diagonal F). A long-first-half /
  short-second-half hedge book: factor_risk=1.494e-6 vs diag_risk=4.0e-5 -- the
  factor model prices the SAME hedged book at ~3.7% of the diagonal model's risk
  (a ~26.8x reduction) because it recovers the shared exposure the long/short
  legs cancel; the diagonal model (X=0) is structurally blind to any shared
  structure and just sums per-name variances regardless of the offsetting
  position.
- PitGuardIgnoresFutureRows: perturbing rows >= fit_end (with the SAME fit_end
  truncation the caller applies) yields a byte-identical serialized artifact —
  no look-ahead.
- TwiceRunByteIdenticalArtifact: same (panel, cfg) -> identical serialized bytes
  + identical digest across two independent calls.

RED verified first (linker error: undefined symbol
atx::impl::build_risk_model(...), confirmed via a full build+link before any
.cpp was written).

Full gate re-run after S1-1: atx-impl-tests 206/210 pass (4 pre-existing skips,
0 new failures — 4 new AtxImplRiskModel tests + the full pre-existing
AtxImplOptimize/AtxImplReport/etc. suites unaffected); atx-engine-risk-tests
256/258 pass (same 2 pre-existing RobustPipelineE2E failures, unrelated to any
S1 file).

S1-2: complete. stage_optimize.cpp:27-30 — the public zero-arg
`run_optimize(const RunConfig&)` (declared in stages.hpp, the S5-hub-owned
public surface, itself UNCHANGED) now forwards to a NEW overload
`run_optimize(const RunConfig&, const risk::RiskModelConfig&)` with an inert
default RiskModelConfig{} — byte-identical BY CONSTRUCTION (same code path,
same input every existing caller already had, no parallel-maintained
duplicate). The new overload's body is stage_optimize's old MVO-path code,
minimally changed at the old line 202: `diagonal_risk_model(research)` ->
`build_risk_model(research, risk_cfg)` + `data::artifact_to_factor_model(artifact)`
(stage_riskmodel.hpp's S1-1 producer + the existing S6.6 adapt_factor lowering
seam). model_at's TYPE (const risk::FactorModel&) and the mpo.run call are
UNCHANGED -- only the model's SOURCE differs, exactly per the brief's wiring
snippet. RiskModelConfig is deliberately NOT added to RunConfig (Sprint 5 CLI
hub scope, per the brief's "Out of scope" + the ROADMAP's file-ownership
matrix); the new overload IS the direct-call integration surface S1's tests
use, and stages.hpp/config.hpp/config.cpp remain untouched. diag_risk.hpp's
now-unused #include was dropped from stage_optimize.cpp (diagonal_risk_model
is only called transitively, from within stage_riskmodel.cpp's Diagonal
branch, which still calls it directly and unconditionally on that path).

New suite AtxImplOptimizeRiskModel (atx-impl/tests/stage_optimize_riskmodel_test.cpp)
3/3 green, first GREEN attempt (RED verified first: 5 "too many arguments"
compile errors against the old 1-arg run_optimize, before the overload was
added):
- DiagonalByteIdentical: run_optimize(cfg, RiskModelConfig{}) produces the
  IDENTICAL digest AND byte-identical books.bin vs the plain run_optimize(cfg)
  call -- the off-path byte-identity gate.
- FactorDeleversVsDiagonal: on the S1-1-style common-shock correlated panel
  with a fixed long-half/short-half combo, the Factor-routed book has strictly
  LOWER ex-ante risk under the factor model it was optimized against than the
  Diagonal-routed book (same alpha, same V, different w), and gross on the
  crowded pair is <= the diagonal book's.
- TwiceRunFactorByteIdentical: same inputs -> identical digest + byte-identical
  books.bin on the Factor path across two independent run_optimize calls.

Full gate: atx-impl-tests 209/213 pass (4 pre-existing skips, 0 new failures —
the ENTIRE pre-existing atx-impl-tests suite, including every AtxImplOptimize.*
byte-identity/turnover/trade-rate test, re-ran green after the stage_optimize.cpp
body edit); atx-engine-risk-tests 256/258 (same 2 pre-existing
RobustPipelineE2E failures, engine files untouched this unit).

## Sprint-3 seam handoff (S1-3, binding)

`atx::engine::data::cleaned_alpha_cov(centered)` (data/factor_model_artifact.hpp)
is a PURE function: T×N column-demeaned alpha-return window in, cleaned N×N
covariance out. It reuses the existing S8.7 toolkit verbatim (no new estimator
math): constant_correlation_shrinkage (Ledoit-Wolf constant-correlation target,
PSD by construction) -> correlation-form Marchenko-Pastur eigen-clip (mp_clip,
q=N/T) -> a final eigenvalue_clip strict-PD floor (psd_repair.hpp). Its
signature is a drop-in shape match for combine::detail::mle_covariance(centered, n)
(same `centered` contract, non-fallible MatX return).

**Sprint 3's job** (owns stage_combine.cpp): at stage_combine.cpp:755, thread
`RiskModelConfig.kind==Factor ? data::cleaned_alpha_cov(centered)
: combine::detail::mle_covariance(centered, na)` — S1 does NOT edit that file.
At kind==Diagonal (today's default) the seam is never called, so combine.bin
stays byte-identical; only Sprint 3's threaded call activates the cleaned path.

S1-3: complete. New accessor `atx::engine::data::cleaned_alpha_cov` added to
data/factor_model_artifact.hpp (includes risk/shrinkage.hpp + risk/psd_repair.hpp
-- no risk->data include edge introduced, since data already depends on risk
one-way via adapt_factor.hpp's forward decl; shrinkage/psd_repair have no data::
dependency themselves). New suite CleanedAlphaCov
(atx-engine/tests/risk/risk_cleaned_alpha_cov_test.cpp) 3/3 green:
- ShrinksSpuriousEigenvalueAndDiversifies: on an N≈T (T=20,N=18) fixture with a
  common factor + one large-amplitude near-independent "spurious" column, the
  cleaned covariance's min-variance weights have a strictly LOWER max|w_i| than
  the raw sample covariance's -- more diversified, not chasing the noisy outlier
  direction.
- ReturnsPsd: cleaned_alpha_cov's output passes risk::FactorModel::create's own
  SPD gate (X=Identity(N) so V=F+D exactly; create's Cholesky check is reused
  as the SPD oracle rather than inventing an independent check).
- PureAndDeterministic: same input -> byte-identical output across two calls.

RED verified first (5 compile errors: cleaned_alpha_cov undeclared in
atx::engine::data). One test-fixture bug fixed mid-unit (ReturnsPsd's first
draft passed X with 0 columns against an N×N F, tripping create's K==X.cols()
shape check unrelated to cleaned_alpha_cov itself; fixed by using
X=Identity(N) so V=F+D exactly matches the intended SPD oracle).

Full gate: atx-engine-risk-tests 259/261 pass (2 pre-existing RobustPipelineE2E
failures, unrelated); atx-engine-data-tests 118/121 pass (3 pre-existing
skips, 0 failures); atx-impl-tests build clean (data/factor_model_artifact.hpp
is transitively included by stage_optimize.cpp/stage_riskmodel.cpp).

S1-4: complete. build_risk_model gained three trailing default-nullptr/empty
parameters: `const library::Library* dead_lib = nullptr`,
`std::span<const combine::AlphaId> dead_ids = {}`, `atx::usize dead_as_of = 0`
(stage_riskmodel.hpp; library/fwd.hpp's forward decl keeps the header light,
the .cpp includes the full library/library.hpp). Internally split into
build_base_components (the pre-augmentation (X,F,D) — Diagonal delegates to
diagonal_risk_model exactly as before, reassembled as a FactorComponents so
augmentation composes with EITHER kind uniformly; Factor is S1-1's existing
body, unchanged) + a post-step in build_risk_model itself: when
cfg.dead_alpha_factors && dead_lib!=nullptr && !dead_ids.empty(), calls
risk::extract_dead_factors(*dead_lib, dead_ids, dead_as_of, universe_size)
then risk::augment_factor_model(comp, dead) — both FROZEN free functions from
risk/dead_factor.hpp, called verbatim, no new estimator math. FAIL-OPEN
guardrail (per the sprint's risk table): dead_lib==nullptr or empty dead_ids
is a documented no-op (the base model passes through), never an Err — a
caller without a library does not lose its whole risk-model build.

New suite AtxImplDeadFactor (atx-impl/tests/stage_riskmodel_dead_factor_test.cpp)
3/3 green, first GREEN attempt (RED verified first: 3 "too many arguments"
compile errors against the old 3-arg build_risk_model):
- RaisesCrowdedVariance: two dead alphas whose holdings concentrate on ONE
  instrument (a rank-1-ish overlap), walked to LifecycleState::Dead in a real
  library::Library fixture (reusing risk_dead_factor_test.cpp's proven
  admit-then-mark pattern); risk(w) for a book aligned with that instrument is
  STRICTLY higher with dead_alpha_factors=true than without.
- InertOff: dead_alpha_factors=false produces a BYTE-IDENTICAL artifact
  whether or not a library + dead_ids are supplied -- the library is never
  even consulted when the flag is off.
- ERankTruncationAddsExactlyMinKErank: 4 dead alphas all concentrated on the
  SAME instrument (eRank==1 by construction) add EXACTLY 1 column to X,
  regardless of k=4 contributing alphas -- min(k, eRank) == min(4,1) == 1.

Full gate: atx-impl-tests 212/216 pass (4 pre-existing skips, 0 new failures);
atx-engine-risk-tests 259/261 (same 2 pre-existing RobustPipelineE2E failures,
no engine files touched this unit).
