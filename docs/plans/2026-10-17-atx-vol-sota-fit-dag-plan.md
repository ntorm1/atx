# atx-vol state-of-the-art fitting — DAG sprint plan (speed × accuracy × surface shapes)

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development with Opus
> implementers and fresh exact-SHA reviewers, run through the resume-6 lane process
> (`C:/atx/.superpowers/sdd/2026-10-17-vol-resume6/SHARED-BRIEF.md`, pool leases, file briefs and
> reports). Steps use checkbox (`- [ ]`) syntax. Implementation before tests (standing user rule);
> every behaviour change still ships with a test that fails on revert.

**Goal:** make atx-vol's American equity vol-surface fitter state of the art on both axes at once:
≤ 30 ms per expiry and ≤ 1 s per SPY-sized surface single-threaded (≤ 150 ms with all cores),
while keeping or improving today's accuracy against the bid/ask market and the SpiderRock yardstick
across nine surface-shape classes, with zero no-arbitrage violations on anything served.

**Architecture:** keep the American price-space certification as the arbiter of what is served, but
stop paying for it on every iteration. Fit in de-Americanised vol/variance space from a once-per-
snapshot IV cache, memoise everything a policy rung can reuse, certify with a shared per-expiry
Andersen-Lake geometry/boundary and a fast-then-accurate screen, refuse cheaply before refitting,
and schedule expiries across all cores. Accuracy work (calendar clamp on the union of knots,
short-dated and LEAPS handling, dividend vol convention, sparse/wide-spread admission) rides the
same opt-in-key discipline. Every lane is opt-in behind a policy key with default parity proven by
hash; promotion to default happens once, after a held-out run under the oracle Ratchet rules.

**Tech stack:** C++20 (clang-cl 18, `vol-rel` preset; `vol-rel-avx2` for pins only), GoogleTest,
Python 3.12 scorer (`atx-vol/tools/quote_benchmark.py`), pool worktrees, Opus subagents.

**Spec:** this document §1-§3 (targets, shape matrix, constraints) +
`atx-vol/sprints/2026-10-17-common-model-resume6-handoff.md` (state, deferred minors, rulings) +
`atx-vol/sprints/2026-10-17-spiderrock-parity-status.md` (accuracy baseline) +
`C:/atx-cache/quote-benchmark/fit-timing-20261017-cohort5/RESULTS.md` (speed baseline).

## 0. Why this plan exists (the gap, measured 2026-10-17)

Per expiry chain, one fit worker, unqualified host: graded-v1 SPY 1.66 s (33 chains, 4 fitted),
AAPL 0.45 s, MSTR 0.39 s, CRWV 16 ms, RIVN 51 ms; repair-v3 2.62 / 0.65 / 0.39 / 0.047 / 0.071 s;
knot-ladder 3.20 / 0.80 / 0.30 / 0.18 / 0.11 s. SPY surface 55-106 s single-threaded. A production
fitter does SPY in well under a second. Structural causes (no profiler run yet — Lane L1 confirms):
the channel solve iterates in American price space (median 12-19 Newton iterations per chain, each
re-pricing up to 325 strikes through the AL kernel), nothing is memoised between policy rungs
(graded-v1 attempts all 33 SPY chains, channel-refits 25, calendar-retries 4; the ladder re-runs the
stack per rung), refusals are discovered only after the full solve (24 of 33 SPY chains refused at
`AmericanPriceSlice` certification), certification is a full American reprice per attempt and per
calendar pair, and fitting runs 3 workers by policy on a 16-thread host with the Fast AL profile
unused inside fitting.

## 1. Targets (falsifiable; all must hold on the final integrated binary)

### 1.1 Speed (vol-rel, pinned on a quiet host: load < 15% sampled before/after, interleaved
arms, ≥ 5 repetitions, min and median reported; `fit_workers = 1` unless stated)

| Metric | Today (unqualified) | Target | Stretch |
| --- | --- | --- | --- |
| Per-expiry fit+reprice+certify, ≤ 400 admitted quotes, median | 0.39-3.2 s | ≤ 30 ms | ≤ 10 ms |
| Per-expiry, p99 over dev200 chains | ~3 s | ≤ 100 ms | ≤ 40 ms |
| SPY surface (33 expiries, 5,196 admitted quotes), single thread | 55-106 s | ≤ 1.0 s | ≤ 0.4 s |
| SPY surface, `fit_workers = 0` (all cores) | 20-35 s (3 workers) | ≤ 150 ms | ≤ 60 ms |
| dev200 (191 surfaces, ~1,138 fits), all cores, graded + repair + ladder stack | to be pinned in L1 | ≤ 10 s | ≤ 4 s |
| Served-path record IV warm (unchanged scope) | 404 ns quiet | ≤ 500 ns | — |
| Fit output byte-identical for any `fit_workers` | holds | holds | — |

### 1.2 Accuracy (development cohorts dev200 / dev200b per shape class; holdout only at §5 gate)

| Metric | Baseline (C5 / ladder) | Target |
| --- | --- | --- |
| No-arbitrage violations on served slices (monotone, convex, vertical-spread, calendar) | 0 | 0 (hard gate) |
| Containment (served price inside bid/ask), every class | 98.1-99.0% | ≥ 98.5% |
| MAE/spread, index + mega-cap classes | 0.11-0.14 | ≤ 0.12 |
| MAE/spread, high-vol + sparse classes | ~0.15 | ≤ 0.15 |
| Coverage of eligible non-deliverable rows, dev200 / dev200b | 93.5% / 98.6% (C5) | ≥ 96% / ≥ 98.8% |
| Index class coverage (SPY today: graded 4 of 33 chains served natively) | 12% native, rest fallback | ≥ 95% served by a certified slice fit (no m6 fallback) |
| Median \|IV − srVol\| on served index / mega-cap slices | 0.9-4.6 bp | ≤ 5 bp |
| Exact-intersection price MAE vs C5 | — | never worse by > 0.5% on either cohort |
| Dividend payers: reported vol convention | undocumented field | `vol_convention` in every record; spot-vol view available |

### 1.3 Qualification rules

Numbers count only when produced by the harness of L1 with the baseline JSON committed, on a quiet
host, with the binary sha256 and the policy sha256 in the receipt. "Unqualified" numbers may guide
work but never close a done criterion. Default paths stay byte-identical until §5.

## 2. Surface-shape × underlying matrix (what "across a range" means)

Built once by L1b from the existing 2026-10-14 snapshots (no new market data); each class is a
symbol list + expiry filter with its own scorer stratum. Holdout symbols are never named here.

| Class | Representatives (dev200 / dev200b / cohort5) | What breaks today |
| --- | --- | --- |
| index / ETF, dense chains | SPY, QQQ, IWM | convexity refusals on 1-cent micro-kinks; calendar crossings; 1.7-3.2 s per expiry |
| mega-cap single name | AAPL, MSFT, NVDA, AMZN | calendar refusals (AAPL Nov 20 only served by the ladder) |
| high-vol / crypto-proxy | MSTR, TSLA, COIN | wide wings, 26 bp vs srVol, grade B |
| sparse / new listing | CRWV | point fits escaping the market (fixed by containment-first) |
| low-price, wide spread | RIVN, SOFI, F | srVol itself outside the band; bp metrics meaningless, containment only |
| discrete-dividend payers | ASML, HSBC, XOM | escrowed vol ≠ spot vol; lattice 3x slower; certify/refuse 149/31 |
| negative / high carry, hard-to-borrow | GME, AMC, high-borrow names | carry inference, one-sided interval refusals (Markov) |
| short-dated (≤ 3 DTE) | SPY/QQQ front expiries | time-value floors, kIvMin channel floor, certificate floors |
| long-dated (> 365 DTE, LEAPS) | SPY, AAPL LEAPS | non-monotone AL in vol on deep-ITM puts; >365 d worst band for fallback rows |

## 3. Global constraints (binding; copied from the standing user rules)

- Opus subagent-driven development; PM context small; briefs/reports as files; fresh exact-SHA
  review of every lane before integration; a BLOCK gets one fix round and a scoped re-review.
- Implementation before tests; every behaviour change has a test that fails on revert.
- Source/doc edits only via Edit/Write tools; PCH-off hygiene on every touched header.
- Every behaviour change is opt-in behind a policy key; default outputs byte-identical: dev200
  default `c89d0739`, graded-v1 `1348f63d` (knot-ladder era: modulo the ruled one-column
  `vol_status` normalisation), R104 `6b129f64`, dividend default `abc75955`, dev200b `4b273b08`,
  plus the keys-ON hash set recorded at each integration.
- SpiderRock marks are the yardstick, never an input. Holdout stays closed until §5.
- Nothing under `C:/atx-cache/quote-benchmark/` is overwritten; new folders with `sha256.txt`.
- No pushes, no PRs, no merges into main. Lease with the pool-1 scripts; release with the acquiring
  RunId. `-Jobs = clamp(floor(12/N),1,8)`; timing lanes run ALONE on the host.
- Python ctests need `-DPython3_EXECUTABLE="C:/Program Files/Python312/python.exe"`.

## 4. The DAG

```
W0 land in-flight ───────────────────────────────────────────────────────────────┐
  L0a knot-ladder fresh review → integrate        L0b al-speed review finish → fix → integrate
  L0c deferred-tests finish → light review → integrate                           │
  → FULL parity set on the final W0 binary → C6 (ladder → m6) composite           │
W1 measure (needs W0 binary) ────────────────────────────────────────────────────┤
  L1  fit profile + latency harness + budgets      L1b shape-class cohorts (tool-only)
  L1c kernel correctness: non-monotone AL region refusal (al-batch brief part 1)  │
W2 speed core (file-disjoint; all opt-in) ───────────────────────────────────────┤
  L2a vol-space channel fit from a DeAm IV cache   [quote_slice.cpp, deamer, curve_fit]
  L2b shared AL geometry/boundary + batched certification [american*.cpp, american_price_slice.cpp]
  L2c rung memo + early refusal                    [quote_surface.cpp, quote_slice.cpp — AFTER L2a]
  L2d all-core expiry scheduler                    [curve_fit.cpp parallel section, quote_bench]
  → integrate L2b, L2d, then L2a, then L2c; parity; L1 harness re-pinned (speed gate G2)
W3 accuracy across shapes (file-disjoint) ───────────────────────────────────────┤
  L3a union-of-knots calendar clamp + slice-repair deferred tests [quote_slice/quote_surface]
  L3b short-dated + LEAPS handling                 [quote_slice floors, deamer, calib]
  L3c vol_convention + spot-vol view for payers    [report schema opt-in, dividend records]
  L3d sparse / wide-spread admission + ladder ∘ dominance-gate composition [quote_surface, policy]
  L3e policy consolidation → one candidate default set (C7)  [policies, composer tool]
  → integrate; accuracy gate G3 per class on dev200 + dev200b
W4 qualify + promote ────────────────────────────────────────────────────────────┘
  L4a quiet-host pins (fit latency, surface wall, record IV, batch, AL, archive) + archive bench EOF fix
  L4b held-out run under the oracle Ratchet rules (once) → promotion decision → defaults flipped in ONE commit with the hash set re-baselined
```

Concurrency: ≤ 4 lanes at a time; a timing lane (L1, L4a) runs alone. Shared files: `quote_slice.cpp`
and `quote_surface.cpp` belong to exactly one live lane at a time (L2a → L2c → L3a → L3d in
sequence); `american*.cpp` to L1c → L2b; `curve_fit.cpp` to L2a (DeAm cache) then L2d (scheduler) —
L2d may start once L2a has committed its `curve_fit.cpp` hunk, or L2d limits itself to
`core/parallel_for.hpp` + `quote_bench_main.cpp` until then.

## 5. Gates

- **G0 (end of W0):** all in-flight lanes integrated; full parity set MATCH on the W0 binary; C6
  numbers recorded. No new default.
- **G1 (end of W1):** profile report with a per-rung cost table for SPY 23 d/37 d, AAPL Nov 20,
  MSTR Nov 20, CRWV Nov 20; `fit_latency` harness rows pinned in `bench/baselines/<host>-fit-latency.json`;
  shape cohorts built with sha256; budget table signed off (this plan §1.1 updated if the profile
  changes the split).
- **G2 (end of W2, speed):** with all W2 keys ON, per-expiry median ≤ 30 ms and p99 ≤ 100 ms over
  dev200 chains; SPY ≤ 1.0 s single thread, ≤ 150 ms all cores; keys OFF byte-identical; keys ON
  served set ⊇ today's keys-ON served set on dev200 and dev200b (no row lost to speed work), price
  identical to ≤ 1e-9 USD where the same slice is served.
- **G3 (end of W3, accuracy):** every §1.2 row met per class on dev200 and dev200b with the
  candidate set C7; 0 violations; intersection MAE vs C5 not worse by > 0.5%.
- **G4 (promotion):** quiet-host pins committed; held-out run under the Ratchet rules shows no
  regression vs C5 on the held-out scorecard; defaults flipped in one commit with the new hash set;
  ledger lines for every pin.

Any gate FAIL stops the wave; the fix is a lane with its own review, never a controller edit.

## Review Focus (failure modes no single lane's tests cover; each pinned in the owning lane)

1. **A faster path serves a slice the accurate path would refuse** (fast screen false-negative on a
   violation inside the Fast kernel's error bound). Owner L2b: test that a crafted slice violating
   convexity by less than the Fast error bound is still refused by the accurate confirmation.
2. **Memoised state crosses a snapshot or quote-set boundary** (stale DeAm IVs or boundaries reused
   after a quote changes). Owner L2c: test that changing one bid in the chain invalidates the rung
   memo and the served price changes accordingly; memo key includes quote-set hash + policy sha.
3. **Multithreaded scheduling changes output** (non-deterministic merge, worker-count-dependent
   rounding). Owner L2d: hash-identity test across `fit_workers ∈ {1, 2, 7, 0}` on cohort5.
4. **Vol-space fit moves the served price outside bid/ask on wide-spread names** where vol error
   maps to large price error. Owner L2a: containment test on RIVN/SOFI class — price containment
   computed in price space after the vol-space solve, with the certification unchanged.
5. **Short-dated chains hit vol floors and get served with a flat, wrong smile** (≤ 3 DTE). Owner
   L3b: test on a 1-DTE SPY chain that the fit refuses or serves inside bid/ask with 0 violations,
   never a flat kIvMin curve.

---

## Wave 0 — land the in-flight lanes (all briefs exist in the resume-6 workspace)

### L0a: knot-ladder fresh full review → integrate
- Brief `lane-review-knot-ladder-brief.md` (REVIEW_SHA = `2e60186c`). BLOCK → one fix round (resume
  the implementer pattern) → scoped re-review. Integrate with the full parity set (graded-v1 modulo
  the ruled normalisation). Done: merge SHA in the root sprint note; keys-ON hashes recorded.

### L0b: al-speed review finish → fix round → integrate
- Resume `al-speed-409ee7aa-review-v1` (NOT-verified list: base builds, Fast error-bound rerun,
  timing A/B + default-path probe, hygiene, revert spot-checks, bench self-check). Fix round for the
  two minors: alternating-order quiet-host A/B on the `accurate` row (resolve the "default path 3%
  slower" question with numbers), declare the timing set (dev200 kernel inputs). Integrate; kernel
  fingerprint `53c4df68` and 3 parity hashes MATCH.

### L0c: deferred-tests finish → light review → integrate
- Items 5-6 (quote-bench N2 reorder python test; Markov retry min-sigma mirror), revert runs for
  items 1-3, anchored ctests, PCH-off, `c89d0739`, sprint note. Tests-only diff list is the parity
  proof. Carry the serve-path edge-geometry concern (`expiry_record.cpp:292-299`) to L3b's brief.

### G0 close-out
- Full parity set + keys-ON set on the final W0 binary; C6 (ladder → m6) composite on both cohorts
  into `composite-<date>-c6/` with `RESULTS.md`; ledger lines.

## Wave 1 — measure

### L1: fit profile, latency harness, budgets (timing lane: runs ALONE)

**Files:** Modify `atx-vol/src/fitting/counters.hpp` (new `FitTimingCounters`), `atx-vol/src/fitting/quote_slice.cpp`
(count at `solve_channel_family`, `certify_and_serve`, `project_in_channel`, `repair_quote_slice_calendar_in_channel`),
`atx-vol/src/fitting/quote_surface.cpp` (per-chain wall + rung attempts), `atx-vol/tools/quote_bench_main.cpp`
(`--timing-detail` opt-in → `graded_stats.slices[].timing` in `--report`), `atx-vol/bench/fitting_throughput_bench.cpp`
(new `fit_latency/<class>/<policy>` rows), `atx-vol/bench/baselines/<host>-fit-latency.json` (new).
**Interfaces (produces):**
- `struct FitTimingCounters { uint32_t al_price_calls, al_greeks_calls, certification_passes, channel_solves, jacobian_evals, rung_attempts, calendar_pairs; uint64_t wall_us; }` per chain, zero-cost when `--timing-detail` is off (default output byte-identical — parity proves it).
- Report fields `chain_latency_us_p50/p95/p99` must be real percentiles at `fit_workers = 1` (today p50 = p95 = p99; fix and test).
- Bench rows `fit_latency/<class>/<policy>` with p50/p95/p99 ms per expiry and `surface_wall_ms`.
- [ ] Add counters + opt-in report plumbing; PCH-off on `counters.hpp`; parity `c89d0739` MATCH with the flag off.
- [ ] Sampled profile (ETW/xperf or VS sampling, release symbols) of SPY 23 d, SPY 37 d, AAPL Nov 20, MSTR Nov 20, CRWV Nov 20 under graded-v1, repair-v3, ladder: table of inclusive time by {AL pricing, AL Greeks, certification, channel projection, calendar checks, curve solve (eSSVI/spline/convex-dense), IO}. Paste counters per rung.
- [ ] Harness: `fit_latency` rows for the nine classes × three policies at `fit_workers = 1` and `0`; baseline JSON on a quiet host (load pasted); `compare_baseline.py` wired.
- [ ] Budget split: allocate the 30 ms per-expiry budget to {DeAm IV, curve solve, certification, calendar} from the profile; write it into this plan §1.1 as a PR to the plan (docs commit).
- **Done:** profile table + harness baseline committed; counters opt-in with parity MATCH; the three hypotheses in §0 each marked confirmed/refuted with numbers.

### L1b: shape-class cohorts (tool-only)
**Files:** Create `atx-vol/tools/shape_cohort.py` (+ test `atx-vol/python/tests/test_shape_cohort.py`); outputs `C:/atx-cache/quote-benchmark/shape-classes-20261014/<class>.csv` + `manifest.json` + `sha256.txt`; scorer stratum flag `--class-manifest` in `quote_benchmark.py` (opt-in; default output byte-identical, test).
- [ ] Classify dev200 + dev200b + cohort5 symbols/expiries into the §2 classes from quote features only (spread/mid, quote count, price level, DTE, dividend schedule presence, carry sign from the implied-forward report) — never from srVol.
- [ ] Scorer emits per-class coverage / MAE / MAE÷spread / containment / violations; re-score C5 and the ladder per class → `RESULTS.md`.
- **Done:** nine class files with counts; per-class baseline table for graded-v1, repair-v3, C5, ladder on both cohorts.

### L1c: kernel correctness — non-monotone AL region
**Files:** `atx-vol/src/pricing/american.cpp`, `atx-vol/include/atx/vol/api/pricing/american_iv.hpp`, `atx-vol/src/pricing/batch_chain_iv.cpp`, tests.
- [ ] Reproduce put S=100 K=128.989 T=2 (+7.7e-6 at vol 0.14, flat 0.17-0.18); map the region (T, moneyness, r−q) where `andersen_lake` is not monotone in vol with the accurate preset and with Fast.
- [ ] Either fix the kernel (quadrature/boundary resolution; bit-identity elsewhere proven by fingerprint on default inputs, otherwise opt-in profile) or add a typed refusal `IvStatus::NonMonotoneRegion` in `american_implied_vol` and both batch routes (slope floor on the cold map AND bracket ambiguity); tests fail on revert; PerContract polish N2 test added.
- **Done:** no Ok returned inside the mapped region on the dev200 long-dated deep-ITM rows; ledger line.

## Wave 2 — speed core (all opt-in; integrate L2b, L2d, L2a, L2c in that order)

### L2a: vol-space channel fit from a de-Americanised IV cache
**Files:** `atx-vol/src/fitting/quote_slice.cpp` (`fit_quote_slice_channel`, `build_channel_observations`, `solve_channel_native/_family`), `atx-vol/include/atx/vol/api/fitting/quote_slice.hpp` (config), `atx-vol/src/fitting/deamer.cpp` / `deamer.hpp` (`de_americanize_chain` reuse: cache type), `atx-vol/src/fitting/curve_fit.cpp` (`run_deam_prepass` sharing), tests `quote_slice_test.cpp`, `deamer_test.cpp`.
**Interfaces (produces):**
- `enum class QuoteSliceFitSpace { PriceChannel /*default*/, VolChannel };` key `graded.fit_space` (policy JSON string `"price"|"vol"`).
- `struct DeAmIvCache { uint64_t quote_set_hash; std::vector<DeAmQuoteIv> ivs; /* per quote: bid_iv, ask_iv, mid_iv, well_posed, carry diag */ };` built once per chain per snapshot by `de_americanize_chain` with the Fast profile for inversion (opt-in `graded.deam_profile = fast`), accurate for any quote whose Fast/accurate IV differ by more than the documented bound; key `graded.deam_cache = true`.
- `Result<ChannelFitDiagnostics> solve_channel_vol_space(QuoteSliceFit&, const DeAmIvCache&, const ChannelFitConfig&)`: the channel objective in total-variance space (bid/ask IV band → variance band), same curve families (eSSVI anchored, spline-vol, convex-dense, knot-spline), followed by ONE American reprice + `AmericanPriceSlice::create` certification (unchanged semantics) and the existing containment check in price space.
- [ ] Implement `DeAmIvCache` + `solve_channel_vol_space`; price-space path untouched when the key is off.
- [ ] Tests: (a) keys OFF hashes `c89d0739`, `4b273b08`, `1348f63d` MATCH; (b) VolChannel on cohort5 serves ⊇ repair-v3's served slices with 0 violations and price inside bid/ask on ≥ the same strike count; (c) Review-Focus #4 containment test on the wide-spread class; (d) iteration count per chain ≤ 5 median (vs 12-19) — counter-based test on SPY 23 d.
- **Done (unqualified allowed, pinned later by L4a):** per-expiry median ≤ 60 ms on SPY with L2a alone (L1 harness), keys OFF byte-identical, served set not smaller.

### L2b: shared AL geometry/boundary + batched fast-then-accurate certification
**Files:** `atx-vol/src/pricing/american.cpp`, `american_boundary.hpp`, `american_detail.hpp`, `atx-vol/include/atx/vol/api/pricing/american.hpp`, `american_batch.hpp`, `atx-vol/src/fitting/american_price_slice.cpp` (+ `american_price_slice.hpp`), `batch_chain*.cpp` consumers, tests.
**Interfaces (produces; the batch-pricing lane's four requests, now owned here):**
- `struct AlExpiryGeometry` (sigma/strike-free time grid, Chebyshev nodes, Gauss-Legendre tables) + `AlExpiryGeometry prepare_al_geometry(double T, double r, double q, const AlOpts&)`; overloads `andersen_lake(const AlExpiryGeometry&, ...)`, `american_greeks_al(const AlExpiryGeometry&, ...)`.
- `class AlBoundaryHandle { Status solve(double sigma); double price_from_boundary(double S, double K) const; }` for equal-sigma strikes of one expiry.
- `void al_price_pack(const AlExpiryGeometry&, std::span<const AlContract>, std::span<const double> sigma, std::span<double> price, std::span<double> price_sigma_bumped)` (laned; exact Newton slope).
- `AmericanPriceSlice::create(..., const CertificationKernel& k)` with `enum class CertificationKernel { Scalar /*default*/, Batched }` and `enum class CertificationScreen { AccurateOnly /*default*/, FastThenAccurate }`: Fast pass prices all nodes; nodes whose violation margin exceeds `fast_error_bound + tol` are refused immediately; remaining nodes confirmed with the accurate kernel; certification verdict is always the accurate kernel's.
- [ ] Implement geometry/boundary/pack with bit-identity of the existing entry points (fingerprint `53c4df68`); tests: shared-geometry prices == fresh solves to 0 ulp on a random chain; boundary reuse across strikes == fresh solves; pack slope == central difference within documented bound.
- [ ] Batched certification behind the keys; Review-Focus #1 test; certification count/time counters from L1 show ≥ 10x fewer accurate prices per certification on SPY 23 d.
- [ ] Consumer: batch routes use the pack (closes the 3x single-thread target or reports the number).
- **Done:** fingerprint + parity MATCH; certification of a 325-node SPY slice ≤ 3 ms (unqualified) with keys ON; Scalar batch ≥ 3x the loop single-thread or an honest miss.

### L2c: rung memo + early refusal (after L2a is committed)
**Files:** `atx-vol/src/fitting/quote_surface.cpp`, `quote_slice.cpp` (`certify_and_serve` pre-screen), `atx-vol/include/atx/vol/api/fitting/quote_slice.hpp`, tests `quote_surface_test.cpp`.
**Interfaces (produces):** `struct RungMemo { key = {chain_id, snapshot_ns, quote_set_hash, policy_sha256, curve_family}; DeAmIvCache ivs; std::optional<AlBoundaryHandle> boundary; std::optional<CertificationOutcome> last_cert; }` owned by the surface fit, key `graded.rung_memo = true`; `PreScreenOutcome prescreen_slice(const QuoteSliceFit&, const DeAmIvCache&)` — cheap necessary conditions (variance-band emptiness, monotone mid-IV band crossings, vertical-spread bound on bid/ask prices) that refuse BEFORE any solve with a typed reason.
- [ ] Implement; the ladder and repair rungs consult the memo (no second DeAm pass, no second boundary solve, no re-certification of an unchanged candidate).
- [ ] Tests: Review-Focus #2 invalidation; attempt counters on SPY 23 d drop from 3 rungs × full solve to 1 solve + 2 memo hits; refusal reasons unchanged in the keys-ON served/refused set vs L2a alone.
- **Done:** SPY surface single-thread ≤ 1.0 s with L2a+L2b+L2c ON (unqualified; pinned in L4a); served set identical to L2a's.

### L2d: all-core expiry scheduler, deterministic
**Files:** `atx-vol/src/fitting/curve_fit.cpp` (the `parallel_for_dynamic` sections, `in.fit_workers` semantics), `atx-core` `core/parallel_for.hpp` if a longest-first order is added, `atx-vol/tools/quote_bench_main.cpp` (`ATX_VOL_FIT_WORKERS=0` → `hardware_concurrency`), tests.
- [ ] Longest-expected-first ordering by admitted quote count; worker count 0 = all cores; outputs merged into fixed slots (existing determinism pattern).
- [ ] Review-Focus #3 hash-identity test across `fit_workers ∈ {1, 2, 7, 0}` on cohort5 and dev200b; speed-up table SPY surface at 1/4/8/12/16 workers.
- **Done:** SPY surface wall at all cores ≤ 150 ms once L2a-c are ON (pinned in L4a); hash identity holds.

### G2 close-out
Integrate L2b, L2d, L2a, L2c; full parity set; L1 harness rerun with all W2 keys ON; the §1.1 table updated with measured (still unqualified) numbers; ledger lines.

## Wave 3 — accuracy across shapes (opt-in; integrate in the order listed)

### L3a: union-of-knots calendar clamp + slice-repair deferred tests
Brief exists: `lane-knot-union-clamp-brief.md`. Done: ≥ 50% of the 1,038 displaced dev200 rows recovered with intersection MAE within 0.5% of the ladder policy; gate undo/re-admission tests; stale calendar-repair record fixed; 0 violations.

### L3b: short-dated and long-dated handling
**Files:** `quote_slice.cpp` (channel floor at kIvMin ~1276 per the smooth-div follow-up; certificate floors), `deamer.cpp` (time-value floors ≤ 3 DTE), `calib.cpp` (LEAPS wing extrapolation), `expiry_record.cpp:292-299` (edge geometry on refused certification rows), tests.
- [ ] ≤ 3 DTE: price-space channel floors scaled by sqrt(T); serve only with 0 violations inside bid/ask; Review-Focus #5 test.
- [ ] > 365 DTE: L1c's refusal wired into the fitter; wing extrapolation bounded by the Lee moment bound in variance space; test on SPY/AAPL LEAPS chains from dev200.
- **Done:** short-dated and LEAPS class rows meet §1.2; the >365 d band is no longer the worst for fallback rows on dev200 (per-class scorer).

### L3c: `vol_convention` + spot-vol view for dividend payers
**Files:** quote-slice report writer (schema 2 → opt-in schema 3 via `--schema 3`; default output byte-identical), `ExpiryRecord` / archive record field, dividend lattice schemes (`american_discrete_div.cpp`) to emit both escrowed-stock vol and the spot-vol equivalent, scorer stratum for payers.
- **Done:** every served record carries `vol_convention ∈ {spot, escrowed_stock}`; payer srVol comparison re-scored like-for-like (dev200b payers 205-217 bp → reported on the spot view); default parity MATCH.

### L3d: sparse / wide-spread admission + ladder ∘ dominance-gate composition
**Files:** `quote_surface.cpp` (ladder refuses the dominance-gate and knot-spline profiles today — compose instead), policy JSON, tests.
- **Done:** CRWV class served with ≤ 1/41 strikes outside; wide-spread class containment ≥ 98.5% with the bp metric reported but not gated; ladder + gate + knot-spline composable with 0 violations; `point_min_outside_quotes` behaviour pinned.

### L3e: policy consolidation → candidate default set C7
**Files:** `atx-vol/tools/compose_predictions_by_status.py`, policy folder `C:/atx-cache/quote-benchmark/c7-policies-<date>/`, `RESULTS.md`.
- [ ] C7 = {L2a-c speed keys ON, L3a-d ON, escrowed-lr on payers, repair-v3, ladder, m6 fallback}; per-class table on both cohorts vs C5 and the ladder; exact-intersection deltas.
- **Done (G3):** §1.2 met per class; intersection MAE vs C5 not worse by > 0.5%; 0 violations; the C7 policy files hashed and the recipe written in the sprint note.

## Wave 4 — qualify and promote

### L4a: quiet-host pins (runs ALONE)
- `fit_latency` rows (§1.1 every line), record IV warm, batch Scalar/Laned/Lockstep, AL cold/Fast ladder grid + dev200 kernel set, archive open/read; interleaved, ≥ 5 reps, load pasted; baselines committed under `bench/baselines/`; fix the archive bench EOF minor on the way (loop until `total < bytes`, test).
- **Done:** every §1.1 target row marked met/not met with pinned numbers; a miss is a numbered follow-up, not a reworded target.

### L4b: held-out run and promotion (once)
- Run C7 and C5 on the holdout under the oracle Ratchet rules (`vol-oracle-iter`, exact-SHA, scorecard committed by the Ratchet only); no tuning on it.
- If C7 ≥ C5 on the held-out scorecard with 0 violations: flip the C7 keys to defaults in ONE commit, re-baseline the hash set in the same commit, ledger line per hash, update `atx-vol/CLAUDE.md` parity section. Otherwise: no promotion; write the gap as the next sprint's §0.

## 6. Process notes for the PM running this

- Workspace: new `C:/atx/.superpowers/sdd/<date>-vol-sota/` with `SHARED-BRIEF.md` copied from resume 6 (root SHA and RunId refreshed), one brief per lane above (the W0 briefs already exist in the resume-6 workspace), `progress.md` ledger with rulings.
- Dispatch order and CPU: W0 lanes (3) + nothing else; W1: L1 alone first (timing), then L1b + L1c together; W2: L2b + L2d first (disjoint), L2a next, L2c after L2a; W3: L3b + L3c + L3e-prep in parallel while L3a → L3d hold `quote_slice/quote_surface` in sequence; W4: L4a alone, then L4b.
- Each lane: brief → Opus implementer → report → fresh Opus reviewer at the exact SHA → (fix round → scoped re-review) → integrator in pool-1 with the lane's focused gates; full parity set at each wave close; ledger lines appended by the integrator; sprint note per lane.
- Token/time budget guidance from resume 6: implementer lanes 80-480k tokens / 15-90 min; reviews 290-330k / 20-40 min; integrations 150k / 12 min. W0+W1 ≈ 1 session; W2 ≈ 1-2 sessions; W3 ≈ 1-2 sessions; W4 ≈ 1 session.
- Stop conditions: any parity mismatch (stop, do not "fix" in the integrator), any served-slice violation count > 0 at a gate, any lane touching the holdout.

## 7. Self-review against the targets

- §1.1 rows: per-expiry and SPY targets → L2a-c (+L2d for all-core); dev200 all-core → L2d + L2c; record IV → unchanged scope, pinned in L4a; byte-identity across workers → L2d.
- §1.2 rows: violations/containment/MAE → L2a (Review-Focus #4), L3a, L3d, G3; index coverage → L2a + L3a (SPY's convexity micro-kink refusals need the vol-space fit's certification to stay accurate — L2b's accurate confirmation keeps the arbiter); srVol bp → unchanged metric, L1b scorer; `vol_convention` → L3c; intersection MAE → L3e.
- §2 classes: index L2a/L3a, mega-cap L3a, high-vol L3d/L3e, sparse L3d, wide-spread L2a#4/L3d, payers L3c, negative carry L1c/L3b (carry), short-dated L3b, LEAPS L1c/L3b.
- Open unknowns the profile (L1) decides: whether the channel projection (`project_in_channel`) or the AL pricing dominates; if projection dominates, L2a's vol-space objective is the fix; if pricing dominates, L2b's shared geometry is; both lanes exist because today's evidence (12-19 iterations × 325-strike reprices, 24/33 late refusals) points at both.
