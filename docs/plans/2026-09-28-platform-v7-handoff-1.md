# Platform v7 sprint -- handoff 1 (2026-09-28, end of the first PM session)

Written for: the next parent (PM) agent on this repo. It assumes the reader knows the mega-alpha v6 process
(docs/plans/2026-09-28-mega-alpha-v6-handoff-5.md) and picks up here.

## 0. Where things are

| item | value |
|---|---|
| root worktree | C:/atx-wt/pool-2, branch feat/aes-codex-integration-20260925, HEAD 5dadcafe (clean) |
| local main | C:/atx main at c0dc376f (v6/v6.1 merge). Everything below is NOT yet in main. Merge is the owner's action: `git -C C:/atx merge --no-ff feat/aes-codex-integration-20260925 -m "Merge ...: platform v7 wave 1-3"` |
| sprint dir | .superpowers/sdd/platform-20260928/ (ledger progress.md newest-first; briefs, reports, reviews; v7-prereg.md; plan-v7.md; code-review-v7.md; literature-v7.md; library-v7-draft.md) |
| build tags used | v7-rel0 (equity-rel A/B), v7-l3, v7-l1, v7-l4, v7-2, v7-2a, v7-2b, v7-w4, v7-w1, v7-f1, v7-w2, v7-w1b, v7-f2. Next free: v7-3. Receipts build-equity/mega-<tag>-receipt.json. build-equity/mega-build.ps1 (gitignored) gained `-Preset` |
| exes (Debug, build-equity/bin) | atx-equity-strategy-ic (L1 cache v2 + W2 ops), atx-equity-strategy-targets (L3 decide, L4 v7 rules, W4 orders/reconcile, W1 spo-v1), atx-equity-strategy-risk (L4 + F1 + F2) |
| baseline book | mega-nav-v61u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247: S2 net +1.239, gross SR 1.594, cost 13.55 bps/$, tau .038; cell-count DSR .9032 (N 33), effective-N DSR .9512 (N_eff 2-5), Lo .5153; PSR(0) .978, MinTRL 95% 501 sessions |
| freeze gate | UNMET (pre-registered: S2 net >= 1.0 AND mechanics AND cell-count DSR >= .95). Effective-N DSR is reported beside it, never substituted (R5.2) |
| trial accounting | TRAIN construction cells 33 (29 v6 era + C1-C3 + spo-v1, all four rejected); ledger build-equity/trials.jsonl 33 lines; validation trials 2 spent (2023-2024, read twice); 2025+ reserved; no per-candidate VAL statistic read |

Binding rules carried forward unchanged (from the /goal prompt of 2026-09-28 and mega-alpha): root alone builds
(mega-build.ps1) and runs real data (scripts/run_bounded_research.py --seconds 180 --max-rss-mib 1536 --min-free-mib 512);
tree must be clean before every real-data run (children write reports into pool-2's sprint dir asynchronously -> commit
with `git add -f .superpowers/sdd/platform-20260928` immediately before every run; two runs were refused today for this);
children own pool worktrees, never build, never run real data, never spawn subagents, trailer
`Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`, task-TN-report.md, short replies; TRAIN 2020-2022 only; any
2023-2024 read = validation trial #3 (owner gate U1); never touch C:/atx except read-only (atx-db docs and stage exports);
no pushes/warehouse/broker; every ruling declared before the measurement it could bias; Appendix A block on every result.
Never kill processes of the atx-db session (it owns C:/atx and atx-db).

## 1. What landed this session (all merged into pool-2, all accepted on real TRAIN data unless marked)

Research: P1 code review (code-review-v7.md, 21 findings, measured stage timings), P2 literature (literature-v7.md, 7
topics, ranked build list), plan-v7.md (decisions D1-D10), R1 adversarial review (task-R1-review.md: 1 I, 7 M, 16 m; all I/M
fixed or disclosed), P3 library v7 draft (library-v7-draft.md).

| lane | what | acceptance evidence |
|---|---|---|
| L1 | SHA-NI sha256; content-keyed IC candidate cache v2 (dsl sha + per-field payload sha), `--cache-legacy-fields`, `--cache-report`; short partial-file names (Windows 259-char limit) | v6.1 u pass 76-105 s -> 23.9 s (38/39 hits), w 31.5 -> 19.3 s (39/39), outputs byte-identical to mega-v61-train-u-1 / v61w |
| L2 | scripts/research_cycle.py (plan/lock/run/status; hard-stop on refused receipt), specs/v61.json, prepare_research_fields.py `--reuse`, studies/backtest_integrity.py + nav_summ.py (trial ledger, ONC effective-N DSR, CSCV PBO, PSR/MinTRL) | `run specs/v61.json --suffix r7` reproduced all 10 v6.1 CSVs pin-for-pin; reuse 40/41; refusal stops; stats deterministic |
| L3 | `--emit-holdings`, `decide` verb (targets from actual positions via the replay's own code), atx.book-deploy/v1 manifest, health checks | flag off identical; decide at 3 TRAIN dates parity 0 |
| L4 | atx-risk-v1 (market + FF49 + 11 styles, EWMA 84/504, NW, VRA, Bayesian specific) + bias harness; cost model v2 (S2-KO, S2-FIM) + capacity curve by replay; aim-partial-v6 (kappa, band exponent, regime rate) | bias factor family b .995; S2-KO 1.269 / S2-FIM 1.347 vs S2 1.238; capacity net SR 1.27/1.24/1.18/1.05/0.91 at .5/1/2/4/8x $1bn; v6 identity with kappa 0 / exponent 0 / clip 1,1 |
| W1 | spo-v1 cost-aware optimiser (FISTA around the GP aim, factor risk + cost v2), 12 gtests | tests green; the pre-registered cell FAILED (see §3) |
| W2 | DSL ops 89-104 (ts_topk_mean, bucket->Group, group_cross, ts_resid_on/ts_beta_on, cs_resid_on, ts_count_increases, *_mp min-periods, pack2/3); golden digests pinned; checker POLICY_OPS (vec_sum, max, sign) | v6.1 IC pass on a fresh cache byte-identical; AlphaLit 23/23 |
| W3 | atx-impl/tools/alpha_report_card.py, book_monitor.py (M1-M4), fitter WorkStore, cycle phases card/monitor (specs/v61-ops.json) | 39 cards in 22 s, runner mismatches 0; fitter computed 39 -> reused 39 in 0.5 s, admission candidate-identical |
| W4 | binary holdings writer (f64 + index), share orders (lots, min notional), `reconcile` verb with corporate actions, decide pins/freshness/TC band | flag-on 1.21x wall; decide 1.8-2.1 s parity 0; reconcile 0 self-breaks, 3 planted breaks caught + split explained |
| F1 | R1 fixes I-1 (v6 capacity pricing), M-5 (bias drop counters + 5% refusal), M-6 (reuse keyed on producing code), M-7 (pitch sig_corr DSL sha) | tests green; risk verb exposes that random portfolios loaded 28% on unforecast factors (-> F2) |
| F2 | structural forecast for factors with < 63 obs (class-average blend n/63) | risk verb: 11 structural factors, random family ok, dropped 0. Risk model dir with per-date exposures: build-equity/v7-f2-risk (manifest sha 17f9328f...138bbe9f) |
| W5a | research_fields_sec.py: 14 PIT fields from atx-db earnings calendar, Form 4 (CMP routine/opportunistic), 8-K | lo1-fields-v8 (55 fields) 56 s / 399 MiB, v7 payloads identical, coverage .93-.98 |
| W5b | research_fields_holdings.py: 8 PIT fields (13F own/breadth/best-ideas, FTD, Reg SHO, off-exchange SV); role rule linked-operating-v2 (identity-bridge-v2-pit, delisting block, optional `--delisting-returns`) | lo1-fields-v9 (63 fields) 121 s / 571 MiB, v8 payloads identical; role lo2 kept 1,967,838 vs 1,940,364 cells |

Reports regenerated: docs/plans/2026-09-28-mega-alpha-v6-report.html (33 cells, N 33, v7 grid + spo shown rejected).
The PM pitch (2026-09-28-mega-alpha-v6-pitch.html) was NOT regenerated this session (iteration 3 pending, §5).

## 2. Data delivered by atx-db (read-only under C:/atx/atx-db/data/alpha_panel/v1; docs in C:/atx/atx-db/docs)
Response to our request: docs ALPHA_PANEL_REQUEST_V7_RESPONSE.md / ALPHA_PANEL_STATUS.md. Delivered: identity link table
(U1 95.6-99.1%/yr), delisting (U4), security master, 13F 2013q2-2026q2, earnings calendar with expected dates, Form 4,
FTD, Reg SHO (NYSE lists were still landing), 8-K metadata, all 15 statement items, 2010+ fundamentals / 2012+ prices,
corporate actions. Not available: consensus, borrow fee/utilisation, GICS/NAICS, options skew/OI, VWAP, index add/drop.
Their panel v2 assembly, borrow proxy, section-5 metrics and the lo1 aligned export were pending (1.5-2.5 h on their
side); we consume stage exports directly. Stage pins used today are in task-W5a-report.md / task-W5b-report.md.

## 3. Open results and decisions

1. **aim-partial-v6 grid C1-C3: REJECTED** (S2 net 1.160/1.121/1.112 vs 1.239; dSR -.08..-.13; cost -13-16% but gross SR
   -10%). Disclosed deviation: the rule rescales each side to its entry gross (prereg addendum A1). Lesson G1: never shrink
   by cost alone.
2. **spo-v1 cell: REJECTED as a defect** (S2 net -0.939; gross .65 never binding; tau 12%/day; exante_vol_current garbage;
   gamma 1575). W1 was asked to root-cause (alpha sign/scale, gamma calibration, 20-session amortisation, diagnostics) and
   propose corrected defaults. Any rerun = spo construction trial #2: pre-register the corrected defaults in v7-prereg.md
   (new section) BEFORE running, with the F2 risk model pin 17f9328f. Expect N 33 -> 34.
3. **Library v7.0 (wave 1) pre-registered** (v7-prereg.md "Library v7.0" + correction + ruling 7.0-b): qmj_safety (8-slot
   exception), nincr, q5_eg (RoF 2021 slopes), smax5, res_mom_ind; 5 admission trials; fields-v7 unchanged; one fit + one
   NAV cell (v6.1 construction, L 1.247). L7 is building the generator, checker changes, fitter theme list
   (`ownership_flow`), specs/v70.json. Acceptance: wave whole, dSR > 0 vs v6.1 AND mechanics.
4. **Wave 2 (ins_opp, inst_best_ideas, ftd_fail, ea_overdue)**: NOT pre-registered yet. P4 was reconciling the draft DSL
   against the real W5a/W5b field semantics into library-v7-wave2-prereg.md. Pre-register only after reading it; fields-v9
   has 63 of the runner's 64 field slots (W5a-F1: the limit binds at the next wave; raise it or prune fields).
5. **Owner decisions still open**: U1 (spend validation trial #3 on the v6.1 cell or on v7.0 if accepted); merge to local
   main; Release exes (A/B identity passed, timing inconclusive under load: re-measure quietly, tag v7-rel0 exists);
   FX source for non-USD reporters and the atx-engine 2025 seal (atx-db asks).

## 4. Lanes in flight at handoff (their commits are in their pools; hand-backs to this session are lost)
| lane | pool | branch | state |
|---|---|---|---|
| L7 library v7.0 | pool-10 | feat/platform-v7-l7-libv70-20260928 | working (10 files dirty at handoff); check `git -C C:/atx-wt/pool-10 log/status`; report task-L7-report.md when done |
| W1 spo root-cause | pool-3 | feat/platform-v7-w1-spo-20260928 | working (5 files dirty); fix-up section in task-W1-report.md |
| P4 wave-2 prereg | none (read-only) | -- | writes .superpowers/sdd/platform-20260928/library-v7-wave2-prereg.md |
State after the owner's stop (all three agents were stopped by root at session close):
- W1: COMMITTED before the stop -> pool-3 b63829f0 "fix(platform-v7 W1): spo-v2 after the spo-v1 TRAIN root cause; aim-partial
  shadow" (tree clean). Read its fix-up section in task-W1-report.md (may be partial), merge, build, test, then write the spo
  trial #2 prereg from the defaults it proposes.
- L7: NOT committed -> pool-10 has 11 uncommitted files on feat/platform-v7-l7-libv70-20260928 (last message: working on the
  gate `require` mode in research_cycle.py). Spawn a fresh Opus agent in pool-10 with task-L7-brief.md + "review the
  uncommitted work in this tree, finish, run the pytests, commit, report".
- P4: no output written. Re-run the P4 prompt (in progress.md / this session's ledger: reconcile library-v7-draft.md �2
  against the W5a/W5b field semantics into library-v7-wave2-prereg.md).
If a lane is idle with uncommitted work, spawn a fresh Opus agent in that pool with the same brief and "finish and commit".

Pools: 2 root; 3 W1; 4 W4 (done, reusable); 7 F2 (done, reusable); 8 W5a (done); 9 W5b (done); 10 L7; 11 W3 (done,
reusable). Never use pool 1 or 6. Lease status: `powershell scripts/lease-worktree.ps1 -Status` (all v7 lanes ran on
existing dead leases; that was fine).

## 5. Next steps, in order
1. Merge L7 (pool-10) when it reports; run `research_cycle.py plan specs/v70.json`; commit; `run specs/v70.json`
   (fields-v7 as-is; u pass should hit 39 and evaluate 5; gate prints the five admission rows; w; nav; summ --dsr-n 34 with
   `--ledger --effective-n dirs --psr --pbo`). Read the result under the pre-registered rule. Update ledger + Appendix A.
2. Merge W1's root-cause fix; build (tag v7-3: atx-impl-strategy-target-tests, atx-equity-strategy-targets); tests; write
   the spo trial #2 prereg section; identity flag-off check; run the cell; score with N + 1.
3. Pre-register wave 2 from library-v7-wave2-prereg.md (after checking it against fields-v9 semantics); L7-style lane for
   library v7.1 = v7.0 + wave 2 on fields-v9 (or v9 pruned to 64); run through research_cycle.
4. Rebuild the role with atx-db SIC events (`--sic-events C:/atx/atx-db/data/alpha_panel/v1/fundamentals/sic_events.parquet`
   or the stage's manifest; W5b knows the interface) to recover the 53.7k no_visible_sic cells (W5b-F1); ask atx-db about
   the 820k unlinked base-role cells (W5b-F2). A universe change is a pre-registered universe trial.
5. Scorecard + pitch iteration 3: extend docs/plans/mega-alpha-v6-report.config.json (cells v7.0, spo #2, wave 2) and the
   pitch config; add sections for: capacity curve (build-equity/v7-l4-nav-stress/v7_extras.json), risk model bias
   (build-equity/v7-f2-risk/bias_summary.json), report cards (build-equity/v7-w3-cards), monitor baseline
   (build-equity/v7-w3-monitor-baseline), decide/orders/reconcile (build-equity/v7-w4-*), integrity stats (nav_summ
   n33/n34 JSON, PBO JSON), trial ledger. Regenerate both HTML reports; write docs/plans/2026-09-2x-mega-alpha-scorecard-v7.md
   (same format as scorecard-v6.md) with the Appendix A block. Publish the pitch as an artifact for the owner.
6. Release adoption (A5): quiet host, re-run the v6.1 u pass and NAV cell with build-equity-rel/bin exes (rebuild tag
   v7-rel1 with `-Preset equity-rel`), adopt only if byte-identical and faster.
7. Owner: merge to local main; decide U1.

## 6. Commands that work (pool-2 root; PY="C:/Program Files/Python312/python.exe")
- Build: `powershell -File build-equity/mega-build.ps1 -Tag <new> -Targets "a,b" [-Preset equity-rel]`
- Tests: `build-equity/bin/atx-impl-strategy-target-tests.exe --gtest_filter='Spo*:Risk*:NavV7Hook*:AimV6*:CostV2*:StrategyLive*:StrategyNavReplay*:NavV5*:TargetReplayV5*'`; `atx-impl-strategy-ic-tests --gtest_filter=StrategyIcRunner.*`; `atx-engine-alpha-tests --gtest_filter='AlphaLit*'`; `atx-core-tests --gtest_filter=Sha256.*`
- Python tests: `"$PY" -m pytest -q -p no:cacheprovider scripts/tests atx-impl/tools atx-engine/tools atx-impl/strategies` (subset files listed in the lane reports)
- Cycle: `"$PY" scripts/research_cycle.py plan|run|status specs/<spec>.json [--suffix X] [--reuse-fields DIR] [--stop-after fields] [--runner-override max_rss_mib=64]`
- Scoring: the n29 argv of build-equity/mega-nav-v61-summ-n29.json (nav_summ_run.argv) plus new cells, `--dsr-n N --reference <baseline> --json OUT --ledger build-equity/trials.jsonl --effective-n dirs --psr --pbo <cells> --pbo-json OUT2 --ledger-n build-equity/trials.jsonl`
- Report: `"$PY" atx-impl/tools/mega_report --config docs/plans/mega-alpha-v6-report.config.json --out docs/plans/2026-09-28-mega-alpha-v6-report.html`
- Decide / reconcile / risk: full lines in task-W4-report.md, task-L4-report.md, task-F1-report.md, task-W1-report.md.

## 7. Pitfalls met today
Dirty-tree refusals from child writes (commit before every run); heredoc apostrophes break under the shell hook (use the
Write tool); ninja stops on the first failing TU so exes may be stale after a failed build (check bin timestamps); the
runner's memory admission refuses `--cache-report` without `--max-memory-mib 1536`; TC has two definitions (decision.json
carries both; the replay's is `transfer_coefficient`); the M-6 reuse rule keys on the whole builder module's code (any
hook edit recomputes every module field, payloads stay identical).
