# Handoff A4 (stretch lane; pool-27, branch `feat/p9-a4-20261005`) - OWNER STOP

Stopped at the owner's relayed STOP (PM, 2026-10-05) during the read phase. No code, test, build or merge was done.

## State

- Branch `feat/p9-a4-20261005`, base = root `1b64ce45`. The only commit on top is this handoff.
- T2's head `dfa4218d` is NOT merged (dispatch step 1 not started).
- No build tree, no configure, no build tag used (`p9-a4-a` is still free). No process started. Disk unchanged.
- No permission refusal.

## Read so far (root sprint dir `C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/` unless noted)

1. `resume4-dispatch.md`, `resume6-addendum.md`, `r6-wave3-common.md`, `resume7-addendum.md` (whole).
2. `.agents/cpp/agent.md` (whole, in pool-27).
3. `r7-dispatch.md`: header (:1-9) and the A4 section (:249-279).
4. `briefs/brief-A4.md` (whole; the lane section is :46-55).
5. `wave3-carry.md`: "All lanes" (:175-199) and the A4 section (:255-279).

## Not yet read (next agent reads these before changing anything)

- `wave3-carry.md`: "Brief text superseded" entry for `brief-A4.md` (:139-~150; names the AL-SIG 13F reader files
  `research/fields/sources/thirteenf.hpp`, `thirteenf_filings.cpp`, `thirteenf_holdings.cpp`), W3-13 (:467),
  Q13 (:433-434), M-4 (:416, :456).
- `task-A3-report.md`, `task-A3-rereview-1.md`, `task-AL-SIG-report.md` (13F reader, P18).
- `C:/atx-wt/pool-12/.superpowers/sdd/platform-p9-20261003/handoff-A3.md` item 4 (A1-SHIM, :210-215; read-only).
- `docs/plans/2026-10-02-platform-core-migration.md` section 4 slice 5; `docs/plans/2026-10-02-platform-core-audit.md`
  section 2 rows `research_fields_sec.py`, `research_fields_holdings.py`, `research_fields_v9.py`.
- Ledger `progress.md`: grep A4-1, A3-SHIM, A3-FIX1, A3-3, P18, DEC-5, OD-A3-6F, S-R2, W1-I1; read the block from the
  line starting "- RESUME 7" to the end. Also check for any ruling after this STOP about A4 (it may have moved to P10).

## Design decided so far

None. No file:line design was fixed before the STOP. Constraints gathered (from carry :255-279 and dispatch :260-277):

- Clock primitives: check `asof_series.hpp` before adding a "Latest" clock; `fields/clock.hpp` has another meaning.
- FTD / RegSHO stage readers in the style of AL-SIG's 13F reader on A3's source interface; no second 13F decoder (P18).
- Builder kinds + registry rows per moved field; planted-leak probe with teeth per builder; fixture byte identity vs
  the Python builders (on T2's re-dated fixtures). No new `research_fields_*.py` (DEC-5).
- A1-SHIM: route `prepare_research_fields_{draft,xdata,ohlc,ydata}.py` `main` through the registry entry; keep
  `register`, `DRAFT_MODULES`, "DEPRECATED".
- S-R2: `prepare_research_fields_engine.py:268` refuses statistics whose `PANEL_KEYS` are all `None`.
- A3 review S-2: put the history extent (or co-requested price set) in the reuse key, or build the union from every
  requested plan. A3-3: per-field re-hash under `--reuse`.
- S-6 boundary: `coskew_60m`, `xrd0_ttm` stay Python in P9.
- Every `kind: engine` flip is its own commit at the END of the branch, after the pre-flip head is complete and tested.

## Done / not done per item (r7-dispatch A4 steps 1-5)

| Step | Item | State |
|---|---|---|
| 1 | `git merge dfa4218d`, then root's head | not done |
| 2 | clock primitives | not done |
| 2 | FTD / RegSHO stage readers | not done |
| 2 | builder kinds + registry rows | not done |
| 2 | planted-leak probes with teeth | not done |
| 2 | fixture byte identity vs Python | not done |
| 2 | A1-SHIM, S-R2, A3 S-2, A3-3, S-6 boundary | not done |
| 3 | `kind: engine` flips (own commits at the end) | not done |
| 4 | Debug / Release builds, fields tests, pytests | not done |
| 5 | `task-A4-report.md` | not written (nothing to report) |

## Next step for a fresh agent

1. Confirm with the PM / ledger that A4 is still dispatched in P9 (stretch; W3-13 allows a move to P10 whole).
2. Finish the "Not yet read" list above, then `git merge dfa4218d` in pool-27 (dispatch :260).
3. Then dispatch steps 2-5 as written in `r7-dispatch.md:262-277`.
