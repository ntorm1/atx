# p7 Sprint 5 — Conviction-Scaled Sizing — Progress Ledger

Base: main @ e0659e4 (Wave-1 S1/S2/S3 merged; risk/kelly_sizing.hpp/.cpp +
combine/conviction.hpp already on base).

Branch: feat/p7-s5  Worktree: C:\atx-wt\p7-s5

Decision override D4 (config SPLIT): struct fields added to config.hpp
(kelly_fraction=0.0, kelly_max_gross=1.0); config.cpp CLI parsing deferred to S7.
Tests set RunConfig fields directly.

## Unit checklist
- [x] S5-0  Open ledger (marker) + Kelly math unit tests (kelly_sizing_test.cpp)
- [ ] S5-1  Conviction KV telemetry (apply_conviction collector + KV emission)
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
