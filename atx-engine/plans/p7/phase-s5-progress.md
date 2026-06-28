# p7 Sprint 5 — Conviction-Scaled Sizing — Progress Ledger

Base: main @ e0659e4 (Wave-1 S1/S2/S3 merged; risk/kelly_sizing.hpp/.cpp +
combine/conviction.hpp already on base).

Branch: feat/p7-s5  Worktree: C:\atx-wt\p7-s5

Decision override D4 (config SPLIT): struct fields added to config.hpp
(kelly_fraction=0.0, kelly_max_gross=1.0); config.cpp CLI parsing deferred to S7.
Tests set RunConfig fields directly.

## Unit checklist
- [x] S5-0  Open ledger (marker) + Kelly math unit tests (kelly_sizing_test.cpp)
- [x] S5-1  Conviction KV telemetry (apply_conviction collector + KV emission)
- [ ] S5-3  Walk-forward conviction-awareness unit test (conviction_wf_test.cpp)
- [ ] S5-2  Fractional-Kelly wiring (config fields + Kelly call site + KV)
- [ ] S5-4  Integration smoke + off-path byte-identity (conviction_sizing_test.cpp)

## Determinism contract (every unit)
- kelly_fraction=0.0 -> Kelly block skipped entirely; conviction=false -> no conviction KVs.
- No-flag stage_combine digest byte-identical to pre-S5.
- Diagonal FactorModel (per-name realized variance) — minimal scope, no full factor model.
- oracle.hpp frozen; no golden re-baseline.

## Log
S5-0: complete (KellySizingMath 7/7 green; risk group 15/15 in kelly slice; engine
oracle/golden/digest slice 18/18 green) — Kelly math contract pinned on verbatim
plan fixtures (V=diag(0.01,0.04) => V^-1mu=[10,5]); suite KellySizingMath distinct
from existing KellySizing to avoid TEST collision; no atx-impl/oracle touched.
S5-1: complete (AtxImplConvictionSizing 5/5 green; AtxImplCombine 27/27 no regress;
full atx-impl-tests 176 pass / 4 pre-existing skips) -- apply_conviction gained an
optional out_scores collector (default nullptr => off-path byte-identical); 3 additive
KVs (conviction_scores / _dsr_terms / _stability_terms) emitted only when the
collector is non-empty (i.e. --conviction on). WF folds pass nullptr (scratch, not
telemetry). Score == (w_dsr*dsr+w_stab*stab)*0.75 pinned to the emitted terms.
