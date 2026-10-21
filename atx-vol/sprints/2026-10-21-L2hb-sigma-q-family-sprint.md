# L2h-b — the (σ, q) American price family: q as a second interpolation axis (2026-10-21)

Lane L2h-b of the vol-sota sprint (PM ruling RESUME 4 §6a-1, option c1). Kernel only, additive,
opt-in: nothing in the fitter changes and nothing default constructs the new type. Base `a4b34521`
(L2h `6e94ba40` + L3g's State code motion). The SPY1s owner wires it.

## 1. Why

Under `--dividends funding-pv` the effective yield q is per STRIKE. SPY cohort5 (`quotes-SPY.csv`
`b3fafd80…`, 12,278 two-sided rows) carries 1-144 distinct q per expiry over 33 expiries (owner
Phase 2 note §3.1). An `AlSigmaFamily` prices one (expiry, side) at ONE q, so a representative-q
family misprices a strike by ≈ T·S·Δq/2 (2e-4 … 0.86 USD; measured below as the
`sigma_family_qmid` baseline rows), and keying one family per distinct q costs 3,662 builds. The
SPY ≤ 1 s bar needs iterates + edges + the certification grid ALL on a family → the family must
carry q as an axis.

## 2. Design (what was built)

| Item | Where | Contract |
| --- | --- | --- |
| `AlSigmaQFamily` (+ `AlSigmaQFamilyStatus` / `Price` / `Chord`, `kAlSigmaQFamily{Min,Max}QNodes` = 2 / 8, `kAlSigmaQFamilyDegenerateWidth` = 1e-12) | `api/pricing/american.hpp` (L2h-b sub-block after the L3g block), NEW TU `src/pricing/american_sigma_q_family.cpp`, NEW internal header `src/pricing/american_sigma_q_family.hpp` (the State) | One (expiry, side) family bound to a `const AlExpiryGeometry &` (T, r, AL scheme, the premium quadrature's tables). `prepare(σ_lo, σ_hi, n_σ, q_lo, q_hi, n_q, axis)` lays a tensor of Chebyshev-Lobatto nodes: σ exactly as `AlSigmaFamily` (LogSigma default, same box rule), q linear over the caller's box; each (σ_s, q_j) node is one `al_solve_put_boundary_prepared` at the anchor on an orientation prepared at that node's internal-put (rate, yield) — the σ-family's per-node builder. `price(S, K, σ, q)` interpolates y[] at (σ, q) (2nd-kind barycentric, σ then q, one coefficient set per call) and runs the European leg + premium quadrature at the strike's OWN q (`al_put_price_from_boundary`, xmax = K·min(1, r/q) at that q). `price_and_chord` adds σ + 1e-4 at the same q. |
| Collapsed axis | same | `q_hi − q_lo ≤ 1e-12` → one q node at the box mid: the family IS `AlSigmaFamily` at that q bit for bit (σ coefficients accumulated exactly as `bary_eval`; pinned by `CollapsedQBoxIsBitIdenticalToTheSigmaFamily`, 1,050 price + chord pairs, both axes, both sides, three expiries). |
| Refusals | same | `prepare`: every `AlSigmaFamily` InvalidArgument case, a non-finite / inverted q box, n_q ∉ [2, 8] on a real box (InvalidArgument); no American orientation on the geometry for the side, the side not American at q_lo or q_hi (a call needs q > 0), the box straddling q = r (the K·min(1, r/q) kink), a collapsed / frozen node (NotImplemented); missing tables (Internal). Never half-built. `price`: the σ-family's statuses with the same values plus `YieldOutsideBox` (= 6; never extrapolated). |
| Threads / memory | same | Immutable after `prepare()`; `price*` const, allocation-free (counted: `AlSigmaQFamilyAlloc.PricePathAllocatesNothing` in its own binary), thread-safe (8 threads bit-identical to the serial loop). `state_bytes()` = 34,592 (fixed-capacity 32 × 8 × 16 tensor + the node grid); `prepare()` allocates one ~90 KB scratch block for its duration. |

## 3. Accuracy and cost on the real SPY board (the headline)

Producing binary: `build-vol-rel-avx2\bin\atx-vol-al-preset-ladder-bench.exe` sha256 `a80eb9d3…`
(source = this lane's `e948b362`), one run 2026-10-21 18:37:13 EDT, main thread pinned (CPU 7),
`ATX_VOL_FIT_WORKERS=2`, host load 18.8 % before / 34.8 % mid / 20.7 % after, 5.54 GB free —
**unqualified (loaded host)**. Command:
`ATX_VOL_AL_SQFAMILY=1 ATX_VOL_AL_SPY23=…\L2f-al-fast-tier-20261019-inputs\spy23-quotes.csv
ATX_VOL_AL_SPYLONG=…\L2hb-20261021-errors\inputs\spy-21533-quotes.csv
ATX_VOL_AL_SPYALL=…\L2hb-20261021-errors\inputs\spy-all-quotes.csv <bench> --benchmark_filter=american/sqfamily/`
(inputs extracted from cohort5 `quotes-SPY.csv` `b3fafd80…`: rows `input_status ok`, bid > 0, ask > bid;
S = spot, T = years, r = rate, q = sdiv; the 23 d extraction reproduces `spy23-quotes.csv` `0c68bb76…`
field for field). Every contract is queried at its OWN q at 16 (spy23, spylong) / 4 (spyall) σ values
spanning its chain's σ box (log-uniform cell midpoints, off the nodes) vs the cold Reference
`andersen_lake(S, K, T, σ, r, q_K)`; rel on P ≥ 0.01. Outputs `L2hb-20261021-us\sqfamily-rows.{txt,json,err.txt}`.

| set (queries) | family | build p50 / max µs | build Σ ms | price p50 / p99 µs | chord p50 µs | max abs USD | max rel | p99 rel |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| SPY 23 d (5,072; 2 families) | σ-family at box-mid q (baseline) | 1,577 / 1,721 | 3.3 | 6.4 / 7.8 | — | 2.35e-3 | **4.16e-4** | 2.91e-4 |
| | n_q 3, n_σ 12 | 4,855 / 5,625 | 10.5 | 7.3 / 10.5 | 14.5 | 6.48e-9 | 1.38e-10 | 1.09e-10 |
| | **n_q 4, n_σ 12** | **7,176** / 7,470 | 14.6 | **6.8** / 8.7 | 13.6 | 6.47e-9 | **1.38e-10** | 1.09e-10 |
| | n_q 6, n_σ 12 | 10,490 / 11,885 | 22.4 | 7.6 / 11.3 | 15.2 | 6.47e-9 | 1.38e-10 | 1.09e-10 |
| | n_q 4, n_σ 16 | 8,467 / 9,411 | 17.9 | 6.5 / 8.8 | 13.1 | 1.14e-9 | 1.58e-11 | 3.90e-12 |
| SPY 2.17 y puts (5,040; 2 families; calls European, q < 0) | σ-family at box-mid q (baseline) | 1,427 / 1,437 | 2.9 | 5.6 / 6.7 | — | 0.252 | **1.75e-2** | 1.42e-2 |
| | n_q 3, n_σ 12 | 4,306 / 4,324 | 8.6 | 5.7 / 7.0 | 11.4 | 7.56e-7 | 1.64e-8 | 1.23e-8 |
| | **n_q 4, n_σ 12** | **5,613** / 5,733 | 11.3 | **5.8** / 6.7 | 11.5 | 1.67e-7 | **1.22e-9** | 5.48e-10 |
| | n_q 6, n_σ 12 | 7,547 / 7,916 | 15.5 | 5.7 / 6.9 | 11.4 | 1.67e-7 | 1.22e-9 | 5.48e-10 |
| | n_q 4, n_σ 16 | 7,568 / 7,649 | 15.2 | 5.6 / 7.1 | 11.2 | 2.32e-9 | **4.91e-11** | 3.22e-11 |
| SPY whole board (41,908; 54 families, 22 sides cold) | σ-family at box-mid q (baseline) | 1,548 / 2,472 | 87.8 | 6.0 / 9.2 | — | 0.249 | 1.54e-2 | 5.74e-3 |
| | n_q 3, n_σ 12 | 4,424 / 5,340 | **237** | 5.9 / 7.9 | 11.8 | 4.45e-6 | 8.36e-8 | 6.78e-9 |
| | **n_q 4, n_σ 12** | 5,910 / 8,933 | **321** | 6.0 / 8.2 | 12.0 | 3.97e-6 | 7.45e-8 | 4.91e-9 |
| | n_q 6, n_σ 12 | 10,421 / 13,816 | 549 | 7.1 / 12.0 | 14.2 | 3.97e-6 | 7.45e-8 | 4.91e-9 |
| | **n_q 4, n_σ 16** | 9,741 / 13,367 | **519** | 7.5 / 11.5 | 15.0 | 1.94e-9 | **7.09e-11** | 8.96e-12 |

Reading:
- **The q axis is exact enough at n_q = 4 on every SPY expiry**: from n_q 4 to 6 no set moves (the σ
  floor remains); n_q 3 is at the floor on the 23 d chain (q spread 9.0e-5, T·Δq 6e-6) but 13× off on
  the 2.17 y chain (spread 1.03e-3, T·Δq 2.2e-3). The representative-q σ-family is 6-7 orders worse
  (4.2e-4 / 1.75e-2 max rel; 0.25 USD) — the owner's §3.1 finding, now measured on the kernel.
- **The σ axis sets the floor**: at n_σ 12 the 2.17 y chain sits at 1.22e-9 (just over the brief's 1e-9)
  and the board's worst is the 20833 (T 0.254) put chain at 7.45e-8 (σ-limited: n_q-independent, gone at
  n_σ 16). **n_σ 16, n_q 4 meets 1e-9 board-wide (7.1e-11 max rel, 1.9e-9 USD max abs).**
- **Cost**: price p50 5.6-7.6 µs (target ≤ 8 µs: met; the σ-family baseline in the same run 5.6-6.4 µs —
  the q tensor adds ≤ 0.4 µs p50); build is n_σ·n_q Reference solves at ≈ 120-150 µs per node (σ-family
  ≈ 120-130 µs per node in the same run): 7.2 / 5.6 / 5.9 ms p50 at (12, 4) — **the ≤ 5 ms target is
  missed by 1.1-1.4× on this loaded host**; (12, 3) meets it (4.3-4.9 ms). Board build total (54
  families, single thread): 237 ms (12, 3), 321 ms (12, 4), 519 ms (16, 4).
- Board groups: the cohort5 rows carry 1-2 spot stamps per expiry, so the bench's (S, T, r, side) groups
  are 76 for 33 × 2 sides; an owner build per (expiry, side) at one spot builds ≤ 54 families. Cold sides
  (22): every call side of a q < 0 expiry (European — no boundary), both sides of 20741 (T below
  `kAlSigmaFamilyMinTau`), and the 20833 call side, whose q box [−2.6e-4, 9.2e-4] crosses 0 (refused:
  split it — q ≤ 0 calls are European, closed form).

## 4. Accuracy pins (gtest, synthetic SPY-like sets; vol-rel)

| test | set | measured | pin |
| --- | --- | --- | --- |
| `CollapsedQBoxIsBitIdenticalToTheSigmaFamily` | 3 expiries × both sides × both axes × 21 σ × 5 strikes | 0 of 1,050 price + chord pairs differ | bit identity |
| `NodePricesAtTheAnchorMatchTheColdSolve` | Sigma axis, (σ, q) nodes, anchor strike, both sides | 1.507e-16 | 1.6e-16 |
| `Spy23ChainAtItsOwnYieldsWithinMeasuredBound` | S 772.99, T 0.0682, q ∈ [0.001589, 0.001678], 25 K × 12 σ × 2 sides | n_q 3 / 4 / 6: 2.763e-11 / 2.709e-11 / 2.709e-11 | 2.8e-11 (n_q 3, 4) |
| `SpyLongDatedPutsAtTheirOwnYieldsWithinMeasuredBound` | T 2.1673, q ∈ [−0.003298, −0.002272], 25 K × 12 σ puts | 4.885e-8 / 2.257e-9 / 2.160e-9 | 4.9e-8 (n_q 3), 2.3e-9 (n_q 4) |
| `WideCarryBoxErrorFallsWithTheQNodeCount` | T 1, r 5 %: put q ∈ [0, 4 %], call q ∈ [0.5 %, 4.5 %] | 5.10e-3 / 6.18e-4 / 1.255e-4 (monotone) | 1.3e-4 (n_q 6) |
| `StateBytesIsPinned` | — | 34,592 B | 34,592 |

Fail-on-revert (lane report §5): routing the collapsed axis through the tensor path → 9 of 1,050
identity pairs differ; pricing at q_lo instead of the strike's q → SPY 23 d 5.8e-4 / 2.17 y 3.7e-2
(both pins fail); a shared static boundary scratch → 518 of 616 concurrent prices differ; one heap
allocation in `price_at` → the alloc test counts 48 per family.

## 5. Robustness findings

1. **A wide q box converges slowly** (the WideCarry pin: 5.1e-3 → 6.2e-4 → 1.3e-4 over n_q 3 / 4 / 6
   for a 4 %-wide box). Inferred cause: y(q) is analytic only up to q = r for puts (the near-expiry
   boundary changes form there) and to q = 0 for calls (xmax = S·q/r → 0), so the Chebyshev rate
   is set by box half-width ÷ distance to that point; SPY's boxes (≤ 1.8e-3 wide, ≥ 3 % from r) sit
   far inside it. Rule for callers: keep the half-width ≪ the distance to r (puts) / to 0 (calls), else
   split the box (two families). The refusals already block the two singular cases themselves (a box
   straddling r; a call box reaching q ≤ 0).
2. **Calls on negative-q expiries are European** (15 SPY expiries: 20742 and every expiry from 0.29 y
   on): no family, closed form.
3. **Build cost is the open lever** for the ≤ 1 s bar: (16, 4) = 519 ms of the board's single-thread
   budget. Candidates (not built here): the n_q orientations at one σ are 4 independent boundary solves —
   exactly the AVX2 boundary batch's 4 lanes; adaptive n_q by T·Δq (3 when T·Δq ≲ 1e-5) and n_σ by
   the box / T; Fast-tier nodes for an iterate-only family with Reference confirmation.

## 6. How the owner calls it

```cpp
// Once per (expiry, side) per fit — e.g. in the rung-memo slot; the geometry outlives the family.
const AlExpiryGeometry geo = prepare_al_geometry(T, r, q_eff_expiry); // q inside the box (calls: > 0)
const auto [q_lo, q_hi] = /* min / max of the side's strikes' effective_yield(fit, K) */;
AlSigmaQFamily fam(geo, side, spot);                                // anchor: the spot (both sides)
if (Status st = fam.prepare(box.lo, box.hi, 16, q_lo, q_hi, 4); !st) { /* the side prices cold */ }
// Iterate / edge / grid, each strike at its OWN q (one q -> n_q() == 1, bit-identical to AlSigmaFamily):
const AlSigmaQFamilyChord c = fam.price_and_chord(S, K, sigma, q_K);
if (c.status != AlSigmaQFamilyStatus::Ok) { /* cold path (box / guard refusal) */ }
const double slope = (c.price_sigma_bumped - c.price) / kAlPackSigmaBump;
// The L1c non-monotone band test stays IN FRONT of any family inversion (L2h note §7).
```
Calls on a q ≤ 0 strike: European closed form (the family refuses a box reaching q ≤ 0). A q box that
straddles r: split it at r. `AlSigmaQFamilyStatus` values 0-5 equal `AlSigmaFamilyStatus`'s.

## 7. Gates (lane report has the commands and outputs)

`AlSigmaQFamily.*` 9/9 + `AlSigmaQFamilyAlloc` 1/1; `AlSigmaFamily.*` 11/11 and the L2h sibling suites
(69 tests / 10 suites: 68 passed + the pre-existing `SigmaInterp.SolveCount` skip); chain-batch 31/31;
dev200 default parity `c89d0739` MATCH (cap 2); AL fingerprint `53c4df68` MATCH (vol-rel-avx2); the
pricing / simd name set at cap 3 = `52324ef8`'s 19 names + the two `AlSigmaFamilyGreeks.*` failures
inherited from base `a4b34521` (L3g's wip checkpoint: "11/13") + the ledgered
`PricingExecutor.NestedDispatch` flake — no `AlSigmaQFamily` name; PCH-off hygiene on every includer
of the touched headers (lane report §3).
