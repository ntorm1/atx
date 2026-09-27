# Mega-alpha — parent handoff 4 (v5 "deploy the book" sprint closed on TRAIN, 2026-09-27)

Author: v5 controller (Claude Opus 5.5, SDD), resuming from the interim handoff
`docs/plans/2026-09-27-mega-alpha-v5-interim-handoff.md`. Integration checkout `C:/atx-wt/pool-2`, branch
`feat/aes-codex-integration-20260925`. Spec: `docs/plans/2026-09-27-mega-alpha-v5-dag-plan.md`. Ledger (newest first):
`.superpowers/sdd/mega-alpha-20260926/progress.md`. Gate report: `.superpowers/sdd/mega-alpha-20260926/task-T40-report.md`.
Trust the ledger and `git log` over this summary.

## 0. TL;DR

- **R-1 disclosure:** validation trial #2 (v4.1, net +0.641) came from a book with mean gross leverage 0.24 and net +0.04 — a
  ~$240m book, not a $1bn deployment. The band `band_multiple/N_d` blocked entry. Recorded as-is.
- **v5 deploys the book.** The `aim-partial-v5` rule (GP partial adjustment toward a gross-1 aim, dust band, aim leverage L)
  plus the pre-registered L re-run (L = 1.279) runs the reference book at **mean gross 1.002, |net| .015, τ .044/.061, held
  share .984**. S2 net SR +0.712 (gross SR 1.167, mu 3.1%/yr, vol 4.4%, MDD 4.3%). v4.1 got +0.687 at gross 0.256, so v5
  makes about 4x the dollar P&L at the same Sharpe.
- **Objective not met on TRAIN.** No cell has TRAIN net SR ≥ 1.0. The best is +0.759: the v5.1 `opex_at` library, reference
  construction, L = 1, gross .78. So no freeze is proposed and **no validation was run**. Validation trial #3 is unspent.
- **Levers tested, none helped significantly.** Paired ΔSR against the reference, with Ledoit-Wolf studentized bootstrap:
  - Aim-weighted composition trails ew in all 5 paired cells (−0.10 to −0.29).
  - Per-name trading rate is worse (ew −0.195, p .053; aim −0.293, p .032).
  - θ .03/.08 and dust 0 are indistinguishable from θ .05 / dust .1.
  - v5.1 `opex_at` is +0.017 (SE .011, n.s.).
  - DSR at N = 13: the cross-cell variance of 13 near-duplicate cells gives .846 at most (SR0 .150). The plan's §4.E
    Lo-variance null gives .18-.35 (REF .342; SR0 .985). Either way, the DSR deflates only this 13-cell construction search,
    not the v3/v4/v4.2 family search.
- **Binding constraint (unchanged):** gross alpha of ~1.17-1.19 SR against a $1bn S2 cost hurdle of ~0.45 SR. The linear
  6 bp stress (S1) reaches net .93 on the same book, so the cost model and the AUM level decide the objective as much as
  the alpha does (U4).
- **Delisting lane parked.** T33a is NO-GO: 59.9% of terminations are classifiable, below the 80% gate. η = 0 stays primary.
  The S3 K = 1 adverse stress is negative in 13/13 cells. U3 is now a concrete data ask.

## 1. What was done (task by task)

| Task | Result | Evidence |
|---|---|---|
| T28 audit | v4.1 ran at mean gross .256 TRAIN / .235 VAL (R-1) | `construction-audit-v4.md`, `44e6c28a` |
| T29 prereg | `## v5 revision` R1'-R7' before any v5 TRAIN read; v5.1 family pre-registered | `56e5b148`, `c8192c46` |
| T30 `aim-partial-v5` (C++) | review clean 0C/0I/6m; built v5-0 | `task-T30-review.md` |
| T36 per-name rate (C++) | review clean 0C/0I/5m; fixed-rate path byte-identical to T30 on real data | `task-T36-review.md`, ledger "T37" |
| T31 aim fitter + v5_train.sh + nav_summ | complete after 1 fix round (DSR producer) | `task-T31-*.md` |
| T33a delisting feasibility | NO-GO, lane parked (T33b/T32 not built) | `delisting-feasibility.md` |
| T34a/T34b library v5.1 | GO (proxy `opex_at` = (sale_ttm − oi_ttm)/at); review clean; plan admitted 1,381.9 MiB | `breadth-check-v5.md`, `task-T34b-review.md` |
| T34c `v51_train.sh` (added) | brief reconstructed after the pause; fix round by a fresh implementer; review clean 0C/0I/5m | `task-T34c-*.md` |
| T37 build v5-1 | 19.6 s; tests 18/18 filtered, 55/55 target, 67/67 IC; D2 byte-stability PASS (again at v5-2 after T41 fix) | `mega-v5-1-receipt.json`, `mega-v5-2-receipt.json` |
| T38 grid + L + nav_summ | 10/10 + 2 L cells completed (28-45 s, ≤ 339 MiB each) | ledger "v5 TRAIN grid", "T38 steps 4-5" |
| T39 v5.1 | u (37 hits/1 miss), fits ×2, w (ew), NAV reference cell | ledger "T39 step 2b" |
| T40 gate | 1/13 mechanics pass; no cell ≥ 1.0; no freeze | `task-T40-report.md` |
| T41 whole-branch review | 0C/1I; one fix wave (I1 + nav_summ M1/M2/M5/M7), re-review all addressed; build v5-2 | `task-T41-review.md`, `task-T41fix-*.md` |

## 2. v5 TRAIN table (S2 = modeled-1bn-stale5-v1 × swap-fin-v1; 2020-2022; $1bn; cadence 1)

Full table (13 cells with gross, |net|, τ, NR, ΔSR ± SE, CBB and LW CIs, DSR, mechanics):
`.superpowers/sdd/mega-alpha-20260926/t40-table.md`. Headline rows:

| cell | net SR | gross SR | gross lev | τ mean/p95 | ΔSR vs REF (SE) | DSR N=13 (cross-cell) | mechanics |
|---|---|---|---|---|---|---|---|
| ew θ.05 d.1 fixed (REF, L 1) | +0.742 | 1.177 | .782 | .043/.060 | — | .839 | gross FAIL |
| **ew θ.05 d.1 fixed L 1.279** | **+0.712** | 1.167 | **1.002** | .044/.061 | −0.030 (.013) | .827 | **PASS** |
| ew θ.05 d0 fixed | +0.724 | 1.178 | .781 | .046/.064 | −0.018 (.027) | .832 | gross FAIL |
| ew per-name | +0.546 | 0.990 | .739 | .043/.067 | −0.195 (.096) | .748 | gross FAIL |
| aim θ.05 d.1 fixed | +0.616 | 1.012 | .836 | .036/.055 | −0.126 (.112) | .783 | gross FAIL |
| aim L 1.279 | +0.607 | 1.022 | 1.072 | .037/.057 | −0.135 (.112) | .778 | gross FAIL (>1.05) |
| v5.1 ew θ.05 d.1 fixed | +0.759 | 1.193 | .781 | .043/.060 | +0.017 (.011) | .846 | gross FAIL |

Stresses on REF: S1 linear-6bps net +0.933; S2 × engine-tiers +0.651; S2 × flat-300 +0.458; S3 terminal-adverse −0.102.

## 3. Alphas and weights

- Reference composition `ew-theme-v1` (W_ew `9a9c949a`): 31 admitted of library v4 (37), 9 themes at 1/9 each.
  weighted_standalone_turnover .0519. Netting ratio at REF .835 (v4.1 .26). NR = τ_book / Σ w_k τ_k compares a θ .05
  partial-adjustment book with full-rebalance standalone turnovers, so it mixes construction with netting (T41 M4). It is
  not a pure netting measure, and the v4.1 value came from a banded f.25 book.
- Aim composition `ew-theme-aim-v1` (W_aim `54f823c1`): θ .05 gains g_k ∈ [.358, .985]; half-sample |Δg| ≤ .05.
  - Gains by theme: value .960-.978, profitability .907-.985, investment .936/.976, low_risk .854-.975, momentum .890-.926,
    short_interest .972/.935/.449 (si_change), earnings_momentum .753-.847, iv_rv_spread .565, reversal .487/.358.
  - Theme weights: investment .131, value .132, profitability .131, low_risk .127, momentum .125, earnings .111,
    short_interest .108, options .077, reversal .058.
  - wst .0425. It lowers turnover by 18%, but gross SR also falls, from 1.177 to 1.012. The down-weighted fast signals
    carried more gross alpha than their cost saving is worth.
- v5.1: `opex_at` admitted (τ .014, HAC t 3.20, max |ρ| .522 with roe_q, weight .0139); 32/38 admitted. W_ew51 `198375f9`,
  W_aim51 `89b146f8`, C_ew51 `1a0119a6`.
- Pins: library v4 `daa9663e`; v5.1 `9e5ea08c`; role v2 `210fff96`; fields-v6 `32565c32`; C_ew `24a6cc76`; C_aim `00439b98`;
  NAV exe v5-1 `fb2d3e94` (grid) / v5-2 `59b4e944` (after I1; byte-identical outputs); IC exe `647c71a7`.

## 4. Trial accounting (Appendix A)

```
Trial accounting (TRAIN 2020-2022 only):
  v3 era: admission 48 + 121; composition 4 + 7; construction 14.
  since run #1: libraries v4 (37), v4.2 (40), v5.1 (38); compositions v4, v4.2, ew-theme-aim-v1, v5.1 x2 (ew-theme-v1, ew-theme-aim-v1);
  construction v4 1 + v4.1 grid 5 + v4.2 2 + v5 grid 10 + 2 L re-run + 1 v5.1 (delisting re-run not run: T33a NO-GO);
  studies T26 5 paper books, T16, T28 audit; stat-arb cluster study (separate family, negative).
  Not trials (reproductions): D2 checks (v5-0, v5-1), v5-0 reference byte-check.
  validation: #1 (v3, book level), #2 (v4.1, 24%-gross book). Per-candidate VAL statistics: never read.
  DSR inputs: N = 13, V[SR_n] = 3.083e-05 per session (cross-cell), skew = -1.279, kurtosis = 14.532 (reference), T = 754.
```

## 5. Whole-branch review (T41)

`task-T41-review.md` (Opus 5.5) returned 0 Critical, 1 Important and 7 new Minor findings. All seven named risks passed:
- (a) baseline bytes are identical across the v4, v5-0 and v5-1 binaries;
- (b) every v5/v5.1 receipt is TRAIN-only;
- (c) ADV is in dollars and no NaN appears;
- (d) aim gains re-derive exactly;
- (e) the SHA chain is intact;
- (f) nothing from T33b/T32 landed;
- (g) the gate statistics recompute exactly.

The reviewer independently recomputed the T40 verdict from the CSVs, and it stands. The review triaged 30 deferred minors:
1 must-fix (T36 m1, which is I1), 1 already resolved, 28 acceptable.

One final fix wave (`task-T41fix-*`, re-review ALL ADDRESSED) closed:
- **I1**, the release fallback for the per-name liquidity cache;
- **M1**, `nav_summ` now emits the ruled all-rows gross and net and labels the gate basis;
- **M2**, cost per GMV-τ now uses the same session basis as τ;
- **M5**, warnings on DSR-count mismatches and duplicate series;
- **M7**, provenance in the `--json` output.

Root verification after the fix:
- Build v5-2 (NAV exe `59b4e944`): tests 18/18 and 55/55, and the D2 check holds.
- The per-name cell re-run is byte-identical in 5 files, so I1 changes no output.
- pytest: 87 passed.
- The nav_summ 13-cell re-run changed only the cost metric.

M3, M4 and M6 are disclosed in `task-T40-report.md` §6:
- M3: the DSR under both benchmarks;
- M4: the netting-ratio caveat;
- M6: per-name scenario books hold different portfolios.

One out-of-scope residual is parked with a ruling (ledger "T41 fix wave"): `per_name_rates` reads the cache with no release
fallback, but `check_rates` fails loudly on NaN/inf. **Merge readiness: no open Critical or Important finding** (the merge
itself is U5).

## 6. Owner decision packet (U1-U5)

| Gate | Decision asked | Recommendation |
|---|---|---|
| U1 | Holdout policy: disclosed validation trial #3 on 2023-2024, or release 2025+ | **Do not spend it now.** No TRAIN cell clears the freeze rule (net ≥ 1.0). Grant U1 only when a TRAIN cell does. |
| U2 | Pre-2020 history for selection and estimation | **First priority** (plan recommendation). 3-year TRAIN gives SE ≈ 0.58 per SR estimate. Paired ΔSR SEs of .08-.14 cannot resolve lever effects of about 0.1. Only the per-name and L contrasts reached |t| > 2, and both were negative. A longer TRAIN is the most direct way to separate levers of this size. |
| U3 | New data, now concrete: bridge CIK coverage for the 203 no-CIK terminated lines (ETFs/ETNs/SPACs/ADRs/preferreds/warrants), or a sealed submissions export plus a ticker-continuity input; also analyst revisions and options skew | Needed for delisting returns (S3 is −0.10 to −0.43) and for new alpha breadth |
| U4 | Cost target: S1 vs S2 primary, and/or AUM below $1bn | The same book is S1 +0.93 vs S2 +0.71. The objective depends on this as much as on alpha. Decide the cost model and the AUM before another construction sprint. |
| U5 | Merge the integration branch into main (real merge; main has diverged) | After the T41 verdict (§5); the branch has no open Critical/Important findings if §5 says so |

## 7. Open items

- Deferred minors (T30 ×6, T31 ×8+2, T34b ×4, T36 ×5, T34c ×5) are triaged in `task-T41-review.md` (28 acceptable). Two more
  are in the ledger: the nav_summ single-dir warning noise, and the parked `per_name_rates` cache read.
- The per-name-v1 scenario books hold different portfolios (T41 M6). Stress deltas inside per-name cells are therefore not
  construction-controlled.
- The delisting lane (T33b/T32) is parked pending U3.
- `aim L1.279` overshoots gross (1.072), because the L re-run uses one L from the ew reference (R5'). A per-C L would be a
  new disclosed revision.
- The partial-adjustment rule under-deploys at L = 1 (gross .71-.88), and the deterministic L closes the gap. A
  gross-targeting rule, with L solved each session, is the principled fix. It would be a new construction family (a new
  disclosed revision).

## 8. Rulings made in this resumed session (all in the ledger, each with cost-if-wrong)

1. T34c brief reconstructed from the "T39 step 2a" ruling after the pause lost the dispatch; a fresh implementer verified the
   committed script.
2. R-b ratified: the per-name rate uses NAV_d = the pre-trade NAV (nav_pre).
3. ⚠️3 check: the reference cell was run with the v5-0 exe before the v5-1 build. Its bytes are identical to the v5-1 grid cell.
4. L = round(1/0.78191, 3) = 1.279, from the all-rows CSV mean (the ruled definition).
5. v5.1 NAV = the reference cell only (ew), construction +1. The w pass ran for ew only.
6. T40 DSR uses N = 13; the step-5 table is N = 10.
7. The final fix wave carried I1 plus nav_summ M1/M2/M5/M7; M3/M4/M6 became disclosures.
8. Root verification ran in parallel with the re-review.
9. The `per_name_rates` cache read is parked: `check_rates` fails loudly on NaN/inf.

## 9. Goal prompt for the next parent agent

```
/goal Resume mega-alpha after the v5 sprint. Read, in order: docs/plans/2026-09-27-mega-alpha-parent-handoff-4.md (this
handoff), .superpowers/sdd/mega-alpha-20260926/task-T40-report.md (the v5 TRAIN gate, incl. the §6 addendum), and the top
sections of .superpowers/sdd/mega-alpha-20260926/progress.md (newest first). Trust the ledger and git log over memory.

State: v5 closed on TRAIN. aim-partial-v5 + L 1.279 deploys the $1bn book (gross 1.002) at S2 net +0.712; no TRAIN cell
reaches net 1.0 (max +0.759, v5.1 ew L 1); no freeze, no validation (trial #3 unspent). T41 whole-branch review: ready to
merge after the I1 fix (landed, build v5-2). Owner packet U1-U5 is in handoff 4 §6.

First: check which owner decisions (U1-U5) the owner has answered (ask if none). Do NOT spend validation trial #3 unless U1 is
granted AND a TRAIN cell has net >= 1.0. Default next sprint if U2 is granted: a data sprint that extends TRAIN before 2020
(role/fields families for the earlier years; bounded runner; new pre-registration section in v4-prereg.md before any read),
then re-measure the v5 reference book (ew, theta .05, dust .1, fixed, L from the new reference) on the longer TRAIN. If U4
changes the cost model or AUM, pre-register it as a new disclosed revision before any read.

Method: subagent-driven development; Opus 5.5 for every implementer, explorer, researcher and reviewer; artifacts as files;
children get their brief path, plan-s3-constraints.md, their Interfaces block and lane-contract.md; reviewers get
review-TN.diff + reviewer-contract.md (re-reviews: re-review-contract.md); <= 5 fix rounds; controller never implements.
Pools: 2 = root; children create their own branch in their pool (the controller cannot switch pool branches); never 1 or 6.

Rules (verbatim, binding): work only in C:/atx-wt/pool-2 (feat/aes-codex-integration-20260925); never mutate/build/switch/commit
in C:/atx (read-only ok; the tier1-v2 session owns C:/atx and atx-db: no locks, never kill its processes). Root alone builds
via powershell -File build-equity/mega-build.ps1 -Tag <new> -Targets "<t1,t2>" (tags v5-0, v5-1, v5-2 used; check
build-equity/mega-<tag>-receipt.json does not exist). Root alone runs real data under
"C:/Program Files/Python312/python.exe" scripts/run_bounded_research.py --seconds 180 --max-rss-mib 1536 --min-free-mib 512
--output build-equity/<new-dir> --bind <manifests> -- <cmd>; tree must be clean (untracked included): commit docs, briefs,
ledger and scripts (git add -f for the gitignored sprint dir) before every run. Children: own pool worktree, never build,
never run real data, never spawn subagents, Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com> trailer, task-TN-report.md,
reply < 15 lines. TRAIN 2020-2022 only unless U2 extends it; 2023-2024 used twice (any new read = disclosed validation trial #3,
needs owner gate U1); 2025+ reserved; never read a per-candidate VAL statistic. No pushes, warehouse writes or broker actions.
No TDD; postimplementation fixtures; tasks accepted on root real-data measurements. RAM: limits stay 180 s / 1536 MiB,
efficiency fixes not longer caps. Every ruling: "Ruling: <decision> -- <why> -- cost if wrong: ...", declared before any
measurement it could bias; report every TRAIN result with the Appendix A trial-accounting block.
```
