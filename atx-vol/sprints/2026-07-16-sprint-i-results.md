# Sprint I — Integration Results & Handoff (2026-07-17)

Supersedes the mid-flight `2026-07-16-sprint-i-status.md`. Read with the PLAN
(`sprints/2026-07-16-atx-vol-sota-parallel-subsprints.md`), SPRINT, and REVIEW.

Branch: **`feat/sota-integration`** @ `3ecc3e3`, based on `main@51df565`
(Sprint R through the Illinois-step commit). **Nothing pushed. Not merged to
main** — the main merge is deliberately gated on Sprint R landing (see §6).

## 1. What landed on the integration branch

Merge order R → K → A → S → (I-S, I-K) honored:

| Commit | Content |
|---|---|
| `afb4277` / `cc80c97` / `c3a8ebe` | Phase-1 merges of Sub-Sprints K / A / S |
| `4f45728` | Triage of the two pre-existing v2-path Debug failures |
| `b05b443` (`4cc4f77`) | **I-S**: CStar-vs-eSSVI panel extended to real OPRA; KEEP-R&D verdict |
| `3ecc3e3` (`90d2e77`,`a1cb9ab`,`c0df506`,`5ef6238`) | **I-K**: R-23 aliasing, K2 wing-patch retirement (+2 latent-bug fixes), R-24 no-flip |

Disjointness held: I-S touched only `examples/cstar_panel.cpp` + docs + PLAN;
I-K touched only Agent-K TUs (`batch.cpp`, `vector_math.hpp`, `simd/{black76,greeks,iv}_batch*`,
bench, tests) — verified it never touched `american_boundary_avx2.cpp` or any A/S/R TU.

## 2. Phase-2 outcomes

### I-K (kernel loose ends) — COMPLETE
- **R-23**: exact `in==out` identity aliasing permitted at the batch boundary
  (partial/output-output overlap still rejected); pre-W1 behavior restored;
  positive in-place tests added.
- **K2 wing-patch retirement**: the `|d|>kNormCdfWing` scalar detour removed from
  the B76 price/greeks AVX2 batch. **Removal surfaced and fixed two real latent
  bugs the patch had masked**: (1) `exp_pd` emitted ~1e290 garbage below
  `ln(DBL_MIN)` → now flush-to-zero; (2) non-finite (`±inf`) `d` slipped past the
  R-22 NaN-only self-compare → escape broadened to `nonfinite_mask`. Deep-wing max
  abs err now 0.0 (Φ saturates to exactly 1.0/0.0). This is the highest-value
  correctness find of Sprint I.
- **R-24**: IV batch re-measured on the quiet host — AVX2 `implied_vol_batch` is
  ~0.95× scalar and *looser* (max rel err 8e-8 vs scalar 2e-11). Decisive no-flip;
  both public entries stay scalar-routed; the AVX2 kernel is retained off-dispatch
  (labeled "retained off-dispatch, R-24") for a future AVX-512 port.

### I-S (real-OPRA CStar panel) — COMPLETE, verdict KEEP R&D
`atx-vol-cstar-panel` gained a read-only `--real` mode (load_opra_cbbo_parquet →
data_install → Universe → read-only VolaSession; no pipeline/library TU touched).
Ran on the real OPRA present on disk (`C:\atx-data\spy-dispersion\opra`): SPY + 10
mega-caps × 3 dates (the PLAN's 25-name recovery cohort is **not on disk** —
data-gated). Result: the synthetic modal-board win does **not** generalize —
CStar vol-RMSE ~2× worse than eSSVI, SPY admission collapses (89% butterfly-
inadmissible), and it raises arb flags where eSSVI raises none; it only improves
price-χ²/in-band on liquid single names. **Recommendation: KEEP R&D — do not add
to the production ladder, do not kill.** Evidence:
`docs/reviews/2026-07-16-cstar-vs-essvi-evidence-panel.md`. The ladder call is the
user's; no `curve_selector.cpp` wiring was done.

## 3. Correctness gate

**Debug / `rel` (SSE2) is the correctness config → PASS.** The integration test
surface (verified directly against the I-K worktree's Debug `atx-vol-tests.exe`,
which = integration minus the docs-only I-S merge) has **only the 6 documented
pre-existing v2-path failures** — zero new failures introduced by any merge:
`SurfaceV2Provenance.ValidationFallbackAdmissionRecordsTheServedFamily`,
`PricerFitterTest.LocalRiskRefitPublishesCopyOnWriteGeneration`,
`PreparedPortfolio.GroupedPriceEqualsIndependentOracleAndPinnedFingerprint`,
`OpraBreadthCorpus.UnifiedPolicyFitsEveryAvailableBoard`,
`SurfaceV2Qualification…/{Latency,Balanced}`.

**rel-avx2 (perf preset) is NOT bit-identity-clean — by design, not a regression.**
The Release suite under `rel-avx2` shows 17 failures = the 6 above + **11 that fail
ONLY under rel-avx2** and **all pass under Debug** (verified by re-running all 11
against the Debug exe → 11/11 PASS, incl. `SpyBidAskRegression.AutoSelectPicksDenseForSpy`).
The 11 are `*BitIdentical*` / `*PinnedValues*` / `*Prechange*` goldens pinned under
SSE2/Debug; global `/arch:AVX2` FMA-contraction shifts the LSBs (measured examples:
American price `7.5263639623979586` vs `…568` ≈ 2e-15; `Pin` diff 1.2e-15 vs a
1-ULP tolerance) — economic-bound-negligible. One (`SpyBidAskRegression`) is a
board sitting exactly on the butterfly arb boundary (`slack=0`) that the LSB drift
tips from admit→reject. This is the "bit-identity is a telltale, not a gate"
class (PLAN §8.5) amplified by the ISA change.

**Action for the team (not blocking integration):** if rel-avx2 is to be a green
correctness gate, the ~11 goldens need per-ISA tolerance (or the SpyBidAsk board
needs a non-zero butterfly-slack margin). Otherwise keep correctness on Debug/`rel`
and use rel-avx2 for perf only.

## 4. Quiet-host bench re-measurement (step 4)

Best-of-5, i7-1260P laptop, single-process (still a laptop — treat as
provisional-quiet, not datacenter-quiet).

| Metric | Result | Gate / standing |
|---|---|---|
| **Boundary AVX2 batch** (4096-put) | scalar 167.8 µs/op vs avx2 89.6 µs/op median = **1.87×** (best-pair 1.91×) | 2.0× ship gate **not** cleared → `kShipAvx2Boundary` **stays false** |
| **K5 IV inversion** (scalar) | **~329 ns/op**, median rel err ~1e-15, max ~2e-11 | vs Jäckel LBR 180 ns ref: ~1.8× slower, tighter accuracy |
| **K5 IV** (avx2, off-dispatch) | ~414 ns/op, max rel err 8e-8 | confirms R-24 scalar route |
| **A3 American price** | fast ~47.5 µs/op (max abs err 1.4e-3), accurate ~158 µs/op (8.3e-5), reference ~2 ms | above the ALO ~10–22 µs envelope — recorded standing, not beaten |
| **S3 CStar normal_eq fusion** | legacy 6141 ns → fused 3131 ns = **1.96×** | consistent with S2's ~1.8× |
| **SPY e2e one-op** (`fit/e2e/spy_real`, embedded fixture) | **347 ms** (from 492 ms, −29%) | ≤200 ms gate **not met** — see below |

**Why SPY e2e is 347 ms, not ≤200 ms (the load-bearing finding):** the breakdown
is dominated by `observation_deam_cpu_ms_per_board = 272.9` (de-Americanization),
with carry 54.1 ms and value 16.8 ms. That de-Am cost is precisely what the
**deferred** seam — wiring A's proven AVX2 boundary batch into the shared-boundary
9-node de-Am build in `boundary_interp.cpp::build` — is designed to cut. PLAN §7
predicted this exactly ("R-01 alone ~310 ms; R-11 + A-batch wiring close the
rest"). We are at 347 ms with R-01 in and the A-batch de-Am wiring + Sprint R's
R-11 still pending. **The ≤200 ms gate is reachable but requires the §6 user-gated
wiring, not more sub-sprint work.**

**Φ-swap accuracy panel: data-gated in this environment.** `accuracy_panel` needs
`data/spy_fit_slices` + `data/opra_universe`, which are not in the repo (they live
on the user's box; that is how the checked-in `…-sse2-accuracy-panel.csv` baseline
was produced). The Φ-swap + wing-patch numeric changes are nonetheless validated
at the economic-bound level by the in-repo gates that DO run (K2 Φ-vs-long-double
ULP test; wing-patch `DeepWingLanesMatchScalarTightly` / `DegenerateLanesAreBitExact`;
the full batch-vs-scalar parity suite — all green under Debug). To close the
aggregate real-data gate, run on the user's data:
```
accuracy_panel --spy-root <data>/spy_fit_slices --universe-root <data>/opra_universe \
  --symbols atx-vol/tests/accuracy_panel_success_symbols.txt --mode commit --require-ok
```
and confirm in-band ≥ prior, χ² ≤ prior, vol-RMSE ≤ prior vs the checked-in CSV
baseline (update the golden with documented justification if the only delta is the
K2 Φ / wing-patch numeric shift — PLAN §8.5).

## 5. Ship-flag decisions (final for Sprint I)

- `kShipAvx2Boundary` → **stays false** (1.87× quiet < 2.0×). Ship path unchanged:
  vectorize the 12-node BAW Newton seed (breaks bit-parity → needs economic-bound
  parity + gate re-verify). Deferred to Sprint X. AVX2 boundary kernel remains
  available via `ForceAvx2` and parity-tested every run.
- IV batch routing (R-24) → **stays scalar** on both public entries. AVX2 kernel
  retained off-dispatch for AVX-512.

## 6. Blocked on the user — the R → integration merge (NOT done here)

`feat/sota-integration` is intentionally **not** merged to main. Two coupled
reasons:

1. **Sprint R is not fully landed.** The user has uncommitted Sprint R work in
   `C:\atx` (`calib.cpp`, `boundary_interp.cpp`, `calib.hpp`, tests). Merge order
   stays **R → integration**.
2. **The de-Am boundary wiring lives in a Sprint-R TU.** Wiring A's AVX2 boundary
   batch into `boundary_interp.cpp::build` (the 9-node de-Am solve) — the single
   biggest lever on the SPY e2e number (§4) — must be done in `boundary_interp.cpp`,
   which PLAN §1 reserves for Sprint R until it lands, and which the user is
   actively editing. Doing it here would collide with the user's live work.

**Handoff:** once Sprint R lands, merge R → `feat/sota-integration`, resolving the
expected `calib.cpp` / `boundary_interp.cpp` conflicts **with the user present**,
then do the de-Am wiring in `boundary_interp.cpp::build` and re-measure the SPY
e2e gate. That is the path from 347 ms toward ≤200 ms.

## 7. Deferred / backlog (unchanged from the plan)

- **W4.2** sibling fit pool + **W4.5** H² guard + small-book cutoff at
  n={1,2,3,4,6,8,12,16} — needs Sprint R's R-14/R-15 executor state (still open).
- The **two pre-existing v2 failures** (SurfaceV2Provenance, OpraBreadthCorpus):
  triage doc has concrete fixes; owner is the user / v2 path.
- **Chebyshev-Φ retirement** once `american_boundary_avx2.cpp` migrates to the
  Cody-erfc Φ.
- **Sprint G** (DoD closure, universe-cycle baseline, published shootouts) and
  **Sprint X** (BAW-seed vectorization to ship the boundary batch, Schadner ~60 ns
  IV, SplineVol, AVX-512).

## 8. Incidents this session

- Two phase-2 agents (I-S, I-K), on their first CONFIGURE, hit the shell-cwd trap:
  the shell tool's cwd defaults to `C:\atx`, so a *relative* `.\scripts\atx-build.ps1`
  resolved to the user's live tree and reconfigured `C:\atx\build` (configure-only,
  **no source touched**). Both were remediated immediately (live tree restored to
  `FETCHCONTENT_BASE_DIR=C:/atx-cache/deps`, `ATX_BUILD_BENCH=OFF`; SHA
  `51df5655…-dirty` intact) and both agents switched to absolute worktree script
  paths. Lesson for future dispatch: instruct agents to invoke
  `C:\atx-wt\<wt>\scripts\atx-build.ps1` by **absolute path** from the start.
