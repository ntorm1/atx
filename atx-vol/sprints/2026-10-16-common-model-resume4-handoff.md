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

## Artifacts

All receipts, reviews, benchmark outputs and binaries live under
`C:/atx-cache/quote-benchmark/` (review folders `*-review-v1/2`, lane folders
`<lane>-20261016-*`, cohorts `20261014-dev200`, `20261014-dev200b`). Every
review JSON and results note carries SHA256s; nothing under that root may be
overwritten.
