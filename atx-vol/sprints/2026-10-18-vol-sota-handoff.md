# vol-sota sprint handoff: stopped at user request, October 18, 2026

The user explicitly requested **stop here and write a handoff sprint file**. The sprint executes
`docs/plans/2026-10-17-atx-vol-sota-fit-dag-plan.md` (SOTA fit speed × accuracy DAG, waves W0-W4).
The goal is **paused**, not complete: W0 is integrated (G0 closed modulo what §"G0" says below), W1/W2
lanes are checkpointed mid-flight. Resume only on a new user instruction.

## Start here on an authorized resume

- Integration branch: `feat/vol-resume4-20261016` (unchanged name), root tree `C:/atx-wt/pool-1`,
  head `05a7da3e` when this file was written (the three W0 merges; the integrate-W0 lane was still
  running its docs commit — see §"G0" and §"Handoff commit"). This handoff lives on branch
  `docs/vol-sota-handoff-20261018` (off `05a7da3e`) with the ledger copy; the next PM merges it into
  the integration branch after the integrator's docs commit (`git merge --no-ff docs/vol-sota-handoff-20261018`).
  Main `C:/atx` untouched; no pushes, no PRs, no main merges.
- Read, in order: this file; the sprint ledger `C:/atx/.superpowers/sdd/2026-10-18-vol-sota/progress.md`
  (every ruling, every lane outcome, every deferred minor — copied beside this file as
  `2026-10-18-vol-sota-pm-ledger.md`); the plan (§1 targets, §4 DAG, §5 gates, §6 process); the resume-6
  handoff `2026-10-17-common-model-resume6-handoff.md` (standing rules, benchmark state before this
  sprint); the root sprint note `2026-10-16-common-model-resume4-sprint.md` (integration-log table);
  ledger lines dated 2026-10-18 in `docs/LEDGER.md`.
- Lease a fresh root tree from the branch head with the ROOT tree's scripts (extract them from the branch
  head into your scratchpad first — the lease script switches the pool it runs from):
  `powershell -NoProfile -File <scratch>\lease-worktree.ps1 -Branch feat/vol-resume4-20261016 -Base <head> -Agent pm-root10 -RunId vol-<date>-root10 -HeartbeatId vol-<date>-root10-hb -MaxPool 40`
  (`-MaxPool 40` required; `lease-heartbeat-keeper.ps1` must sit beside the lease script). Release with
  the acquiring `-RunId`. `-Preset vol-rel` everywhere, `vol-rel-avx2` only for the AL fingerprint and
  pins; focused test targets only; `-Jobs = clamp(floor(12/N),1,8)` with N concurrent lanes; Python
  ctests need `-DPython3_EXECUTABLE="C:/Program Files/Python312/python.exe"`; ad-hoc pytest is refused
  by a repo guard — use the registered ctest or `python/tests/_ctest_pytest_driver.py`.
- Standing user rules (unchanged, binding): Opus subagent-driven development (user: "sub agent opus 5.5
  driven dev"), PM context small (briefs/reports as files), implementation before tests (no TDD) with a
  fail-on-revert test per behaviour change, source/doc edits only via Edit/Write tools, PCH-off hygiene on
  every touched header, SpiderRock marks are the yardstick never inputs, every behaviour change opt-in
  with default parity proven by hash, no default promotion from development scores, holdout stays closed,
  nothing under `C:/atx-cache/quote-benchmark/` overwritten, every lane independently reviewed at its
  exact SHA before integration, a BLOCK gets one fix round and a fresh scoped re-review.
- **Two user directives given this sprint (binding from now on):**
  1. *Reduce reliance on micro-benchmarks and long-running tests/benchmarks; speed up progress;
     get the library within the right order of magnitude on performance and accuracy FIRST.*
     Applied as: reviews reproduce gates + parity hash(es) + one revert, never benchmarks; timing numbers
     are unqualified with a one-line load note; no repetition schedules or attribution passes unless a
     brief names them; quiet-host pins deferred to W4; C6 from existing CSVs, no new fits.
  2. *Deploy more parallel subagents to parallelise the plan.* Applied as: a PM "prebase" branch
     (`feat/vol-w2-prebase-20261018` @ `7cebbbd3` = root + the three W0 tips merged the way the
     integrator merges them) so W1/W2 lanes started while W0 reviews were still running; six lanes
     concurrently at `-Jobs 2`; file ownership relaxed where hunks are disjoint and the integrator
     resolves conflicts.

## Process used this sprint (reuse it)

Workspace `C:/atx/.superpowers/sdd/2026-10-18-vol-sota/`: `SHARED-BRIEF.md` (rules + lease/build
commands + the "Speed of progress" section + report contract; root SHA line must be refreshed to the new
head and the RunId to the new root RunId), `lane-<id>-brief.md` / `lane-<id>-report.md` per lane,
`lane-review-brief-template.md`, `lane-integrate-W0-brief.md` (template for the next integration),
`progress.md`. Dispatch = Agent tool, `general-purpose`, `model: opus`, prompt = "read these two files
first" + lane id + hard rules. Lanes lease their own pool with the pool-1 lease script; the integrator
works in pool-1 without leasing. Briefs are written with the Write tool. Prebase trick: when the next
wave's lanes need commits still under review, merge the reviewed-or-nearly-reviewed tips into a PM
branch, base the lanes there, and let the real integration merge the same SHAs (clean merges).

## Integrated this sprint (all on `feat/vol-resume4-20261016`)

| Merge | Lane SHA | What | Review |
| --- | --- | --- | --- |
| `8ac4be4a` | deferred-tests `083ce77a` | tests for the resume-5 deferred review items (Markov carry fallback causes, Parity Contaminated exclusion, serve-path refused row + box-end reuse, quote-bench N2 route-window label, Markov min-sigma retry characterisation); tests only | light review APPROVE (1 doc minor) |
| `3ca49eb0` | al-speed `e8472fe2` | AL kernel tier 1 (bit-identical hoists) + tier 2 opt-in `AlProfile::Fast` with measured bounds, carry memo keyed on the profile; fix round 1: counterbalanced A/B ("not resolved", unqualified), declared timing set, dev200 latency bench row, adjoint Reference-pin test, bound text | review v2 APPROVE `409ee7aa` (4 minors) → re-review APPROVE `e8472fe2` (0 new) |
| `05a7da3e` | knot-ladder `085e7380` | knot-first admission ladder, confidence-ordered calendar eviction, PriceOnly serve status, strict `vol_status` normaliser; fix round 2: six tests pinning the five containment checks and calendar key 3, note corrections. Merge conflict in `tools/compose_predictions_by_status.py` resolved as the union of `--restrict` and multi-value `--served-status` | fresh review BLOCK `2e60186c` (2 majors, tests) → fix 2 → re-review APPROVE `085e7380` (0 new) |

Root binary (vol-rel) sha256 `9ce6f6c6…` at `05a7da3e`. The integrate-W0 docs commit (root sprint-note
integration-log rows, ledger lines, parity table) was in flight at the stop; its report
`lane-integrate-W0-report.md` carries the final state.

## G0 (end of W0) state

Parity on the merged root `05a7da3e` (folder `integrate-W0-20261018-parity`), as reported by the
integrator before the stop:
- Exact MATCH: dev200 default `c89d0739`, dev200b default `4b273b08`, divsched `abc75955` / `747d3e6c`,
  divsmooth `2cfda793` / `5a9c1f5f`.
- MATCH modulo the ruled one-column `vol_status` normalisation (strict normaliser PASS, normalised sha256
  == expected): graded-v1 raw `fc277413` / `46ad6c26` / `171fa2d2` → `1348f63d` / `4c70d2bc` /
  `7fa3c415`; cohort5 repair-dom-v3 raw `a714e814` → `97fad2f0` (30 rows).
- Still running at the stop: dev200 / dev200b repair-v3 (`14d9dc6d` / `81cc69e0` expected after
  normalisation), R104 `6b129f64`, the keys-ON set (knot-ladder bench-v2 hashes, AL fingerprint
  `53c4df68` on the root), the pricing/simd name-set gate, C6 (`composite-20261018-c6/` created). Read the
  integrator's report for the verdicts; a MISMATCH there other than the normalised pattern means G0 is
  NOT closed and the wave stops until a fix lane lands.
- Ruling in force: raw hashes are the post-ladder expected set; the strict normaliser
  (`knot-ladder-fix1-20261017-normalise/normalise_vol_status.py`) bridges to the pre-ladder numbers.
- The `^atx-vol-quote-slice$` ctest filter excludes `QuoteSurfaceLadder.*` (L2d finding): the integrated
  ladder tests have only been run by the lanes' direct gtest invocations, never by that ctest — fix the
  registration before relying on the ctest as a gate.

## Lanes in flight at the stop (all checkpointed; every pool released)

All six wave-A lanes are based on the PM prebase `feat/vol-w2-prebase-20261018 @ 7cebbbd3` (= root
`9261d685` + the three W0 tips, i.e. the same tree as the integrated root after W0). Resume each with a
FRESH implementer (`-Base <tip>` below), its brief and its report file as memory.

| Lane | Branch @ SHA | State at the stop | Next |
| --- | --- | --- | --- |
| L1 (lite) fit timing counters | `feat/vol-L1-fit-profile-20261018` @ `a1003ec1` (WIP) | `counters::fit_timing` slot-local per expiry (armed via `SurfaceTrace`), bucket scopes in `quote_slice.cpp` (+15 lines) and chain/rung scopes in `quote_surface.cpp` (line ranges in report), `--timing-detail` emits `graded_stats.slices[].timing` + real per-chain percentiles behind the flag; ctest `^atx-vol-quote-slice$` pass, CLI pytest 162; fail-on-revert demos recorded. Smoke CRWV: p50/p95/p99 21.4/34.2/39.2 ms; **the channel solve reprices no American option** (hypothesis (a) refuted). Not done: parity flag-off, PCH-off `counters.hpp`, the six attribution runs, plan §1.1 budget paragraph, note. Deviation: exclusive (not inclusive) bucket times. | parity + PCH-off + 6 attribution runs (SPY 23 d / AAPL / CRWV × graded-v1 / ladder) + budget paragraph + note → light review |
| L1b shape cohorts | `feat/vol-L1b-shape-cohorts-20261018` @ `54e9488d` | **DONE_WITH_CONCERNS** (finished before the stop): nine classes from quote features, manifest `ad323ccb`, opt-in `score --class-manifest` (default stdout byte-identical; score JSON differs only in `evaluator_sha256`), tools ctest 163/163, two revert checks fail as expected; per-class baselines in `shape-classes-20261014/RESULTS.md` §4-5. | fresh light review → integrate (tool-only; after W2 or with it) |
| L1c AL non-monotone refusal | `feat/vol-L1c-al-nonmonotone-20261018` @ `b0a15fb1` (WIP) | Region map in `L1c-20261018-nonmonotone-map` (dips only inside the kernel's exercise region, carry advantage ≥ ~+0.015, T ≥ 0.1, moneyness ≥ 1.01; largest relative dip 1.2e-6 accurate/Fast, 9.1e-5 `al_fast_opts`). Opt-in refusal written and compiling, UNTESTED: `IvStatus`/`IvOpts`/`IvResult` + overload of `american_implied_vol`, `al_non_monotone_gate/_region`, `ChainRowStatus::NonMonotoneRegion` + `ChainIvOptions::refuse_non_monotone` in PerContract and Lockstep; `batch_chain.hpp` edited additively (scope deviation); `american_iv.hpp` (Tier-A) extended. Not run: parity, AlFastProfile, dev200 flag-on counts, tests, PCH-off, note. L2b follow-up hypothesis: `american.cpp:2290-2296`, `:2548-2554` lack an "S ≤ B(T) → intrinsic" check. | confirm gate constants with exercise-region columns → dev200 flag-on counts (3 routes) → tests (N2 pin, refusals, flag-off identity) → gates/PCH-off/parity/note → review |
| L2a vol-space fit | `feat/vol-L2a-vol-space-20261018` @ `63400aeb` (WIP) | Keys `graded.fit_space / deam_profile / deam_cache`, per-quote IV cache (thread-local memo keyed by exact inputs, Fast-kernel guard), compiles /W4 /WX. **Finding: the channel knot-spline Newton is already vol-space (0.09% of SPY time); the cost is American IV inversions repeated by every rung (~24.5k inversions + 23k band-edge prices to build the cache), then the certification reprice (~9 s per 87 certifications).** SPY graded-v1 (unqualified, load 51-96%): accurate-kernel vol-space CSV byte-identical to OFF (`087e6a54`), fit 88 → 59 s, p50 2,665 → 1,787 ms; Fast-kernel cache ~32 s but CSV changes (`45313d61`). Not done: tests, revert runs, keys-OFF parity, policy files, cohort5 table, timing table, PCH-off, note. Deviations to rule on: no separate `solve_channel_vol_space` (existing solve + certification on cached observations); no total-variance penalty (band membership identical in σ and σ²T). | PM ruling on the deviations (recommend: accept both) → keys-OFF parity → tests → cohort5 served/containment table → review. Re-aim W2 at inversion sharing (L2c memo) + cheaper inversion/certification (L2b) |
| L2b AL geometry / certification | `feat/vol-L2b-al-geometry-20261018` @ `cc97f387` (WIP, 3 files +699/−65) | Fingerprint BEFORE edits `53c4df68` MATCH (`L2b-20261018-cert`). Compiles, UNTESTED: `AlExpiryGeometry` per (T, r, q, opts) put+call; `andersen_lake` / `american_greeks_al` geometry overloads; `AlBoundaryHandle` (calls share one boundary per spot+sigma; puts per strike); `al_price_pack` (returns `Status`); `al_solve_put_boundary` + `andersen_lake_core` refactored into shared seed/sweep/dispatch, arithmetic unchanged. Not started: certification overload (`CertificationKernel/Screen`), `batch_chain` consumer, tests, timing, note. Not run: fingerprint after, parity, identity tests. Concerns: bit-identical levers ~5-10% (est.); certification nodes each have their own sigma so no boundary sharing there; the 3 ms target is plausible only for refusals via early exit; SPY 23 d has 148 nodes (the ~325-node chain is 78 d). | fingerprint after → identity tests + reverts → Fast screen (per-prefix checks, tolerance widened by 1e-10·max(P,1)) + RF#1 tests → batch_chain → gates → review |
| L2d scheduler (redirected) | `feat/vol-L2d-scheduler-20261018` @ `3391b6a1` (impl `b1dc4243`, tests WIP) | `quote_surface.cpp` point_pass / knot_pass / B1 / B2 run in parallel over new `QuoteSurfaceConfig::fit_workers` (default 1), densest-first, cross-expiry stages serial; `curve_fit.cpp` one longest-first helper for all three fan-outs; quote-bench graded fit at AUTO width (`ATX_VOL_FIT_WORKERS=N` caps, 0/unset = all cores); 4 fan-out tests pass, 2 revert checks fail as expected; hash identity {1,2,7,0} cohort5 graded-v1 = `171fa2d2`. **SPY surface wall (unqualified, load 39-81%): 87.8 s @1 → 30.5 s @4 → 21.4 s @8 → 15.2 s all cores, outputs identical (`087e6a54`).** Not done: curve_fit tests + reverts, ladder-policy hash identity, `c89d0739` / dev200b@0, PCH-off, RESULTS.md, note. Deviations to rule on: B1/B2 parallelised too; `fit_workers` added to `quote_slice.hpp`; quote-bench all-cores when env unset. Gap: the `^atx-vol-quote-slice$` ctest filter excludes `QuoteSurfaceLadder.*` (knot-ladder tests never run under that ctest — fix the registration). | PM ruling on deviations (recommend: accept all three; add the ladder ctest registration) → ladder hash identity + parity → remaining tests → review → integrate FIRST in W2 (biggest lever) |

## Key findings this sprint (change the plan's assumptions)

1. **The quote-bench graded path fits expiries SERIALLY.** `fit_graded` → `fit_quote_surface` →
   `point_pass` / `knot_pass` loop expiries one by one; `fit_workers` / `ATX_VOL_FIT_WORKERS` only
   reach `curve_fit.cpp` (surface-db/session path), which already resolves 0 → hardware_concurrency and
   dispatches longest-first. So the plan's "SPY surface ≤ 150 ms all cores" needs a per-expiry fan-out in
   `quote_surface.cpp` (point/knot passes in independent slots; calendar pass and ladder repair serial).
   L2d was redirected to build it (see its disposition). Found by L2d.
2. **Status-routed composites break calendar no-arbitrage across sources.** C5 has 24 convex and 2,511
   calendar breaks on dev200, 36 and 1,917 on dev200b; every calendar break pairs two different serving
   sources; C6 / C6d look the same. The plan's hard gate "0 violations on anything served" is therefore
   NOT met by C5 today, and a composite C7 needs a cross-source calendar certificate (or a composer that
   refuses a row whose calendar neighbour comes from another source). Found by L1b's per-class scorer.
3. **The index class is empty on dev200 / dev200b** (QQQ, IWM absent; SPY only in cohort5). G3 cannot
   gate the index class on the development cohorts; cohort5 is the only index evidence until a dev
   snapshot with QQQ/IWM is admitted (no new market data is allowed by the plan — rule on it).
4. **Per-class misses today** (L1b `shape-classes-20261014/RESULTS.md` §4-5): coverage misses in nearly
   every class for the slice policies; mega-cap median |IV − srVol| 18-61 bp vs ≤ 5; sparse MAE/spread
   0.24-0.31 on dev200b vs ≤ 0.15; SPY (cohort5) coverage 11-47%, MAE/spread 0.14-0.78, containment
   68-96%. Met: high-vol and mega-cap MAE/spread; 0 violations per class for graded / repair / ladders.
   The carry class is unstable (11 of 116 decisions flip with the supplied carry); CRWV classifies as
   high-vol, not sparse.
5. **AL default-path slowdown question: not resolved, no kernel change.** Quiet-ish A/B (unqualified,
   load mean 21.7%): accurate p50 tier2 − tier1 +3.84 us, 95% CI [−0.60, +10.70], A/A spread 4.30 us.
   Fast kernels are inlined into the Reference sweep/premium functions (sizes ~2x) — layout is the only
   plausible cause; outline the Fast dispatch only if a W4 pin confirms a delta. Side: tier 1 bulk ~5%
   faster than base (5/5 pairs).
6. **The vol_status normalisation exception is general.** Every default-path output through the
   quote-slice serve path now differs from the pre-ladder hashes only by the one column
   (`invalid_volatility,not_evaluated` → `unidentifiable`, blank price/vol); the strict normaliser
   (`knot-ladder-fix1-20261017-normalise/normalise_vol_status.py`) is the bridge. The raw hashes are
   the expected set from now on (table in the integrate-W0 report and the root sprint note).

## Deferred minors (open, by area)

- Knot-ladder: vol-bp MAE on the paired intersections is worse (+2.7 to +4.7%) while the median improves
  (reviewer NOTE; now in the lane's sprint note §7) — W2/W3 lanes should report vol-bp MAE beside USD.
  Reviewer could not run `c89d0739` on the fix-2 tip (permission classifier); the integrator's set is the gate.
- AL speed: NIT-1..4 from review v2 stand; NIT-5 kept A/B runs not position-balanced for tier 1 (note
  wording); the zone attribution and Fast-vs-Reference passes were not run (user directive) — L2b has no
  qualified Fast-vs-Reference number (resume-6 x1.8-2.3 unqualified stands); pricing/simd name-set gate
  was run by the integrator, not the fix lane.
- Deferred-tests: sprint note's "every refused-row record has no edge" is too broad — only a NaN box-end
  edge solve blanks `has_edge_`; narrow the wording when ledgering / in the L3b brief.
- Serve path (library): refused certification row + NaN box-end edge → `has_exercise_edge()` false
  (`expiry_record.cpp:293-301`), fails closed, geometry wrong → L3b.
- Composer: `tools/compose_predictions_by_status.py` now carries both `--restrict` and multi-value
  `--served-status` (PM merge resolution); no test covers the combination — add one in the next tool lane.
- Surface archive bench EOF minor, manifest cross-check nits (resume 6) — untouched.
- Batch pricing N2 (PerContract polish NoConvergence on 17/1,800 rows) — folded into L1c's scope, untested.
- L2d: `^atx-vol-quote-slice$` ctest filter excludes `QuoteSurfaceLadder.*` — register the ladder tests.

## Rulings made this sprint (also in progress.md; undo any you disagree with)

1. New SDD workspace `2026-10-18-vol-sota`; resume-5/6 workspaces are read-only history. vol-sprint
   broker workflow not used (same reason as resume 5/6). Holdout closed. Opus for every lane and review.
2. `-Jobs = clamp(floor(12/N),1,8)`; after the parallelism directive, six lanes at `-Jobs 2`.
3. Pre-flight plan conflicts: L3b owns only the channel-floor / certificate-floor region of
   `quote_slice.cpp` (L3a the calendar-clamp regions; integrate L3a first); L3c owns the record struct /
   header + archive record + report writer, L3b the `expiry_record.cpp` serve-path lines; L2b adds
   overloads / defaulted parameters only; L1's percentile fix is outside the CSV hash set (confirm).
4. al-speed review finish: the timing reproduction moved out of the review into a dedicated fix lane
   (host shared); the reviewer did the code-only 4b inspection. Fix round closed all four minors; the
   kernel outlining was allowed only if the A/B confirmed a slowdown AND was fingerprint-identical — the A/B
   came back "not resolved" (within A/A noise), so no kernel change; accepted as the default-path answer.
5. knot-ladder fix round 2 was tests-only (a finding needing a library change would have stopped for
   adjudication; none did).
6. User directive 1 (speed of progress): reviews reproduce gates + parity + one revert, never
   benchmarks; G0 keeps the full parity set but C6 reuses existing CSVs; L1 rescoped to L1-lite (no
   sampling profiler, no nine-class harness/baseline — W4); W1/W2 dispatched with more parallelism.
7. User directive 2 (parallelism): PM prebase branch `feat/vol-w2-prebase-20261018 @ 7cebbbd3`; the
   composer merge conflict resolved by the PM (union of `--restrict` and multi-value `--served-status`,
   receipt wording byte-identical for one status without routing; python ctest exit 0); six lanes based
   on it while the knot-ladder re-review ran; L1c refuse-only with `american.cpp` forbidden (L2b owns it);
   L2a new files + minimal `quote_slice.cpp` hunk; L2b IV routes forbidden (L1c owns them).
8. L2d scope extended to `quote_surface.cpp` (per-expiry fan-out of point/knot passes, deterministic
   slots, `QuoteSurfaceConfig::fit_workers`); L1 told to keep its `quote_surface.cpp` hunks slot-local.
9. The vol_status normalisation exception extends from graded-v1 to every default-path output through
   the quote-slice serve path (repair-v3 on cohort5 / dev200 / dev200b, R104 if the same pattern), under
   the same proof (strict normaliser PASS + normalised sha256 == expected); raw hashes become the
   post-ladder expected set.
10. Pending (recommended, not yet ruled): accept L2a's two deviations (no separate
    `solve_channel_vol_space`; no variance penalty) and L2d's three (B1/B2 parallel; `fit_workers` in
    `quote_slice.hpp`; quote-bench all-cores default) — each costs nothing if wrong beyond a one-line
    revert, and each is proven byte-identical by hash where it matters.

## Traps found this sprint

- The resume-6 `lane-review-knot-ladder` reviewer's wait loops outlive the agent; stop stray
  `until grep` shells after a hand-back.
- The permission classifier can refuse a reviewer's quote-bench parity run ("Interfere With
  Workloads") while allowing the implementer's; the integrator's full parity set is the gate either way.
- `ATX_VOL_FIT_WORKERS=N` caps the PricingExecutor pool: with N ≤ 3 one PricingExecutor test skips, so
  failed/skipped name sets differ by that name across caps — compare under the same cap (ledgered).
- The AL ladder-bench row names carry `/min_warmup_time…/manual_time` suffixes: a `$`-anchored filter
  matches nothing; anchor with a trailing `/`.
- A 4-arm Williams square ×2 with a 25% load-rejection rule needed 68 attempts for 28 kept runs on this
  host; do not plan micro-benchmark A/Bs on a shared host (user directive 1 makes this moot).
- Merging the knot-ladder lane into a root that already has the composite lane conflicts in
  `tools/compose_predictions_by_status.py` (`--restrict` vs multi-value `--served-status`); the union
  resolution is on the prebase branch and in the integrate-W0 merge.

## Next steps the evidence supports (in order)

0. **Re-aim W2 on the measured cost structure** (findings 1 and L2a's): the solve is already vol-space
   and negligible; the per-expiry cost is (i) American IV inversions repeated by every policy rung,
   (ii) the certification reprice, (iii) serial expiries. So: L2d's fan-out first (15.2 s SPY surface
   all-cores already, unqualified); then an inversion memo shared across rungs (L2c's rung memo, now the
   primary single-thread lever — key it on quote-set hash + AL profile like the carry memo); then L2b's
   cheaper certification (Fast screen → accurate confirm) and batched inversion via the shared geometry;
   L2a's cache stays as the carrier of the per-quote IVs (accurate kernel; the Fast-kernel cache changes
   the CSV and must stay opt-in with its own hash set).
1. Resume the checkpointed lanes with fresh implementers (brief + report as memory): L2d first
   (ladder hash identity, parity, remaining tests), then L2b (fingerprint after, identity tests), L1
   (parity, attribution runs), L1c (tests, counts), L2a (parity, tests, re-scoped per step 0). Light
   reviews (gates + parity + one revert) for each and for L1b (already DONE).
2. W2 integration order: L2d, L2b, L1, L1c, L2a (then L2c after L2a + L2b land); expect conflicts in
   `quote_surface.cpp` (L1 counters vs L2d fan-out — both lanes listed their line ranges),
   `quote_slice.cpp` (L1 scopes vs L2a dispatch), `quote_slice.hpp` (L2a config vs L2d `fit_workers`),
   `curve_fit.cpp` (L2d helper vs L2a prepass), `batch_chain*.hpp` (L1c vs L2b), CMake source lists.
3. G2 measurement = one keys-ON vs keys-OFF cohort5 run with per-chain p50 from `--report` (unqualified),
   SPY surface wall at `fit_workers` 1 and 0 once L2d's fan-out lands.
4. L2c (rung memo + pre-screen) after L2a + L2b; then L3a (`lane-knot-union-clamp-brief.md` in the
   resume-6 workspace) for the 1,038 displaced rows; L3b must carry the serve-path edge-geometry concern
   (`expiry_record.cpp:293-301`: a refused certification row with a NaN box-end edge reports no exercise
   edge — fails closed, geometry wrong) and L1c's refusal.
5. Before any C7: decide the cross-source calendar rule for composites (finding 2) — otherwise the hard
   0-violation gate cannot be met by any status-routed composite.
6. W4 pins only at the end, alone on a quiet host.

## Artifacts

`C:/atx-cache/quote-benchmark/`: review folders `L0a-knot-ladder-2e60186c-review-v1`,
`L0a-knot-ladder-085e7380-rereview-v1`, `L0b-al-speed-409ee7aa-review-v2`,
`L0b-al-speed-e8472fe2-rereview-v1`, `L0c-deferred-tests-083ce77a-review-v1`; lane folders
`L0a-fix2-knot-ladder-20261018-evidence`, `L0b-fix-al-speed-20261018-al-ab`,
`L0c-deferred-tests-20261018-parity`, `shape-classes-20261014` (nine class files, manifest
`ad323ccb`), `L1b-shape-cohorts-20261018-parity`, `integrate-W0-20261018-parity`, plus whatever the
checkpointed lanes list in their reports. Sprint workspace `C:/atx/.superpowers/sdd/2026-10-18-vol-sota/`
(git-ignored; ledger copied beside this file).

## Lane dispositions at the stop

| Lane | Final SHA | Disposition | Resume with |
| --- | --- | --- | --- |
| L0c deferred-tests | `083ce77a` | light review APPROVE → MERGED (W0) | — |
| L0b al-speed | `e8472fe2` | review v2 APPROVE `409ee7aa` → fix round 1 (A/B "not resolved", MINOR-1..4) → re-review APPROVE → MERGED (W0) | — |
| L0a knot-ladder | `085e7380` | fresh review BLOCK `2e60186c` (2 majors: untested containment checks + calendar key 3) → fix round 2 (6 tests) → re-review APPROVE → MERGED (W0) | — |
| L1b shape cohorts | `54e9488d` | DONE_WITH_CONCERNS, unreviewed | light review brief from the template → integrate |
| L1 fit timing (lite) | `a1003ec1` (WIP) | CHECKPOINTED, see table above | `lane-L1-fit-profile-brief.md` + report; `-Base a1003ec1` |
| L1c non-monotone refusal | `b0a15fb1` (WIP) | CHECKPOINTED, untested | `lane-L1c-al-nonmonotone-brief.md` + report; `-Base b0a15fb1` |
| L2a vol-space fit | `63400aeb` (WIP) | CHECKPOINTED, premise finding | `lane-L2a-vol-space-fit-brief.md` + report; `-Base 63400aeb`; re-scope per finding |
| L2b AL geometry | `cc97f387` (WIP) | CHECKPOINTED, untested | `lane-L2b-al-geometry-cert-brief.md` + report; `-Base cc97f387` |
| L2d scheduler (fan-out) | `3391b6a1` (WIP tests) | CHECKPOINTED, biggest measured win | `lane-L2d-scheduler-brief.md` + the redirect message in progress.md + report; `-Base 3391b6a1` |
| prebase | `feat/vol-w2-prebase-20261018 @ 7cebbbd3` | PM scaffold; same tree as the W0-integrated root; delete after W2 lanes are merged | — |

## Handoff commit and lease disposition

- This file and `2026-10-18-vol-sota-pm-ledger.md` are committed on `docs/vol-sota-handoff-20261018`
  (based on `05a7da3e`), not on the integration branch, because the integrate-W0 lane was still
  committing its docs in pool-1 when the user said "don't wait for it". Merge it after that commit.
- Root lease pool-1 (RunId `vol-20261018-root9`, keeper 4308) was LEFT LIVE for the integrator. After
  `lane-integrate-W0-report.md` ends with `## Outcome`, release it:
  `powershell -NoProfile -File <scratch>\lease-worktree.ps1 -Release pool-1 -RunId vol-20261018-root9`
  (the tree must be clean; if the integrator left it dirty, inspect `git status` before anything else).
  Investigate the owner state before any stale recovery; never `-RecoverStale` blindly.
- Every lane pool (3, 4, 5, 33, 34, 35, 36) was released by its lane; `lease-worktree.ps1 -Status` should
  show only pool-1 live among the `vol-20261018*` RunIds, plus the foreign dead leases (2, 8, 12-32).
- Lane branch tips at the stop: L1 `a1003ec1`, L1b `54e9488d`, L1c `b0a15fb1`, L2a `63400aeb`,
  L2b `cc97f387`, L2d `3391b6a1`, prebase `7cebbbd3`; W0 lanes merged (`083ce77a`, `e8472fe2`, `085e7380`).
