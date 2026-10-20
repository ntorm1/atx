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
