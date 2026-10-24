# L3f cash-dividend kernel sprint note (vol-sota W3, 2026-10-20)

Lane `feat/vol-L3f-cash-dividend-kernel-20261020`, based on the W2b root
`feat/vol-resume4-20261016 @ be379ad4`. Pool-5, RunId `vol-20261020r3-L3f`. Brief
`.superpowers/sdd/2026-10-18-vol-sota/lane-L3f-cash-dividend-kernel-brief.md`; design
`research-cash-dividend-report.md` (D1). PM rulings in force: the accurate root for dividend
rows is the CONVERGED Bbs lattice (column 1 of every table) with the vendor-reproducing 301
`SplicedCrr` default beside it (column 2); the default serve path is unchanged; promotion is an
integration ruling. User directives: fastest AND most accurate; order of magnitude first; no run
above ~10 min (every command here ran in seconds).

Everything here is OPT-IN by construction: no default path selects the workspace, the tier or
the inversion. Proof at the final tree: dev200 default `c89d0739` MATCH, the opt-in lattice
hashes `abc75955` (dev200) / `747d3e6c` (dev200b) MATCH, AL fingerprint `53c4df68` MATCH,
`atx-vol-discrete-div-tests` 88/88, `-smoothing-tests` 28/28, `-dividend-schedule-tests` 21/21,
chain-batch 31/31. All timings are UNQUALIFIED (shared host, other lanes building; the load
line is beside each table). Evidence: `C:\atx-cache\quote-benchmark\L3f-cash-dividend-kernel-20261020-{inputs,us,gates,parity}\`
(each with `sha256.txt`).

## 1. What landed

- `api/pricing/american_discrete_div_kernel.hpp` (Tier-B, outside the umbrella) +
  `src/pricing/american_discrete_div_kernel.cpp`:
  - `DiscreteDivWorkspace`: move-only owner of the four lattice rows, reserved once for
    `max_steps` (`reserved_bytes` = 4 x (max_steps + 2) x 8 B: 9,696 B at 301, 76,896 B at the
    2,401-step reference). The `american_discrete_div_price(..., DiscreteDivWorkspace &)`
    overload is bit-identical to the allocating entry on every scheme over a 14,400-case grid
    (incl. ex-date on expiry, first-step ex-date, cash exhausting the low nodes, two events
    7 days apart, nine events, empty schedule), allocation-free after construction, and gives
    the same bits from 4 threads with their own workspaces as the serial loop. Oversized
    `steps` are REFUSED (InvalidArgument), never grown.
  - `american_discrete_div_converged_price`: the PM's reference — SplicedBbs at 2,401 steps,
    two-point Richardson against 1,201, floored at intrinsic; returns fine / coarse too so the
    reference's own uncertainty (fine - coarse) is reported with every table.
  - `DiscreteDivTier {Accurate, Screen}`, `discrete_div_screen_steps` (typed OutOfRange when an
    ex-date lands before step 2 of the tier's coarsest lattice — LEDGER L522 — or when there is
    nothing to screen with), `american_discrete_div_screen_price`,
    `kDiscreteDivScreenSteps = 101`, `kDiscreteDivScreenScheme = SplicedCrr` (§3),
    `discrete_div_screen_equivalent` (the cost charged per Screen call).
  - `american_discrete_div_implied_vol(price, ..., steps, scheme, DiscreteDivIvOpts{seed, tol,
    max_iter, tier, screen_steps}, workspace) -> Result<DiscreteDivIvResult{sigma,
    screen_lattices, accurate_lattices, fallback}>`: seeded safeguarded secant (Illinois inside
    a bracket, capped extrapolation outside, one re-chord retry across a CRR ripple) on the
    Screen tier to 100 x tol, ONE accurate confirm on the caller's (steps, scheme), then up to
    3 Newton polish steps on the accurate lattice accepted only when they REDUCE the accurate
    residual (L2e's pattern), acceptance = Newton distance <= tol. Every fallback
    (`DiscreteDivIvFallback`: NotApplicable / RangeEdge / NotIncreasing / VolBound /
    PricerFailure / NoConvergence) hands the root to `dated_cash_iv`'s algorithm on the
    accurate kernel, seeded with the accurate points already paid for — the answer is always
    the accurate lattice's root; a fallback is a cost record. `dated_cash_iv`'s safeguards are
    kept verbatim: CRR-window lower endpoint (the coarsest lattice of each tier), quote at or
    below the lower lattice price / above the maximum-volatility price = OutOfRange, the
    iteration budget = `Unavailable`.
- `src/pricing/american_discrete_div_detail.hpp`: the internal seam (`atx::vol::ddiv_detail`:
  `LatticeBuffers`, `LatticeOutcome`, `validate`, `run_lattice`, `run_escrowed_lattice`,
  `run_single_rollback`, `richardson_price` — the last now takes the caller's buffers). In
  `american_discrete_div.cpp` only the declarations moved out of the anonymous namespace and
  `richardson_price` reuses the public entry's own `LatticeBuffers`; no arithmetic changed.
- Tests `atx-vol-discrete-div-kernel-tests` (17; ctest `atx-vol-discrete-div-kernel`), bench
  `atx-vol-discrete-div-bench` (+ name-coverage ctest), one CMake source-list line.

## 2. Item 0 — the lattice's release cost per price (the number the W3 plan is sized on)

`american/discrete_div/price/<scheme>/<steps>` on the SPY dividend ladder (cohort5
`quotes-SPY.csv` sha256 `b3fafd80…`: spot 772.98, the 17 dividend expiries' events inferred by
`infer_dividend_schedule` from the 34-expiry (years, rate, ddiv, ddiv_pv) ladder — status
`ambiguous`, 8 events, unit 1.8311 — mapped by `dividend_schedule_expiry_events`; 20 strikes in
log-moneyness [-0.30, 0.20] around the dated-cash forward, both sides, sigma in {0.12, 0.20,
0.35}: 2,040 contracts). `vol-rel-avx2`, allocating entry, one run, per-price p50 / p99 us
(`ddiv-spy-run4.json`, load 23 % before / 31 % after; the same rows in run2 / run3 at load
72 → 53 % / 35 → 48 % read 2.3x / 1.9x higher at 301 — the host, not the kernel):

| scheme \ steps | 101 | 151 | 301 | 601 | 1201 | 2401 | 301 on a workspace |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| spliced (default) | 6.5 / 12.6 | 10.7 / 17.9 | **24.8 / 43.5** | 78.4 / 117 | 288 / 380 | 1,196 / 2,010 | 29.2 / 46.3 |
| spliced-bbs | 15.9 / 26.5 | 26.3 / 40.7 | 71.4 / 134 | 173 / 215 | 461 / 540 | 1,577 / 2,787 | 70.2 / 130 |
| spliced-bbsr | 24.6 / 39.1 | 37.2 / 55.6 | 88.7 / 125 | 215 / 279 | 596 / 723 | 1,708 / 2,010 | 73.3 / 113 |
| escrowed-lr | 8.5 / 9.9 | 16.9 / 20.9 | 62.9 / 69.6 | 219 / 232 | 839 / 886 | 3,096 / 3,320 | 55.4 / 63.1 |

Fitted exponent in `steps` (spliced, p50): 1.86 over 301 → 2,401, 1.23 over 101 → 301 (the
small lattices are fixed-cost bound: 302 `exp` for the terminal levels, the step map, four
`assign`/`resize`). Steps >= 601 time every 4th contract, >= 1,201 every 8th. The workspace
row moved -14 / -4 / +4 us against the allocating row over runs 2-4: the allocation's share of
a 25-60 us price is inside this host's noise and is not claimed as a saving here; what the
workspace buys is the guarantee (0 allocations, measured) and the HFT shape.
dev200 payer slice (40 lattice-routed chains, 1,382 contracts, `ddiv-dev200-run1.json`, load
42 → 40 %): spliced 301 34.3 / 64.4 us, workspace 31.3 / 55.9, Screen 5.4 / 13.6.

So: **one 301-step rollback costs 25-60 us in release on this host** (the research estimate
was 30-120 us; LEDGER L406's /Od ratio 3.33x an AL price would have put it near 190 us against
the L2f Reference solve — it is the class of a Fast AL solve, not a Reference one). The
BBS final step adds a FIXED 15-25 us (Black-76 at every step-(N-1) node, two per node when cash
lands on expiry — SPY's normal case), which is what rules the BBS schemes out as a cheap tier.

## 3. The Screen tier: scan and choice

`american/discrete_div/screen_scan/spy` (vs the converged lattice; p50 us per price in the same
run; relative on P >= 0.01; IV bp on vega-bearing contracts, vega >= 1e-2 max(P, 1) from a
SplicedBbs-301 chord at sigma +/- 1e-3; `r` = BBS + Richardson, `p` = plain BBS, `c` = plain
spliced CRR):

| candidate | p50 us | max abs USD | p99 abs | max rel | p99 rel | IV bp p99 (max) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 101r (the brief's expected shape) | 19.8 | 0.379 | 0.279 | 3.6e-2 | 2.3e-2 | 41.0 (904) |
| 101p | 13.2 | 0.587 | 0.553 | 8.5e-2 | 5.1e-2 | 15.9 (107) |
| **101c (chosen)** | **5.4** | 0.761 | 0.696 | 1.03e-1 | 5.1e-2 | 18.9 (106) |
| 151r | 31.5 | 0.241 | 0.203 | 3.0e-2 | 1.8e-2 | 18.5 (272) |
| 151p | 22.3 | 0.455 | 0.430 | 5.7e-2 | 3.8e-2 | 11.9 (163) |
| 151c | 9.8 | 0.585 | 0.520 | 6.1e-2 | 3.9e-2 | 14.3 (164) |
| 201r | 47.9 | 0.253 | 0.222 | 2.5e-2 | 1.5e-2 | 22.0 (219) |
| 201p | 34.0 | 0.384 | 0.357 | 4.4e-2 | 3.1e-2 | 13.9 (87) |
| 201c | 15.1 | 0.476 | 0.426 | 4.9e-2 | 3.1e-2 | 14.0 (88) |

The IV "max" column is carried by near-zero-vega contracts that pass the weak vega filter; p99
is the number. Every `r`/`p` candidate costs 0.5-1.9x the 301 rollback it would screen for
(§2) — a BBS tier cannot reach the brief's 0.1-0.3 cost target at N <= 201. The plain spliced
CRR at 101 steps costs 0.18-0.22 of the accurate rollback (0.113 in node count,
`discrete_div_screen_equivalent`) and its root sits a ripple's amplitude (~1e-4 vol) from the
accurate root, which the accurate Newton polish removes in the same two steps the BBS tier
needed (the inversion's `accurate` count was 3 at p50 under 151r too). Decision: Screen =
SplicedCrr at 101 (header text on `kDiscreteDivScreenSteps` carries the reasoning). The
measured inversion is the proof (§5): under 151r it cost 596 us per root — SLOWER than the
twin's 527 at the same load — under 101c 129 us vs 385.

Pinned (`DiscreteDivScreenTier.*`, 2-figure round-ups of the test's own loops on a 240-contract
SPY-shaped ladder and the 10 schedule corners; fail-on-revert in the lane report): Screen vs
converged max rel <= 6.5e-2 (measured 6.4282e-2), IV <= 21 bp (20.381), corners max rel
<= 2.8e-2 (2.7390e-2); the 301 default vs converged on the same ladder <= 2.8e-2 (2.7073e-2;
IV 9.08 bp max). Refusals: the one-day-ahead ex-date on a three-month chain (coarse step 0.8)
is refused and priced on the accurate tier; 0 of 2,040 SPY contracts refused (SPY's ex-dates
sit on expiries); 80 of 1,382 dev200 payer contracts refused at 101 (ex-dates inside the first
coarse steps of long chains).

## 4. Error tables vs the converged lattice (column 1) and vs the 301 default (column 2)

`american/discrete_div/error/spy` (2,040 contracts; converged reference uncertainty, fine -
coarse, max 5.3e-2 USD on the T = 2.26 y / 8-event expiries). Each cell: max abs USD / p99 abs
/ max rel (P >= 0.01) / p99 rel / IV bp p99 (max) on 1,851 vega-bearing contracts.

| scheme at 301 (Screen at 101) | vs converged | vs spliced301 |
| --- | --- | --- |
| spliced301 (default) | 0.357 / 0.321 / 3.9e-2 / 2.3e-2 / **9.0 bp (80)** | 0 |
| bbs301 | 0.294 / 0.275 / 3.5e-2 / 2.4e-2 / 8.8 (81) | 0.147 / 0.077 / 1.0e-2 / 5.1e-3 / 3.1 (3.9) |
| bbsr301 | 0.155 / 0.139 / 1.8e-2 / 1.1e-2 / 20.0 (350) | 0.238 / 0.201 / 3.3e-2 / 1.5e-2 / 11.3 (270) |
| escrowed-lr301 (a different MODEL: escrowed vol) | 1.553 / 1.464 / 5.9e-2 / 4.1e-2 / 36 (72) | 1.890 / 1.747 / 8.6e-2 / 6.2e-2 / 43 (46) |
| Screen (101 spliced) | 0.761 / 0.696 / 1.03e-1 / 5.1e-2 / 18.9 (106) | 0.628 / 0.422 / 8.3e-2 / 3.4e-2 / 13.4 (80) |

Worst contracts are all T = 2.17-2.26 y, sigma 0.35, 8 events (K 805-871). Reading for the PM
(ruling a): **the vendor-reproducing 301 default sits 9 bp (p99) / 0.32 USD (p99) from the
converged lattice on long-dated SPY** — its own truncation is of the same order as the whole
SpiderRock agreement claim (6.96 ticks); BBSR at 301 halves the price distance but its IV
distance is worse (the Richardson ripple, L247). The escrowed-LR row is NOT an error: at the
same sigma it prices a different model (escrowed-stock vol), as its header says.
dev200 payer slice (1,382; `error/dev200`): the max column is dominated by ONE contract whose
converged reference is itself invalid (uncertainty 2.6 USD: a long chain with an ex-date inside
the first steps of the 1,201 lattice, L522 on the reference side — the reference needs the same
first-step guard; not added in this lane); p99 columns: spliced301 0.53 USD / 3.2e-2 / 25.0 bp,
bbs301 0.49 / 3.1e-2 / 25.2, bbsr301 0.26 / 4.6e-2 / 62, escrowed-lr 1.60 / 0.20 / 154,
Screen 1.03 / 5.7e-2 / 44 (n 1,302 after 80 refusals).
Corners (test `BoundOnTheScheduleCorners`, S = 100): cash exhausting the low nodes (D 60 at
0.5 y) Screen 56.73834 vs converged 56.73835 (put) / 9.5121 vs 9.5122 (call); ex-date on expiry
(K 95, 0.5 y) 3.29279 vs 3.29263 / 9.71558 vs 9.71567; ITM call one day before an ex-date
(K 80, D 3 at 0.25 y) 21.1820 vs 21.1836; two ex-dates 7 days apart (1 y) 7.1460 vs 7.1286 /
9.0718 vs 9.0561; T = 2.26 y nine events 11.318 vs 11.194 (put) / 12.025 vs 11.918 (call) — the
worst corner (2.7e-2 rel; the 301 default is 1.4e-2 / 28 bp there); the deep-ITM put with an
ex-date one day ahead is refused by the Screen (coarse step 0.8) and priced accurately.

## 5. The inversion: lattices per root before → after (item 3 / item 6)

Quote = the 301 SplicedCrr price at the contract's sigma; seed = 1.05 x sigma (the funding-pv
IV's class) or none; vega-bearing contracts only (1,850 SPY, 1,190 dev200). `vol-rel-avx2`, one
run (`ddiv-spy-run4`, load 23 → 31 %; `ddiv-dev200-run1`, 42 → 40 %):

| set, row | us per root p50 / p99 | Screen calls p50 / p99 | accurate calls p50 / p99 / max | accurate-lattice equivalents p50 / p99 / max | fallbacks |
| --- | ---: | ---: | ---: | ---: | --- |
| SPY secant twin (`dated_cash_iv` verbatim, 301) | **385 / 671** | — | 13 / 18 / 24 (mean 12.79) | 12.79 mean | 2 refusals |
| SPY seeded | **129 / 234** | 6 / 10 | 3 / 4 / 12 | **3.68 / 4.79 / 12.9** | none 1,844, no_convergence 6; 1 refusal |
| SPY unseeded (Black-76 seed, else bracket) | 120 / 228 | 6 / 10 | 3 / 4 / 23 | 3.68 / 5.13 / 24.1 | none 1,842, no_convergence 7; 2 refusals |
| dev200 secant twin | 449 / 841 | — | 12 / 16 / 27 (mean 12.67) | 12.67 mean | 1 refusal |
| dev200 seeded | 146 / 710 | 6 / 9 | 3 / 16 / 21 | 3.68 / 16.0 / 22.1 | none 1,122, not_applicable 56 (Screen refused), not_increasing 1, no_convergence 11 |
| dev200 unseeded | 147 / 673 | 6 / 11 | 3 / 16 / 27 | 3.68 / 16.0 / 29.0 | none 1,124, n/a 56, not_increasing 3, no_convergence 7 |

**Per inversion 385 → 129 us p50 (3.0x), 671 → 234 p99 (2.9x); 12.8 → 3.7 accurate-lattice
equivalents (3.5x).** The brief's <= 4 equivalents target holds at p50 (3.68) and not at p99
(4.79, pinned); the tail is the fallbacks (0.3 % on SPY, 5.7 % on dev200 — mostly Screen
refusals on chains with an ex-date inside the first coarse steps, which pay the plain accurate
secant). Accuracy: |sigma - sigma*| <= 1e-7 against the accurate lattice's root by bisection to
1e-10 on every vega-bearing ladder contract, seeded (max 8.40e-8, p99 7.03e-8) and unseeded
(9.98e-8 / 7.87e-8); pinned at tol. The bench's `dist_max` 0.195 (SPY) / 2.77 (dev200) is the
distance to the GENERATING sigma on contracts whose 301 price is flat in sigma (deep ITM, the
L1c intrinsic-region property) — the twin reports the same 0.146 / 2.77; not a root error.

Why the unseeded path is as cheap: a Black-76 inversion at the dated-cash forward seeds every
contract whose American price is under its European ceiling; the rest bracket from the Screen's
lower endpoint by doubling — on the Screen tier, so the cost is Screen calls.

## 6. Items 5a / 5b

NOT REACHED (budget: items 0-4, 6 and the gates took the lane's ~4 h). For the PM's D2 / D3
decisions the tables above already say: `SplicedCrr` is not the record scheme (staircase +
ripple; the Screen tier here is a per-contract seed, not a (K, sigma) record), the 301 default
is ~9 bp p99 from the converged lattice on long-dated SPY, and the BBS schemes carry a fixed
Black-76 cost per lattice that a record BUILD (one lattice per node) would amortise but a
per-price tier cannot.

## 7. What L3f-wire must assume (the hook)

- `dated_cash_iv(fit, quote, target, cfg, q_effective)` (quote_slice.cpp:222-290) becomes
  `american_discrete_div_implied_vol(target, m.spot, quote.strike, m.variance_years,
  d.effective_rate, q_effective, quote.side, fit.variance_clock_dividends, fit.discrete_div_steps,
  fit.discrete_div_scheme, opts, workspace)` with `opts = {seed_sigma = the aggregate-model AL IV
  of the same quote (or a neighbour's root; 0 when none), tol = cfg.iv_tolerance, max_iter =
  cfg.max_iv_iterations, tier = Screen under the key, screen_steps = the key's}` → `->sigma`;
  `OutOfRange` / `Unavailable` are today's refusals one for one (same texts).
- `model_price` (quote_slice.cpp:200-215) → `american_discrete_div_price(..., workspace)` with a
  workspace sized `max(fit.discrete_div_steps, screen_steps)` (9.7 KB at 301).
- `SliceDeAmModel::has_fast_kernel` stays FALSE for DatedCash (the Screen tier is a seed with a
  bp-class bound, not a Fast tier with a 1e-12 bound; `price(kernel == Fast)` must not route to
  it); `implied_vol_premium` → the seeded inversion above (the Premium leg's role).
- Policy keys (named here, wired there): `dividend_kernel: lattice | screen-seeded`,
  `dividend_kernel_screen_steps` (default 101). Graded admission refuses `inferred-schedule`
  routing (`quote_bench_policy.hpp:870-874`), so the wiring lane measures on the non-graded
  `policy-divsched-v1.json` profile and names it.
- Ownership: one `DiscreteDivWorkspace` per fit worker / thread (a `thread_local` or a per-slice
  member); never shared across threads; moved, never copied. Statuses that route to the accurate
  tier are inside the inversion already (`fallback`); the caller only sees the root.

## 8. Gates (lane report has every command and output)

Workspace identity 14,400 / 14,400 cases, 4-thread concurrency bit-identical, 0 allocations on
every scheme / step count / the converged reference / the inversion; 17/17 kernel tests;
88 / 28 / 21 / 31 unchanged; fingerprint `53c4df68` MATCH; parity `c89d0739` / `abc75955` /
`747d3e6c` MATCH (cap 2); name set at cap 3; PCH-off hygiene 0 diagnostics on the four touched
TUs; bench name coverage ctest passed; `git diff --check` clean, 100 columns held.
