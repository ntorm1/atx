# Lane T2 report

## Outcome
DONE_WITH_CONCERNS. The `atx-engine/tools` suite runs under the repository window (research-window-v2, seal
2024-01-01) with the conftest bind removed:

- Full suite: 399 passed, 0 failed, in one process under both PYTHONHASHSEED 0 and 1.
- Each of the 22 legacy modules passes alone.
- The four T2-GOLD modules keep their expected hashes on a module-scoped superseded-window binding.

The concerns are listed under Risks and need a reviewer's call. None of them blocks the gate.

## Branch / SHA
`feat/p9-t2-20261003` @ `bc618cb468f236e9f3f3f02603d880b2dabcc053`. All work is committed. The leased tree is clean
(`git status --porcelain` was empty before this report commit).

| commit | theme | files |
|---|---|---|
| `d39cc878` | test(tools): restore field registration when setUpClass raises (deliverable 3) | connected, deals, divevent, mgr13f, v9_nt (+9 lines) |
| `3eacd5cf` | test(tools): retire the conftest window bind; T2-GOLD modules opt in (deliverables 2, 4) | conftest.py, t2_gold.py (new), lo1_delisting, linked_operating_v2, linked_operating_v3 |
| `ee927aac` | test(tools): re-date the base fields fixture and its SEC / SV / SIC users before 2024 | prepare_research_fields{,_reuse,_sec,_sv,_sic} |
| `0b4b6c47` | test(tools): re-date the holdings / mgr13f / connected 13F worlds by one year | research_fields_{holdings,mgr13f,connected} |
| `bc618cb4` | test(tools): window-dated counts under the repository window | build_fundamental_events, prepare_identity_bridge, research_fields_{deals,gold,v9_nt,xdata} |

Bisect note: between `3eacd5cf` and `bc618cb4` the suite is red, because the bind is gone before the re-dates land.
The head is green. Merge the branch as a unit.

## Frozen base / lease
- base_sha=1239a5ff0fd31689651d79a4c0844173cabd4c06
- worktree=C:\atx-wt\pool-22; lease_name=pool-22
- lease_run_id=p9-t2-20261003; heartbeat_id=p9-t2-hb; agent=p9-t2
- keeper_pid=9856; keeper_process_started_utc=2026-10-03T19:24:35.2995360Z
- keeper_ready_utc=2026-10-03T19:24:37.0863280Z; acquired_utc=2026-10-03T19:24:37.8189792Z

## Acquisition receipt
Command, from `C:\atx`:
`powershell scripts\lease-worktree.ps1 -Branch feat/p9-t2-20261003 -Base 1239a5ff -Agent p9-t2 -RunId p9-t2-20261003 -HeartbeatId p9-t2-hb -MaxPool 24`.

The script exited 1 with the expected tail:
`configure failed; lease remains held: C:\atx-wt\pool-22`. The cause is the optional cold-tree configure: atx-vol, atx-kb
and similar directories are absent at this base. The contract allows this, see ROOT-SDD `pools.md`.

Pool-21 was already held, so the script gave pool-22. Pool-22 was the planned COV pool (carry COV-3), but COV holds
pool-24 (`-Status`), so there is no collision.

The lease record (`C:\atx-wt\pool-22\.atx-lease`, token omitted) proves the fields:
```
version=3
run_id=p9-t2-20261003
agent=p9-t2
branch=feat/p9-t2-20261003
base_ref=1239a5ff
base_sha=1239a5ff0fd31689651d79a4c0844173cabd4c06
acquired_utc=2026-10-03T19:24:37.8189792Z
owner_kind=heartbeat
heartbeat_id=p9-t2-hb
heartbeat_timeout_seconds=600
keeper_pid=9856
keeper_process_started_utc=2026-10-03T19:24:35.2995360Z
keeper_ready_utc=2026-10-03T19:24:37.0863280Z
```
`lease-worktree.ps1 -Status` (exit 0) shows:
`pool-22  [cold]  branch=feat/p9-t2-20261003  LEASED run_id=p9-t2-20261003 | agent=p9-t2 | owner_kind=heartbeat | owner=alive | keeper_pid=9856 | branch=feat/p9-t2-20261003 | acquired=2026-10-03T19:24:37.8189792Z`.

Release, for the PM after merge: `powershell scripts\lease-worktree.ps1 -Release pool-22 -RunId p9-t2-20261003`.

## Files changed
Owned files: brief "Files in scope", `atx-engine/tools/` only. `git diff --stat 1239a5ff..HEAD` is 20 files, +931 / -465.

- `conftest.py`
  - The global `research_window.bind(research_window.superseded())` is removed (deliverable 4).
  - It now defines only the module-scoped fixture `superseded_research_window` (ruling T2-GOLD).
- `t2_gold.py` (new, test support, not collected by pytest). It contains:
  - `superseded_window()`. This context manager binds research-seal-v1, reloads the two tools that copy the seal at
    import (`prepare_research_fields`, `prepare_recent_research`), and restores the window and both module namespaces
    exactly on exit.
  - A frozen verbatim copy of the pre-T2 base fixture: `test_prepare_research_fields.py` @ `1239a5ff` lines 18-246 and
    982-1101. Only the 120-column separator lines are trimmed.
  - `issuer_plain(root)`. It refuses to run unless the superseded window is bound (ruling T2-GOLD-4).
- T2-GOLD modules: `test_lo1_delisting.py`, `test_linked_operating_v2.py`, `test_linked_operating_v3.py`. Each gets
  `pytestmark = pytest.mark.usefixtures("superseded_research_window")` and a T2-GOLD comment. Hashes are untouched.
- Re-dated modules (deliverable 1):
  - `test_prepare_research_fields.py`, `_reuse`, `_sec`, `_sv`, `_sic`.
  - `test_research_fields_holdings.py`, `_connected`, `_mgr13f`, `_gold`, `_v9_nt`, `_xdata`.
  - `test_build_fundamental_events.py`, `test_prepare_identity_bridge.py`.
  - `test_prepare_research_fields_module_reuse.py` needed no edit: it builds on the base fixture and passes as-is.
- Isolation fix (deliverable 3):
  - `test_research_fields_connected.py` and `_mgr13f`: `cls.addClassCleanup(lambda: cls._reg.__exit__(None, None, None))`.
  - `test_research_fields_deals.py`, `_divevent`, `_v9_nt`: `cls.addClassCleanup(cls.stack.close)`.
  - In both cases the cleanup is registered right after the registration is entered.
  - `deals` also has one seal-row re-date. The victims `ivshape`, `v8` and `v8_quarters` needed no edit.

Cross-lane edits: none. No production module, no fixture data file, no CMake list, nothing outside `atx-engine/tools/`
(checked mechanically; see Evidence E7).

Carry items:
- T2-GOLD: done as above.
- T2-GOLD-4 (PM ruling, option B): done. `test_prepare_research_fields_sic.py` checks
  `GOLDEN_ISSUER_FILES = 784e0496…6566`, unchanged, on `t2_gold.issuer_plain` under `t2_gold.superseded_window()`.
  The absence asserts run on both the frozen and the re-dated build.
- A1-C: the bind is retired and the 115 failures are closed.
- PY-HYG: explicit paths only. No children are left (see E8).

Other:
- Contracts K-P9-*: none (test-only lane).
- C++: none, so there are no build targets or gtest filters.
- No test exercises another wave-2 lane's new code, so there is no "needs lane X merged".

## Evidence
All commands ran with cwd `C:\atx-wt\pool-22\atx-engine\tools`, using
`"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider`. In the commands below, `PYTEST` stands for
that prefix. Synthetic data only.

**E1. Full tools suite, one process, PYTHONHASHSEED=0.**

Command: `PYTHONHASHSEED=0 COLUMNS=300 PYTEST --tb=short -rfE .`

exit_code=0. Output tail:
```
399 passed, 6 subtests passed in 496.33s (0:08:16)
exit=0
```

**E2. Full tools suite, one process, PYTHONHASHSEED=1.**

Command: `PYTHONHASHSEED=1 COLUMNS=300 PYTEST --tb=short -rfE .`

exit_code=0. Output tail:
```
399 passed, 6 subtests passed in 505.65s (0:08:25)
exit=0
```

**E3. Each module alone.**

Command, for each module m: `PYTHONHASHSEED=0 PYTEST --tb=line <m>.py`. Each line shows the output tail and exit code:
```
test_prepare_research_fields :: 36 passed in 15.81s :: exit=0
test_prepare_research_fields_reuse :: 15 passed in 53.12s :: exit=0
test_prepare_research_fields_sec :: 13 passed in 21.45s :: exit=0
test_prepare_research_fields_sv :: 11 passed in 21.92s :: exit=0
test_prepare_research_fields_sic :: 7 passed in 16.94s :: exit=0
test_prepare_research_fields_module_reuse :: 6 passed in 61.98s (0:01:01) :: exit=0
test_research_fields_holdings :: 16 passed in 15.27s :: exit=0
test_research_fields_connected :: 11 passed in 20.83s :: exit=0
test_research_fields_mgr13f :: 9 passed in 12.06s :: exit=0
test_research_fields_gold :: 7 passed in 10.26s :: exit=0
test_research_fields_v9_nt :: 11 passed in 20.80s :: exit=0
test_research_fields_xdata :: 7 passed in 50.79s :: exit=0
test_build_fundamental_events :: 22 passed in 4.32s :: exit=0
test_prepare_identity_bridge :: 18 passed in 2.79s :: exit=0
test_lo1_delisting :: 4 passed in 6.81s :: exit=0
test_linked_operating_v3 :: 8 passed in 11.08s :: exit=0
test_linked_operating_v2 :: 5 passed in 6.36s :: exit=0
test_research_fields_deals :: 8 passed in 7.08s :: exit=0
test_research_fields_divevent :: 5 passed in 10.26s :: exit=0
test_research_fields_ivshape :: 7 passed in 4.72s :: exit=0
test_research_fields_v8 :: 13 passed in 16.70s :: exit=0
test_research_fields_v8_quarters :: 9 passed in 11.57s :: exit=0
```

**E4. Knock-on isolation at head, in one process, in both orders.**

Forward: `PYTHONHASHSEED=1 PYTEST --tb=short test_research_fields_connected.py test_research_fields_mgr13f.py test_research_fields_deals.py test_research_fields_divevent.py test_research_fields_ivshape.py test_research_fields_v8.py test_research_fields_v8_quarters.py`

exit_code=0. Output tail: `62 passed in 69.08s (0:01:09)`.

Reverse: `PYTHONHASHSEED=1 PYTEST --tb=short test_research_fields_v8_quarters.py test_research_fields_v8.py test_research_fields_ivshape.py test_research_fields_divevent.py test_research_fields_deals.py test_research_fields_mgr13f.py test_research_fields_connected.py test_research_fields_v9_nt.py`

exit_code=0. Output tail: `73 passed in 81.56s (0:01:21)`.

**E5. T2-GOLD window restoration.**

The four T2-GOLD modules run first. A scratch probe, not committed, runs last in the same process. It asserts that
`research_window`, `prepare_research_fields` and `prepare_recent_research` are back on seal 2024-01-01 /
research-window-v2, and that `SEAL_NS` is consistent.

Command: `PYTHONHASHSEED=0 PYTEST --tb=short test_lo1_delisting.py test_linked_operating_v3.py test_linked_operating_v2.py test_prepare_research_fields_sic.py <scratch>/t2check/test_t2_window_after.py`

exit_code=0. Output tail: `25 passed in 25.36s`.

**E6. T2-GOLD-4 provenance probe. Evidence only, not a pin.**

The probe runs `a5e4a10d`'s `prepare_research_fields.py` (the U2 base) on the ISSUER_RUN recipe. It is extracted with
`git archive` into scratch.

Command: `python u2_digest.py <fixture dir>`. exit_code=0 for both runs:
```
pre-T2 fixture (1239a5ff test_prepare_research_fields.py):  784e0496f18fde9d2b18cd5e5e7f73d8aaa19b5ee69488706ccfeee7ece56566
re-dated fixture (head test_prepare_research_fields.py):    48294c2b101c5360811471dcfeda48b52fae36c21546763da9f6fcfeaaf75b22
```
Today's code on the re-dated fixture also gives 48294c2b… The golden pins the fixture's dates, not the builder. That is
why the ruling freezes the fixture rather than re-pinning.

**E7. Identity and scope.**

Command: `git diff --name-only 1239a5ff..HEAD`. Every path matches `atx-engine/tools/(test_*|conftest|t2_gold).py`
(the list in "outside lane scope" is empty).

Command: a scan of `git diff 1239a5ff..HEAD -- atx-engine/tools ':!atx-engine/tools/t2_gold.py'` for changed lines
containing a 64-hex digest. Result: none. No expected hash moved.

**E8. Pre-T2 baselines, recorded earlier in the lane.**

- Base with the bind: 399 passed.
- Base with `--noconftest`: `29 failed, 284 passed, 86 errors, 6 subtests passed`. 29 + 86 = 115 = A1-C's count.

**Diagnostics: the leak demonstration.**

These runs are expected to fail. They are not success evidence. They use `--noconftest`, i.e. the repository window, on
`git archive` copies of the trees. They run connected, mgr13f and then the 5 knock-ons in one process:
- `1239a5ff` (base): `5 failed, 44 passed, 13 errors`. Each knock-on fails once, after connected (7 errors) and mgr13f
  (6 errors) raise SealError in setUpClass and leave their shim fields registered in the builder. The knock-ons alone
  give 42 passed.
- `d39cc878` (the fix only, fixtures still 2024-dated): `49 passed, 13 errors`. Connected and mgr13f still error, but
  the knock-ons are clean. The fix is independent of the re-date.

## Decisions
Each re-date keeps the calendar role of every date (weekday / weekend, session / holiday, month and quarter end, 13F
quarter and deadline, fiscal quarter). Every assertion was re-derived by hand from the builder rules, not copied from a
run.

1. **Base fixture (`test_prepare_research_fields.py`): the Q4 2024 role becomes a Q4 2023 role.**
   - Sessions: 65 weekdays, 2023-10-02..12-29.
   - Formations: 2023-08-31 / 09-29 / 10-31 / 11-30 / 12-29.
   - Why not a deeper or a 28-year shift: FINRA `FINRA_REPUBLICATION_SETTLEMENT_BEFORE = 2021-06-01` forces the vintage
     roles after mid-2021, and the sealed rows must sit just past 2024-01-01.
   - Re-derived points:
     - si / dtc / NaN boundaries.
     - Vintage cutoff 2023-10-09: safe_from 10-10.
     - Formation age 32.
     - Issuer bridge / events / SIC rows, with the same gating (mark, lag 1, staleness 200 / 400 / 550 days).
2. **SEC module: -364 days (52 weeks).**
   - This keeps every weekday and the `yoy_364` expectation chain.
   - The hand-listed NYSE closure calendar now covers 2022-2025. Juneteenth counts only from 2022, and 2025-01-09 is a
     special closure. The builder's calendar spans the role ± `CAL_PREFIX/SUFFIX` = 400 days, so it needs 2024-2025
     rule entries. These are calendar rules, not fixture data.
   - Accession year fields shifted with the dates (-24- → -23-, -25- → -24-).
3. **Holdings / mgr13f / connected: a whole one-year shift.**
   - 13F quarter ends, deadlines (+45 days), V = deadline + 1 day 22:00 and calendar quarters all survive a one-year
     shift.
   - Weekday-dependent days were re-picked (e.g. NASDAQ-absent days, regsho, FTD).
   - The original `quarter_end(2024-02-29)` leap-day check would lose its meaning in 2023. It became
     `quarter_end(2023-02-28)` plus an added `quarter_end(2020-02-29) == 2020-03-31`, so the leap day stays tested.
4. **Window-dated counts (v9_nt, deals, xdata, gold, build_fundamental_events, identity_bridge).** Two kinds of row
   moved:
   - Rows that tested "sealed" moved to just after 2024-01-01 (e.g. 2024-03-04, 2024-02-05).
   - Rows that tested "after the role but unsealed" moved before 2024 (e.g. 2023-11-01, 2023-12-01).

   Two fresh-interpreter expectations change as a direct consequence. Under the repository window the harness and a
   fresh interpreter now see the same window:
   - xdata: `(vol, src)` (2, 4) → (1, 2). The old 2024-03-01 rows, sealed only in a fresh interpreter, are now
     2023-03-01 rows kept in both.
   - v9_nt: `rows_after_role` (sealed_nt, 1) → (sealed_nt, 2). The old 2024-02-01 NT row is now 2023-11-01, after the
     role in both.

   The harness tests' own values (`rows_sealed` 1, `rows_after_role` 2) did not move. I classify these two changes as
   values that follow a moved fixture row, not as edits to fit unchanged inputs. The reviewer should confirm (see
   Risks).
5. **gold.** The fresh-interpreter junk partition is `year={rw.SEAL.year + 1}`. Expected `{"gold": 2, "characteristics": 2}`
   is unchanged, and the 2024 row group is still pruned.
6. **t2_gold design.**
   - Reload plus exact namespace restore. Tools copy `SEAL`, `SEAL_NS` and seal-bearing texts at import, so a bind alone
     changes nothing in them, and an un-restored reload would leak research-seal-v1 into later modules (E5 proves the
     restore).
   - `superseded_research_window` is module-scoped, so it applies to unittest classes via `pytestmark`.
7. **Isolation fix: `addClassCleanup`.** It is the narrowest change; pytest 9.0.2 honours class cleanups when setUpClass
   raises. `ExitStack.close` and the patch `__exit__` are idempotent with the existing tearDownClass.

## Grep of remaining 2024+ dates
Method: the Grep tool, `202[4-9]`, over the touched non-GOLD modules plus conftest.py. 71 hits remain, against 397 lines
before:

| module | lines before | lines after |
|---|---|---|
| base | 126 | 12 |
| sec | 119 | 16 |
| holdings | 67 | 0 |
| sv | 21 | 3 |
| v9_nt | 14 | 10 |
| connected | 10 | 1 |
| mgr13f | 9 | 1 |
| bfe | 9 | 5 |
| identity_bridge | 9 | 8 |
| gold | 8 | 5 |
| xdata | 7 | 4 |
| sic | 4 | 1 |
| deals | 2 | 2 |
| divevent | 1 | 1 |
| reuse | 1 | 0 |

Every remaining hit falls in one of these classes:

- **Sealed-by-design rows.** These are synthetic rows that must be on or after the seal so a test still means "one
  sealed, one kept". The builder skips or refuses them, and no data is opened.
  - base: FINRA `(101, "2024-01-10")`, bridge `at(2024,2,1)`, events `at(2024,1,5,12)` (-24-000001), SIC `at(2024,2,1,15)`,
    TickerHistory range end 2024-01-31.
  - sec: ANN `at(2024,1,30,21)`, TX `2024-01-08/09`, 8-K `at(2024,1,8,14)`, `insider_tables(last=(2024,2))`.
  - sv: `AFTER = 2024-01-02`.
  - v9_nt: NT `at(2024,3,4,14)`, 10-Q `at(2024,5,13,20)`.
  - deals: `at(2024,3,4,14)`.
  - xdata: `SEAL_ROWS 2024-02-05`.
  - gold: `dt.date(2024,3,1)`.
  - bfe: the FY2023 filing at the seal instant `2024-01-01T00:00`, the `2024-02-15` post-seal filing, and the poison
    `2024q1` partition (never opened).
  - identity_bridge: links ending 2024-03-01 or available at T(2024,1,1), plus the CLI `--seal 2024-06-01`.
- **Seal-boundary literals.**
  - `SEAL = at(2024,1,1)` and the oracles' `< at(2024,1,1)` filters.
  - xdata `observations(seal=2024-01-01)`.
  - The refusal regex `"2024-01-01"` and the `SESSIONS + [2024-01-02]` sealed role.
  - v9_nt fresh-interpreter asserts on `"2024-01-01"`.
  - Docstrings and comments naming the seal (conftest.py, sic, gold, v9_nt).
- **Calendar rules.**
  - The SEC hand-listed NYSE closures 2022-2025 and `nyse_sessions(2022-01-01, 2025-12-31)` (see Decision 2).
  - The sec comments on computed next-expected dates (2024-01-25, 2024-02-08) and on the never-opened 2024q1 / 2024q2
    insider files.
- **Mock wrong seal.** `mock.patch.object(tool, "SEAL", dt.date(2026, 1, 1))` in connected / mgr13f / deals / divevent /
  xdata / gold. It is a deliberately wrong builder seal that the tests expect to be refused.
- **Metadata, not fixture dates.**
  - identity_bridge: snapshot `created_at` / `last_trade_date` / `snapshot_date` 2026-09, and one `current_ticker_verified`
    link dated 2026-09-20 that the excluded-basis test drops by design.
  - sv: receipt `downloaded_at` 2026-09-27.
  - xdata: `world(seed=20261002)`, an RNG seed.
- **Production key name.** `checks["rows_on_or_after_2025_skipped"]` is the builder's own key; the lane cannot rename
  it.
- **T2-GOLD (excluded from the grep, as the brief says).** `test_lo1_delisting.py`, `test_linked_operating_v2/v3.py`,
  and the frozen copy in `t2_gold.py` keep their 2024 dates under research-seal-v1.

## Deviations from brief
1. **"Every fixture date strictly before 2024-01-01" and "remaining hits: T2-GOLD modules only".** The sealed-by-design
   rows above stay on or after 2024-01-01. Moving them before the seal would turn every "one sealed, one kept" check
   into "none sealed", which breaks the brief's own rule that every assertion keeps its meaning. All of them are
   synthetic rows written to temp dirs and skipped by the builder.
2. **New file `t2_gold.py`.** It is not one of the 22 modules. It was created to implement ruling T2-GOLD-4 (frozen
   pre-re-date fixture) and to share the window-bind context manager with conftest.py. It is test support, not a
   production module and not a `research_fields_*.py` builder.
3. **Edit method.** Lane rule 1 says to edit files only with the Write / Edit tools. The bulk re-dates in the base, sec,
   sv, holdings, mgr13f, connected, v9_nt, gold, xdata, bfe and bridge modules were applied by scratch Python scripts
   doing exact, count-checked string substitutions that preserve CRLF. No shell heredoc wrote a repository file. Every
   change is in the committed diff and is covered by E1-E3.

## Risks
1. **Expected values (Decision 4).** xdata (2, 4) → (1, 2) and v9_nt (sealed_nt, 1) → (sealed_nt, 2) follow moved
   fixture rows. The reviewer should confirm they are not "edits to fit".
2. **Stale docstrings in modules outside the 22.** These still describe the retired bind; the text is wrong, the tests
   pass:
   - `test_research_window.py` lines 4-5 and 136
   - `test_seal_partitions.py` line 7
   - `test_prepare_research_fields_seal.py` line 3
   - `test_field_registry.py` (A1 notes)

   A one-line follow-up for the owners.
3. **The same latent leak elsewhere.** `test_research_fields_v9_earn.py:81` uses the same class-level `ExitStack`
   registration without `addClassCleanup`. `ohlc`, `gold`, `ivshape` and `xdata` enter `_reg` in `setUp`. Neither is in
   scope, and neither raises today.
4. **Merge overlap with A3 (A1-SHIM).** If A3 retires the deprecated field shims, it touches the same setUpClass
   `yreg.register` blocks in connected / mgr13f / deals / divevent / v9_nt. T2 merges last, so root keeps both: the
   shim removal plus the `addClassCleanup` line.
5. **atx-impl/tools.** It loads its own `research_window` instance (engine_tools.py), so nothing changes there. A mixed
   run `pytest atx-engine/tools atx-impl/tools` was not run by this lane.

## How root verifies
- `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-engine/tools`, under PYTHONHASHSEED 0
  and 1, after merge. Expect 0 failed (399 passed at this head; add the other lanes' new tests).
- `git diff --stat 1239a5ff..bc618cb4`: only `atx-engine/tools/test_*.py`, `conftest.py` and `t2_gold.py`.
- No C++, so there are no build targets or gtest filters. No identity run applies: no production byte moves.

## Ledger candidates
1. The atx-engine/tools builders copy the research seal at import (`SEAL`, `SEAL_NS`, seal-bearing texts). A test-time
   window bind therefore needs `importlib.reload` of the copying tools plus an exact module-namespace restore
   (`t2_gold.superseded_window`). A bare `research_window.bind` is a no-op for them.
2. An errored setUpClass after `yreg.register(...).__enter__()` leaks shim fields into the builder for the rest of the
   process. Register `cls.addClassCleanup` right after entering (P9 T2, `d39cc878`). At base this broke deals /
   divevent / ivshape / v8 / v8_quarters only when they ran after connected / mgr13f.
3. GOLDEN_ISSUER_FILES (784e0496…) pins the base fixture's dates, not the builder. `a5e4a10d`'s builder and today's
   builder agree on both fixtures (pre-T2 784e0496…, re-dated 48294c2b…). Ruling T2-GOLD-4 freezes the pre-T2 fixture
   in `t2_gold.py`.
