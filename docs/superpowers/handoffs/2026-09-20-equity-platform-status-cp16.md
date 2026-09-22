# Equity platform status — end of checkpoint 16 (2026-09-20, night session)

Worktree `C:\atx\.worktrees\equity-platform`, branch `feat/equity-platform-20260920`, HEAD
`dffb609b7a3c` (unchanged all session). ~224 uncommitted entries. No commit or merge was made;
none is authorized. Nothing is in flight after this file (the `cp16-docs` writer finished; see
`cp16-docs-report.md`). Ledger (memory, read it): `.superpowers/sdd/equity-platform-parent-goal/progress.md`,
entries from "Session start (v3 goal" to the end. Binding constraints: v1 §"Critical workspace
rules" + §"Integration constraints", v2 §4–§7, v3 §5–§7, v4 handoff (this session) — all still apply.

## 1. THE NUMBER (what the user asked for)

First multi-year, net-of-cost alpha scorecard on the real universe, pre-registered, all
caveats printed: `atx-engine/reviews/2026-09-20-equity-alpha-scorecard-cp16.md`
(SHA-256 `b36ab30b…`; data copy `C:/atx/data/equity_scorecard16_scorecard_20260920/`).

Pooled 2013–2019 NET decile-spread Sharpe, h = 21, headline variant/restriction, 95 % CI:

| signal | top-1000 (n=74) | top-3000 (n=63, 2017 NOT FIT) |
|---|---|---|
| momentum_252 | −0.04 [−0.75, +0.72] | −0.26 [−0.99, +0.69] |
| momentum_126 | −0.19 [−0.70, +0.67] | +0.08 [−0.45, +0.70] |
| blend_equal | −0.23 [−0.80, +0.66] | −0.10 [−0.69, +0.65] |

Per-year (≈ 12 obs each, indicative only): positive 2013, 2014, 2015, 2019; negative 2016
(−0.4 … −1.8), ≈ 0 2017, negative 2018 (−0.1 … −1.5), both cuts. Sign stability 2/7–4/7.
Rank-IC mean 0.017–0.032. Implied turnover 0.001–0.007. Breadth 998 / 2,773.

**Acceptance bars (written before the run, R16-8): all three signals FAIL bars 1 and 2 on both
cuts. Verdict: NO CANDIDATE.** Post-hoc diagnostic (not pre-registered, not a trial): pooled
GROSS Sharpe at h=21 is also ≈ 0 (−0.12 … +0.24); cost drag ≈ 38–40 bps per 21-session period.
The failure is the signal, not the cost model. Checkpoint 14's +1.2 … +1.9 was ONE favourable
year (2013); it must not be quoted as alpha evidence again.

R16-9 materiality test FIRES for momentum_252 on both cuts (2016 outside the union of the
2015/2019 CIs) → a pre-registered hole-remediation attempt 2 is AUTHORISED but NOT started
(R16-26): the test cannot separate the archive hole from a genuine 2016 momentum reversal.

## 2. COMPLETED this session (lean path, user decision, R16-13)

| item | evidence |
|---|---|
| v3 handoff | `docs/superpowers/handoffs/2026-09-20-equity-platform-parent-goal-v3.md` (349 lines) |
| Research | `.superpowers/sdd/equity-platform-parent-goal/research-cp16-scorecard.md` (396 lines) |
| Design (frozen) | `atx-engine/reviews/2026-09-20-iteration16-alpha-scorecard-design.md` 203 lines, SHA `a83484037cae527e2fbfba5dce04289742d1c81666b2c03c132f3da9268c04d1`; addendum 1 (`…-design-addendum-1.md`, SHA `7173cc33…`) records the two measure-first deviations |
| Data span | one prepare+load 2012-03-26..2019-12-31 (`iteration16_ingest_span.py`; receipt `iteration16-ingest-span-2012_2019-attempt1.json` accepted; prepare 754 s, load 615 s) → `C:/atx/data/tickerhistory_training_native_2012_2019_20260920/segments` |
| C++ | panel allow-list: `HistoryDataConfig::allow_ids` (+ `allow_list_excluded_columns`), `history_panel.cpp` Step 5a-restrict, `panel` flags `--universe-membership/--universe-cut/--universe-eval-start`, additive recipe keys; `stage_equity_baseline.cpp` accepts the cp16 membership recipe (R16-24) and commits signals-only for it (R16-25, status `complete-signals-only`, NOT a book baseline). `stage_equity_ic.cpp`, engine IC, `kMaxIcInstruments` untouched. Tests: +3 (DataHistoryPanel 10/10, AtxImplPanelMembership 2/2); full exes data 186 pass, impl 466 pass + pre-existing StageRunSyntheticSmoke failure only |
| Binaries | `atx-impl.exe` `d1224b32844b16329b73011af944a3c13f6d0b5f1307874e3c2878bd2c42159c` (all 13 cells; the measure cell's panel by `33c6ca8b…`, identical panel code) |
| Runs | 13 cells (`iteration16_run_cells.py`; receipts `iteration16-cells-attempt1-full.json` + `-measure2015e.json`), every phase exit 0; panel peak WS 2.51–2.76 GB (< 3 GB), IC ≤ 0.38 GB; contexts = predicted unions (t1000 1,228–1,697; t3000 3,544–3,608); 2017 t3000 pre-declared NOT FIT (5,072 > 4,096) |
| Scorecard | `iteration16_equity_scorecard.py` (self-test 10/10; parent hand-check reproduces the anchor cell exactly) → scorecard.csv (1,080 rows, `9bab19ff…`), scorecard.md, receipt.json (`6d2c2b50…`) |
| Ledger | canonical `trial-ledger.jsonl` 4 lines UNTOUCHED (`d4f12620…`); sidecar `trial-ledger-cp16-restrictions.jsonl` 26 lines (13 runs × 2; "declared 30" per run is the unchanged binary's constant; N_14 stays 30, cells are AR-7 restrictions) |
| Docs | PLATFORM_PROGRESS "## Checkpoint 16" + Next candidates rewrite, READMEs, v4 handoff (`cp16-docs`, see `cp16-docs-report.md`) |

## 3. NOT done (lean shortcuts, to retire only if a number says so)

As-of membership predicate in equity-ic (membership is the YEAR UNION — selection look-ahead
within a year, direction unknown); cp16 ledger mode in the stage (sidecar file instead); stdlib
oracle suite (one hand-checked cell instead); receipt writer (scorecard's receipt.json instead);
design review / end review; hole remediation attempt 2; 2017 top-3000 cell; the cp14/cp15
receipts' `stage_equity_baseline.cpp` source pin now drifts (legitimate edit, receipts untouched).
Carried from v3 §5: cp13 T3–T5, cp14/cp15 deferred minors, StageRunSyntheticSmoke, identity caveats.

## 4. Rulings (all in progress.md with cost-if-wrong)

R16-1 route (b′); R16-2 AR-7 restrictions, N stays 30; R16-3 allow-list = eff-in-Y ∪ last prior
rebalance; R16-4 2017 t3000 NOT FIT; R16-5 PIT floors → cp16 contexts ≠ cp14 context; R16-6
headline = IncludeAuditedTerminalV1/full as a label; R16-7 Sharpe recipe; R16-8 bars; R16-9 hole
as-is + materiality; R16-10 one data span; R16-11 measure first; R16-12 layout; R16-13 LEAN path
(user); R16-14 CI 2.5/97.5; R16-15 turnover = 1 − rho_rank; R16-16 zero dispersion; R16-17 stdlib
seeding; R16-18 blend inherits 2018 flag; R16-19 materiality scope; R16-20 unreportable rows;
R16-21 evaluation start = first session ≥ Y-01-01; R16-22 one turnover per signal; R16-23 pooled
CI blank if n < 8; R16-24 baseline pin widened; R16-25 signals-only baseline commit; R16-26
attempt 2 authorised, not started.

## 5. Traps learned this session

Python stdout is block-buffered under `Start-Process` (infer progress from output dirs / phase
logs); runner `--receipt-tag=-x` needs the `=` form; never `mkdir` a stage's fresh output root;
log files are mode "x" — version old ones before a retry; PowerShell here-strings do NOT pipe
into `python -` (REPL banner = nothing ran) — use Bash heredocs; inside a Bash python heredoc a
C++ `"\n"` must be written `"\n"` or `chr(92)`; `-Wmissing-field-initializers` on aggregate
init → give new struct fields a default initializer; the baseline stage pins the cp14 screen
AND its replay rejects any held name with a missing close — PIT universes need the signals-only
path; the measured panel peak (2.6 GB) is set by SOURCE columns (~9k) not by the allow-list.
