# Equity platform parent goal — handoff v3 (2026-09-20, end of checkpoint 15)

Supersedes the stop point in `2026-09-20-equity-platform-parent-goal-v2.md` (v2) and its
predecessor v1. Every constraint in v1 and v2 still binds unless restated here. Read v1
§"Constraints", v2 §4–§7, then this file top to bottom. The exact stop point is in §1. Built
from `2026-09-20-equity-platform-status-cp15.md` (primary source),
`.superpowers/sdd/equity-platform-parent-goal/progress.md` lines 80–110 (checkpoint-15
session, starting "Session start (v2 resume...)"), and `2026-09-20-next-goal-prompt-v3.md`
(the user's alpha-first re-prioritization). No fact below is invented; unclear items say
"(not recorded)".

## 1. Exact stop point

- Checkpoint 12: validated, receipt immutable. Unchanged.
- Checkpoint 13 (claims-aware replay seam): MINIMAL-CLOSED, unchanged since v2. T3 (engine
  scenarios B/C), T4 (allocation certification), T5 (equity-book wiring, `--transitions`)
  remain DEFERRED by ruling.
- Checkpoint 14 (cross-sectional forecast evaluation, Stage 1): COMPLETE, unchanged since v2.
  2013-only rank IC / net decile spread on 189 observations remains the only forecast
  evidence on the branch.
- Checkpoint 15 (point-in-time universe builder): COMPLETE this session. Design frozen
  (Revision 3 + §15, SHA-256 `4810fda251c6c285b29413ab6bea05b46db66e9bb0620cf17950b45075267dc8`),
  engine unit + stage + ledger extension built and tested, real run measured on 2013–2019,
  receipt immutable (SHA-256 `2c52c6a4c2d2e841d8ffe3b8e1ced3e8c79d40eeba5a2176bcfc75b06068c07e`),
  docs written. Detail in §3.
- Checkpoint 16: NOT STARTED. Post-stop user directive (progress.md:109) re-prioritized it to
  "first alpha scorecard"; `2026-09-20-next-goal-prompt-v3.md` was rewritten accordingly and is
  authoritative — see §6. A read-only cp16 research agent (per-year contexts via `panel`
  restricted by `membership.bin` vs. a thin exporter) was dispatched alongside this
  handoff-writer (progress.md:110); no `research-cp16*.md` report exists yet — (not recorded);
  check before re-dispatching.
- This file was the first deliverable named by the user after the stop; a prior `cp15-handoff`
  agent was stopped before writing it (progress.md:108) — this write discharges that.
- No commit or merge was made; none is authorized. Nothing is in flight after this write
  (edit-only; no build/test/git commands run by this writer).

## 2. Where everything is

Worktree `C:\atx\.worktrees\equity-platform`, branch `feat/equity-platform-20260920`, HEAD
`dffb609b7a3c` (unchanged all session, all work uncommitted). `git status --short` ~214
entries (up from ~204 at v2); expected. Shared checkout `C:\atx` untouched except git-ignored
`C:/atx/data/` (see §2.3). Shared stash stack not re-verified this session (carry v2's count
of 9 as last known).
SDD ledger: `.superpowers/sdd/equity-platform-parent-goal/progress.md`, 110 lines at stop (was
78 at v2; checkpoint-15 session is lines 80–110). New reports this session, same directory:
`research-cp15-universe.md`, `cp15-design-review.md` (includes the scoped C-1..C-6
re-verification), `cp15-task-{T1,T2,T3}-report.md`, `cp15-idgap-investigation.md`,
`cp15-end-review.md`, `cp15-docs-report.md`. All v2-listed reports (cp14/cp13/cp12) unchanged.

### 2.1 Pinned artifacts (SHA-256 at handoff time)

| artifact | sha256 |
|---|---|
| cp12 design (must stay unchanged) `atx-engine/reviews/2026-09-20-iteration12-security-transition-design.md` | `0a964aebc15f14ea25ddd19fad5be723835b6b7a6e99a6aa308f3b7ecf95caee` |
| cp14 design (must stay unchanged) `atx-engine/reviews/2026-09-20-iteration14-cross-section-ic-design.md` | `888c726b123c02b39636876a667cfab890b4304794f6be42d46d35befbedbe25` |
| cp14 receipt `atx-engine/reviews/2026-09-20-cross-section-ic-validation.json` | `38dd653f7069cb4935b51d003baf9cdf87401434eccc9af5df15e907b319d723` |
| cp15 design (must stay unchanged; Revision 3 + §15, 1,060 lines) `atx-engine/reviews/2026-09-20-iteration15-point-in-time-universe-design.md` | `4810fda251c6c285b29413ab6bea05b46db66e9bb0620cf17950b45075267dc8` |
| cp15 receipt (immutable) `atx-engine/reviews/2026-09-20-point-in-time-universe-validation.json` | `2c52c6a4c2d2e841d8ffe3b8e1ced3e8c79d40eeba5a2176bcfc75b06068c07e` |
| `build-equity/bin/atx-impl.exe` (built post cp15 T2; == manifest producer of the real run) | `8425fc03c136169a…` (prefix as recorded) |
| `build-equity/bin/atx-impl-tests.exe` | `ca42f54a1d584ce4…` (prefix as recorded) |
| `build-equity/bin/atx-engine-data-tests.exe` (post T1-fix1) | `1e17b8283d3593b0…` (prefix as recorded) |
| cp15 oracle `build-equity/audits/iteration15-universe-oracle-v1.json` (42 cases) | `8b21cf57…` (prefix as recorded) |

cp15 design SHA MUST NOT change: embedded as `kEquityUniverseDesignNoteSha256` in
`atx-impl/src/stage_equity_universe.cpp`, written into ledger lines 3–4 and `request.json`,
runner-verified fail-closed (cp14's `kDesignNoteSha256` pattern). It superseded an earlier
frozen revision SHA-256 `be4b3b670eaced4f0b6f5cee8a7d5e01dc8b201ae1ecd4ef8b3377bcdb41020f`
(1,000 lines, pre-§15); the cp15 oracle intentionally still cites that predecessor per §15.5
(`ORACLE_DESIGN_LINEAGE`), not a stale pin. Any further edit = new §-amendment + re-freeze.
`atx-impl.exe` was built fresh after T2 and used for both the dry-run and the real run
(manifest `producer_executable_sha256` == `8425fc03…`, verified equal in the cp15 receipt).
Trial ledger `atx-engine/reviews/trial-ledger.jsonl`: 4 lines (was 2 at v2). Lines 1–2
`iteration14-cross-section-ic-0001` pre-registered→completed, N=30. Lines 3–4
`iteration15-point-in-time-universe-0001` pre-registered→completed, checkpoint 15, purpose
`point-in-time-universe-construction`, `trial_count_declared` 0 (accepted under the cp15
non-trial-purpose allow-list extension). Chain verified valid (cp15 receipt, 47/47 checks).
Sidecar now 4 lines; its digest at this state is (not recorded) — v2's `cc888bde…` describes
only the earlier 2-line state.

### 2.2 Native evidence (`build-equity/audits/iteration15-*`, junit under `build-equity/`)

Ingestion (2014–2019, detached): attempt 1 killed after ~40 s (Bash 10-min cap; log +
reservation kept, versioned, not evidence); attempt 2 relaunched via `Start-Process`, 6/6
accepted 20:40:59Z–21:06:23Z.
T1 (engine): configure/check/build clean; first test run 36/37 (decode trailer-before-body-
parse defect), comparator still PASS 42/42 (failing case outside the 42 markers); parent fix
(`decode_membership_bin`); rebuild → 37/37, comparator PASS 42/42; data-group regression 94/94.
T2 (stage/wiring): configure/check clean; build FAILED once (`-Wrange-loop-construct`/`/WX`,
`stage_equity_universe_test.cpp:527`); parent one-char fix (`const fs::path &dir`); rebuild
clean; impl suites 60/60 (ConfigEquityUniverse, StageEquityUniverse, TrialLedger,
ConfigEquityIc, StageEquityIc, EquityIcTerminalValue).
Real run: `atx-impl.exe`/`atx-impl-tests.exe` built; dry-run #1 flagged an expected
oracle/design lineage mismatch (fixed: `ORACLE_DESIGN_LINEAGE`); dry-run #2 clean; real run
detached, 1,066 memory samples, peak WS 87,539,712 B; runner crashed post-run (its own lineage
edit put a non-digest string into the pin dict) — fixed + re-evaluated via a versioned script,
not re-run (`accepted_reevaluated=true`).

Scripts (never rerun a receipt writer; version new attempts): `iteration15_ingest_
tickerhistory.py`, `iteration15_universe_oracle.py` (`7fea11ca…`), `iteration15_native_
comparator.py` (`1859fd95…`), `iteration15_run_equity_universe.py` (fixed, `bf868e67…`),
`iteration15_reevaluate_attempt1.py` (`35b91ee9…`), `iteration15_write_validation_receipt.py`
(47/47 checks).

### 2.3 Real-data outputs

`C:/atx/data/equity_universe_pit_2013_2019_20260920/`: `membership.csv` (94.1 MB),
`membership.bin` (12.1 MB), `delisting.csv` (1.9 MB), `churn.csv`, `coverage_by_year.csv`,
`union_by_year.csv`, `survivorship.json`, `request.json`, `seal.json`, `manifest.json`.
`universe_id` `7f54d392…`. Inputs: existing window `tickerhistory_training_native_20260919/`
(445 dates, 7,650 IDs, unchanged) plus six new years `tickerhistory_training_{Y}0101_{Y}1231_
20260920/` + `tickerhistory_training_native_{Y}_20260920/segments`, Y = 2014..2019
(232–284 MB native segments each).

## 3. What checkpoint 15 established (and did not)

ESTABLISHED (universe construction, measured): a point-in-time, liquidity/cap-ranked,
delisting-aware universe builder, native/engine-layer, 100% agreement against a 42-case stdlib
oracle, run once on real data 2012-12-31..2019-11-29 (84 monthly rebalances, 1,955 attached
sessions, 13,369 source IDs, three top-N cuts × two bands, 28.84 s wall, 87,539,712 B peak WS).
Headline numbers:
- Monthly one-way turnover, band 0.00, 83 non-seed rebalances: t1000/t2000/t3000 mean
  4.65/3.86/3.66 %, median 3.50/2.60/2.37 % (seed-inclusive 84-rebalance mean is higher:
  5.19/4.41/4.21 %); band 0.10 roughly 30 % lower.
- Cumulative distinct members 2013–2019: t1000 2,278; t2000 4,447; t3000 6,624 — t2000/t3000
  both exceed `kMaxIcInstruments` 4096 for a full 2013–2019 block.
- Survivorship t1000 band 0.00: 467/2,278 ever-members (20.5 %) ended before 2019-12-31;
  per-year exits 2013–2017 run 2.3–4.5 % vs. Russell ≈5.2 %/yr; both LOWER BOUNDS (backfill
  policy unknown).
- Coverage: 7.2k–9.5k source IDs/year, eligible median 5.9k–7.3k; members median = top_n every
  year/cut; `gics_missing_members` 0 all cuts.
- CONTAMINATED by a data hole (§5), labelled not corrected: churn at 2016-12-30/2017-01-31
  (all cuts), 2016 `drops_last_bar` totals, `union_by_year` 2017 and every cumulative column
  from 2017 on, 2016/2017 non-missing medians.

NOT ESTABLISHED: alpha, Sharpe, net P&L, capacity. Checkpoint 15 measures membership/churn
only — no forecast signal, return, or cost. The only forecast evidence on the branch is still
checkpoint 14's 2013-only rank IC (+0.09 to +0.10 at h=21) and net decile spreads on 189
observations (sign-and-shape only, one strong momentum year, no out-of-sample test). The real
run does not touch sealed 2023–2025 (0 segments refused, latest attached 2019-12-31, seal
policy `RefuseAtOrAfterValidationBeginV1` verified active).

## 4. Rulings made this session (all also in progress.md, with cost-if-wrong)

Every progress.md line in 80–110 containing "RULING"/"Ruling" is represented; read the cited
line for full sub-ruling text.

1. (:81) Scope: Stage 1 = universe builder (this checkpoint), Stage 2 = multi-year/multi-
   universe equity-ic, Stage 3 = alpha library, Stage 4 = combination/portfolio; v2's "Stage 2
   vs D-3 vs walk-forward" question superseded (D-3 moves to Stage 4). Cost if wrong: none —
   user directive explicit.
2. (:84) R15-1..R15-16, design methodology: ADV$ 63-session median rank key (≥57/63
   nonmissing); vendor-shares market cap reported-only, never a rank key; all three cuts
   {1000,2000,3000} emitted side by side (AR-7, ledger Option A, `trial_count_declared` 0);
   monthly cadence, next-session effect, bands {0.0,0.10}; $1 price floor (Russell rule);
   instrument type unknown accepted; per-year contexts deferred to cp16 with union sizes
   reported; 63-session warmup; six ingestions under `tickerhistory-qa-v1`; two lower-bound
   survivorship metrics; `warehouse.duckdb` not a parent; bounded/no-heap engine; seal refuses
   ≥2020-01-01; output set; oracle/comparator protocol; process. Cost if wrong: baked into the
   frozen design and the one real run before review could catch a methodology error —
   correction needs a new pre-registered attempt, and every cp16 number inherits the error.
3. (:87) First-draft §12: Q1 ACCEPT v1 ledger schema shape; Q2 CONFIRM extending both
   zero-rejection validator sites (`trial_ledger.cpp:301`, `:729-733`) under the allow-list;
   Q3 ACCEPT `;`-separated dir/manifest flags; Q4 RULE `membership.bin` codec lives in the
   engine unit. Cost if wrong: Q4 explicitly "minor relocation"; Q1–Q3 would surface as a
   design-review finding pre-freeze, not a silent production error.
4. (:89) DR15-1..DR15-12 (design review, 6C/12I): DR15-1 revoked the direct ID→slot index (real
   2013 IDs already exceed 2^20: 2,199 of 7,650, max 4,163,749) for a pre-sized open-addressing
   hash; DR15-2 pinned membership SET (keep-then-fill) and OUTPUT ORDER (rank-ascending);
   DR15-3 pinned emission timing (write only once effective session observed; missing follow-on
   = stage ERROR) and `--rank-end` 2019-11-29; DR15-4 integer-only markers; DR15-5
   `Fraction(float)` comparator with mirrored double arithmetic. Cost if wrong: these are
   Criticals against IDs already present in 2013 data — unfixed, real securities would be
   mis-slotted/rejected and `membership.bin` could be non-deterministic or silently drop a
   rebalance with no error.
5. (:91) DR15-13..DR15-16 (scoped re-verify found 3 NEW Criticals in Revision 2 itself):
   DR15-13 provisional-insert-then-rollback for a duplicate unknown ID within one session;
   DR15-14 pinned `PitDropKind`/`PitExitKind` int↔text mapping; DR15-15 all marker medians as
   exact `_x2` integers; DR15-16 reconfirmed Q1/Q3 accepted. Cost if wrong: DR15-13 guards
   against silent ID-table corruption uncaught by any test — this class of bug had already
   slipped past the first review once.
6. (:93) T3 ambiguities 1–13, ALL ACCEPTED: oracle case IDs/inputs/keys authoritative; rank-0 =
   not-ranked; Add=0/Keep=1, Rank=0/LastBar=1; calendar year attributed by EFFECTIVE session
   (later found NOT to match shipped engine behavior — see ruling 8/I-1); F9 turnover in engine
   test; versioned measurement JSON; `request.json` key `rebalance_count`; generic manifest
   digest walker; inputs mismatch fails the case; F8 tolerates extra keys; exe substring check
   acceptable. Cost if wrong: these define what "oracle == engine" means — the 42/42 PASS is
   only meaningful if they're right, and one (year attribution) was later found to describe the
   oracle's convention, not the engine's (hence I-1 is a flagged deviation, not a silent pass).
7. (:104) R15-17, data hole: cp15 run STANDS AS MEASURED (pre-registered, no re-tune); hole and
   its contamination list go into receipt qualifications, `PLATFORM_PROGRESS.md`, and this
   handoff; remediation is a NEW pre-registration, not a silent fix; frozen design not edited.
   Cost if wrong (stated explicitly): 2017 union and 2016 churn numbers are wrong in the
   published artefacts — but labelled contaminated, a known documented bias, not undetected.
8. (:105) End-review I-1..I-5: I-1 `coverage_by_year` attributes by RANK-session year, contrary
   to §15.2 pin 5/6 wording — MEASURED CONVENTION STANDS. I-2 receipt carries re-evaluation
   provenance + binds manifest producer digest to current `atx-impl.exe`. I-3 receipt
   qualifications add the hole and `kMaxIcInstruments` exceedance as numeric cp16 blockers.
   I-4/I-5 deferred (§5). Cost if wrong: I-1 wrong means cp16 silently misattributes a
   rebalance's year at every boundary unless it re-derives attribution itself; I-4/I-5 unfixed
   means a crash between pre-registration append and guarded body could leave an orphan ledger
   line, undetected short of a manual chain audit.

Process rulings (:82, :102, :109, carried forward, no separate cost-if-wrong in source): any
cp15 CMakeLists.txt edit drifts cp13/cp14 receipt CMake pins exactly as cp14 drifted cp13 —
record in an addendum if it recurs, never touch a receipt; two one-line parent fixes (T1 decode
order, T2 `fs::path` copy) made and measured without a re-review round per standing directive;
a runner lineage edit caused a post-run crash — fixed + versioned re-evaluation, not a re-run
(a re-run declares 0 new trials for no new information, same logic as cp14's attempt-1
handling).

## 5. Open items

### 5.1 Pre-existing, unrelated, open
`StageRunSyntheticSmoke.SyntheticSmoke_OnFlagsProducesFiniteScorecard` still fails with
`invalid stod argument` (CTest #944), unchanged, untouched this session. See v2 §5.1 for prior
diagnosis; cp15 did not touch `stage_run.cpp` or its test.

### 5.2 Deferred by ruling / measured deviations open for cp16
cp13 T3–T5 unchanged since v2. cp14 Stage 2 per-year contexts carried into the alpha-first
cp16 plan (§6), not the older full-block shape (see §6 supersession note).

Data hole (2016-01-15..2018-02-16, 19 corrupted pre-holiday sessions; R15-17, :104): vendor
archive has corrupted OHLC the session before every listed NYSE holiday (open == prior close in
98.7 % of rows vs. 12 % normal; high/low truncated), rejected by `tickerhistory-qa-v1`'s
`ohlc_order_violation` check. Contaminates churn at 2016-12-30/2017-01-31 (all cuts), 2016
`drops_last_bar` totals, `union_by_year` 2017+ and its cumulative columns, and 2016/2017
non-missing medians. Remediation (`tickerhistory-qa-v2` accepting valid close/volume with OHL
flagged, and/or a hole-aware rank rule) is a NEW pre-registration, conditional per the v3 goal
prompt: only if per-year 2016/2017 alpha numbers differ materially from neighbors.
`coverage_by_year` attribution (I-1, :105): engine attributes by RANK-session year; design
§15.2 pin 5/6 and the T3 oracle pin (:93) said effective-session year. Engine, oracle, and
42/42 comparator all agree on rank-session — ruled the measured convention stands. cp16 should
accept this explicitly or re-derive its own attribution from effective sessions.

Identity caveats: 56 same-ticker ID-segment overlaps; 610 tickers used by >1 security ID; 955
IDs with >1 `todayTicker`; vendor `shares` reported-only, never a rank key; instrument type
unknown accepted, no `require_sector` variant; 2018 has 83 duplicate-key dates, quarantined.
`kMaxIcInstruments` 4096 vs. measured unions: cumulative t2000 4,447 / t3000 6,624 (both above
cap for a full 2013–2019 block); 2017 per-year t3000 union 5,022 (also above cap, contaminated
by the hole via stale 2016-12-30/2017-01-31 membership). Per the v3 goal prompt the cap is
respected via per-year contexts, not raised; a year that still doesn't fit is evaluated on
top-3000 as a documented failure-to-fit.

### 5.3 Close-out-style edits this session
No separate addendum — the receipt was written once already containing this content, since no
prior cp15 receipt existed to addend. Runner `iteration15_run_equity_universe.py` edited twice:
once to accept the documented predecessor-design lineage (`ORACLE_DESIGN_LINEAGE`) instead of
failing on the oracle's `be4b3b67…` citation; once (post-crash) replaced by a fixed 946-line,
digest-only-pin version (`bf868e67…`) after the original crashed post-run processing on a
non-digest pin value (outputs/ledger unaffected; fixed via a versioned re-evaluation script,
not a re-run). Receipt writer extended after the end review to carry I-1/I-2/I-3 content before
it was ever run. Two in-source parent fixes without re-review: `decode_membership_bin` (trailer
verified after body parse) and `stage_equity_universe_test.cpp:527` (`const fs::path &dir`).

### 5.4 Deferred minors
Carried unchanged from v2 §5.4: cp13 T1 D1–D5 + F4–F12, cp13 T2 Minors 1–11 + 2, cp14 T1 MINs +
NEW-1..4, cp14 T2T3 M-1..M-10, cp14 T4 M-1..M-8, cp14 T5 M-1..M-13 (notably `reserve_directory`
misreporting I/O errors) — enumerated in `final-branch-review.md` "Open items".
New this session, `cp15-end-review.md` (0C/5I/11M; I-1/I-2/I-3 resolved per §4 ruling 8 and
§5.2/§5.3): I-4 — two fallible calls between the ledger pre-registration append and the guarded
stage body (`stage_equity_universe.cpp:1071-1072`); I-5 — no exception/write-failure test after
pre-registration. Both deferred for cp16. The 11 Minors are enumerated in `cp15-end-review.md`
directly, not restated here.

## 6. How to resume

**Priority change, read first**: a post-stop user directive (:109) puts the SHORTEST HONEST
PATH to a first multi-year, net-of-cost alpha scorecard ahead of further universe-track
engineering. `2026-09-20-next-goal-prompt-v3.md` is the authoritative next-parent prompt; read
it in full before dispatching anything. It defines checkpoint 16 as "first alpha scorecard":
per-year 2013–2019 evaluation contexts (preferred route: the existing `atx-impl panel` stage
run PER YEAR, warmup 256 sessions + the year, membership-restricted via
`C:/atx/data/equity_universe_pit_2013_2019_20260920/membership.bin`; measure peak WS on one
year before committing), cp14 configurations UNCHANGED (momentum_252, momentum_126,
blend_equal; horizons {1,5,10,21,63}; Q=10; existing bootstrap recipe; cost model 5 bps + 365
bps borrow) on top-1000 and top-3000 (band 0.00) as AR-7 RESTRICTIONS of the cp14
pre-registration (N stays 30 — years/cuts are not new trials), plus exactly ONE new
pre-registered OUTPUT: annualised net decile-spread Sharpe per (signal, horizon, year, cut)
from non-overlapping sub-series (stride h) with a block-bootstrap CI, plus a pooled 2013–2019
value and a sign-stability count. Data hole runs AS-IS, labelled; remediation is conditional
only (§5.2). Ends with a one-page "alpha scorecard" markdown (per signal × cut: pooled net
Sharpe + CI, per-year Sharpe row, rank IC row, turnover, breadth, caveats) BEFORE any Stage 3
or Stage 4 work.
**Supersession notice**: `2026-09-20-equity-platform-status-cp15.md` §3 "PLANNED" (hole
remediation as a mandatory step 1, a from-scratch bounded streaming context builder as a
mandatory step 2) predates the alpha-first directive and is SUPERSEDED: remediation is now
conditional on 2016/2017 differing materially from neighbors, and the streaming builder is
built only if the per-year `panel`-stage approach (or the thin exporter fallback) fails a 3 GB
peak-WS measurement on one year. Do not restart the older plan; measure first.

1. Read this file, v1 §"Constraints", v2 §4–§7, `progress.md` in full (esp. lines 80–110),
   `cp15-end-review.md`, `2026-09-20-next-goal-prompt-v3.md`.
2. Verify §2.1 pins with `sha256sum`; cp12/cp14/cp15 designs MUST match exactly. Verify trial
   ledger: 4 lines, pre-registered/completed (1–2 cp14 N=30, 3–4 cp15 declared 0).
3. Native loop (parent-only, serialized, ≤ 2–3 wrapper calls per PowerShell process,
   `CCACHE_DISABLE=1`, never raw cmake/ninja) — command block copied verbatim from v2 §6:
   ```powershell
   $env:CCACHE_DISABLE='1'
   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/atx-build.ps1 configure -Preset equity-dev -Groups 'risk;data;core;book;eval' '-DVCPKG_MANIFEST_MODE=OFF' '-DVCPKG_INSTALLED_DIR=C:/Users/natha/vcpkg/installed' '-DFETCHCONTENT_BASE_DIR=C:/atx/.worktrees/equity-platform/deps/equity-dev'
   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/atx-build.ps1 check 'atx-impl\src\<file>.cpp' -Preset equity-dev
   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/atx-build.ps1 build atx-impl-tests -Preset equity-dev -Jobs 4
   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/atx-build.ps1 -Ctest -Preset equity-dev -Jobs 1 -R '^(ConfigEquityIc|EquityIcTerminalValue|StageEquityIc|TrialLedger)\.' -VV --output-junit 'C:/atx/.worktrees/equity-platform/build-equity/<name>.xml' --output-log 'C:/atx/.worktrees/equity-platform/build-equity/audits/<name>.log'
   ```
   Traps carried from v2: PowerShell 5.1 swallows ctest `-O`/`--verbose` (use `--output-log`,
   `-VV`); `check` on a new TU needs a reconfigure first; a relative `--output-junit` resolves
   against the build dir; `atx-impl-tests` does NOT rebuild `atx-impl.exe` — build `atx-impl`
   explicitly before a real-data run; `-Wunused-result`/`/WX` on `Json::parse` — cast `(void)`.
   New traps from checkpoint 15: Bash background dies at 10 min — detach long native runs with
   `Start-Process` and monitor the PID; always `--dry-run` a runner before the real invocation;
   `-Wrange-loop-construct` is fatal under `/WX` for a range-for copying a non-trivial type by
   value — bind `const &`; never put a non-pin value into a runner's pin-comparison dict — keep
   `is_pin`/`pins_differ` digest-only; an oracle cut before a design's later `§N` amendment
   legitimately cites the predecessor digest — special-case that lineage, don't treat it as
   drift; a receipt writer must not pin a doc that cites the receipt's own SHA (cp14 §8.3
   circular-pin trap, cp15 avoided it); a stage's pre-registration manifest carries only the
   pre-registration line's digest — the terminal ledger line carries the manifest digest, not
   the reverse.
4. Engine gate for reused machinery: `-R '^(DataPointInTimeUniverse|EvalCrossSectionIc)\.'`,
   then the matching `iteration1{4,5}_native_comparator.py` (cp14 expects 42/42 vs. oracle v2;
   cp15 expects 42/42 vs. oracle v1; refuses an existing `--out`).
5. cp16 research (check for `research-cp16*.md` first — §1): the per-year-context question from
   `2026-09-20-next-goal-prompt-v3.md` item 1 (panel-stage-per-year vs. thin exporter; measure
   peak WS on one year; don't build the full streaming builder unless both fail 3 GB).
6. cp16 design note (one, frozen, embedded SHA, one design review) per
   `2026-09-20-next-goal-prompt-v3.md` items 2–4: pre-registration = cp14 configurations
   UNCHANGED as AR-7 restrictions (N stays 30), plus the one new pre-registered Sharpe output
   with acceptance bars written BEFORE the run. Then implement, measure natively, receipt,
   docs, ledger line, and the one-page scorecard — then STOP for the user's next decision (more
   signal families vs. combination/portfolio vs. data quality) per the v3 goal prompt item 4.

## 7. Agent/process conventions used this session

Same as v2 §7 (fresh implementer per task, edit-only, reports to `cp<N>-task-<T>-report.md`;
parent builds/tests and appends every measurement/ruling to `progress.md` before the next
dispatch; independent read-only reviewers write `...-review.md` with verdict lines; receipts
immutable Python-written JSON with pins, failed attempts versioned; Edit/Write tools for file
changes, not Python workarounds), plus additions from this session:
- Design review can be resumed for a SCOPED re-verification (only the Critical fix set) when a
  revision is narrow — used twice (Revision 2's C-1..C-6 fixes, then its own new N-1..N-3).
- T2 and T3 can be dispatched in parallel once T1 (engine unit) builds, since both depend on
  the engine's public interface but not on each other.
- A validator extension accepting `trial_count_declared` 0 for an explicit allow-listed
  purpose is the accepted pattern for stages that are not forecast trials but still need
  hash-chained, receipted provenance.
- A runner crash discovered AFTER a real run's outputs are already correct is fixed by
  versioning a new runner script, writing a separate re-evaluation script that re-derives the
  accept/reject decision from the existing outputs, and recording that provenance in the
  eventual receipt — never a silent re-run.
- Agents cannot be resumed across a session restart; re-dispatch fresh with the same brief.
