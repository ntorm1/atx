# Resume 5 handoff: stopped at user request, October 17, 2026

The user explicitly requested **stop and write a handoff**. The long-running goal
("complete the resume-4 handoff; build atx-vol into the fastest, most accurate
American equity options fitting / pricing / analytics library") is **paused**, not
complete. Resume only on a new user instruction.

## Start here on an authorized resume

- Integration branch: `feat/vol-resume4-20261016` (unchanged name), root tree
  `C:/atx-wt/pool-1`, head `1555b171` at the stop (the handoff commit SHA and the
  final lease disposition are appended at the end). Main `C:/atx` untouched
  (`e696e208`); no pushes, no PRs, no main merges.
- Read, in order: this file; the sprint working ledger
  `C:/atx/.superpowers/sdd/2026-10-17-vol-resume5/progress.md` (every ruling,
  every lane outcome, every deferred minor — the authoritative chronological
  record of this resume); the root sprint note
  `2026-10-16-common-model-resume4-sprint.md` (integration-log table, appended
  by every integrator this resume); the ledger lines dated 2026-10-17 in
  `docs/LEDGER.md` (`| integration |`, `| benchmark |`, `| trap |`, `| note |`).
  The resume-4 handoff (`2026-10-16-common-model-resume4-handoff.md`) is
  superseded except for its "Direction reset" and research sections.
- Lease a fresh root tree from the branch head with the ROOT tree's scripts:
  `powershell -NoProfile -File C:\atx-wt\pool-1\scripts\lease-worktree.ps1 -Branch feat/vol-resume4-20261016 -Base <head> -Agent pm-root6 -RunId vol-<date>-root6 -HeartbeatId vol-<date>-root6-hb -MaxPool 40`
  (run from inside any tree of the repo; `-MaxPool 40` is required, the default 20
  excludes pool-33..36). TRAP: the `C:/atx` and old pool copies of the lease script
  predate the build-infra fix; use the branch-head copy (this resume extracted it
  with `git show 7ca31c95:scripts/lease-worktree.ps1` + `lease-heartbeat-keeper.ps1`
  into a scratch folder and ran it from `C:\atx`). Release always with the
  acquiring `-RunId`. `-Preset vol-rel` for every configure/check/build/ctest,
  `vol-rel-avx2` only for timing pins; focused test targets only (never
  `atx-vol-tests`); `-Jobs = clamp(floor(12/N),1,8)` with N concurrent lanes.
  Python ctests need `-DPython3_EXECUTABLE="C:/Program Files/Python312/python.exe"`.
- Standing user rules (unchanged, binding): subagent-driven development with
  Opus subagents, PM keeps its own context small (briefs/reports as files),
  implementation before tests (no TDD), source/doc edits only via Edit/Write
  tools, compile-time hygiene tracked (PCH-off on every touched header),
  SpiderRock marks are the yardstick not the architecture, srPrc/srVol never
  inputs, every behaviour change opt-in with default parity proven by hash, no
  default promotion from development scores, holdout stays closed, nothing under
  `C:/atx-cache/quote-benchmark/` overwritten, every lane independently reviewed
  at its exact SHA before integration, a BLOCK gets one fix round and a fresh
  scoped re-review.

## Process used this resume (reuse it)

Workspace `C:/atx/.superpowers/sdd/2026-10-17-vol-resume5/`:
`SHARED-BRIEF.md` (rules + lease/build commands + report contract, prepended to
every dispatch), `lane-<id>-brief.md` (one per lane, falsifiable done criteria,
files in scope / forbidden), `lane-<id>-report.md` (lane writes; fix rounds
append "Fix round N" sections), `lane-review-<id>-brief.md` /
`lane-rereview-<id>-brief.md` (fresh exact-SHA reviewers; artifacts under
`C:/atx-cache/quote-benchmark/<lane>-<sha>-review-v1/`), `lane-integrate-brief.md`
(integrator merges APPROVED SHAs into pool-1, runs combined gates, appends
ledger + sprint-note rows; full parity hash set only at the final integration of
a wave), `lane-review-brief-template.md`, `progress.md` (ledger). Dispatch =
Agent tool, `general-purpose`, `model: opus`, prompt = "read these two files
first" + lane id + hard rules; fix rounds resume the same agent by name; the PM
never edits source. Builder lanes write their report file early (file-ownership
section), so watch for an Outcome line, not file existence.

## Integrated this resume (all on `feat/vol-resume4-20261016`)

| Merge | Lane SHA | What | Review |
| --- | --- | --- | --- |
| `ef5bc6c3` | review-cleanup `3465221c` | Markov one-sided-interval refusal, Jacobian retry, implied-forward Parity exclusions, time-knot helper split, definition texts | APPROVE (2 minor, 3 nit deferred) |
| `3fa27b00` | serve-path `03a5463b` | Records serve mode + edge gate (opt-in), near-ATM rebuild order (opt-in), record IV warm seed 404-601 ns, box-end solve reuse; e695a411's certification default changes ACCEPTED as correctness fixes | APPROVE (3 minor, 4 nit) |
| `53f728c7` | smooth-div `4e263541` | escrowed-LR / BBS / BBSR lattice schemes (opt-in), calibrated tests, minors M1-M3/N2-N4 | BLOCK (untested BBSR bound) → fix → APPROVE |
| `37addcd0` | slice-repair `5e30330c` | repair-inside-channel (opt-in), dominance gate (runs last, min two-sided count), dense mult_ceil 6, validate() ordering minor | BLOCK (gate without min count; untested calendar repair) → fix → APPROVE |
| `1050dc54` | composite-bench `bf3acaae` | composer `--restrict` (payers to dividend model first), tool-only | APPROVE |

Final wave-1 parity on binary `4829965a` (root `37addcd0`, every key OFF):
dev200 default `c89d0739`, graded-v1 `1348f63d`, R104 `6b129f64`, dividend
default `abc75955`, dev200b default `4b273b08` — all MATCH, plus `747d3e6c`,
`4c70d2bc`, `7fa3c415`, `2cfda793`, `5a9c1f5f`, `97fad2f0`, `14d9dc6d`.
Receipts `C:/atx-cache/quote-benchmark/integrate-1d-20261017-parity/`.
The composite merge changed no library source (binary still `4829965a`).

## Benchmark state (development cohorts; nothing promoted)

Coverage / price MAE USD / MAE÷spread. Source
`C:/atx-cache/quote-benchmark/composite-20261017/RESULTS.md` (sha256 bc01b889),
reproduced by the reviewer with the official scorer to 0 difference on 40 metrics.

| Model | dev200 (63,942 eligible) | dev200b (62,369 eligible, disjoint symbols, 15:00 ET) |
| --- | --- | --- |
| graded-v1 (per-expiry fitter, static policy) | 78.7% / 0.2343 / 0.109 | 87.2% / 0.2393 / 0.116 |
| slice-repair v3 (repair + dominance gate, opt-in) | 84.9% / 0.2250 / 0.114 | 90.5% / 0.2367 / 0.111 |
| C1 graded → m6 | 88.6% / 0.2356 / 0.151 | 98.5% / 0.2395 / 0.119 |
| C2 dividend → graded → m6 | 88.6% / 0.2332 / 0.150 | 98.5% / 0.2313 / 0.116 |
| C3 graded → X1 | 88.6% / 0.2359 / 0.152 | 98.5% / 0.2400 / 0.120 |
| C4 repair-v3 → m6 | 92.2% / 0.2278 / 0.145 | 98.6% / 0.2346 / 0.113 |
| **C5 escrowed-lr (payers) → repair-v3 → m6** | **92.4% / 0.2197 / 0.144** | **98.6% / 0.2245 / 0.110** |
| knot-ladder point policy (UNREVIEWED, lane run) | vol-served 85.97% (price-served 86.98%) / 0.223 / 0.108, intersection −2.75% vs graded-v1 | vol-served 89.30%, intersection −2.12% |

Exact-intersection deltas (dev200 / dev200b): C1→C4 −1.4% / −2.0%; C4→C5
−3.5% / −4.3% (payer rows); C1→C5 −4.9% / −6.3%. m6 vs X1 as fallback is a wash.
m6 rerun after the one-sided-seed fix: 0.3390 → 0.3363 on dev200 (18 snapshots);
X1 byte-identical. Unserved after C5: 4,849 dev200 rows (META/ASML 4,096-quote
cap, V/TSM parameter cap, 716 adjusted deliverables), 855 dev200b.
Smile cohort (five symbols, 10:30 ET): slice-repair v3 serves SPY 44 d (2 bp vs
srVol at 23 d in v2), MSTR 4/4, CRWV 1/41 strikes outside bid/ask (was 17/41),
RIVN 4/4; AAPL Nov 20 and SPY 23 d are served only by the knot-ladder lane's
containment-first calendar ordering (unreviewed). Figures
`plots-20261017/` and `plots-20261017-v2/`.
Dividend payers (escrowed-lr, opt-in): dev200 lattice chains certified/refused
117/63 → 149/31, payers MAE 0.197 → 0.147; dev200b 170/20 → 183/7 (HSBC lost),
0.209 → 0.186; ASML 11/13 chains certify, 0.448 → 0.271. Reported vol under
escrowed schemes is the escrowed stock's vol, not spot (documented; no
machine-readable `vol_convention` field yet).

Performance (all unqualified, loaded host): record IV warm 404 ns quiet / 459-601
loaded (target ≤ 500 ns met only quiet); ladder tick p50 65.7 us / p99 82.3 us;
record build 266 ms (84-104 ms with 3 workers, opt-in); batch pricing (lane
`fe6a516f`, BLOCKED on IV semantics, see below): scalar batch bit-identical to
the loop, 3.19x at 3 threads, opt-in AVX2 2.6x single / 6.4x at 3 threads;
AL kernel tier-1 bit-identical hoists committed (`a64dfb83`, numbers in the lane
note), cold AL 25.8 us → tier-2 not finished; ATXVFA fit archive 12.1 MB for the
dev200 graded-v1 set, open 5.4-6.9 ms, random ExpiryRecord read 400-500 ns median.
Nothing pinned; no "fastest" claim is justified yet.

## Lanes in flight at the stop

| Lane | Branch @ SHA | State | Next |
| --- | --- | --- | --- |
| knot-ladder | `feat/vol-knot-ladder-20261017` @ `901757ec` | DONE_WITH_CONCERNS; fresh review IN PROGRESS (`knot-ladder-901757ec-review-v1`) | finish/redo review → fix round if BLOCK → integrate with full parity (graded-v1 parity is modulo the documented one-column `vol_status` normalisation: fc277413→1348f63d on 564 rows, PM ruling) |
| al-speed | `feat/vol-al-speed-20261017` @ `a64dfb83` + WIP | tier 1 (bit-identical hoists) committed; tier 2 (opt-in fast profile) WIP, uncommitted at the stop signal | checkpoint commit (see dispositions below) → finish tier 2 with error bounds → review → integrate |
| batch-pricing | `feat/vol-batch-pricing-20261017` @ `fe6a516f` (+ fix WIP) | review BLOCK: both IV routes can report Ok inside a flat exercise interval (MAJOR-1 Lockstep stops on residual alone; MAJOR-2 PerContract no slope/residual check — the ledgered intrinsic trap); fix round 1 IN PROGRESS | finish fix (same refusal semantics as `american_implied_vol` / `ExpiryRecord::implied_vol`, tests that fail on revert, PerContract/Lockstep status agreement on the TSM chain) → scoped re-review → integrate. Its report lists 4 kernel-interface requests for al-speed (shared boundary/tables) needed to reach 3x single-thread |
| surface-archive | `feat/vol-surface-archive-20261017` @ `431b724f` (+ fix WIP) | review BLOCK: uint32 overflow in `expected_curve_words` (crafted file → ~16 GiB OOB read), `..\` path traversal in series segment names, bench never round-tripped, raw handle in bench tool, non-deterministic bytes (timing field); fix round 1 IN PROGRESS | finish fix → scoped re-review (re-run the 16 crafted files + 2 new overflow ones) → integrate |
| composite | merged | — | re-run with knot-ladder variant (C6 = ladder → m6) after that lane lands |

## Deferred minors (open, by area; details in progress.md and the review folders)

- Markov cleanup: tests for carry fallback causes InvalidMarket/NoModelYield/SliceRefused and Parity Contaminated; retry-probe ulp step near max_sigma; budget-blocked retry labelled jacobian_failure; quote-slice report still schema 2 after the fallback:invalid key rename.
- Serve path: no test for the refused-row (uncertified record) path; box-end solve reuse pinned only by the bench fingerprint; catch-up ticks 0.26-0.75 ms when the budget binds (> 300 us tick budget).
- Smooth dividends: `quote_surface.cpp` lattice-scheme one-liner (unreachable, untested); N2 route-check reorder untested; no machine-readable `vol_convention`; channel pass prices its floor at kIvMin so spliced-bbsr skips channel observations on long-dated high-carry chains (now `quote_slice.cpp` ~1276).
- Slice repair: gate undo/re-admission phases untested; a kept point fit can carry a stale calendar-repair "applied" record; constants (32-strike / 1-vol-point budgets, 0.05 gate, dense mult_ceil 6) were chosen on the five figure slices; point fits escape the market where the gate is off (pre-existing, now refused by knot-ladder containment); 6 nits.
- Knot ladder (lane-reported): 1,038 dev200 rows lost to displacement — 62 of 87 calendar refusals needed ≤ 0.5 half-spreads but the calendar repairs clamp at the slice's own nodes; follow-up = clamp on the union of knots; the ladder refuses the dominance-gate and knot-spline profiles (combination gap to resolve).
- Composite: policy-experiments-20261016 manifest fails only on IDE `.mypy_cache` files (deliberately not re-baselined); receipt counter differs under `--restrict`.
- Batch pricing: 3x single-thread not reachable without kernel sharing; sprint-note overclaims to restate after the fix.

## Rulings made this resume (also in progress.md; undo any you disagree with)

1. vol-sprint broker workflow not used (PM doc says it fails closed; user mandated Opus subagent-driven dev) — process only.
2. Holdout stays closed; composite bench deferred until after wave-1 integration (run once).
3. knot-ladder sequenced after slice-repair (both own `quote_slice.cpp`); slice-repair took the validate() minor.
4. Cleanup/serve-path/smooth-div integrations skipped the parity hash runs (reviewer-proven on the lane binary; docs-only or disjoint divergence); full parity ran once at wave-1d — all MATCH.
5. Serve-path review MINOR-1: e695a411's tick-path certification default changes accepted as correctness fixes (8-ulp certificate floor, max_cert_rel 5e-7, exercise Greeks at the floor, refused cert row → uncertified record); sprint note corrected.
6. Slice-repair fix round scope: majors + gate-bypass/diagnostics/disclosure/budget minors; confidence-ordered eviction and point-fit market escape deferred to knot-ladder; brief criterion "AAPL Nov 20 served" released for slice-repair and made a hard criterion of knot-ladder (met there, unreviewed).
7. Knot-ladder proceeds to review despite the ≤ 200 displacement criterion miss (principled containment-first rule, mechanism identified).
8. al-speed owns the AL kernel files; batch-pricing calls the public kernel API only and requests interfaces via its report.
9. `.mypy_cache` dirs the IDE wrote under two 20261016 cache folders were removed (pure mypy JSON, verified); the policy-experiments manifest is not re-baselined.
10. Composite integration skipped parity (tool-only, verified no src/include change).

## Traps found this resume (ledgered where marked)

- RTK-wrapped Bash `diff` reported two different files as identical with exit 0 — use sha256 or `rtk proxy diff` for parity claims (ledgered).
- Stale lease script: pool-33's `scripts/` were reset to 5e978fed on release; take the script from the branch head.
- `sed` with Windows paths containing `\2026` in the replacement — backreference error left an empty brief; write briefs with the Write tool.
- Subagent transcript files under the session `tasks/` folder can be 0 bytes; progress is visible from lane branch commits, pool dirtiness (`git -C C:\atx-wt\pool-N status --short`) and new cache folders.
- Builder lanes write their report file early; monitor for an Outcome line.
- Reviewer `git checkout --` can be refused by the permission system; reviewers undo temporary reverts with the inverse Edit and prove the blob id.

## Next steps the evidence supports

0. Land the four in-flight lanes: finish the batch-pricing and surface-archive fix rounds (scoped re-reviews), redo/finish the knot-ladder review, finish al-speed tier 2; integrate with the full parity set on the final binary; then C6 (ladder → m6) on both cohorts.
1. Union-of-knots calendar clamp in `quote_slice.cpp` (recover most of the 1,038 displaced dev200 rows); resolve the ladder vs dominance-gate/knot-spline profile combination.
2. AL kernel interfaces for batch sharing (the 4 requests in the batch-pricing report) → 3x single-thread batch; then cold AL ≤ 15 us via the opt-in fast profile with bounded error.
3. Perf qualification on a QUIET host: pin chain-refresh, record IV, batch throughput, AL cold, archive open/read; until then every number is unqualified.
4. Declare a candidate default set from C5 (+ ladder) only after the held-out run under the oracle Ratchet rules; never tune on it.
5. `vol_convention` field in the report schema; the Markov/serve-path/smooth-div deferred tests above.

## Lane dispositions at the stop (all pools released; every lane branch tip committed)

| Lane | Final SHA | Disposition | Resume with |
| --- | --- | --- | --- |
| knot-ladder | `901757ec` | review INCOMPLETE (`knot-ladder-901757ec-review-v1`): verified parity, normalisation, cohort (AAPL Nov 20 168/168, SPY 23 d 334/334, 0 violations), order independence (34 permutations), containment revert; NOT verified dev200 spot-check, ctest/pytest/PCH-off reruns, concerns (a)/(b). Open: MAJOR knot branch of the ladder repair rung untested; MAJOR no bench test produces `ok_price_only`; MINOR price-only row can become vol-served with `price_only` off (0 occurrences, no key); MINOR normalisation-script check weaker than claimed, string mappings partly tested, undocumented `ladder_fallback=None`. | fix round 1 (resume implementer, findings verbatim) → FRESH full review → integrate (full parity; graded-v1 modulo the one-column normalisation) |
| al-speed | `c2bf8bb8` (tier 1 `a64dfb83`, tier 2 `5a0a4d21`, docs/WIP note `c2bf8bb8`) | code complete, timing pass unfinished. Tier 1 bit-identical (kernel fingerprint 53c4df68 at base/tier1/tier2; c89d0739, 4b273b08, 1348f63d MATCH), bulk p50 17.7 → 16.8 us. Tier 2 opt-in `AlOpts::profile = AlProfile::Fast` / `al_fast_profile(opts)`: max price error 8.0e-13, IV 0.0054 bp, Greeks p99 ≤ 5.2e-7 (volga) over grid + 6,000 random + 10,694 dev200 contracts; 6 `AlFastProfile.*` tests; bulk 18.5 → 7.4 us p50 in ONE run at 30% load (15 us target met once, interleaved A/B not run). Gates: pricing/simd failures equal base (2 pre-existing), certification 52/52, fitting 48 / backtest 9 names byte-equal, hygiene 21 TUs. Not run: interleaved A/B, per-tier Greeks timing, bench self-check, candidate baseline JSON. Concern: fitting does not carry the profile yet; the carry-memo key must include it before fitting can use Fast. | finish the timing pass (A/B) → fresh exact-SHA review → integrate; then the batch-lane kernel interface requests |
| batch-pricing | `583e3a75` (fix `5da11835` + WIP note) | fix round 1 COMPLETE (checkpointed): both IV routes use `american_implied_vol`'s screen incl. the zero-vol band; Lockstep Ok only with residual + evaluated-American slope above floor + bracket/correction within sigma_tol (row 1295 → VegaFloor); PerContract polishes on the cold map, flat slope → VegaFloor, up to 4 polish steps then NoConvergence (deep-ITM put +2e-7/+1e-6 and call +1e-6 refused); UB fixed; tests fail on fe6a516f; chain-batch 27/27, PricingExecutor 24/24, PCH-off 4 files, parity c89d0739 + 4b273b08. Greeks hashes unchanged; new IV hashes chain 22ce29207191495b / c0dd99622b63d825, dev200 5471894473c1a004 / 144a6b11b337506e; route status disagreements 1 → 0. Timing bench stopped (partial output). NEW KERNEL FINDING (ledger candidate): `andersen_lake` is not monotone in vol on long-dated deep-ITM puts, so such a target can have two valid IV answers. | scoped re-review of `fe6a516f..583e3a75` (finish the timing bench first if the note's numbers must be final) → integrate |
| surface-archive | `c0545fe9` (WIP) | fix round 1 PARTIAL: overflow MAJOR partial (64-bit node-count check, size_t index sums, copy-on-upgrade size check; crafted-file test per curve kind + revert paste missing); path traversal partial (manifest entries must carry the writer's own `seg-<sequence>.atxvfa` name, checked at manifest open / segment open / pre-compaction delete; tests for open / CLI verify / compaction missing); bench round-trip NOT started; bench handle cleanup NOT started; byte determinism DONE + tested (channel solve time out of the fit record; optional section 40 via `record_timings(true)`; ARCHIVE_FORMAT.md update + fixed bench `created_unix_ns` + second-run sha256 compare still to do). fit-archive tests 44/44; ctest/PCH-off/bench not rerun. | finish the four open items → scoped re-review (re-run the 16 crafted files + new overflow/traversal ones) → integrate |

## Artifacts

`C:/atx-cache/quote-benchmark/`: lane folders `<lane>-20261017-*`, review folders
`<lane>-<sha>-review-v1` / `-rereview-v1`, parity `integrate-1d-20261017-parity`,
composite `composite-20261017`, plots `plots-20261017`, `plots-20261017-v2`.
Every results note and review JSON carries SHA256s; nothing under that root may
be overwritten. Sprint workspace: `C:/atx/.superpowers/sdd/2026-10-17-vol-resume5/`
(git-ignored; its ledger is copied to `2026-10-17-resume5-pm-ledger.md` beside
this file).

## Handoff commit and lease disposition

- Handoff committed on `feat/vol-resume4-20261016` as `26673577` (this file +
  `2026-10-17-resume5-pm-ledger.md`); this closing section is a second docs
  commit on top of it — the branch head at the stop is that second commit.
- Root lease pool-1 (RunId `vol-20261017-root5`, keeper 18700) released by the PM
  right after the closing commit; every lane pool (3, 4, 33, 34, 35, 36) was
  released by its lane. `lease-worktree.ps1 -Status` should show no live
  `vol-20261017-*` lease. Lane branch tips: knot-ladder `901757ec`, al-speed
  `c2bf8bb8`, batch-pricing `583e3a75`, surface-archive `c0545fe9`,
  composite-bench `bf3acaae` (merged).
