# SPY1s-owner sprint note — the SPY surface fit in < 1 s on ONE core (vol-sota resume 3, 2026-10-20)

Lane `SPY1s-owner`, branch `feat/vol-spy1s-20261020`, base `f4a5028e` (= the W2 root, pool-1
`feat/vol-resume4-20261016`), pool-5, RunId `vol-20261020r3-SPY1s`. Brief:
`.superpowers/sdd/2026-10-18-vol-sota/lane-SPY1s-owner-brief.md`. User directive (2026-10-20, verbatim,
binding): "I want an additional agent who is solely responsible for fitting SPY < 1 second on a single core
while maintaining existing accuracy to spiderrock. If it can be achieved it should be merged in. Novel
approaches allowed."

The bar (brief, exact): input `fit-timing-20261017-cohort5\input\quotes-SPY.csv` (sha256 `b3fafd80…`;
13,168 rows, 32 chains fitted as one surface, 1,410 served at today's default), `--model quote-slice
--policy graded-v1.json --dividends funding-pv`; clock = `fit_and_reprice_seconds` of the quote-bench
report (the research's clock A plus the in-clock per-stage worker-thread creation and the policy /
dividend-schedule resolution), the runner's / process `wall_s` beside it; `ATX_VOL_FIT_WORKERS = 1`;
one run, load sampled before and after. BAR: `fit_and_reprice_seconds` < 1.0 s. Accuracy gate: dev200 +
dev200b + cohort5, all-keys vs all-keys-OFF of the same binary — 0 served violations, coverage and
containment not lower, MAE (USD and /spread) not higher beyond 1e-3 relative, p99 not higher, the served
set identical or every difference listed, every served price difference explained by distance to the
accurate root (SHARED-BRIEF ruling a). SpiderRock `srVol` / `srPrc` are the yardstick, never inputs.

Phase 0 (this note's §0-1) makes NO source change. Everything measured here is UNQUALIFIED (shared host,
three other vol lanes live; the load line is beside every number). Evidence folder
`C:\atx-cache\quote-benchmark\SPY1s-20261020-phase0\` (`runs.log`, `spy\*`, `scripts\run_phase0.ps1`,
`scripts\summarize_phase0.py`, `bin\`, `sha256.txt`).

## 0. Where the seconds go (SPY graded-v1, one core, binary built from `f4a5028e`)

Binary `bin\atx-vol-quote-bench-f4a5028e.exe` sha256 `a3448101e8f06da1745868682bd8b19edc6f18a77c94d2ba17da8cd43efd7a91`
(pool-5 `vol-rel`, `-Jobs 1`, `build atx-vol-quote-bench` exit 0). Policies: keys OFF =
`slice-coverage-20261016-policies\graded-v1.json` (`d7f89d13…`); keys ON = every graded key that exists at
`f4a5028e` = `L2a-vol-space-20261019-policies\graded-v1-volspace.json` (`faf2f3f7…`: `fit_space vol`,
`deam_profile accurate`, `deam_cache true`; `rung_memo` / `deam_inversion` do not exist in this binary).
Command per row (direct binary, `$env:ATX_VOL_FIT_WORKERS = '1'`):
`atx-vol-quote-bench-f4a5028e.exe --input quotes-SPY.csv --output <row>.csv --model quote-slice --policy
<policy> --dividends funding-pv --report <row>-native.json --timing-detail`.

### 0.1 The @1 numbers

| row | exit | `fit_and_reprice_seconds` | `wall_s` (process) | chain p50 / p95 / p99 ms | served / fitted | load before / after | predictions sha256 |
| --- | --- | ---: | ---: | --- | --- | --- | --- |
| keys OFF (`graded-v1.json`) | 0 | **61.365** | 61.68 | 1,742 / 3,694 / 3,743 | 1,410 / 4 of 32 | 59 % / 17 % | `087e6a54` MATCH (= the SPY graded-v1 identity) |
| keys ON at `f4a5028e` (`graded-v1-volspace.json`) | 0 | **34.484** | 34.80 | 1,116 / 1,589 / 1,665 | 1,410 / 4 of 32 | 8 % / 41 % | `087e6a54` MATCH (byte-identical to OFF, as L2a proved) |
| keys OFF re-run (the brief's rule: load-before > 30 % → ONE re-run; the host was NOT quieter — the W2b integrator was building) | 0 | 87.865 | 88.2 | 2,829 / 4,762 / 4,930 | 1,410 / 4 of 32 | 33 % / 40 % | `087e6a54` MATCH |

Both OFF rows did exactly the same work (226,234 AL solves, 257,323 IV iterations, 88 rung attempts, 88
passes) in 61.4 s and 87.9 s: the per-solve cost moved 289 → 418 µs (`deam`) and 168 → 219 µs (`price`)
with the host's load, while a single 2 s load sample (59 % / 33 % before) does not rank the two runs. So:
COUNTS are the primary evidence of this note, seconds are the host's; the bar measurement must be taken
on a quiet host with the load sampled across the run, not once before it. No further repetitions (order
of magnitude first).

`--timing-detail` is report-only (L1's CLI test): all three CSVs are the known identity `087e6a54`. The surface
wall equals the chain-wall sum to 2 ms (61.350 vs 61.348 s; 34.463 vs 34.462 s): the surface-level work
(term priors, calendar pass) is nothing; the seconds are inside the 32 chain fits.

### 0.2 Stage table (exclusive buckets, whole surface, `graded_stats.slices[].timing` summed by `summarize_phase0.py`)

| bucket (what it is) | OFF s | OFF share | OFF AL solves | OFF µs / solve | ON s | ON share | ON AL solves | ON µs / solve |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `deam` — American IV inversions of bid / mid / ask + the kIvMin / kIvMax floor-cap prices (OFF: rebuilt at every rung attempt; ON: point-admission mids once) | 55.379 | 90.3 % | 191,511 | 289 | 3.609 | 10.5 % | 11,425 | 316 |
| `other` — at this base the vol-space band fills (the same inversions + edges, once per expiry; L2c re-charges them to `deam`) | 0.004 | 0.0 % | 0 | — | 24.641 | 71.5 % | 79,787 | 309 |
| `price` — the certification-grid American reprice (88 passes × ~394 prices) | 5.813 | 9.5 % | 34,694 | 168 | 6.029 | 17.5 % | 34,694 | 174 |
| `certify` — the certificate, its repairs, row re-implication | 0.014 | 0.0 % | 29 | 499 | 0.016 | 0.0 % | 29 | 536 |
| `solve` — the channel solve (56 solves, 1,126 Jacobian evaluations; prices no American option) | 0.137 | 0.2 % | 0 | — | 0.167 | 0.5 % | 0 | — |
| `project` / `calendar` | 0.001 | 0.0 % | 0 | — | 0.002 | 0.0 % | 0 | — |
| **chain wall** | **61.348** | 100 % | **226,234** | | **34.462** | 100 % | **125,935** | |

Counters (both rows): `rung_attempts` 88, `certification_passes` 88, `channel_solves` 56, `calendar_pairs` 14,
`repair_rung_visits` 0; `iv_newton_iters` 257,323 (OFF) → 121,272 (ON) for the same quote set. Top three
chains by wall: OFF 3,746 / 3,737 / 3,665 ms (all `arbitrage_failure`, dense, 242-325 admitted, 3 rungs, 3
certification passes, 8.9-11.7k `deam` solves each); ON 1,683 / 1,627 / 1,563 ms (same chains: `deam` 172-221
ms, `price` 244-336 ms, the rest in `other` = the band fill).

**Reading.** (i) Keys OFF, 90 % of the surface is the de-Americanised inversion pass redone per rung
(191,511 cold solves = 29,725 inversions × 2.31 counted cold Search solves + ~23k floor / cap edge prices,
× the rungs). (ii) The existing keys (L2a) cut the inversion work to one pass per expiry: 91,212 solves
(11,425 + 79,787) in 28.25 s = 310 µs per counted solve — the Search strategy's ~4.6 uncounted warm
`AloPricer` sweeps per inversion ride inside that figure (L2f §1: 918 µs per Search inversion). (iii) The
certification-grid reprice is now 17.5 % (6.0 s, 34,694 Reference prices at 168-174 µs, which matches L2f's
quiet-probe 169 µs: the grid solve is pure AL and did not inflate under load). (iv) Everything else — the
channel solve, the certificate, projections, calendar, surface-level work — is 0.2 s. There is no hidden
bucket: the bar is an inversion-count × µs-per-solve problem plus the grid reprice.

### 0.3 µs per solve by tier (what the fit pays today vs the tiers in the tree)

| tier | µs per boundary solve + price | where measured |
| --- | ---: | --- |
| Reference inside the certification grid (this run, loaded host) | 168-174 | §0.2 `price` |
| Reference inside a Search inversion, per COUNTED cold solve (this run; includes ~2 uncounted warm sweeps per counted solve) | 289-316 | §0.2 `deam` / `other` |
| Reference cold, quiet probe / bench p50 (SPY 23 d) | 169 / 260 | L2f note §1 / §3 |
| Fast (`al_fast_profile`), bound 1e-12·max(P, 1) | 69 | L2f §3 |
| Screen (`al_screen_opts`), bound 7.8e-4·max(P, 1) (4.6e-6 for T ≤ 0.1), 0.48 bp IV | 21-24 | L2f §3 |
| σ-family price / price + chord (12 Reference nodes; build 1.14 ms per (expiry, side)) | 4.7 / 9.3 | L2h (one run on the `850b6d4f` tree, ungated) |
| One American IV inversion: Search Reference / Premium (L2e) / Screen Reference | 918-1,130 / 1.77 cold solves + 0 warm / 107 | L2f §1, L2e COUNT line |

Host factor: L2e's idle-host run (load 2 %) did 75,153 cold solves + 28.9k grid prices in 15.6 s ≈ 150 µs
per solve all-in; this host (load 8-59 %) pays 289-316 per counted inversion solve. Quiet-host arithmetic
below should be read × 1.5-1.9 for this host under load.

## 1. Plan to < 1 s (one core)

Populations per SPY surface at the W2b root (one core; counts from L2e / L2c / L2a, all measured):
inversions N = 29,725; Premium iterate solves ≈ 0.77 N = 22.9k; accepted-root confirms ≈ 28.9k; Search
fallbacks 781 (× ~918 µs each — 0.72 s on their own); floor / cap band edges ≈ 23k; certification-grid
prices 28.9k (34.7k without L2c's candidate memo); (expiry, side) pairs 64. Unit costs = the quiet-host tiers
of §0.3 (Reference 169, Fast 69, Screen 21, family 4.7 / 9.3, build 1.14 ms). Projection = Σ population ×
tier + the fixed 0.2 s (`solve`, `certify`, calendar, surface). Every lever is opt-in behind a policy key
with keys-OFF byte-identity (`c89d0739` / `4b273b08` / `171fa2d2`, SPY `087e6a54`) and ships with a test that
fails on revert; promotion is the PM's at integration on the three-cohort gate.

| # | lever (in the order I take it) | status | what moves | seconds saved (quiet arithmetic) | projected @1 after | accuracy risk — how the gate catches it |
| --- | --- | --- | --- | ---: | ---: | --- |
| 0 | today, `f4a5028e`: 61.4 s OFF / 34.5 s existing keys ON (measured, loaded) | measured | — | — | 34.5 (loaded) | — |
| 1 | **W2b root** = L2c rung memo × L2e Premium inversion × L2f Screen tier (merge FIRST in Phase 1) | L2c / L2f APPROVED; L2e provisional APPROVE; integrating in pool-1 | Search's 2.31 cold + ~4.6 warm sweeps per inversion → 1.77 cold (L2e); the grid memo 34.7k → 28.9k | 34.5 → ≈ 15.6 (L2e measured idle; 29.9 loaded). Arithmetic 22.9k×169 + 28.9k×169 + 0.72 + 23k×169 + 28.9k×169 ≈ 18.4 | ≈ 15.6-18 | Premium roots ≤ 2.4e-10 vol from the accurate root (Search up to 1.75e-3); served set identical on SPY / cohort5 / dev200 (L2e); own hash set (SPY `2c8b113c`) — the integrator's G2 + gate tables |
| 2 | **Phase 1 arm A** — Screen seeds for the Premium iterates (`deam_profile = screen`, `IvOpts::seed_profile`), accurate (Reference) confirm, **1a bound-decided floor / cap edges** on Screen (decide only when the quote clears the edge price by > b; else one accurate edge price), **3a reason split** (`range_edge` / `iterate` / `confirm` / `ambiguous_edge`), the r = q `NonMonotoneRegion` guard → accurate strategy | specified (L2g items 1-3, 6, 9-10) | iterates 22.9k × (169 → 21); edges 23k × (169 → 21) | 3.4 + 3.4 = 6.8 | ≈ 11.6 | None by construction: the confirm is the accurate kernel; an edge is decided only outside the tested bound b = 7.8e-4·max(P, 1) (4.6e-6 for T ≤ 0.1), the ambiguous few re-priced accurately → served set identical; a Screen `NonMonotoneRegion` is a routing, never a refusal. Gate: served-set diff, ambiguous fraction, keys-OFF parity |
| 3 | **Phase 1 arm D** — the confirm on `AlProfile::Fast` (R3), and — PM ruling at integration — Fast as the certification-grid kernel | specified (L2g item 7); promotion = PM ruling b | confirms 28.9k × (169 → 69); grid 28.9k × (169 → 69) | 2.9 + 2.9 = 5.8 | ≈ 5.7 | Fast bound 1e-12·max(P, 1), 0.0054 bp IV (L2f `AlFastProfile.*`): invisible to any band; fingerprint `53c4df68` unchanged (Reference / Fast numerics untouched). Hash set changes → recorded as its own set with max \|Δprice\| vs arm A; the PM promotes or not |
| 4 | **Phase 1 arms B / B3** — band-gated confirm `graded.confirm = band`, 5a acceptance statement (ε_A + ε_r) / v_min < half-spread / k, k = 10 and 3; `never` as the measurement-only arm C; `--deam-audit` reports the confirm fraction and the max accurate-map Newton distance of every SKIPPED root | specified (L2g items 5, 8, 11) | confirms: SPY median IV band 99 bp vs Screen's 0.48 bp bound → most quotes skip (fraction MEASURED) | 0.8-0.95 × 28.9k × 69 ≈ 1.6-1.9 (Fast) / 3.9-4.6 (Reference) | ≈ 3.9 (Fast grid) / ≈ 6.8 (Reference grid) | Fit inputs carry the seed tier's bound (≤ 0.48 bp, k-gated), measured as the max Newton distance of skipped roots — never assumed; a non-positive v_min, a `NonMonotoneRegion` or a kIvMin / kIvMax clamp always confirms. PM decision 2: 1e-7 binds SERVED roots (the serve path re-implies on the accurate kernel). Gate: the Newton-distance column + the three-cohort MAE / containment table |
| 5 | **Phase 2 — the σ-family** (L2i on L2h): Premium iterates by `price_and_chord`, floor / cap edges and the certification grid as family quadratures; adaptive build (L2h §7 rule, c = 3); every family refusal → the previous path; grid nodes accepted only where the certificate allowance 1e-8 + 1e-9·max(S, K) exceeds the family bound, else cold | L2h checkpointed `850b6d4f` (resume lane live; review next); L2i specified | iterates 22.9k × (21 → 9.3); edges 23k × (21 → 4.7); grid 28.9k × (69 → 4.7); + 64 builds × 1.14 ms | 0.27 + 0.37 + 1.86 − 0.07 ≈ 2.4 (on top of #4 Fast) | **≈ 1.4-1.6** — the first row within 2× of the bar | Family price max rel 1.3e-10 vs cold Reference at 12 nodes (L2h, one run — must be re-measured at the reviewed SHA); refusals (`SigmaOutsideBox`, `SigmaBelowGuard`, re-prepared geometry) route cold; dated-cash expiries never touch it; the L1c refusal stays in front; served prices stay certified by the accurate kernel. Gate: served-set identity + \|Δprice\| explained by the CONFIRM arm only; grid-fallback test fails on revert |
| 6 | **Phase 3a — the 781 Search fallbacks** (no_seed 381 deep-ITM puts where BAW sits at intrinsic; no_convergence 4; not_increasing 157; range_edge 239 = correct refusals, untouched): a floored Black-76 + EEP seed leg (L2e's deferred "principled fix") or a family-chord bracket; the residual Search on a Screen-seeded bracket with ONE accurate confirm | novel (L2e named it, deferred) | 542 fallbacks × (918 → ~50-100) | ≈ 0.45-0.5 | ≈ 0.9-1.1 | Roots still accepted only at Newton distance ≤ tol on the accurate map; `range_edge` stays a refusal; fallback counts by reason in `--deam-audit` fail-on-revert |
| 7 | **Phase 3b — certificates instead of the residual confirms** (gap report §6.6 with the family's tested chord bound as v_min): the 5-20 % confirms the band gate keeps | novel (research-costed) | 1.5-6k × 69 | ≈ 0.1-0.4 | ≈ 0.6-0.9 | The bound (ε_A + ε_r) / v_min IS the acceptance statement; reported per arm with the max Newton distance of the certified roots |
| 8 | **Phase 3c — if still ≥ 1 s**: iterate count 1.77 → ~1.3 by seeding bid / ask from the mid's family chord (the `IvPremiumMemory` already carries the mid's points), Fast-profile family nodes (64 × 12 × 69 µs = 0.05 s of builds), and the W2b G2 numbers deciding whether any per-chain fixed cost (channel solve 0.17 s, 88 rung attempts) is worth a policy question for the PM | novel | iterates −25 %; builds −0.08 | ≈ 0.1-0.2 | ≈ 0.5-0.8 | Same acceptance as #5-7; a rung-structure change is a POLICY change (served / refused outcomes) and is the PM's, not mine |

Projected floor on a quiet host after #1-7: `solve` 0.17 + `certify` 0.02 + iterates 0.21 + edges 0.11 +
grid 0.14 + builds 0.07 + residual confirms ≈ 0.1 + fallbacks ≈ 0.05 + surface / other ≈ 0.05 ≈ **0.9 s**.
That is the honest arithmetic: the bar is reachable at one core, with little margin, and only when every
lever above lands; on this host under its current load (× 1.5-1.9) the same work reads 1.3-1.7 s, so the
bar measurement must be taken when the host is quiet (the brief's load rule). The per-phase hand-backs
re-base this table on measured numbers (Phase 1 = the first all-keys @1 number; Phase 2 = the second).

### 1.1 Levers rejected, with the number that rejects them

| lever | the number | verdict |
| --- | --- | --- |
| eSSVI / SVI-shaped quoting surface, snapshot seed, iteration budgets (research §7) | `solve` = 0.137-0.167 s (0.2-0.5 % of the surface; 0 American prices in 56 solves); the family is an accuracy decision (LEDGER L308: eSSVI leaves a 56 bp W-shaped residual against the vendor curve) | reject for speed; W3 accuracy lanes |
| Convex price-space QP initialiser (research §9) | `solve` + `project` = 0.17 s; `ConvexDense` + `AmericanPriceSlice` + bounded channel repair already exist | reject for speed (R5 is accuracy, W3) |
| AVX2 / SIMD vectorisation of the cold solve inside the fit | 2.0-2.3× on the `price` bucket = 6.0 → 2.7 s; the family route takes the same bucket to 0.14 s and the inversion loop is one contract at a time | reject now (research §9 item 7); revisit only for family builds |
| Support reduction / fewer inversions per quote (research §8) | the cost is inversions per quote (3 + 2 edges), the channel needs the bid AND ask IV; dropping quotes changes the served set (ruling b) | reject |
| GPU | the whole per-surface work after #5 is < 1 CPU-second; transfer + launch overhead dominates | reject (research §9 item 7) |
| `fit_workers 0` fan-out (L2d) | not a one-core lever by the bar's definition; it stays for the @0 row (≤ 150 ms target) | out of scope for the bar |
| Fewer rung attempts / certification passes by policy (88 attempts, 88 passes, 28 of 32 chains refused after 3 rungs; refused chains = 95.6 % of the OFF wall, L1) | after #5 a pass costs 325 × 2 × 4.7 µs ≈ 3 ms; L2c found no NECESSARY pre-screen condition; a rung change alters served / refused outcomes | reject as a speed lever; policy is the PM's |
| A faster REFERENCE solve (bit-identical tier 1) | L2f §2: no allocation, no recomputation left; 86 % of a Reference solve is 1,584 `std::erfc` node evaluations — the special function, not the iteration count, is the lever, and Fast / Screen already are that lever | reject (done by L2f) |

### 1.2 What Phase 1 measures (so the table above becomes numbers)

The L2g matrix at `fit_workers` 1 (and 0 for information): A (Screen seeds, Reference confirm), B (band,
k = 10), B3 (band, k = 3), C (never — measurement only), D (A with the Fast confirm); per arm
`fit_and_reprice_seconds` + `wall_s`, chain p50 / p95 / p99, AL solves by tier and by reason, µs per solve,
confirm fraction, max accurate-map Newton distance of skipped roots, served-set diff + max |Δprice| vs
all-keys-OFF, the usable / unusable line; one dated-cash lattice row beside arm A (PM decision 1: the bar
stays on funding-pv); the clock declaration and the four manifest lines in `RESULTS.md`
(`C:\atx-cache\quote-benchmark\SPY1s-20261020-phase1\`).

## 2. Phase 1 — Screen seeds + confirm policy (the L2g spec, items 1-13, on the W2b root)

Base: `1da69e2e` merged with the W2b root `be379ad4` as `92d4ba0d` (`--no-ff`, no conflicts, 35 files). Pool-4,
RunId `vol-20261020r3-SPY1s-p1`, `-Jobs 2`, `ATX_VOL_FIT_WORKERS` 2 for parity / cohort runs and 1 for the bar rows.
Evidence `C:\atx-cache\quote-benchmark\SPY1s-20261020-phase1\` (`runs.log`, `spy\`, `audit\`, `cohort5\`, `dev200\`,
`dev200b\`, `score\`, `policies\`, `scripts\`, `bin\`, `RESULTS.md`, `sha256.txt`). Every timing is UNQUALIFIED
(shared host; the host load is sampled every 2 s ACROSS each bar run and its mean / max stand beside the number).
The two binaries: the merged root before any Phase 1 change `bin\atx-vol-quote-bench-92d4ba0d.exe` (`6a63a9cb…`)
and the Phase 1 source `bin\atx-vol-quote-bench-phase1.exe` (`ed3667cc…`, = the final SHA's `build-vol-rel` binary).

### 2.1 Concern-1: the W2b `band()` merge (PM ruling: read the three re-sync lines and state a verdict)

`deam_iv_cache.cpp` `DeAmIvView::band` at `be379ad4`: `filled` is copied from the slot `entry` at the top; the
mid-first block then calls `mid_iv(i)`, which writes the SLOT (`entry.mid_iv`, `entry.mid_memory` through the
inversion's memory pointer, `entry.computed |= kMid`); `publish()` ends with `entry = filled`. Without the three
lines `filled.mid_iv = entry.mid_iv; filled.mid_memory = entry.mid_memory; filled.computed = entry.computed;` the
publish would overwrite the fresh mid with the stale copy (NaN mid, empty memory, no kMid) — exactly the drop the
integrator described. The three lines copy precisely the fields `mid_iv()` mutates and nothing else (`carry(i)` ran
before the copy, so q_effective / local_forward / kCarry are already equal), and `seeded` reads the slot's memory
after the copy. **Verdict: the merge is correct — the minimal faithful union of L2c's local-fill-then-publish and
L2e's mid-first seeding.** Phase 1 keeps the three lines (the block now sits behind `mid_inside`, section 2.2 (4)).

### 2.2 What was built (all opt-in; every key refused where it would be inert)

1. **`graded.deam_profile = "screen"`** → `QuoteSliceDeamProfile::Screen` (requires `deam_inversion premium`;
   a Search fit on Screen is refused by `validate_channel` and by the policy loader). The cache's `kernel()` is
   `DeAmKernel::Screen`; `SliceDeAmModel` holds `al_screen_opts()` beside the Fast profile and prices band edges on it;
   `implied_vol_premium` sets `IvOpts::seed_profile = AlProfile::Screen` and hands the model's `DeAmPremiumControls`
   (seed kernel, confirm policy / k / half-spread, confirm kernel) to `american_implied_vol_premium(..., gate)`.
2. **`IvOpts::seed_profile = Screen` selects the MEASURED tier `al_screen_opts()`** (7 nodes, l 16, 5 sweeps, p 48),
   not al_opts' 12-node scheme on the Screen arithmetic (which would cost Fast's 69 µs, not Screen's 21). Pinned by
   `IvPremiumStats::seed_nodes` = 7 (`AmericanIvPremium.ScreenSeedsRunTheMeasuredTierAndAreConfirmedOnTheAccurateMap`).
3. **The confirm is a bounded Newton on the confirm kernel** (`kIvPremiumConfirmSolves = 2`): a Screen root sits up
   to 7.8e-4·max(P, 1) off the accurate map, in vol often > tol = 1e-7, so L2e's single-solve rule (|dc| ≤ tol or
   fall back) turned 150 of the 672-contract grid's 551 accepted roots into Search fallbacks (first build: 271
   fallbacks vs Search's 121). Now the first confirming solve accepts when its Newton distance is ≤ tol, else takes
   the step and one more solve confirms; a second miss falls back to Search. Grid: 551 accepted = Search's set,
   150 of 502 confirmed roots needed the second solve, max accurate-map Newton distance 1.34e-11.
4. **Item 6 — the r = q guard as a routing**: before the iterates, a price with `price − zero_vol ≤
   al_non_monotone_gate(K, T, seed_opts)` (Screen: the WIDE gate 4e-4·K·max(1, T²), 80× the accurate 5e-6) runs its
   iterates on the ACCURATE kernel (seed_opts := al_opts, no confirm needed; `IvPremiumStats::seed_routed`,
   counter `deam_seed_routed`). Screen is never classified as the accurate scheme (the gate call is on the Screen
   opts). A Screen refusal therefore cannot reach a quote: the routed inversion IS the accurate Premium inversion
   (max Newton 1.3e-11 on the grid) and its own fallback is Search. On the constructed r = q grid (3 carries × 3
   maturities × 4 moneyness × 2 sides × 2 offsets inside the wide gate) 122 of 144 candidates were routed (the other
   22 are RangeEdge fallbacks = Search); Screen AS the kernel on those contracts returned Ok roots up to 1.26e-6 vol
   off the accurate root rather than a `NonMonotoneRegion` refusal — the brief's "Screen refuses" half could not be
   constructed there (reported, not asserted; see Concerns).
5. **Item 1a — bound-decided band edges**: `band()` prices kIvMin / kIvMax on Screen; every comparison a quote
   makes with an edge (bid / ask against both, the mid against both = the vol-space builder's own test) is
   `settle()`d: decided on the Screen price when the quote clears it by more than the margin, else THAT edge is
   re-priced on the accurate kernel first (counter `ambiguous_edges`, reason `ambiguous_edge`); the settled edges are
   what the entry publishes, so `vol_space_channel_observations`' mid test agrees. Margin: the Screen bound was pinned
   on sigma 0.12-0.7 (AlScreenProfile.*), and at kIvMin / kIvMax over the 672-contract grid the Screen error reaches
   8.6e-4·max(P, 1) with 9 of 1,344 edges over 7.8e-4 and the short-dated 4.6e-6 not holding at all, so a Screen edge
   decision clears TWICE the whole-grid bound at every maturity (0 of 1,344 edges over it; asserted). Accurate and
   Fast kernels keep strict comparisons (the historical path, byte-identical: parity below).
6. **Item 3a — reason split** `FitTimingCounters::deam_al_by_reason` {range_edge, iterate, confirm, ambiguous_edge,
   gate, search} (+ `deam_confirms`, `deam_confirms_skipped`, `deam_ambiguous_edges`, `deam_seed_routed`,
   `deam_gate_max_bound_vol`, `deam_audit_skipped_pairs`, `deam_audit_max_newton_skipped`) in `--timing-detail`.
7. **Items 5 / 5a — `graded.confirm = always | band | never`, `confirm_k`** (requires `deam_profile screen`).
   `band`: skip the confirm iff (ε_A + ε_r) / v_min < half_spread / (v_max · k) — both sides in VOL units (the
   brief's statement read on the vol scale: the half-spread in price is converted with the chord PLUS its margin,
   the conservative side). ε_A = `al_profile_price_bound(Screen, P, T)` (7.8e-4·max(P,1); 4.6e-6 for T ≤ 0.1);
   ε_r = the root's residual on the seed map; the chord = one more Screen solve a kAlPackSigmaBump (1e-4) away
   (reason `gate`); volga from the chord to the previous cold point (a 2× margin without one); v_min = min(chord,
   Premium slope) − (h·|volga| + 1e-8), floored at kDeAmFastMinRelativeVega·max(P, 1); v_max = max(..) + margin. A
   non-positive v_min, a bidless quote (half-spread 0), a routed price or a root at the vol bounds confirms.
   `never`: the Screen root as is (measurement arm C). The skipped root's bound (ε_A + ε_r)/v_min is recorded
   (`gate_bound_vol`) and `--deam-audit` MEASURES the skipped roots' accurate-map Newton distance.
8. **Item 7 / arm D — `graded.confirm_kernel = reference | fast`** (confirm on `al_fast_profile(al_opts)`) and
   **`graded.grid_kernel = reference | fast`** (the certification grid of `certify_and_serve` priced on
   `al_fast_profile(out.al_opts)` for the continuous-carry models; the dated-cash lattice ignores it; the candidate
   memo key carries the grid kernel word). Both measurement arms (own hash set), nothing promoted.
9. Tests (`atx-vol-pricing-slice` AmericanIvPremium.* 9/9, `atx-vol-quote-slice-tests` QuoteSliceVolSpace.* +
   DeAmIvCache.* 11/11 incl. the new `ScreenSeeds…`, `APriceInsideScreensNonMonotoneGate…`, `BandGate…`,
   `DeAmIvCache.ScreenBandEdgesAreDecidedWithTheBoundOrRepricedAccurately`,
   `QuoteSliceVolSpace.ScreenSeededPremiumCacheServesConfirmedVolatilities`; CLI strict cases for the six key
   rules). Fail-on-revert: section 2.9.

### 2.3 The SPY matrix (graded-v1, funding-pv, `fit_workers` 1 unless noted; direct binary; `spy\*`)

| row (binary) | `fit_and_reprice_s` | `wall_s` | load mean / max | chain p50 / p95 / p99 ms | served | CSV sha256 |
| --- | ---: | ---: | --- | --- | --- | --- |
| before: W2b all keys (`92d4ba0d`, load not sampled — the sampler failed on this row) | **18.385** | — | — | 576 / 1,009 / 1,025 | 1,410 | `2c8b113c` (= W2b) |
| before: W2b all keys, re-run (`92d4ba0d`) | 33.569 | 34.25 | 71 % / 91 % | 897 / 1,717 / 1,972 | 1,410 | `2c8b113c` |
| W2b all keys on the Phase 1 binary (identity: Screen off) | 17.023 | 17.24 | 22 % / 30 % | 486 / 980 / 1,012 | 1,410 | `2c8b113c` MATCH |
| **arm A** — Screen seeds + 1a edges, Reference confirm (`confirm always`) | **13.680** | 13.96 | 24 % / 41 % | 391 / 737 / 809 | 1,410 | `19d47b61` |
| **arm B** — `confirm band`, k = 10 | **11.549** | 11.74 | 15 % / 26 % | 263 / 713 / 760 | 1,410 | `5985dfe1` |
| **arm B3** — `confirm band`, k = 3 | **12.137** | 12.34 | 32 % / 47 % | 331 / 767 / 790 | 1,410 | `3b72a083` |
| **arm C** — `confirm never` (measurement only) | **9.118** | 9.33 | 21 % / 29 % | 260 / 511 / 520 | 1,410 | `8f80be87` |
| **arm D** — arm A + `confirm_kernel fast` + `grid_kernel fast` | **8.549** | 8.75 | 21 % / 32 % | 233 / 531 / 544 | 1,410 | `feac8d2c` |
| arm A at `fit_workers` 0 (information) | 2.912 | 3.13 | 65 % / 79 % | 834 / 1,994 / 2,062 | 1,410 | `19d47b61` (= @1) |

**Every arm is UNUSABLE against the 1 s bar (8.5-13.7 s @1).** The W2b "before" on this host: 18.4 s (the
integrator's 15.0 s was at 9-14 % load); the same W2b work on the Phase 1 binary 17.0 s at 22 %.

### 2.4 The two levers and the reason split (SPY surface, 32 timed chains; `scripts\summarize_phase1.py`)

| arm | de-Am AL solves (`deam_work`) | by reason: range_edge / iterate / confirm / ambiguous_edge / gate / search | grid prices | µs per de-Am solve / per grid price | `deam` wall s | `price` wall s | inversions, AL per inversion, fallbacks |
| --- | ---: | --- | ---: | --- | ---: | ---: | --- |
| W2b all keys (Phase 1 binary) | 75,153 | 22,590 / 50,308 / 0 / 0 / 0 / 2,634 (all Reference) | 28,908 | 171.9 / 137.0 | 12.92 | 3.96 | 29,725, 1.768, 781 (239 / 381 / 157 / 4) |
| arm A | 106,232 | 22,590 Screen / 50,308 Screen / 28,551 Ref / 2,734 Ref / 0 / 2,655 Ref | 28,908 Ref | 86.9 / 148.7 | 9.23 | 4.30 | 29,725, 2.722, 787 (239 / 380 / 157 / 11) |
| arm B (k = 10) | 117,257 | 22,590 / 50,308 / 15,940 / 2,734 / 23,636 Screen / 2,655 | 28,908 | 65.5 / 129.2 | 7.68 | 3.73 | 29,725, 3.093, 787 |
| arm B3 (k = 3) | 106,150 | 22,590 / 50,308 / 4,856 / 2,734 / 23,636 / 2,632 | 28,908 | 70.3 / 156.4 | 7.46 | 4.52 | 29,725, 2.719, 780 |
| arm C (never) | 77,884 | 22,590 / 50,308 / 0 / 2,734 / 0 / 2,632 | 28,908 | 64.0 / 138.3 | 4.98 | 4.00 | 29,725, 1.768, 780 |
| arm D (Fast confirm + grid) | 106,232 | 22,590 / 50,308 / 28,551 Fast / 2,734 / 0 / 2,655 | 28,908 Fast | 62.8 / 60.0 | 6.67 | 1.74 | 29,725, 2.722, 787 |

Per arm the fixed buckets are `solve` 0.11-0.13 s, `certify` 0.01 s. Seed routing (item 6): **5,849 of 29,725
inversions (19.7 %)** lie inside Screen's wide gate and run Reference iterates — their ~10k solves sit in the
`iterate` reason beside the Screen ones. Ambiguous edges: **2,881 of 22,590 (12.75 %)** → 2,734 accurate re-prices.
Confirms (arm A): 23,089 confirmed roots (28,938 accepted − 5,849 routed) took 28,551 Reference solves = 1.24 per
root (the second confirm solve on 24 % of them). Band gate: k = 10 skipped 10,476 of 23,636 gated roots (44 %; max
gate bound 0.033 vol); k = 3 skipped 19,415 (82 %; max bound 0.49 vol) — and every gated root paid one Screen chord
solve (23,636 × ~21 µs ≈ 0.5 s), so at k = 10 the gate saves 10,476 × ~169 µs − 0.5 s ≈ 1.3 s net.

Reading against the Phase 0 arithmetic (arm A projected ≈ 11.6 s, D ≈ 5.7, B ≈ 3.9 on a quiet host): (i) the
confirm is the bucket — 28,551 Reference solves ≈ 4.8 s in arm A (the second solve adds 24 %), Fast confirm 6.67 −
4.98 ≈ 1.7 s in arm D; (ii) the item-6 routing sends a fifth of the inversions to Reference iterates (~1.7 s); (iii)
the gate skips 44 % at k = 10, not the 80-95 % the band-width arithmetic promised (the vol-unit statement with v_max
on the band side is strict: the Screen bound 7.8e-4·max(P,1) is 0.3-0.5 bp of vol only on vega-rich contracts); (iv)
the host ran 15-32 % loaded. Arm C (never) at 9.1 s is the floor Screen seeds alone reach with the Reference grid
(4.0 s) and the routed / fallback Reference work; the grid (28,908 × 137-149 µs ≈ 4 s) is now the largest single
bucket, as Phase 0 predicted — arm D's Fast grid takes it to 1.7 s.

### 2.5 Confirm fraction, skipped roots, audit (`audit\*`, `--deam-audit`; filled from the audit rows below)

Rows `audit\<arm>-audit-w1` (`--deam-audit`: every accepted Premium root AND the OFF Search root of the same quote
re-priced on the accurate map; `fit_workers` 1). These rows ran while the cohort runner was active (load 48-83 %),
so their seconds (55-83 s) are NOT bar numbers; their counts equal the bar rows' (same CSV hashes: A `19d47b61`,
B `5985dfe1`, B3 `3b72a083`, C `8f80be87`, D `feac8d2c`). 29,725 inversions per arm, 28,938 accepted Premium roots
(780-787 Search fallbacks: `range_edge` 239, `no_seed` 380, `not_increasing` 157, `no_convergence` 4-11),
`audit_status_mismatch` 0 in every arm (a Screen-seeded inversion never changes a quote's status).

| arm | confirm solves (per accepted root) | roots gated (one Screen chord solve each) | confirms skipped | max accurate-map Newton distance of a SKIPPED root (vol) | max Newton, accepted Premium roots | max Newton, the OFF Search roots | max price residual USD | max gate bound (vol) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| A (`always`, Reference) | 28,551 (0.987; 1.24 per confirmed root) | 0 | 0 | — | 7.72e-11 | 1.75e-3 | 3.0e-5 | — |
| B (`band`, k = 10) | 15,940 (0.551) | 23,636 | 10,476 (44.3 % of gated) | **1.12e-5** (112 × tol) | 1.12e-5 | 1.75e-3 | 4.0e-5 | 0.033 |
| B3 (`band`, k = 3) | 4,856 (0.168) | 23,636 | 19,415 (82.1 %) | **2.52e-4** | 2.52e-4 | 1.75e-3 | 4.0e-5 | 0.485 |
| C (`never`) | 0 | 0 | 23,636 (100 %) | 2.52e-4 | 2.52e-4 | 1.75e-3 | 4.4e-5 | — |
| D (`always`, Fast confirm) | 28,551 (0.987) | 0 | 0 | — | 7.72e-11 | 1.75e-3 | 3.0e-5 | — |

Reading: (i) with `confirm always` every served root is within 7.7e-11 vol of the accurate map (tol 1e-7) on BOTH
confirm kernels — the Fast confirm (D) is as good as Reference here, as its 1e-12·max(P, 1) bound predicts; (ii) under
`band` a skipped root is NOT 1e-7-accurate: 1.12e-5 at k = 10 and 2.52e-4 at k = 3 (= the unconfirmed Screen root,
arm C's number) — these are FIT INPUTS off the accurate root by up to that distance, inside the gate's own
statement (half-spread / k in vol) but outside the brief's 1e-7 (manifest line 1 below: 1e-7 binds served roots;
band inputs carry the seed tier's bound, reported here as the max Newton distance); (iii) the OFF Search roots sit
up to 1.75e-3 Newton-distance off the accurate map — Search's bracket is 1e-7 in vol, so this is the yardstick
(price residual / local slope) blowing up at a near-intrinsic contract, not a 1.75e-3 vol error; it means the
Newton yardstick overstates vol error where the slope is small (the routing test's near-intrinsic cases needed a
price-residual criterion for the same reason, §2.9).

### 2.6 Served set, accuracy gate, parity (filled from the cohort rows below)

**Served set vs the same binary, all keys OFF (SPY; `scripts\served_compare.py`; OFF row `spy-graded-v1-off-w1`
= `087e6a54` MATCH, 99.2 s at 66 % / 91 % load).** Every arm serves the same 1,410 rows (lost 0, gained 0, status
counts identical). Max |Δprice| USD / max |Δvol| over the served rows (rows differing by > 1e-9): W2b all keys
2.012e-5 / 7.571e-8 (rows 26947337 / 26947299); arm A 2.012e-5 / 7.571e-8 (469 rows); B 1.996e-5 / 7.513e-8 (563);
B3 2.468e-5 / 1.052e-7 (913); C 2.281e-4 / 7.619e-7 (997); D 2.012e-5 / 7.571e-8 (469). Arms A and D reproduce the
W2b differences exactly (the confirmed roots ARE the accurate roots to 7.7e-11; the W2b-vs-OFF difference is
Search-vs-Premium, L2e); B / B3 / C move the served prices by the unconfirmed roots' distance (C: 10× A).

**Parity at the Phase 1 binary** (`bin\atx-vol-quote-bench-phase1.exe` `ed3667cc…`; `fit_workers` 2; `runs.log`,
once each): dev200 auto `c89d0739` MATCH; dev200b auto `4b273b08` MATCH; cohort5 graded-v1 `171fa2d2` MATCH;
dev200 graded-v1 `fc277413` MATCH; dev200b graded-v1 `46ad6c26` MATCH; SPY graded-v1 `087e6a54` MATCH; W2b all
keys SPY `2c8b113c` MATCH. AL fingerprint (`vol-rel-avx2` `atx-vol-al-preset-ladder-bench`, exe `0431af54…`,
`american/ladder_fingerprint` → `fingerprint\ladder_fingerprint-phase1.bin`) `53c4df68…dcea` MATCH, the ten section
fnv1a lines unchanged (`american.hpp` / `american.cpp` / `american_sigma_family.cpp` untouched).

**Accuracy gate** (`quote_benchmark.py score --split all`, keys-OFF graded-v1 vs the arm on the same binary;
`score\*-score.json`, `gate\*-gate.txt`, `scripts\gate_summary.py`; served = rows with status ok / ok_price_only;
MAE / p99 are the scorer's `price_mae` / `price_mae_per_spread` (= `price_in_spreads_mae`) / `price_p99_abs`):

| cohort / arm | served OFF → ON | coverage | containment | MAE USD | MAE / spread | p99 USD | max Δprice / Δvol (served rows) | verdict |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| cohort5 / A | 8,375 = 8,375 | 0.38415669 = | 0.9159403 = | 0.0834123 → 0.0834122 | 0.2101858 → 0.2101860 | 1.561321 = | 4.1e-5 / 5.6e-7 | **PASS** |
| cohort5 / B | 8,375 = | = | = | → 0.0834113 | → 0.2101856 | = | 3.5e-4 / 3.3e-6 | **PASS** |
| cohort5 / B3 | 8,375 = | = | = | → 0.0834114 | → 0.2101858 | 1.561321 → 1.5613218 | 3.5e-4 / 3.3e-6 | **PASS** |
| cohort5 / D | 8,375 = | = | = | → 0.0834122 | → 0.2101860 | = | 4.1e-5 / 5.6e-7 | **PASS** |
| dev200 / A | 50,348 = 50,348 | 0.78740108 = | 0.98816239 → 0.98814253 (**−1 row**) | 0.23426128 → 0.23426124 | 0.10850518 → 0.10850514 | 1.9718781 = | 2.8e-4 / 1.14 (3 rows, below) | PASS on every field but containment: −1 row, explained below |
| dev200 / D | 50,348 = | = | −1 row (the same row) | → 0.23426124 | → 0.10850514 | = | 2.8e-4 / 1.61 | as A |
| dev200b / A | 54,355 = 54,355 | 0.87150668 = | 0.98828075 → 0.98826235 (**−1 row**) | 0.23928121 → 0.23928122 | 0.11635936 → 0.11635937 | 1.7600043 → 1.7600015 | 4.0e-5 / 1.07 (12 rows) | as dev200 / A |
| dev200b / D | 54,355 = | = | −1 row (the same row) | → 0.23928122 | → 0.11635937 | → 1.7600015 | 4.0e-5 / 1.48 | as A |

MAE USD and MAE / spread move by ≤ 2e-6 relative (cohort5 ≤ 1.2e-5), p99 by ≤ 1.6e-6 relative — all far inside
1e-3. The containment row: dev200 `row_id` 21049805 (PHAT, 20777 d, 12.5 C, `bidless_quote`, bid 0 / ask
0.15000000596 — the float32 of 0.15): served price OFF 0.14999999558 (inside by 4.4e-9), arms A / D 0.15000001976
(outside by 1.4e-8), |Δprice| 2.4e-8 USD; dev200b `row_id` 4558128 (BMEA, 21204 d, 1 P, `bidless_quote`, ask
0.55000001192): 0.54999993386 → 0.55000001786, |Δprice| 8.4e-8. Both are a served price sitting ON a float32
quote edge and moving by < 1e-7 USD with the refit — the distance-to-the-accurate-root explanation the brief allows
(the fit inputs moved by the Search-vs-Premium root difference, ≤ 1.5e-6 vol on the Premium grid, L2e) — but the
literal "containment not lower" is violated by one row in each cohort; PM ruling requested. The large |Δvol| rows
(3 / 12, all `quote_status excluded_side`, grade B) have IDENTICAL served prices (e.g. dev200 row 466851: price
24.605 in both, vol 0.005 ↔ 1.1439; dev200b row 11725080: 5.84 in both, vol 1.0703 ↔ 0.005): the price sits at
intrinsic, where the price-vol map is flat and the reported vol of an excluded-side row is unidentifiable (it flips
between the kIvMin clamp and a far root under any change of the inversion path); vol MAE moves by ≤ 1.7e-4 relative
(1531.30 → 1531.15 / 1530.87 bp on dev200; 1450.09 → 1450.18 / 1450.34 on dev200b) and vol is not a gate field.
No run in this phase exceeded 151 s (dev200 graded-v1 OFF at cap 2); the hygiene check 195 s (PM directive
2026-10-20: ≤ ~10 min per run).

### 2.7 The dated-cash row (item 12; PM note 2026-10-20: the loader refuses `dividend_lattice` under graded admission)

Run as the existing NON-graded pair on the SPY cohort5 file, `fit_workers` 1: `policy-divsched-v1.json`
(`46ee9efc…`, every profile `dividends: inferred-schedule`, spliced lattice) vs the same profiles with `--policy auto`
(aggregate funding-pv). `datedcash-divsched-v1-w1`: **5.574 s** (wall 5.71, load 8 % / 10 %), 33 chains, 17 with an
ex-date in life (`cash_dividend_chains`), routes `escrow` 16 (`no_ex_date_in_life`) / `escrow_fallback` 17
(`lattice_certification_refused` 16, `lattice_refused` 1) — every lattice attempt fell back to the aggregate fit,
`lattice_chains_seconds` 0.0 (no lattice chain certified). `datedcash-ctl-auto-w1`: **4.280 s** (wall 4.38, load
10 % / 13 %). Gap: **+1.29 s (+30 %) for the 17 lattice attempts (≈ 76 ms per attempted chain) on this snapshot.**
Caveats: (a) both rows serve 0 rows (`fitted_chains` 0; the static per-expiry path refuses every SPY chain:
6 calendar crossings + convexity / slope certificate failures — the graded surface is what serves SPY), so the gap is
pricing work, not a serving comparison; (b) chain-level lattice-vs-AL timings are not like-for-like — DatedCash skips
the implied-forward carry (`quote_slice_carry.cpp:173-176`, research-cash-dividend-report.md §2.6); (c) graded
admission + lattice routing is lane L3f's. The bar stays on funding-pv (PM decision 1); the CSV of both rows is
`333ae87c` (identical: no lattice chain reached the output).

### 2.8 Usable / unusable and what Phase 2-3 must change

- Arm A: UNUSABLE (13.7 s). Arm B: UNUSABLE (11.5). Arm B3: UNUSABLE (12.1). Arm C: UNUSABLE (9.1; measurement
  only). Arm D: UNUSABLE (8.5). The best accuracy-preserving arm (D: Fast confirm + Fast grid, every root confirmed)
  is 8.5× the bar; the best measurement arm (C) 9×.
- Where the seconds are now (arm D, @1, 21 % load): confirms on Fast ≈ 2.0 s (28,551 × 69), Screen iterates + edges
  ≈ 1.5 s (72.9k × 21), routed + fallback Reference iterates ≈ 2.2 s (10k + 2.7k × 169), ambiguous edges 0.5 s, grid
  1.7 s, fixed 0.15 s. Phase 2 (the σ-family) replaces the Screen iterates / edges / grid (→ ≈ 0.4 s) but NOT the
  confirms, the routed iterates or the fallbacks: those three (≈ 4.2 s here) are the Phase 3 levers — narrow the
  item-6 routing band to the MEASURED Screen dip (4.3e-6 relative, L2f §4, vs the 4e-4 gate: a ~100× narrower band
  would route < 1 % instead of 20 %), make the second confirm solve rarer (a better seed: the mid's family chord),
  gate with a sharper v_min (the family's exact chord) and address the 781 Search fallbacks (Phase 0 lever 6).
- PM information (2026-10-20): L3r1-dense-certificate will GROW the served set (today 1,410 of 13,168 SPY rows;
  28 of 32 chains refused) — every additional served row costs iterates / confirms / grid prices; the Phase 3
  arithmetic must budget for a served set several times larger. Noted; its policy key `graded.certificate` is
  additive in `quote_bench_policy.hpp` (union merge expected with the keys above).

### 2.9 Tests and fail-on-revert

At the final source (`vol-rel`, `-Jobs 2`, `ATX_VOL_FIT_WORKERS` 2): `atx-vol-pricing-slice
--gtest_filter=AmericanIvPremium.*:AmericanCertification.*` **9 / 9 PASSED**; `atx-vol-quote-slice-tests
--gtest_filter=QuoteSliceVolSpace.*:DeAmIvCache.*:QuoteSurfaceRungMemo.*` **20 / 20 PASSED**; ctest
`atx-vol-quote-benchmark-native` **1 / 1 Passed** (12.1 s; pytest 165 passed — `test_native_graded_options_are_strict`
+ 6 new cases: screen without premium, confirm keys without screen, `confirm sometimes`, `confirm_k 0`,
`confirm_kernel screen`, `grid_kernel screen`). PCH-off hygiene: `configure -Preset vol-hygiene` exit 0, `check` of
the 68 direct `.cpp` includers of the six touched headers exit 0, 102 objects, 0 diagnostics, 195 s. New tests:
`AmericanIvPremium.ScreenSeedsRunTheMeasuredTierAndAreConfirmedOnTheAccurateMap` (`[screen seeds] accepted 551
(confirmed 502, of which 150 needed a second confirm solve; routed 49), fallbacks 121 (= Search's), max accurate-map
Newton distance 1.34e-11`; `[screen edges] 1344 … max rel 0.000861, 9 over the tested bound, 0 over twice the
whole-grid bound`), `…APriceInsideScreensNonMonotoneGateRunsAccurateIteratesNotARefusal` (`r = q candidates 144,
routed 122, not served by Screen as kernel 0`), `…BandGateSkipsTheConfirmOnlyInsideItsStatementAndNeverSkipsAll`
(`skipped 502 / kept 0 at k = 10 (max skipped Newton 1.16e-5, max gate bound 0.0062 vol); never 502 of 551; Fast
confirm 502`), `DeAmIvCache.ScreenBandEdgesAreDecidedWithTheBoundOrRepricedAccurately` (scripted model: 2 ambiguous
edges re-priced, published edges 0.5 / 8.01, 8.0, 0.51), `QuoteSliceVolSpace.ScreenSeededPremiumCacheServesConfirmedVolatilities`
(`413 confirms, 54 ambiguous edges, 36 routed, 50 mid IVs compared, max |dsigma| vs Search 2.13e-7; band gate skipped
323`; Screen + Search and Band + Accurate refused), `QuoteSliceVolSpace.FastCertificationGridMovesServedPricesWithinTheFastBoundOnly`
(`84 served prices compared, 56 differ, max |dP| / max(P, 1) 2.12e-14`).

Fail-on-revert (each: inverse `Edit`, focused rebuild, run, restore by the inverse `Edit`; the restored file's
`git hash-object` = the committed blob):

| # | inversion (file) | failing test | failure (verbatim head) | restored blob |
| --- | --- | --- | --- | --- |
| R1 | `premium_seed_opts`: Screen branch removed (seed = `al_fast_profile`, profile Screen) — `american_iv.cpp` | `ScreenSeeds…` | `american_iv_test.cpp(2159): Expected equality … Which is: 12 / Which is: 7` (`seed_nodes`), × 551 | `b78f1bbf` |
| R2 | routing with the ACCURATE gate `al_non_monotone_gate(K, T, opts)` instead of `seed_opts` | `APriceInsideScreens…` | `[screen routing] … routed to accurate iterates 0`; `EXPECT_TRUE(st.seed_routed)` × 122; `(routed) > (0u)` | `b78f1bbf` |
| R3 | Band never skips (`&& false` after `band_gate_holds`) | `BandGateSkips…` | `american_iv_test.cpp(2345): Expected: (skipped) > (50u), actual: 0 vs 50` (`skipped 0 / kept 502`) | `b78f1bbf` |
| R4 | `gate->confirm_opts` ignored (confirm on `al_opts`) | `BandGateSkips…` | `american_iv_test.cpp(2334): Expected equality … <00> vs <01>` (`confirm_profile` Fast), × 502 | `b78f1bbf` |
| R5 | `kIvPremiumConfirmSolves` 2 → 1 — `american_iv_premium.hpp` | `ScreenSeeds…` | `american_iv_test.cpp(2148): Expected: (accurate_seed.fallback) != (None)` × 150 (`accepted 401, fallbacks 271 (7=150)`) | `937cb68d` |
| R6 | `decided = true` in `band()` — `deam_iv_cache.cpp` | `DeAmIvCache.ScreenBandEdges…` + `ScreenSeededPremiumCache…` | `quote_slice_vol_space_test.cpp(691): same_bits(s.bid_iv, a.bid_iv) … Expected: true`; `(696): (screen.band(0).bid_iv) > (0.0), actual: 0`; `ambiguous 0` | `0077cc87` |
| R7 | `fast_grid … && false` — `quote_slice.cpp` | `FastCertificationGrid…` | `quote_slice_vol_space_test.cpp(487): Expected: (differing) > (0u), actual: 0 vs 0` (`84 compared, 0 differ`) | `6b674fb3` |
| R8 | the loader's "screen requires premium" throw removed — `quote_bench_policy.hpp` | `test_native_graded_options_are_strict` | `test_quote_bench_cli.py:764: assert 'deam_profile screen requires deam_inversion premium' in 'quote-bench: InvalidArgument: quote fit policy: invalid channel configuration'` (the library's `validate_channel` still refuses; 1 failed, 164 passed) | `250b04f2` |
| R9 | `SliceDeAmModel::seed_price_bound` Screen margin × 0.0 — `quote_slice.cpp` | `ScreenSeededPremiumCache…` | `quote_slice_vol_space_test.cpp(429): Expected: (ambiguous) >= (50u), actual: 34 vs 50` | `6b674fb3` |

R3's first form (the Band clause deleted outright) did not build: `-Werror,-Wunused-parameter 'last' / 'partner'`,
`-Wunused-variable 'band_gate_holds'` — the second form was used. NOT pinned by a failing test (gaps for review): the
`deam_al_by_reason` counters and the new `--timing-detail` JSON fields (observability, evidenced by the SPY
reports); the Screen edge margin's VALUE is pinned only by the data threshold (54 edges re-priced vs 34 at a zero
margin) and by the american_iv_test edge statistic; the "Screen refuses" half of item 6 (no r = q candidate was
refused by Screen as kernel: all 144 Ok, ≤ 1.26e-6 vol off).

### 2.10 Hunks (final SHA vs `92d4ba0d`; `git diff -U0`; L2h is kernel-only and touches none of these files)

| file | hunks (`-old +new`) |
| --- | --- |
| `atx-vol/include/atx/vol/api/fitting/quote_slice.hpp` | -111 +111,12; -119 +130; -121,0 +133,13; -180,0 +205,18; -266,0 +309; -380,0 +424,10 |
| `atx-vol/include/atx/vol/api/pricing/american_iv.hpp` | -185,0 +186,7 |
| `atx-vol/src/pricing/american_iv_premium.hpp` | -47,0 +48,4; -63,0 +68,65; -70 +139,3; -74 +145,2 |
| `atx-vol/src/pricing/american_iv.cpp` | -1136,0 +1137,2; -1140,2 +1142,2; -1146,0 +1149,42; -1148,2 +1192,4; -1154,5 +1200,7; -1160,8 +1208,31; -1187,2 +1258,2; -1190,0 +1262,12; -1312 +1395; -1337 +1420,3; -1342,0 +1428,3; -1349 +1437,2; -1352 +1441,2; -1360 +1450; -1378 +1468; -1388 +1478,2; -1390 +1481 |
| `atx-vol/src/fitting/counters.hpp` | -1199,0 +1200,16; -1201,0 +1218,12 |
| `atx-vol/src/fitting/deam_iv_cache.hpp` | -88,2 +88,16; -113 +127,2; -114,0 +130; -119,0 +136,10; -149,0 +176,9; -152 +187; -154 +189,2; -157 +193,2; -180,0 +218,13; -186,0 +237 |
| `atx-vol/src/fitting/deam_iv_cache.cpp` | -65 +65,2; -67 +68; -70 +71; -74,2 +75,15; -77,0 +92,9; -107,0 +131; -109,0 +134,13; -120,0 +158,7; -147,0 +192,6; -150,0 +201; -170 +221,12; -172,0 +235,3; -173,0 +239; -176,0 +243,27; -232,0 +326,3; -234,0 +331; -240,3 +337,25; -249,11 +368,8; -265 +381; -269 +385 |
| `atx-vol/src/fitting/quote_slice.cpp` | -48 +48,3; -663 +665,2; -667 +670,2; -678,0 +683,15; -685 +704,2; -686,0 +707,2; -689 +711; -699 +721,2; -702 +725,2; -710,0 +735,9; -713 +746; -728,0 +762; -753 +787,3; -1239,0 +1276,2; -1242 +1280,2; -1261,0 +1301,15; -1264,0 +1319; -1279,2 +1334,2; -1398 +1453,2; -1402 +1458,14; -1667 +1736; -1742 +1811,2 |
| `atx-vol/tools/quote_bench_policy.hpp` | -537,0 +538,5; -558,0 +564,2; -560 +567; -582 +589; -584 +591,51; -615,2 +672,3; -620,0 +679,8; -621,0 +688; -777 +844,2; -846,0 +915,4 |
| `atx-vol/tools/quote_bench_main.cpp` | -567,0 +568,4; -590 +594,9 |
| `atx-vol/tests/american_iv_test.cpp` | -2069,0 +2070,279 |
| `atx-vol/tests/quote_slice_vol_space_test.cpp` | -356,0 +357,136; -368,0 +505,5; -371,0 +513; -377 +519,2; -379 +522,10; -510,0 +663,52 |
| `atx-vol/python/tests/test_quote_bench_cli.py` | -719 +719; -738 +738,23 |
| `atx-vol/sprints/2026-10-20-SPY1s-owner-sprint.md` | -151,0 +152,… (this section) |

Untouched: `american.hpp`, `american.cpp`, `american_sigma_family.cpp` (fingerprint `53c4df68` MATCH), `quote_surface.*`,
`quote_slice_carry.cpp`, every default path (parity above).

## 3. Phase 2 — the σ-family in the fit (the L2i spec on L2h `6e94ba40`): CHECKPOINT at the user stop (2026-10-20 ~21:45 EDT)

Base: `85476626` (Phase 1) merged with the APPROVED L2h kernel `6e94ba40` as `7980a6d3` (`--no-ff`, 6 files, +1,962, no
conflicts; `american.hpp`, `american_sigma_family.cpp`, `american_geometry_test.cpp`, the bench, the L2h note, the
pricing CMake list). Pool-33, RunId `vol-20261020r3-SPY1s-p2`. The lane branch `feat/vol-spy1s-20261020` was checked
out in pool-4 by `review-SPY1s-p1`, so the lease's `git switch` failed and this work sits on the branch
**`feat/vol-spy1s-20261020-p2`** at the same tip (fast-forward of the lane branch by one merge commit; the PM or the
owner fast-forwards `feat/vol-spy1s-20261020` once pool-4 releases it). L3g (`price_and_vega`) had no `## Outcome` and
no review at the start: NOT merged; the Newton slope stays `price_and_chord` (L2h §7 item 1). No source change, no
build, no run was made before the stop (nothing under `C:\atx-cache\quote-benchmark\` written; no evidence folder).

### 3.1 The binding first item — the per-strike effective yield, MEASURED on SPY (every expiry)

Input `fit-timing-20261017-cohort5\input\quotes-SPY.csv` (`b3fafd80…`; 13,168 rows, 12,866 `ok`, 33 chains). The
bench reads each row's `sdiv` into `quote.continuous_yield` (`quote_bench_main.cpp:328`), `build_carry_curve` makes it
the per-strike carry, `effective_yield(fit, calendar_yield_at(fit, K))` the per-strike q of every price (ddiv_pv = 0 on
SPY, so q_eff = sdiv on the variance clock); the chain's representative q is the median sdiv. Per expiry (rows with
`input_status ok`; `spread` = max − min sdiv over the chain's two-sided rows; `dP` = the first-order price move
T·S·½·Δq of a representative-q family versus the strike's own q; allowance = 1e-8 + 1e-9·max(S, K) ≈ 7.7e-7 USD):

| expiry_days | rows | T (y) | distinct sdiv | sdiv min … max | spread Δq | dP ≈ T·S·½·Δq (USD) | dP / allowance |
| --- | ---: | ---: | ---: | --- | ---: | ---: | ---: |
| 20741 | 302 | 0.0059 | 1 | 0.017441 | 0 | 0 | 0 (T < kAlSigmaFamilyMinTau: cold anyway) |
| 20742 | 474 | 0.0094 | 20 | −0.008997 … −0.007177 | 1.82e-3 | 6.6e-3 | 8.5e3 |
| 20745 | 302 | 0.0150 | 23 | 0.002571 … 0.002611 | 4.0e-5 | 2.3e-4 | 3.0e2 |
| 20746 | 292 | 0.0185 | 37 | 0.004053 … 0.004110 | 5.7e-5 | 4.1e-4 | 5.3e2 |
| 20747 | 302 | 0.0220 | 29 | 0.005314 … 0.005332 | 1.8e-5 | 1.5e-4 | 2.0e2 |
| 20748 | 292 | 0.0255 | 38 | 0.006665 … 0.006688 | 2.3e-5 | 2.2e-4 | 2.9e2 |
| 20749 | 342 | 0.0290 | 28 | 0.001539 … 0.001546 | 7.8e-6 | 8.8e-5 | 1.1e2 |
| 20752 | 298 | 0.0346 | 37 | 0.004396 … 0.004406 | 1.0e-5 | 1.3e-4 | 1.7e2 |
| 20753 | 298 | 0.0381 | 39 | 0.004895 … 0.004905 | 9.6e-6 | 1.4e-4 | 1.8e2 |
| 20754 | 60 | 0.0416 | 4 | 0.003259 … 0.003846 | 5.9e-4 | 9.4e-3 | 1.2e4 |
| 20756 | 498 | 0.0486 | 40 | 0.002019 … 0.002052 | 3.3e-5 | 6.2e-4 | 8.0e2 |
| 20763 | 334 | 0.0682 | 74 | 0.001589 … 0.001678 | 9.0e-5 | 2.4e-3 | 3.1e3 |
| 20770 | 334 | 0.0878 | 85 | 0.001443 … 0.001471 | 2.9e-5 | 9.7e-4 | 1.3e3 |
| 20777 | 442 | 0.1074 | 89 | 0.001316 … 0.001324 | 7.9e-6 | 3.3e-4 | 4.3e2 |
| 20784 | 302 | 0.1232 | 81 | 0.000381 … 0.000393 | 1.2e-5 | 5.9e-4 | 7.6e2 |
| 20787 | 564 | 0.1288 | 106 | 0.001125 … 0.001132 | 7.4e-6 | 3.7e-4 | 4.8e2 |
| **20805** | 516 | 0.1820 | **1** | 0.002046 | 0 | 0 | 0 — the ONE expiry a representative-q family serves exactly |
| 20818 | 650 | 0.2139 | 144 | 0.002673 … 0.002693 | 2.1e-5 | 1.7e-3 | 2.2e3 |
| 20833 | 478 | 0.2541 | 22 | −0.000263 … 0.000918 | 1.18e-3 | 0.116 | 1.5e5 |
| 20847 | 360 | 0.2908 | 124 | −0.000757 … −0.000348 | 4.1e-4 | 0.046 | 6.0e4 |
| 20868 | 222 | 0.3472 | 28 | −0.000924 … −0.000922 | 1.6e-6 | 2.1e-4 | 2.7e2 |
| 20875 | 294 | 0.3667 | 100 | −0.001024 … −0.001020 | 3.1e-6 | 4.4e-4 | 5.8e2 |
| 20896 | 398 | 0.4255 | 28 | −0.001269 … −0.000827 | 4.4e-4 | 0.073 | 9.4e4 |
| 20908 | 388 | 0.4552 | 43 | −0.000373 … −0.000354 | 1.9e-5 | 3.3e-3 | 4.3e3 |
| 20986 | 486 | 0.6717 | 29 | −0.001792 … −0.001441 | 3.5e-4 | 0.091 | 1.2e5 |
| 20999 | 368 | 0.7049 | 142 | −0.001145 … −0.000317 | 8.3e-4 | 0.226 | 2.9e5 |
| 21078 | 496 | 0.9225 | 36 | −0.001940 … −0.001063 | 8.8e-4 | 0.313 | 4.0e5 |
| 21091 | 370 | 0.9582 | 117 | −0.001384 … −0.001335 | 4.9e-5 | 0.018 | 2.3e4 |
| 21169 | 506 | 1.1734 | 42 | −0.001826 … −0.000656 | 1.17e-3 | 0.531 | 6.9e5 |
| 21204 | 506 | 1.2664 | 54 | −0.001998 … −0.000841 | 1.16e-3 | 0.566 | 7.3e5 |
| 21351 | 510 | 1.6704 | 64 | −0.002616 … −0.001932 | 6.8e-4 | 0.442 | 5.7e5 |
| 21533 | 456 | 2.1673 | 74 | −0.003298 … −0.002272 | 1.03e-3 | 0.860 | 1.1e6 |
| 21568 | 426 | 2.2579 | 52 | −0.002165 … −0.002057 | 1.1e-4 | 0.095 | 1.2e5 |

Total distinct (expiry, sdiv) pairs: **1,831** → 3,662 families if keyed per distinct q (× 1.24 ms = 4.5 s of builds for
a 8.5 s surface: dead). The induced price error of ONE family per (expiry, side) at the representative q is 2e-4 to
0.86 USD per strike on 30 of 33 expiries — 2 to 6 orders of magnitude above the family bound (SPY-like 3.7e-12
relative = 2e-10 USD) AND above the certificate allowance; in vol units ΔP / vega ≈ 1e-6 … 3e-3, so even as a SEED
the mismatch costs the second bounded confirm solve (69 µs Fast) on most strikes, more than the iterates save
(1.77 × (21 − 9.3) ≈ 20 µs). q-buckets (b) under the allowance need Δq < 3e-7 (shortest) … 1e-9 (longest expiry),
i.e. buckets = the distinct values = the 3,662 families above. (The first-order estimate T·S·½·Δq is the order of
magnitude; the kernel measurement |P(σ; q_rep) − P(σ; q_eff)| at the chain's ATM node was planned as two extra cold
solves per family build under the key and was NOT run before the stop.)

**Decision state (recorded, not executed):** (a) one representative-q family — REJECTED by the table for every price
that is SERVED or DECIDES a band (the grid and the kIvMin / kIvMax edges); exact only on expiry 20805 (and 20741,
below kAlSigmaFamilyMinTau). (b) q-buckets — REJECTED (buckets = distinct values; the adaptive rule N > 72 refuses
~every group: ~160 strikes / 74 q per expiry ≈ 2 strikes per family). (c) q as a second interpolation axis
(σ, q) — a kernel change in `american_sigma_family.cpp` (the boundary y[] AND the premium quadrature's exp(q u)
weights depend on q): the arithmetic that makes it the real lever is 64 × (12 σ × 3-4 q) node solves ≈ 0.25-0.3 s of
Reference builds per SPY surface against iterates + edges + grid ≈ 3.3 s on Screen / Fast today → ≈ 0.8 s; **this
is the PM's call (hand back and ask first, per the L2i brief)**. The kernel-free design I had chosen for the ITERATES
before the stop (not built): one family per (expiry, side) at the representative (r, q) supplies only the
de-Americanisation REMAINDER D(σ) = P_fam(σ; q_rep) − A_BAW(σ; q_rep) and its chord, while the Premium leg A runs at
the contract's own q_eff (the leg carries the q-dependence exactly to first order; D's q-sensitivity is ~1 % of P's);
every root confirmed on the confirm kernel at q_eff as today; the grid on the family ONLY where the strike's q equals
the family's q bit for bit (SPY: one expiry); the band edges stay cold (L2h §7 item 3: kIvMin / kIvMax are outside
any chain box). Expected saving on SPY ≈ 0.6 s (iterates 50k × (21 → 9.3 µs)) + 0.08 s of builds — the family
lever is worth ≈ 0.6 s of arm D's 8.55 s under per-strike q, not the 2.4 s of the Phase 0 plan (§1 row 5), because
the grid (1.74 s) and the edges (0.47 s) cannot be family-priced at a q they were not built at.

### 3.2 The other Phase 2 lever (PM ruling, Phase 1 results item 3), planned, not built

Opt-in key `graded.seed_gate_kappa` (policy → `QuoteSliceChannelConfig`, requires `deam_profile screen`): the Screen
routing test `price − zero_vol ≤ al_non_monotone_gate(K, T, seed_opts)` (kappa 4e-4) becomes `≤ kappa_key · K ·
max(1, T²)` with kappa_key = 10 × the measured Screen dip (Phase 1: 4.3e-6 → 4.3e-5). Expected: the 5,849 routed
inversions (19.7 %, ≈ 10k Reference iterates ≈ 1.6 s) run on Screen (+5.8k Fast confirms ≈ 0.4 s) → ≈ −1.0 s on arm
D. Accuracy by construction under `confirm always`: a mis-seeded root fails the confirm and falls back to Search
bit for bit. Test: a price inside the wide gate but above kappa_key·K·max(1, T²) is routed without the key and not
with it (fails on revert).

### 3.3 Remaining steps, in order (none started)

1. PM ruling on §3.1 (c): a (σ, q) family in the kernel (L2h / L2h-fix1 own `american_sigma_family.cpp`) or the
   kernel-free D-model iterates only. 2. `seed_gate_kappa` key + test + the SPY arm-D-plus-gate @1 row. 3. Family
   provisioning per (expiry, side) in the rung-memo slot (`rung_memo.hpp` holder: geometry + put + call families,
   `shared_ptr<const>`, built once at `VolSpaceCache` construction when 5.4 × two-sided quotes per side > c·2·n_σ,
   c = 3; `always` forces it; box from the Black-76 IVs of the two-sided mids, lo = max(kMin, 0.5·min), hi = 1.5·max
   + 2·kAlPackSigmaBump), keyed into `deam_iv_key` (family mode, c, n_σ, axis words) + the
   `MemoKeyCoversEveryInputThatChangesAnInversion` extension. 4. `premium_root`: the family D-model iterate
   (`IvPremiumGate::family` pointer + the family's q and leg; every refusal → the Screen iterate), counters
   `family_build` / `family_price` / `family_refused` in the §6.3 reason split + `--timing-detail`. 5. The grid on the
   family where q_eff == q_rep and the family bound < allowance, else cold (test fails on revert). 6. The band gate
   with the family's bound (B-family arm; eps_A = family bound + a per-family measured q-term, measurement arm).
   7. Arms on SPY @1 (D-family adaptive, D-family-always, B-family; @0 for D-family), load across the run; the
   two-lever table; served-set diff; cohort5 + a 40-symbol dev200 sample during development; the three cohorts once at
   the end; parity `c89d0739` / `4b273b08` / `171fa2d2`; fingerprint `53c4df68`; PCH-off; note; report; release.

## 4. review-SPY1s-p1 M1 fixed (lane SPY1s-M1, 2026-10-21; one commit on `d2402dc5`)

What: `deam_iv_key` (`src/fitting/deam_iv_cache.cpp`) now words `deam_confirm`, `deam_confirm_k` (bit-cast) and
`deam_confirm_kernel` after `deam_inversion`; the fixed-word reserve is the exact count 29 (was 28 for 26 words).
Why: both memos (the per-thread `deam_iv_memo`, the rung memo's `deam_cache`) reuse a `DeAmIvCache` by this key, so a
cache filled under `confirm band` / `never` (unconfirmed seed roots, up to 1.12e-5 / 2.52e-4 off the accurate map) was
served bit for bit to a later `confirm always` fit of the same chain on the same thread, and vice versa — against the
key's contract "equal keys compute bit-identical caches". Not reachable in quote-bench (one policy per process), so
no recorded hash moves; reachable by any in-process A/B and any library caller. Same commit, same class (review minor):
`certification_grid_key` reserves the one word `certify_and_serve` appends (the grid-kernel word), so a memo-on
certify pass no longer reallocates the key. The grid key carries the three new words at their `QuoteSliceConfig{}`
defaults (constant within and across runs: no hit / miss changes).

Tests (`tests/quote_slice_vol_space_test.cpp`): `DeAmIvCache.MemoKeyCoversEveryInputThatChangesAnInversion` gains four
flips (confirm band, confirm never, confirm k, confirm kernel); `QuoteSliceVolSpace.ScreenSeededPremiumCacheServes
ConfirmedVolatilities` loses the masking `deam_iv_memo_clear()` between the Always and the Band fit (`:408` at
`d2402dc5`; the Band fit now runs on the Always fit's memo and must miss: `reused` false, memo size 2, band skipped
323 as before) and gains the dangerous direction: a `confirm never` fit fills the memo first, then a `confirm always`
fit of the same chain must miss, confirm (> 0 confirms, 0 skipped) and equal a fresh-memo Always fit bit for bit.
Fail-on-revert: the source reverted to the committed blob `0077cc87` → both tests FAIL (the four key flips; Band
`reused`, memo size 1 vs 2, band skipped 0 vs > 0; Always-after-Never reused, 0 confirms, 162 bit mismatches);
restored (blob `e5e05b4f`) → 20 / 20 PASS with the Phase 1 printouts unchanged. The other Phase 1 suites at the fix:
`AmericanIvPremium.*:AmericanCertification.*` 9 / 9 (printouts = Phase 1), `atx-vol-chain-batch-tests` 31 / 31, ctest
`atx-vol-quote-benchmark-native` 1 / 1.

Re-proof on the fix binary `eab01fd5…` (2,519,040 B; ATX_VOL_FIT_WORKERS 2, SPY @1): dev200 default `c89d0739`
MATCH; SPY arm D @1 `feac8d2c` MATCH (= the Phase 1 lane's and the review's; 7.96 s fit_and_reprice, load 2-13 %
sampled before / after, unqualified); cohort5 arm D `45e491ea` MATCH (10 s). Evidence
`C:\atx-cache\quote-benchmark\SPY1s-20261021-m1\` (`runs.log`, `gates\`, `sha256.txt`). No header touched (PCH-off
not applicable); no Phase 2 lever started.
