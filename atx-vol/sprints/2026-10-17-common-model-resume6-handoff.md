# Resume 6 handoff: stopped at user request, October 17, 2026

The user explicitly requested **stop here and write a handoff file, a SpiderRock parity status
summary, and updated vol-smile graphs**. The long-running goal ("continue building atx-vol into
the fastest, most accurate American equity options fitting / pricing / analytics library") is
**paused**, not complete. Resume only on a new user instruction.

## Start here on an authorized resume

- Integration branch: `feat/vol-resume4-20261016` (unchanged name), root tree `C:/atx-wt/pool-1`,
  head `0f643107` at the stop (the handoff commit SHA and the lease disposition are appended at the
  end). Main `C:/atx` untouched (`e696e208`); no pushes, no PRs, no main merges.
- Read, in order: this file; `2026-10-17-spiderrock-parity-status.md` (beside it — where we stand
  against SpiderRock marks); the sprint working ledger
  `C:/atx/.superpowers/sdd/2026-10-17-vol-resume6/progress.md` (every ruling, every lane outcome,
  every deferred minor — copied beside this file as `2026-10-17-resume6-pm-ledger.md`); the
  resume-5 handoff `2026-10-17-common-model-resume5-handoff.md` (process, standing rules,
  benchmark state — still accurate except where this file supersedes it); the root sprint note
  `2026-10-16-common-model-resume4-sprint.md` (integration-log table, appended by every
  integrator); ledger lines dated 2026-10-17 in `docs/LEDGER.md`.
- Lease a fresh root tree from the branch head with the ROOT tree's scripts:
  `powershell -NoProfile -File C:\atx-wt\pool-1\scripts\lease-worktree.ps1 -Branch feat/vol-resume4-20261016 -Base <head> -Agent pm-root7 -RunId vol-<date>-root7 -HeartbeatId vol-<date>-root7-hb -MaxPool 40`
  (`-MaxPool 40` required). The pool-1 `scripts\` copy is the branch-head copy now (the `C:/atx`
  copy is still stale). Release always with the acquiring `-RunId`. `-Preset vol-rel`
  everywhere, `vol-rel-avx2` only for timing pins; focused test targets only; `-Jobs =
  clamp(floor(12/N),1,8)` with N concurrent lanes; Python ctests need
  `-DPython3_EXECUTABLE="C:/Program Files/Python312/python.exe"`.
- Standing user rules (unchanged, binding): subagent-driven development with Opus subagents (user:
  "sub agent opus 5.5 driven dev"), PM keeps its own context small (briefs/reports as files),
  implementation before tests (no TDD), source/doc edits only via Edit/Write tools, PCH-off
  hygiene on every touched header, SpiderRock marks are the yardstick not the architecture,
  srPrc/srVol never inputs, every behaviour change opt-in with default parity proven by hash, no
  default promotion from development scores, holdout stays closed, nothing under
  `C:/atx-cache/quote-benchmark/` overwritten, every lane independently reviewed at its exact SHA
  before integration, a BLOCK gets one fix round and a fresh scoped re-review.

## Process used this resume (reuse it)

Workspace `C:/atx/.superpowers/sdd/2026-10-17-vol-resume6/`: `SHARED-BRIEF.md` (rules + lease/build
commands + report contract; root SHA line must be refreshed to the new head), `lane-<id>-brief.md`
/ `lane-<id>-report.md` per lane, `lane-review-brief-template.md`, `lane-integrate-brief.md`
(RunId line must be refreshed to the new root RunId), `progress.md`. Dispatch = Agent tool,
`general-purpose`, `model: opus`, prompt = "read these two files first" + lane id + hard rules.
Lanes lease their own pool with the pool-1 lease script; the integrator works in pool-1 without
leasing. Briefs are written with the Write tool (Bash heredocs failed under the RTK hook once this
resume; `cat >>` appends worked). Ready-made briefs for the next lanes already exist in the
workspace: `lane-review-knot-ladder-brief.md` (fill REVIEW_SHA), `lane-knot-union-clamp-brief.md`
and `lane-al-batch-interfaces-brief.md` (fill ROOT_SHA), `lane-deferred-tests-brief.md`.

## Integrated this resume (all on `feat/vol-resume4-20261016`)

| Merge | Lane SHA | What | Review |
| --- | --- | --- | --- |
| `49977fda` (root `022e9727`) | batch-pricing `583e3a75` | chain batch pricing API: Greeks + IV, PerContract scalar (bit-identical to the loop) and opt-in Lockstep / LanedAvx2 routes; fix round unified the IV refusal rule on both routes (zero-vol band screen, evaluated-American slope floor, bracket/correction within sigma_tol) | BLOCK (fe6a516f) → fix → scoped re-review APPROVE (2 new minor, 1 nit) |
| `7aa883ce` (root `0f643107`) | surface-archive `6d2f6910` | ATXVFA versioned, checksummed, mmap-able fit archive (opt-in) + CLI + bench; fix round closed the u32 size overflow (crafted files refused at Full/Paranoid), segment-name path traversal, bench round trip from published bytes, RAII handles, byte-deterministic archives | BLOCK (431b724f) → fix → scoped re-review APPROVE (1 new minor, 2 nits) |

Parity for both merges was skipped by ruling (opt-in features; no default-path file in either
diff; reviewers proved `c89d0739` / `4b273b08` on the lane binaries). **The full parity set
(`c89d0739`, `1348f63d`, `6b129f64`, `abc75955`, `4b273b08` + the keys-ON set) is owed once at the
final integration of this wave**, i.e. after knot-ladder and al-speed land.

## Benchmark state

Unchanged from the resume-5 handoff §"Benchmark state" (C5 92.4% / 0.2197 dev200, 98.6% / 0.2245
dev200b; knot-ladder point policy unreviewed), restated with the SpiderRock reading in
`2026-10-17-spiderrock-parity-status.md`. New this resume (all unqualified, loaded host):
- Batch pricing: Scalar t3 3.99x the per-contract loop (reviewer rerun; lane smoke 2.26-3.32x);
  post-fix PerContract IV t1 0.73x the loop; 3x single-thread still unmet (kernel sharing needed).
- AL kernel: Fast profile bulk p50 8.5-9.6 us in every one of 6 interleaved rounds (15 us target
  met on that rung); Reference bulk 15.3-22.5 us; Fast vs Reference x1.8-2.3 price, x1.9-2.8
  Greeks; tier-1 speedup unresolvable on this host (x0.99-1.17) — resume-5's "17.7 → 16.8 us" is
  WITHDRAWN; tier-2 Reference (default) path measured slower than tier 1 on the accurate rung in 5
  of 6 rounds (median p50 141.6 vs 137.0 us) — the reviewer was asked to probe this.
- Archive: open Full 14.9 ms median, record lookup + price 500 ns median / 1.3 us p99; archive
  bytes identical across runs (sha256 50cac121...) with timings off and created time pinned.

## Lanes in flight at the stop

| Lane | Branch @ SHA | State | Next |
| --- | --- | --- | --- |
| knot-ladder | `feat/vol-knot-ladder-20261017` @ `2e60186c` | fix round 1 DONE_WITH_CONCERNS: all 6 findings fixed with revert evidence; gates green; keys-OFF parity MATCH; keys-ON outputs identical to bench-v2; AAPL Nov 20 + SPY 23 d served | FRESH full review (brief ready: `lane-review-knot-ladder-brief.md`, fill REVIEW_SHA) → fix round if BLOCK → integrate (graded-v1 parity modulo the PM-ruled one-column `vol_status` normalisation) |
| al-speed | `feat/vol-al-speed-20261017` @ `409ee7aa` | code + timing pass DONE_WITH_CONCERNS; fresh review INCOMPLETE at the stop (`al-speed-409ee7aa-review-v1`): gates, fingerprint, 3 parity hashes, scope, carry memo all verified; NOT verified: base builds, Fast error-bound rerun, timing A/B + default-path probe, hygiene, revert spot-checks, bench self-check; 0 major, 2 minor (A/B confounded — fixed arm order, 12 vs 3 rows; timed the 104-contract ladder grid not the dev200 kernel set, undeclared), 3 nits | finish the review (reuse the folder evidence) → fix round for the 2 minors (A/B rerun on a quiet host, alternating order, `accurate` row; declare the timing set) → integrate |
| deferred-tests | `feat/vol-deferred-tests-20261017` @ `8d6cafa1` (WIP commit on `022e9727`) | tests-only; items 1-4 written and passing (8 tests, 4 test files, no library change); items 5-6 not started; revert runs / ctests / PCH-off / parity / sprint note not run | finish items 5-6 + gates → light review → integrate (parity by diff list + one `c89d0739`) |
| smile-plots | read-only on root `022e9727` | running at the stop in pool-3 (Opus, read-only); its result and the `plots-20261017-v3` folder state are appended in the closing section below | — |
| knot-union-clamp, al-batch-interfaces | not started | briefs drafted (fill ROOT_SHA) | dispatch after knot-ladder / after al-speed integrate |

## Deferred minors (open, by area; details in progress.md and the review folders)

- Batch pricing: N2 PerContract 4-step polish exits NoConvergence on 17 of 1,800 well-posed
  long-dated deep-ITM rows that Lockstep solves, path untested (typed refusal, never a wrong Ok);
  N3 nit upward Lockstep slope probe never counted "resolved"; post-fix timing bench never rerun
  in full. → `lane-al-batch-interfaces-brief.md` carries N2.
- Surface archive: bench tool's unbuffered-read loop throws on the extra EOF read when the archive
  size is not a multiple of 512 (shipped bench aborts with `--ladders off` on dev200; fix: loop
  until `total < bytes`); manifest counts / snapshot range not cross-checked against the segment;
  bench error paths untested; pre-existing storage-slice 527/530 failures (ledgered, not this lane).
- AL speed: tier-2 default-path overhead question (above); base/tier-1 Greeks timed with
  `atx-vol-american-bench` ns/op (no single-op Greeks row in those builds).
- Serve path (found by deferred-tests): when a record's certification row is refused, a NaN
  box-end edge also turns `has_exercise_edge()` false — fails closed (record uncertified) but the
  edge geometry is wrong (`expiry_record.cpp:292-299`).
- Everything listed in the resume-5 handoff §"Deferred minors" that this resume did not touch
  (Markov cleanup items 5-6 of the deferred-tests brief; serve-path; smooth-div `vol_convention`;
  slice-repair gate phases + stale calendar-repair record → `lane-knot-union-clamp-brief.md`;
  knot-ladder displacement → same brief; composite manifest `.mypy_cache`).

## Rulings made this resume (also in progress.md; undo any you disagree with)

1. New SDD workspace `2026-10-17-vol-resume6` with its own ledger and report files; the resume-5
   workspace is read-only history referenced by every brief.
2. vol-sprint broker workflow not used (fails closed per the PM doc; user mandated Opus
   subagent-driven dev) — process only. Holdout stays closed.
3. knot-ladder fix round 1 given to a FRESH implementer carrying the prior brief, report and
   incomplete review as files (the resume-5 agent no longer existed).
4. al-speed carry-memo: lane had to either key the fitting carry memo on the AL profile or prove
   fitting cannot build a Fast `AlOpts`; it chose the key (`d77beb03`, test fails on revert).
5. batch-pricing scoped re-review proceeded without the full timing bench; the sprint note had to
   label every timing number smoke-run/unqualified (reviewer found one such row — N1 — fixed by
   the integrator as a docs-only qualification).
6. integrate-3a and integrate-3b skipped the parity hash runs (opt-in features, no default-path
   file in the diffs, reviewer-proven on lane binaries); full parity once at the final integration.
7. Deferred-tests lane dispatched as tests-only with label/schema/`vol_convention` changes
   excluded (they alter default output) and slice-repair items routed to the knot follow-up lane.

## Traps found this resume (ledger the first two if not already)

- PowerShell `bash -c` resolves to WSL bash, not Git Bash, and executed a parity CSV as a script
  (al-speed lane; killed, files verified unchanged). Use the Bash tool directly or PowerShell cmdlets.
- Bash heredoc authoring of multi-file briefs failed with a quote-parse error under the RTK hook;
  `cat >> file <<'EOF'` single-file appends worked; briefs are written with the Write tool.
- In-place temporary reverts can be permission-denied for an implementer yet allowed for the
  reviewer in the same session (surface-archive); an old-binary comparison alone was judged
  insufficient revert evidence — the isolated revert closed the gap.
- `andersen_lake` non-monotone in vol on long-dated deep-ITM puts (VERIFIED, ledgered) —
  `american_implied_vol` can return Ok 0.02-0.18 vol off there.

## Next steps the evidence supports

0. Land the in-flight lanes: fresh full review of knot-ladder → integrate; finish/redo the
   al-speed review (incl. the default-path timing probe) → integrate; finish deferred-tests
   → light review → integrate; then the FULL parity set on the final binary and C6 (ladder → m6).
1. `lane-knot-union-clamp-brief.md`: calendar repairs clamp on the union of knots (recover most of
   the 1,038 displaced dev200 rows), slice-repair deferred tests, ladder vs dominance-gate /
   knot-spline composition.
2. `lane-al-batch-interfaces-brief.md`: kernel correctness first (non-monotone region refusal or
   fix; N2), then the four sharing interfaces → 3x single-thread batch.
3. Perf qualification on a QUIET host (pin chain-refresh, record IV, batch, AL cold/Fast, archive
   open/read); fix the archive bench EOF minor on the way.
4. Candidate default set from C5 (+ ladder) only after the held-out run under the oracle Ratchet
   rules; never tune on it.
5. `vol_convention` field (opt-in schema), remaining Markov / serve-path deferred tests.

## Artifacts

`C:/atx-cache/quote-benchmark/`: review folders `batch-pricing-583e3a75-rereview-v1`,
`surface-archive-6d2f6910-rereview-v1`, `al-speed-409ee7aa-review-v1` (INCOMPLETE if so marked
in its review.md); lane folders `al-speed-finish-20261017-*`, `knot-ladder-fix1-*` (if any);
figures `plots-20261017-v3/` (this stop). Sprint workspace
`C:/atx/.superpowers/sdd/2026-10-17-vol-resume6/` (git-ignored; its ledger is copied to
`2026-10-17-resume6-pm-ledger.md` beside this file).

## Lane dispositions at the stop (all lane pools released; every lane branch tip committed)

| Lane | Final SHA | Disposition | Resume with |
| --- | --- | --- | --- |
| knot-ladder | `2e60186c` (code `b627d0cc`, tests `81f5a69d`, `90d12701`, docs `2e60186c`) | fix round 1 DONE_WITH_CONCERNS. MAJOR-1 knot rung test, MAJOR-2 end-to-end `ok_price_only` bench tests, MINOR-1 PriceOnly rows final (never vol-served), MINOR-2 strict normaliser, MINOR-3 mapping tests, MINOR-4 `ladder_fallback=None` documented + tested — each with failing-revert and passing-restore pastes. Gates quote-slice 113/113, IF 27/27, ctest 5/5 (at `90d12701`), pytest 61 + 97 (at `81f5a69d`), PCH-off 9 TUs. Parity keys OFF `c89d0739`, `4b273b08` MATCH; normaliser `1348f63d` (564) / `4c70d2bc` (429) / `7fa3c415` (2). Keys ON 6/6 hashes identical to bench-v2; AAPL Nov 20 168/168, SPY 23 d 334/334, 0 violations. Concerns: gate block not rerun at the docs-only tip; PCH-off not rerun on `quote_surface_test.cpp` after `90d12701`; commit trailer says Opus 5.5. Outputs `knot-ladder-fix1-20261017-{bin,scripts,parity,normalise,bench}`. | FRESH full review: `lane-review-knot-ladder-brief.md` (REVIEW_SHA already `2e60186c`) → fix round if BLOCK → integrate with the full parity set |
| al-speed | `409ee7aa` (code `d77beb03` carry-memo key, `5a0a4d21` tier 2, `a64dfb83` tier 1) | lane DONE_WITH_CONCERNS; fresh review INCOMPLETE (`al-speed-409ee7aa-review-v1`): verified gates (pricing/simd 2/900 = base, certification 52/52, fitting 48 / backtest 9 byte-equal), fingerprint `53c4df68`, parity ×3, scope, carry memo; NOT verified: base `24e01644` builds, Fast error-bound rerun, timing A/B + default-path probe, hygiene, revert spot-checks, bench self-check. Findings 0 major, 2 minor (confounded A/B; undeclared timing set), 3 nits. | finish the review (reuse its folder) → fix round for the 2 minors (quiet-host alternating-order A/B on the `accurate` row, declare the timing set) → integrate |
| batch-pricing | `583e3a75` | MERGED `49977fda` (root `022e9727`); deferred N2/N3 → `lane-al-batch-interfaces-brief.md` | — |
| surface-archive | `6d2f6910` | MERGED `7aa883ce` (root `0f643107`); deferred bench EOF minor + 2 nits | — |
| deferred-tests | `8d6cafa1` (WIP on `022e9727`) | items 1-4 written and passing (8 tests in 4 test files, no library change); items 5-6 not started; revert runs / ctests / PCH-off / parity / sprint note not run; concern: refused certification row leaves `has_exercise_edge()` false via a NaN box-end edge (`expiry_record.cpp:292-299`, serve-path owner). | finish 5-6 + gates → light review → integrate |
| smile-plots | read-only on root `022e9727` | running at the stop in pool-3 (Opus, read-only); its result and the `plots-20261017-v3` folder state are appended in the closing section below | — |

## Handoff commit and lease disposition

HANDOFF_COMMIT
