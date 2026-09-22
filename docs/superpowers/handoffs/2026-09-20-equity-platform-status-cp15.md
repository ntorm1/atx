# Equity platform status — end of checkpoint 15 (2026-09-20, late session)

Worktree `C:\atx\.worktrees\equity-platform`, branch `feat/equity-platform-20260920`, HEAD
`dffb609b7a3c` (unchanged all session). ~214 uncommitted entries. No commit or merge was made;
none is authorized. Nothing is in flight after this file: the `cp15-handoff` writer agent was
STOPPED before it wrote `2026-09-20-equity-platform-parent-goal-v3.md` (that file does NOT exist;
the brief for it is in progress.md's last entries and in §4 below).

Ledger (memory, read all of it): `.superpowers/sdd/equity-platform-parent-goal/progress.md`.
Binding constraints: v1 handoff §"Critical workspace rules" + §"Integration constraints",
v2 handoff §4–§7, all still apply.

## 1. COMPLETED this session — Checkpoint 15: point-in-time universe builder (mission Stage 1)

Every stage-end requirement is met: native measurement, immutable receipt, PLATFORM_PROGRESS
entry, ledger lines.

| item | evidence |
|---|---|
| Research | `research-cp15-universe.md` (archives, code map, memory, 9 web sources, skeleton) |
| Design (frozen) | `atx-engine/reviews/2026-09-20-iteration15-point-in-time-universe-design.md`, Revision 3 + §15, 1,060 lines, SHA-256 `4810fda251c6c285b29413ab6bea05b46db66e9bb0620cf17950b45075267dc8`, embedded in the stage as `kEquityUniverseDesignNoteSha256` and runner-verified |
| Design review | `cp15-design-review.md` (6C/12I/9M → all resolved; scoped re-verify N-1..N-3 → Revision 3) |
| Ingestion 2014–2019 | `iteration15_ingest_tickerhistory.py`; receipt `iteration15-ingest-20260920-attempt2.json` (6/6 accepted; attempt 1 killed by parent, versioned); data `C:/atx/data/tickerhistory_training_{Y}0101_{Y}1231_20260920` + `tickerhistory_training_native_{Y}_20260920/segments` |
| Engine unit (T1) | `atx-engine/{include/atx/engine/data,src/data}/point_in_time_universe.{hpp,cpp}` + `tests/data/data_point_in_time_universe_test.cpp`; `DataPointInTimeUniverse` 37/37 (`iteration15-data-t1fix1-tests.log`); one parent fix (decode: trailer verified after body parse); data-group regression 94/94 |
| Oracle + comparator (T3) | `iteration15_universe_oracle.py` → `iteration15-universe-oracle-v1.json` (42 cases F1–F9); `iteration15_native_comparator.py`; native agreement PASS 42/42 (`iteration15-native-comparison-t1.json`, `-t1fix1.json`) |
| Stage (T2) | `atx-impl/src/stage_equity_universe.{hpp,cpp}`, `atx-impl equity-universe` (kSubcommands 14), trial-ledger non-trial purpose allow-list (0 declared iff purpose `point-in-time-universe-construction`); impl suites 60/60 (`iteration15-impl-t2-tests.log`); one parent fix (`const fs::path &dir`) |
| Real run | `C:/atx/data/equity_universe_pit_2013_2019_20260920/` — 1,955 sessions, 13,369 source IDs, 84 monthly rebalances (rank 2012-12-31..2019-11-29), 3 cuts × 2 bands, 28.84 s, peak WS 87.5 MB; runner crashed post-run (parent's lineage edit), fixed, versioned re-evaluation `iteration15-equity-universe-measurement-attempt1-reeval.json` accepted; no re-run |
| Trial ledger | lines 3–4: `iteration15-point-in-time-universe-0001` pre-registered → completed, checkpoint 15, `trial_count_declared` 0; N14 = 30 unchanged; chain valid; sidecar 4 lines |
| End review | `cp15-end-review.md`: SPEC Fix required (content items), QUALITY Pass; 0C/5I/11M — I-1..I-3 resolved inside the receipt, I-4/I-5 deferred |
| Receipt (immutable) | `atx-engine/reviews/2026-09-20-point-in-time-universe-validation.json` SHA-256 `2c52c6a4c2d2e841d8ffe3b8e1ced3e8c79d40eeba5a2176bcfc75b06068c07e`, status `stage1_universe_construction_measured`, accepted true, 47/47 checks |
| Docs | `PLATFORM_PROGRESS.md` "## Checkpoint 15" (lines 875–1077) + Next candidates item 1 = cp16; `atx-impl/docs/EQUITY_UNIVERSE.md` (198 lines); both READMEs +8 |
| Investigation | `cp15-idgap-investigation.md`: the 2016-12-30 anomaly is a DATA HOLE (vendor OHLC corruption on the session before every NYSE holiday 2016-01-15..2018-02-16, 19 sessions), NOT an ID remap; 99.2 % of names return under the same ID |

Headline numbers (universe construction only — not alpha, not capacity):
- Monthly one-way turnover, band 0.00, 83 non-seed rebalances: t1000/t2000/t3000 mean
  4.65 / 3.86 / 3.66 %, median 3.50 / 2.60 / 2.37 %; band 0.10 roughly 30 % lower.
- Cumulative distinct members 2013–2019: t1000 2,278; t2000 4,447; t3000 6,624
  (t2000/t3000 exceed `kMaxIcInstruments` 4096).
- Survivorship t1000: 467 of 2,278 ever-members ended before 2019-12-31 (20.5 %); per-year
  exits 2.3–4.5 % vs Russell ≈ 5.2 %/yr; both are LOWER BOUNDS (backfill policy unknown).
- Coverage: 7.2k–9.5k IDs seen per year, eligible median 5.9k–7.3k; members median = top_n.
- Contaminated by the hole (labelled, not corrected): churn at 2016-12-30 / 2017-01-31 (all
  cuts), 2016 `drops_last_bar` totals, `union_by_year` 2017 and every cumulative column from
  2017 on, non-missing medians 2016/2017.

Pins re-verified at stop: cp12 design `0a964aeb…`, cp14 design `888c726b…`, cp14 receipt
`38dd653f…` all exact; `build-equity/bin/atx-impl.exe` `8425fc03c136169a…` (== manifest
producer), `atx-impl-tests.exe` `ca42f54a…`, `atx-engine-data-tests.exe` `1e17b828…`.

## 2. IN PROGRESS at stop

- v3 handoff document: NOT written (agent stopped). Everything it needs is in progress.md
  (cp15 entries) and this file. Write it in the v2 shape (§1 stop point, §2 pins table,
  §2.2 evidence, §2.3 outputs, §3 established/not, §4 rulings, §5 open, §6 resume, §7
  conventions).
- Nothing else. No shells, monitors or agents alive.

## 3. PLANNED (mission order)

Stage 2 — Checkpoint 16: multi-year, multi-universe forecast evaluation (extend cp14
`equity-ic` to 2013–2019 per-year folds and ≥ 2 universe cuts on the cp15 memberships):
1. Hole remediation is a NEW pre-registration before any IC run (ruling R15-17): either
   preparation policy `tickerhistory-qa-v2` (accept rows with valid close/volume, flag missing
   OHL) for the 19 corrupted sessions, or a hole-aware rank rule (rank on the last full
   session ≤ rank date). Re-run the universe as attempt 2 (`--attempt 2`; each run appends
   2 ledger lines with 0 declared). Do not edit the frozen cp15 design; write a cp16 design.
2. Memory: full-block K=6,000 IC ≈ 2.93 GB (no headroom) and `kMaxIcInstruments`=4096 <
   t2000/t3000 unions → per-year contexts (warmup 256 + year, union ≤ 4096) or a cap ruling
   (RR-1 revision). The legacy dense `panel` cannot build 2013–2019 (~20 GB); a bounded
   streaming context builder from segments + membership.bin is required (`annual-panel-
   memory-review.md`, `bounded-equity-panel-design.md`).
3. Per-year and per-universe results are restrictions of the pre-registered cp14
   configurations (AR-7 semantics, N stays 30); state so in the ledger. Sign flips across
   years/universes = failed, not tuned. Sealed 2023–2025 never read.
4. Then Stage 3 (alpha library: reversal, momentum variants, vol/idio, liquidity, PIT
   fundamentals when the warehouse universe/delisting tables ship) and Stage 4 (walk-forward
   combination + turnover-aware portfolio + equity-book replay; D-3 cadence/hysteresis).

Deferred items carried: cp13 T3–T5; cp14 deferred minors (T1/T2T3/T4/T5 lists); cp15 end
review I-4 (two fallible calls between pre-registration append and guarded body,
`stage_equity_universe.cpp:1071-1072`), I-5 (no exception/write-failure test after
pre-registration), 11 Minors; coverage_by_year attributes rebalances by rank-session year
(measured deviation from design §15.2 pins 5/6, documented in the receipt); identity caveats
(56 same-ticker ID-segment overlaps, 610 tickers used by > 1 ID, 955 IDs with > 1
todayTicker); vendor `shares` reported-only; instrument type unknown; 2018 has 83
duplicate-key dates (quarantined). Pre-existing, untouched: `StageRunSyntheticSmoke`
failure (`invalid stod argument`).

## 4. Rulings I made this session (all in progress.md with cost-if-wrong)

R15-1..R15-16 (scope, ADV63 median key, market cap reported-only, three cuts side by side +
ledger Option A with validator allow-list, monthly cadence next-session effect + bands
{0, 0.10}, $1 raw price floor, instrument type unknown accepted, union reporting for cp16,
63-session warmup, run the six ingestions under qa-v1, survivorship metrics, warehouse not a
parent, engine layer/bounds/no-heap, seal refusal, output set, oracle protocol, process);
DR15-1..DR15-16 (hash-table IDs, keep-then-fill set / rank-ascending output, emission at
effective session + `--rank-end` 2019-11-29, integer markers, Fraction(float) comparator,
codec in engine, cadence helper in engine, validate-before-mutate, formula-only drop kind,
streamed CSV, provisional-insert rollback, exit_kind 0/1/2, `_x2` medians, Q1/Q3 accepted);
Q1–Q4; T3 pins 1–13; T1 deviations 1–8 + ambiguities 1–11; T2 deviations 1–3 + ambiguities
1–12; R15-17 (hole: run stands as measured, remediation is a new pre-registration); end-review
I-1 (measured attribution stands), I-2 (re-eval provenance + producer digest binding), I-3
(numeric cp16 blockers in receipt), I-4/I-5 deferred. Process: killed ingest attempt 1
(Bash 10-min cap) and relaunched detached; two one-line parent fixes without re-review; runner
lineage edit caused the post-run crash → fixed + versioned re-evaluation instead of re-run.

## 5. Traps learned (add to the v3 handoff §6)

Bash background commands die at 10 min — detach long runs with `Start-Process` and monitor
the PID; runner must be `--dry-run` first; `check` on a new TU needs a reconfigure;
`-Wrange-loop-construct` under /WX; never put non-pin values into a runner's pin dict; an
oracle cut before a §15-style amendment cites the predecessor digest (runner
`ORACLE_DESIGN_LINEAGE`); a receipt writer must not pin docs that cite the receipt; the
stage manifest can only carry the pre-registration line digest (terminal line carries the
manifest digest).
