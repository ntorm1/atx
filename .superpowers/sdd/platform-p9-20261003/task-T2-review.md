# Lane T2 review

## Verdict
**APPROVE.** There are 0 Required findings and 6 Suggested findings, all minor. There is no blocker and no major.

- **Spec compliance: PASS.** Deliverables 1-4, T2-GOLD, T2-GOLD-4, T2-MIN, A1-C and the carry's T2 gate are met. Deviation 1
  (sealed-by-design rows stay on or after 2024-01-01) is justified, see S4.
- **Task quality: PASS.** I found no hash edit, no edit made to fit, no seal breach and no isolation leak. Every
  changed expected value was re-derived by hand.

## Reviewed SHA
- Head: `2e9d6813530e61771f11b0ee8f4d4a22aa219e43` on `feat/p9-t2-20261003`, pool-22. The code head is `bc618cb4`, and
  `2e9d6813` adds the lane report.
- Base: `1239a5ff0fd31689651d79a4c0844173cabd4c06`. This equals `git merge-base HEAD feat/platform-p9-20261003`.
- This review was started by a first reviewer and stopped as PARTIAL by the owner stop. A fresh reviewer finished it.
  The first reviewer's checks are kept below and marked (R1). Everything else is mine (R2).

## Read
- The contract `wave2-review-dispatch.md`, `briefs/review-template.md`, `briefs/brief-T2.md` and `wave2-carry.md`
  ("All lanes", "Brief text superseded", T2).
- `resume-rulings.md` (T2-MIN, PY-HYG, DISK).
- Rulings in progress.md by grep: A1-C (l.56), T2-GOLD (l.98), T2-SLOT (l.228), PY-HYG (l.220), W2-EARLY (l.231),
  T2-GOLD-4 (l.238), the T2 DONE line (l.247).
- The lane report `task-T2-report.md` @ `2e9d6813`.
- The full diff `1239a5ff..2e9d6813 -- atx-engine` (2421 lines, scratch), every hunk of every file.

## Evidence (R2, all inside pool-22, explicit paths, `PYTHONDONTWRITEBYTECODE=1`, `-p no:cacheprovider`)
All runs used cwd `C:/atx-wt/pool-22/atx-engine/tools` and `"C:/Program Files/Python312/python.exe" -m pytest -q -p
no:cacheprovider --tb=short -rfE --basetemp=<scratch>`.

**E-R1. Full tools suite, one process, `PYTHONHASHSEED=0`.** Path `C:/atx-wt/pool-22/atx-engine/tools`. exit_code=0:
```
399 passed, 6 subtests passed in 528.09s (0:08:48)
exit=0
```

**E-R2. Full tools suite, one process, `PYTHONHASHSEED=1`.** Same path. exit_code=0:
```
399 passed, 6 subtests passed in 732.68s (0:12:12)
exit=0
```

**E-R3. Window-leak and knock-on order probe, one process, `PYTHONHASHSEED=1`.** The modules ran in this order:
1. `test_lo1_delisting.py`, `test_linked_operating_v2.py` (the T2-GOLD modules).
2. `test_prepare_research_fields_sic.py` (T2-GOLD-4 inside it).
3. `test_prepare_research_fields.py`. Its sealed-row counts, e.g. `rows_sealed_dropped == 1` for FINRA 2024-01-10, fail
   if research-seal-v1 leaked out of `superseded_window`.
4. The knock-ons in reverse: `test_research_fields_v8_quarters`, `_v8`, `_ivshape`, `_divevent`, `_deals`, `_mgr13f`,
   `_connected`, `_v9_nt`.

exit_code=0:
```
125 passed in 176.66s (0:02:56)
exit=0
```

Hygiene:
- After the runs, no python.exe with a pool-22 command line remained (count 0).
- My `--basetemp` scratch dirs are deleted.
- My runs wrote nothing into the tree: the newest `__pycache__` file predates this session (S6).

## Spec compliance, item by item
| item | state | how checked |
|---|---|---|
| D1: re-date the 14 modules, keeping calendar roles | **met** | I read every hunk. 13 modules are edited. `module_reuse` needs no edit (0 hits of 2024+; it passes alone per E3 and in E-R1/E-R2). Role checks are below. |
| D1: every assertion keeps its meaning | **met** | Sealed/kept pairs survive in every module (table below). The five changed expected values are re-derived below. |
| D1: grep with every remaining 2024+ hit explained | **met, with deviation** | My grep at head matches the report's classes exactly: sealed-by-design rows, seal literals, the calendar rule, mocks of a wrong seal, metadata and the production key name. See S4. |
| D2 / T2-GOLD: lo1, lo-v2, lo-v3 keep their hashes; module-scoped 2025-window fixture; comment names T2-GOLD | **met** | Each hunk only adds `import pytest`, a T2-GOLD comment and `pytestmark = usefixtures("superseded_research_window")`. The fixture is module-scoped in conftest.py. |
| T2-GOLD-4: GOLDEN_ISSUER_FILES kept, checked on a frozen pre-T2 fixture under research-seal-v1 | **met** | (R1) The frozen copy is verbatim (difflib vs base lines 18-246 and 982-1101; LF in both). (R2) `ISSUER_RUN` is identical in base and `t2_gold`: `list(tool.DEFAULT_FIELDS) + list(tool.ISSUER_FIELDS)` at base l.1177. `issuer_plain` refuses to build outside the window. |
| D3: knock-on isolation | **met** | `addClassCleanup` is registered right after the registration is entered, in connected, mgr13f, deals, divevent and v9_nt. It is idempotent with tearDownClass (point 5). E-R3 passes the reverse order in one process. |
| D4: global bind removed | **met** | `conftest.py` binds nothing globally. It only defines the module-scoped `superseded_research_window`. |
| Identity: test files only, no production byte, no hash | **met** | `git diff --name-only` shows 20 paths under `atx-engine/tools/` (conftest, t2_gold and `test_*`) plus the report. My Python scan for 64-hex digests found 0 changed lines. The only 64-hex lines are 2 context lines: `V2_GOLDEN` and `GOLDEN_ISSUER_FILES`. |
| T2-MIN: `t2_gold.py` accepted; are the bulk script edits mechanical? | **mechanical (accept)** | A pairing scan (scratch `pairs.py`) normalised dates, years, `-YY-` accession fields and `YYYYqN`. 60 pairs differ beyond dates, and I read each one. They are deliberate, explained edits: comments, temp-dir names (`m22`→`m21`, `role-2025`→`role-sealed`), the refusal regex, the five value changes below, gold `later`, the added leap-day assertion, the sic T2-GOLD-4 restructure, and the cleanup and pytestmark lines. No accession used as an id, RNG seed or count was changed as collateral. |
| Carry gate: tools suite 0 failed in one process under `PYTHONHASHSEED` 0 and 1, explicit paths | **met** | E-R1 and E-R2. |
| PY-HYG | **met** | Explicit paths. No child process is left (report E8; my own count is 0). |

## Lane-specific points (the R1 table, completed)
| # | point | state |
|---|---|---|
| 1 | No golden hash moved; the frozen fixture is verbatim; reload side effects | **done**, no finding |
| 2 | xdata (2,4)→(1,2) and v9_nt `rows_after_role` 1→2 derived by hand | **done**: both are correct |
| 3 | Bulk substitution collateral; sealed/kept pairs keep their meaning | **done** for all modules, no finding |
| 4 | Remaining rows on or after 2024-01-01 are sealed by design | **done**, see S4 |
| 5 | addClassCleanup restores registration on every failure path | **done**, no finding |
| 6 | Whole tools suite under PYTHONHASHSEED=0 | **done**: E-R1, plus seed 1 in E-R2 |

### (1) Hashes, frozen fixture and reload side effects
- (R1) The frozen fixture is verbatim, and the `issuer_plain` recipe equals base `SicEventsOverride` "plain".
- (R2) The hash scan found 0 changed digest lines, as above.
- **Reload side effects.** I walked the AST of `prepare_research_fields.py` and `prepare_recent_research.py` for
  module-level statements. Reload re-runs:
  - `ALL_FIELDS = {...}` and `FIELD_MODULES = [...]`. Both create fresh objects local to the module.
  - `research_fields_sec / _holdings / _price / _v8 .bind(globals())`.
  - `ALL_FIELDS.update(...)`.

  Each `bind` returns an object that holds the host's `__dict__`. Reload reuses that same dict, so the restored old
  `FIELD_MODULES` objects see the restored values. `_v8.bind` only updates the fresh `ALL_FIELDS`. No module-level
  call writes into another module's state, writes a file or registers in `field_registry`.
- `superseded_window` snapshots `{k: getattr(rw, k) for k in rw.current()}`, which is the currently bound values.
  `rw.bind` validates its argument before it mutates anything. The `finally` block restores even when the second reload
  raises.
- `code_fingerprint` hashes per source through the AST. Nothing in production lists the tools directory, so the new
  `t2_gold.py` moves no production digest.
- E-R3 shows the window is restored in practice.

### (2) The changed expected values, re-derived by hand
- **xdata `(vol, src)` (2,4)→(1,2)** (`test_research_fields_xdata.py:486`).
  - At base, the fresh interpreter (seal 2024-01-01) skipped SEAL_ROWS 2025-02-03 (22, SPY) plus LATE_ROWS 2024-03-01
    (11, SPY). That gives src 4 and vol 2 (SPY only).
  - At head, SEAL_ROWS 2024-02-05 (a Monday, as before) are sealed and LATE_ROWS 2023-03-01 are kept. That gives src 2
    and vol 1.
  - These equal the harness's own unchanged asserts at l.394-395 (`vol_line` 1, `source` 2). Correct.
- **v9_nt `rows_after_role` (sealed_nt,1)→(sealed_nt,2)** (`test_research_fields_v9_nt.py:444`).
  - The role ends 2023-06-30 (l.51).
  - At base, the fresh interpreter saw NT 10-K 2023-08-01 after the role, and both NT 2024-02-01 and 2025-03-03 as
    sealed.
  - At head, NT 10-K 2023-08-01 and 2023-11-01 are after the role and before the seal, and NT 10-Q 2024-03-04 is
    sealed. That gives 2 after the role, the same as the harness's unchanged l.364. Correct.
  - The weekdays are kept: 2025-03-03 and 2024-03-04 are both Mondays, and 2025-05-12 and 2024-05-13 are both Mondays.
- **Three SEC values moved that the report does not list** (S1). I re-derived each on the hand-listed calendar: the
  role's weekday sessions inside the role and the NYSE rule outside it.
  - **l.475, `ea_days_to_expected` 14→17 (101).**
    - Base: from the last role session 2024-12-31 to 2025-01-23 (yoy of 2024-01-25) is 14 rule sessions, with 01-01,
      01-09 and 01-20 shut.
    - Head: from the last role session 2023-12-29 to 2024-01-25 (yoy of 2023-01-26) is Jan 2-5, 8-12, 16-19 and 22-25
      = 17 sessions, with 01-01 and 01-15 shut.
  - **l.480, 66→67 (505).**
    - Head: from 2023-11-03 to 2024-02-08 (yoy of 2023-02-09) is 40 role sessions (Nov 6-Dec 29), plus 21 January
      rule sessions, plus 6 February sessions = 67.
    - Base: 42 + 20 + 4 = 66 to 2025-02-06.
  - **l.482, -52→-51 (404).**
    - Base: 2024-07-18 (yoy of 2023-07-20) is 52 sessions before the first role session 2024-10-01. The count is
      9 + 22 + 20 (Labor Day 09-02 shut) + 1.
    - Head: 2023-07-20 (yoy of 2022-07-21) is 51 sessions before 2023-10-02. The count is 7 + 23 + 20 (09-04 shut) + 1.
    - The new l.483 (-63 on 10-18), with NaN at 10-19 (-64), pins the ">63 sessions" boundary that base only implied.
    - The test is stronger.
  - All three follow from the calendar, so none is an edit to fit. The `-364 d` shift keeps weekdays and the yoy_364
    chain, but not the holiday placement or the role's first and last session. The 2025-01-09 special closure leaves
    this count. It stays pinned by `NyseCalendar.test_rule_matches_the_hand_list` over 2022-2025.

### (3)/(4) Re-date checks (R1: bfe, identity_bridge, sic, sv; R2: the rest)
The role and calendar checks I made (R2):
- **Base prf.**
  - SESSIONS 2023-10-02..12-29: 65 weekdays (base 66). Every index-keyed override is below 65.
  - FORMATIONS are the last weekday of each month in both years. `spine_max_formation_age_days` stays 32 (Sep 29 →
    Oct 31 at head, Nov 29 → Dec 31 at base). The stale-spine message is 36 days in both.
  - FINRA staleness is age 45/46 on both sides. 303's 10-04 + 45 = 11-18 is a **Saturday**, as 10-02 + 45 = 11-16 was
    at base, so the vintage-risk weekend role is kept.
  - The weekend row 10-07 is a Saturday, and 10-09 is the next Monday.
  - SPLIT, C81 (the first Monday of Dec) and 505's last shares (474 days before the role in both) keep their roles.
  - Issuer rows:
    - 2002's 200-day staleness from 04-17 lands on 11-03 (Fri), then 11-06 (Mon, age 203).
    - 505's link available on a **Sunday** (10-22) is gated to Monday 10-23.
    - 5005's 400-day staleness from 2022-10-02 lands on 2023-11-06.
    - The SIC row 550 days old on 2023-11-03 checks out.
    - The 4004 same-clock accession order (-000009 > -000002) is kept.
  - Every sealed row is 2024-01-xx/02-xx, and the oracles' `< at(2024,1,1)` filters were moved with them.
- **sv.**
  - The receipt 403 row 2024-03-29 → 2023-04-07 is **Good Friday in both years**, so the holiday role is kept.
  - LATE_START 2024-09-02 → 2023-09-04 is **Labor Day in both**.
  - AMB Mon-Fri with the Wednesday in the middle, X_NEW on a Monday, and MISSING and Z_CPK keep their weekdays.
  - This closes R1's two open sv notes.
- **Holdings, mgr13f, connected.**
  - The 13F quarter ends, the +45 deadlines and V = deadline + 1 day 22:00 are the same calendar dates in both years.
    The strict next-session visibility is re-derived: 05-17/05-18, 08-16/08-17 and 11-16/11-17 at head.
  - The 150-day forward fill ends 11-27 in both years.
  - FTD_END is the last weekday of Q3, and staleness turns at 60/61 days.
  - NASDAQ_ABSENT is Mon-Thu, and ON_LIST 101 is 20 weekdays in both years.
  - The helpers in the holdings fixture are weekday-only (`weekdays`, `list_available`, `ftd_available`), so
    holidays play no role there.
  - The connected T_Q3 → 2023-09-29 is the quarter-end session (09-30 is a Saturday).
  - The leap-day check is kept by the added `quarter_end(2020-02-29)`.
- **SEC.**
  - The shift is -364 days, so weekdays are kept (spot-checked across ANN, TXS and K8).
  - The routine-insider months stay March.
  - "Filed in the next year: unseen" is kept (2022-12-29 trade, filed 2023-01-03).
  - The insider quarters: 2024q1 is sealed and never opened, and 2024q2 is after the role.
- **deals, gold.**
  - The deals sealed row goes from a Monday to a Monday.
  - gold `later = rw.SEAL.year + 1` keeps "two partitions never opened" (2024 from the fixture's seal year, 2025 as
    junk) and the pruned 2024 row group.

Every remaining 2024+ hit at head falls in a class the report names. Two notes on them:
- The production key `rows_on_or_after_2025_skipped` (`test_prepare_research_fields.py:311`) already counts
  `d >= day_of(SEAL)` (`prepare_research_fields.py:1073`). Only its name is legacy, and it is out of scope.
- Every dated row on or after the seal is synthetic, written to a temp dir and skipped or refused by the builder. No
  real path or row on or after 2024-01-01 is opened.

### (5) addClassCleanup
- **connected and mgr13f.** `registered()` is a `contextlib.contextmanager` generator. The cleanup is registered on the
  line after `cls._reg.__enter__()`, before the fixture build that can raise (SealError at base). A second
  `__exit__(None, None, None)` on the exhausted generator returns False on StopIteration, so it is idempotent with
  tearDownClass (`connected.py:323`, `mgr13f.py:330`).
- **deals, divevent, v9_nt.** The cleanup is `cls.addClassCleanup(cls.stack.close)`, registered right after the
  `ExitStack()` and before the first `enter_context`. `ExitStack.close` is idempotent.
- pytest 9.0.2 runs `doClassCleanups` when setUpClass raises. The report's base-vs-`d39cc878` diagnostic shows this.

## Findings
All are minor and tagged **Suggested**. None blocks the merge.

| # | path:line | tag / severity | problem | fix |
|---|---|---|---|---|
| S1 | `.superpowers/sdd/platform-p9-20261003/task-T2-report.md:250-259, 343-344`; values at `atx-engine/tools/test_prepare_research_fields_sec.py:475,480,482` | Suggested / minor | The report and ledger l.247 say only **two** expected values changed. Three SEC values also moved (14→17, 66→67, -52→-51), and one assertion was added (l.483). I re-derived all of them (point 2), and they are correct, but the disclosure is incomplete. | Root adds the three SEC values (with the derivation above) to the T2 merge entry in the ledger. No code change. |
| S2 | `atx-engine/tools/test_research_window.py:4-5,136`; `test_seal_partitions.py:7`; `test_prepare_research_fields_seal.py:3` | Suggested / minor | These docstrings still describe the retired conftest bind ("conftest.py binds the superseded window"). The files are outside T2's scope, and the report lists them (Risk 2). | A one-line follow-up for the owners, or root at merge. |
| S3 | `atx-engine/tools/test_research_fields_v9_earn.py:81` | Suggested / minor | The same class-level `ExitStack` registration has no `addClassCleanup`. It is a latent version of the leak T2 fixed, outside scope (report Risk 3). | Add `cls.addClassCleanup(cls.stack.close)` in a later slice. |
| S4 | `.superpowers/sdd/platform-p9-20261003/task-T2-report.md:330-333` (Deviation 1) | Suggested / minor (PM ack) | The brief says remaining 2024+ grep hits are "T2-GOLD modules only". Sealed-by-design synthetic rows stay on or after 2024-01-01. That is necessary for "every assertion keeps its meaning", since "one sealed, one kept" needs a sealed row. It is not a seal breach, because no real data is opened. | The PM acknowledges the deviation. No code change. |
| S5 | `atx-engine/tools/t2_gold.py:88-105` | Suggested / minor | The window restore after the T2-GOLD modules is pinned only by an uncommitted scratch probe (report E5) and by the suite's alphabetical order (a later module's sealed counts would fail). A reordered or filtered run would not catch a leak. | Optionally commit E5's probe as a test, e.g. a function in the sic module run after the frozen build that asserts `rw.SEAL_DATE`, `tool.SEAL` and `tool.SEAL_NS` are the repository values. |
| S6 | `atx-engine/tools/__pycache__/`, `atx-engine/tools/.mypy_cache/`, `atx-engine/tests/fixtures/research_fields/__pycache__/` (pool-22, gitignored) | Suggested / minor (DISK) | The lane's regenerable caches (mtimes 15:27-16:37, before this review) remain in the leased tree. | The lane or root deletes them at lease release (ruling DISK). The reviewer did not touch them because the tree is the lane's. |

The two R1 minors are resolved:
- `t2_gold.py` scope is accepted by ruling T2-MIN.
- The script-edit process is judged mechanical under T2-MIN (see the T2-MIN row above).

## Checked
- [x] `.agents/cpp/agent.md` §10: N/A. The lane has no C++, CMake or build target.
- [x] The diff stays inside the files in scope: `atx-engine/tools/{conftest,t2_gold,test_*}.py` plus the report.
  `t2_gold.py` is accepted by T2-MIN. There are no cross-lane edits.
- [x] The report's evidence matches its claims. I re-ran E1 and E2 (399 passed under both seeds, the same counts) and
  an E4/E5-style ordering (E-R3).
- [x] Look-ahead: no window, label or clock semantics changed. Only fixture dates moved, and every visibility
  boundary was re-derived.
- [x] Seal: no real path or row on or after 2024-01-01 is decoded. Post-seal rows are synthetic, in temp dirs, and
  refused or skipped.
- [x] Determinism: green under PYTHONHASHSEED 0 and 1. No new unordered iteration affects an output.
- [x] No Python copy of a C++ rule was added. No production module was touched.

## What root must check at merge
- **T2-SLOT.** T2 merges last in wave 2, as a unit. The intermediate commits `3eacd5cf..0b4b6c47` are red by design,
  so do not bisect into them.
- **The A3 overlap (A1-SHIM).** If A3 rewrote the `setUpClass` `yreg.register` blocks in connected, mgr13f, deals,
  divevent or v9_nt, keep both the shim removal and T2's `addClassCleanup` line.
- **Post-merge gate.** `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider
  C:/atx-wt/<merge-tree>/atx-engine/tools` under PYTHONHASHSEED 0 and 1 must give 0 failed. Expect 399 plus the
  other lanes' new tools tests. Any other lane's new tools test with 2024-dated synthetic fixtures now runs under the
  repository window, because the bind is gone. Such a test fails here and must be re-dated, not given the bind back.
- **Scope.** `git diff --stat 1239a5ff..bc618cb4` shows only `atx-engine/tools/{test_*,conftest,t2_gold}.py`.
- **Ledger.** Record S1's three SEC value changes with T2's two in the T2 merge entry.
- **Not run by anyone.** A mixed run `pytest atx-engine/tools atx-impl/tools` (report Risk 5). The conftest now
  imports `prepare_research_fields` through `t2_gold` at load. That is harmless for a single-directory run.
