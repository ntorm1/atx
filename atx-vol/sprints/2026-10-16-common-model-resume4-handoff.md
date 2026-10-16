# Resume 4 handoff: stopped at user request, October 16, 2026

The user explicitly requested **stop and write a handoff**. The long-running
goal is **paused**, not complete. Resume only on a new user instruction.

## Start here on an authorized resume

- Integration branch: `feat/vol-resume4-20261016`. Root worked in
  `C:/atx-wt/pool-33` (lease RunId `vol-20261016-root4`, keeper pid 10592).
  The handoff commit SHA and final lease disposition are appended at the end.
  Main `C:/atx` is untouched (still `e696e208`); no pushes, no PRs.
- Read this file, then `2026-10-16-common-model-resume4-sprint.md` (lane table
  and integration log) and the ledger lines dated 2026-10-16 in
  `docs/LEDGER.md` (grep `| integration |`, `| benchmark |`, `| trap |`).
  The resume3 handoff (`2026-10-16-common-model-resume3-handoff.md`) is
  historical and superseded.
- On resume lease a fresh root tree from this branch head with the ROOT tree's
  scripts (`C:/atx-wt/pool-33/scripts/lease-worktree.ps1`, default
  `-ConfigurePreset auto` configures `vol-rel`); the `C:/atx` copy of the lease
  script predates the build-infra fixes. Use `-Preset vol-rel` for every
  configure/check/build/ctest, `vol-rel-avx2` only for timing pins, focused test
  targets only (never `atx-vol-tests`), `-Jobs = clamp(floor(12/N),1,8)`.
  Python ctests need `-DPython3_EXECUTABLE="C:/Program Files/Python312/python.exe"`
  (polars 1.40.1); the auto-detected atx-db venv lacks polars.
- Standing user rules this resume: subagent-driven development with Opus
  subagents, implementation before tests (no TDD), file edits through the
  Edit/Write tools (no python/sed/heredoc authoring), compile-time hygiene is a
  tracked requirement, SpiderRock marks are the yardstick not the architecture
  (any family that matches on accuracy/robustness/performance is preferred),
  srPrc/srVol never inputs, no default promotion from development scores, no
  holdout opened. Every lane is independently reviewed at its exact SHA before
  integration; a BLOCK gets one fix round and a fresh re-review.

## Direction reset and the evidence behind it

- Diagnosis (read-only, dev200): the common Markov local-vol model's error was
  a systematic low price level (bias -0.375 of MAE 0.459) from ATM vol stuck
  under resource caps (class B) and the 0.25 sparse prior (class A); the
  per-expiry quote-slice fitter was 3x more accurate where it served but
  limited to 16.7% coverage by static admission. 59% of eligible rows were
  refused by liftable caps. `C:/atx-cache/quote-benchmark/diagnosis-20261016/`.
- Research (cited): SpiderRock srPrc is PRICE(sVol, uPrc, uOff, years, rate,
  sDiv, discrete divs) per contract; srVol is a per-expiry channel-constrained
  spline smile; sparse expiries borrow nearby expirations and a default shape.
  `C:/atx-cache/quote-benchmark/research-20261016/`.
- Consequence: the per-expiry fitter is the primary model; the Markov model is
  the fallback for rows it cannot serve and a consistency layer at best. The
  tick-path serving layer is record-based, not the Markov engine.

## Integrated this resume (all on `feat/vol-resume4-20261016`)

| Merge | Lane SHA | What | Review |
| --- | --- | --- | --- |
| `fc5ffb84` | time-knots `f4416a74` | variance-time-knot shape for the Markov fitter | APPROVE |
| `3495635f` | policy-experiments `9c1005ac` | status-routed composer tool, policy experiments note | tool-only |
| `d7f4b6b5` | implied-forward `a5d2018a` | per-expiry implied forward / sdiv estimator, opt-in carry mode (off) | APPROVE |
| `da3a4179` | build-infra `5cecf923` | vol-only presets, `vol-rel`, lease configure fix, test slices, local deps | infra |
| `ac01702c` | cohort builder `6703f85d` | generic cohort draw; dev200b built | tool-only |
| `dc064a1e` | tick-path `94208f57` | ExpiryRecord, spot ladders, record IV, chain-refresh bench, pinned bench thread | BLOCK -> fix -> APPROVE |
| `be85fd21` | markov-fit `88c77c5d` | scaled budget, Levenberg-Marquardt, sigma-interval seed, seed-as-prior | APPROVE |
| `b9374ae7` | dividend-schedule `a402d615` | dated dividend inference from ddiv/ddivPv/rate, opt-in lattice route | APPROVE |
| `31cdfeac` (ff) | slice-coverage `03583b45` + merge | graded admission, channel knot spline, term prior, family solver | BLOCK -> fix -> APPROVE -> merge APPROVE |

Every lane keeps the default paths byte-identical (dev200 default quote-slice
`c89d0739...`, R104 common `6b129f64...`, dev200b default `4b273b08...`).
Combined-root focused gates after each merge are in the ledger integration lines.

## Benchmark state (development cohorts; no promotion)

dev200 (63,942 eligible rows), price MAE in USD on own population unless noted:

| Model / composite | Coverage | MAE | Notes |
| --- | --- | --- | --- |
| quote-slice static (default) | 16.7% | 0.151 | baseline; core vol MAE 93 bp |
| common R104 (resolvent, 1/104 clocks) | 70.5% | 0.352 | 0.500 -> 0.383 on the three-way 24,581 rows vs E0 |
| common X1 (seed + sparse prior) | 70.5% | 0.343 | |
| common m6 (5 knots, LM, interval seed, seed prior) | 70.5% | 0.339 | post-hoc; MINOR-1 one-sided seed must be fixed before holdout |
| graded knot-spline admission (graded-v1) | 78.7% | 0.235 (mid 0.333 same rows) | all static rows byte-identical; A 0.163 / B 0.273 / C 0.169 |
| status hybrid quote-slice -> X1 | 75.7% | 0.292 | containment 95.1% |
| dividend opt-in (static + inferred schedule) | 18.8% | payers 0.224 -> 0.211 | non-payers identical; ASML blocked by lattice strike kinks |
| **composite C2: dividend -> graded -> m6 (status-routed)** | **88.6%** | **0.233** (mid 0.336 same rows) | p99 2.02, containment 97.8%, MAE/spread 0.150; C1 graded->m6 0.236; C3 graded->X1 0.236 |

Composite C1 beats the earlier quote-slice->X1 hybrid by 24.9% on that
hybrid's 47,769 rows (0.2919 -> 0.2193). The 7,269 unserved eligible rows
(11.4%) are META/ASML (4,886: arbitrage refusals and the 4,096-quote cap),
TSM/V (1,496: arbitrage refusals and the dense parameter cap), 716 adjusted
deliverables (by design), SRLN carry dispersion 86, ARI numeric 30, 55 mixed.
Receipts: `C:/atx-cache/quote-benchmark/composite-20261016/RESULTS.md`.
dev200b composites were NOT run (stop arrived first): no generalisation
verdict for the composite yet.

dev200b (62,369 eligible, 15:00 ET, disjoint symbols): the dev200 ranking
holds on every exact intersection; dollar MAE does not transfer (served rows
are dearer), spread-normalised MAE does (hybrid 0.211 -> 0.169). Report
MAE/spread alongside dollars. Dividend payers are the worst stratum everywhere.

Key traps recorded this resume: the CorrectionCache tensor interpolates across
the exercise boundary (up to $0.58 error on the default cached serve path);
strike-by-strike binomial prices cannot pass certification on dense chains;
one-sided sigma-interval seeds must not become prior centres; focused Markov
test TARGETS carry a `-tests` suffix the ctest NAMES drop.

Performance (unqualified, loaded host): record-ladder chain refresh of 2,000
contracts p99 106-181 us (target 300 us), cached record price ~120-140 ns,
record IV ~1 us (target 500 ns not met), cold Andersen-Lake 25.8 us (target
15 us, untouched). Nothing pinned; `bench/baselines/*chain-refresh-provisional.json`.

## Lanes in flight at the stop (checkpoints appended below)

| Lane | Branch | Base | Purpose |
| --- | --- | --- | --- |
| serve-path-lane | `feat/vol-serve-path-20261016` | `dc064a1e` | wire ExpiryRecords into the session serve path, close tick-path minors, IV <= 500 ns, record build speed |
| smooth-div-lane | `feat/vol-smooth-dividend-20261016` | `a402d615` | BBS/BBSR smoothing and strike-aligned nodes so dated-dividend chains pass certification; dividend-review minors M1-M3 |
| cleanup-lane | `feat/vol-review-cleanup-20261016` | `be85fd21` | deferred review minors (markov-fit MINOR-1..5, implied-forward estimator minors, time-knots header/schema) |
| composite-bench | none (run-only) | `31cdfeac` binaries | composite results on both cohorts: graded -> m6, dividend -> graded -> m6, graded -> X1 |
| knot-ladder-lane | `feat/vol-knot-ladder-20261016` | `31cdfeac` | knot-first ladder with point-fit fallback, typed price-only status, slice minors |
| smile-plot | none (read-only) | | SPY/RIVN/AAPL/MSTR/CRWV smile comparison figure |

Deferred minors still open (not in any lane): implied-forward "dispatch before
validate()" in `quote_slice.cpp` (conflicted with the slice lane; do it on the
integrated root), tick-path Greeks reference near the boundary, graded
`n_butterfly_viol` not computed for channel fits.

## Next steps the evidence supports

1. Integrate the in-flight lanes after fresh exact-SHA review (knot ladder,
   serve path, smooth dividends, cleanup), then re-run the composite benchmark
   on both cohorts and record MAE/spread.
2. Fix the Markov one-sided seed prior (cleanup lane) before any held-out run.
3. Only after a stable composite: declare a candidate default set and open the
   holdout ONCE under the oracle Ratchet rules; never tune on it.
4. Perf qualification on a quiet host: pin the chain-refresh baseline, then
   push record IV to <= 500 ns and cold AL to <= 15 us.
5. Latency/accuracy are still unqualified claims; no "world's fastest" claim
   has been made or is justified yet.

## Lane checkpoints at the stop

### serve-path-lane: items 1-2 committed and gated, item 3 WIP, 4-5 not started

Branch `feat/vol-serve-path-20261016`, base `dc064a1e` (not rebased), final
WIP SHA `7cf6b38b`, pool-4 released, note
`2026-10-16-serve-path-wip-handoff.md` in the WIP commit. Not reviewed.
- `cd427b31` opt-in `SessionServePolicy` Records serve mode (every served
  price carries a certificate; uncertified -> cold) and an opt-in
  exercise-edge gate for the cached tensor. 2,000-contract chain max error vs
  cold accurate AL: tensor $0.969 (775 contracts > $1e-3); gated band 1 $0.453
  at 63 ms/chain; band 3 $0.070 at 122 ms; records $1.08e-4 (p99 $4e-5) at
  0.85 ms. The gate stays OFF by default: the tensor error is spread across
  the chain, a distance gate cannot remove it (ledgered). Gates: fitting slice
  1027 cases with 38 failures identical to base, session serve 13/13,
  PricedSurface 52/52, tick-path 38/38, hygiene clean; backtest slice 3 of 646
  failed, NOT yet confirmed at base.
- `e695a411` all tick-path review minors closed: price-scaled certificate
  threshold with 8-ulp floor (a $50,000 underlying now refuses 0 of 42 instead
  of 7), four fallback counters, floor Greeks consistent with the served
  price, certification refuses unsolvable sigma rows, bench pool mask 0x3f
  and main core 0xc0 (3 P-cores for the pool: pool-row timings no longer
  comparable with earlier baselines, ledgered), zero-allowance walk tests.
  Gates: tick-path 43/43, PricingExecutor 23/23, session/correction 78/78,
  hygiene 7 files, bench self-check 0 failures. Unqualified: ladder tick
  88.3 us median (42.9 ns/contract), direct record route 753 us.
- WIP item 3: nearest-the-money-first rebuild priority when the per-tick
  budget runs short; uncompiled, untested; +7% catch-up bench rows missing.
- Unfinished: items 4 (record IV <= 500 ns) and 5 (record build speed), the
  3 backtest failures at base, sprint note, rebase and final gates, review.

### knot-ladder-lane: design only, no code

Branch `feat/vol-knot-ladder-20261016`, base `31cdfeac`, commit `0e5f393a`
holds only `2026-10-16-knot-ladder-wip-handoff.md`; pool-34 released; no
source edits, builds, gates or benchmarks. Decided design: opt-in
`graded.ladder: "knot-first"` (knot slice + bounded calendar repair, then
point fit clean/repaired, then the stronger-prior refit, then refuse; each
slice records `method` and `ladder_fallback`; A-grade rule unchanged); library
row status `PriceOnly` (certified price, blank vol) exposed in the bench as
`status=ok_price_only` / `vol_status=unidentifiable` under a top-level
`price_only` key; scorer counts price-served as ok|ok_price_only and vol-served
as ok only; composer `--served-status` accepts several values. Decision needed
on resume: the minor asking `vol_status` to read `unidentifiable` on the 564
`invalid_volatility` rows breaks strict graded-v1 parity (1348f63d) on that
one column; the proposal is to prove parity by reverting that column only.
dev200b still lacks graded-v1 / knotspline-v1 comparison runs.

### smooth-div-lane: WIP committed, never compiled or tested

Branch `feat/vol-smooth-dividend-20261016`, base `a402d615`, WIP SHA
`e888791a`, pool-3 released, note `2026-10-16-smooth-dividend-wip-handoff.md`
in the commit. Diagnosis of certification refusals on dividend chains: (1)
lattice staircase noise (ASML 20924: 48 violations, 10 with a BBS final step);
(2) model mismatch - the fitted curve is convex in Black-76 terms at the
dated-dividend forward, but the spliced lattice's European price differs by a
smooth 0.023-0.235 term that is concave across tight strike runs, so BBSR at
4801 steps still fails; (3) vendor per-strike carry kinks (TSM 20896; 33 dev200
chains fail on the aggregate route too). Implemented, opt-in and default
bit-identical (default scheme reproduces `abc75955`): lattice schemes
`spliced` (default), `spliced-bbs`, `spliced-bbsr`, `escrowed-bbs`,
`escrowed-lr` (strike-centred Leisen-Reimer on the escrowed model, CRR
fallback at tiny vols), bench keys `dividend_lattice` /
`dividend_lattice_steps`; certification untouched; existing workspace reused.
UNOFFICIAL exploration (`C:/atx-cache/quote-benchmark/smooth-div-20261016-explore/`):
lattice chains certified/refused dev200 117/63 -> 149/31 with escrowed-lr
(0 chains lost), dev200b 170/20 -> 183/7 (1 lost, HSBC); 11 of 12 servable
ASML chains certify. Runtime unqualified: lattice step 2.06 s -> 10.22 s per
dev200 run. Not done: the new tests (`discrete_div_smoothing_test.cpp`,
`..._alloc_test.cpp`, target `atx-vol-discrete-div-smoothing-tests`) never
compiled and thresholds uncalibrated; no gates on this commit; no rebase; no
official `*-auto-divsmooth-v1` runs; no sprint note/ledger; dividend-review
minors M1-M3/N2-N5 not started (note: the M1 probe's 1.00 special is exactly 4
regular units, so "split only near whole units" would not catch it; a better
signal is an increment holding more units than fit in its window, to be
measured on the 59 dev200 split increments first).

### cleanup-lane: all three groups committed and gated, not reviewed

Branch `feat/vol-review-cleanup-20261016`, tip `3465221c` rebased onto
`2378605e` (root parent of 31cdfeac's docs commits), pool-1 released. Commits
`811282cf` (Markov fit: one-sided sigma intervals are refused with the market
bound reported and the configured initial/prior kept, counted as
`fallback_one_sided_interval`; failed Jacobian probes retried the other way,
else typed `JacobianFailure`; DampingLimit / Cholesky / time-knot prefix-reuse
tests; definition texts fixed with default bytes pinned by a test; ledger
correction "91/91": 51 stopped on step tolerance), `3ed89f9b` (implied forward:
unmeasured-bias Parity pairs marked Unidentified and deactivated with the
forward re-located; inert `implied_forward` block rejected;
estimate_implied_forward split; NITs), `abf15ab0` (time knots: helper bodies
moved to `american_markov_time_basis.cpp`; report schema_version 3 -> 4 with
python expectations updated; NITs), `3465221c` notes. Gates on the rebased
head: fit 41/41, seed 25/25, policy 17/17, implied-forward 26/26, anchored
ctest 5/5, native pytest 149/149, PCH-off 23 objects clean. Parity: R104 dev200
`6b129f64` identical with the final binary; legacy e0/r104/r416 canonical
definitions match (measured with the group-A binary before the rebase). NOT
run: quote-slice default parity `c89d0739` and the definition probe on the
rebased binary. Deferred: `quote_slice.cpp` dispatch-before-validate() minor.
Every m5/m6 `prior: seed` result predates the one-sided fix and must be rerun
before any held-out use. Needs an independent exact-SHA review before merge.

### composite-bench: dev200 only

No lease, no code. Binary `73892e7f` reproduces m6 (`dc435595`) byte for
byte. dev200 composites scored (table above); dev200b composites, |z| and
class strata, and per-grade breakdown not run. Artifacts and SHA256s in
`C:/atx-cache/quote-benchmark/composite-20261016/` (C1 `77d9cdde`, C2
`c3ec3786`, C3 `8515d5c5`).

## Artifacts

All receipts, reviews, benchmark outputs and binaries live under
`C:/atx-cache/quote-benchmark/` (review folders `*-review-v1/2`, lane folders
`<lane>-20261016-*`, cohorts `20261014-dev200`, `20261014-dev200b`). Every
review JSON and results note carries SHA256s; nothing under that root may be
overwritten.
