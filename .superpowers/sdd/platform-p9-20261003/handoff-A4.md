# Handoff A4 (stretch lane; pool-27, branch `feat/p9-a4-20261005`)

## R8 slice, reconstructed (RESUME 9 agent, 2026-10-07; from `git diff 1b64ce45..5c53c2c9`)

The R8 agent worked after the 2026-10-05 stop below, committed `1e4904b7` (A1-SHIM, S-R2, N-1) and left 25 paths
uncommitted at STOP 8. DIRTY-2 commit of those paths as they stood: `5c53c2c9` ("WIP(a4): work uncommitted at STOP 8
(DIRTY-2; untested as committed)"). History before it: `ebfb333e` merges T2 `dfa4218d` (dispatch step 1 done).

### What exists (file:line at `5c53c2c9`; paths under `atx-engine/`)

Committed in `1e4904b7` (Python only):
- A1-SHIM: `tools/prepare_research_fields_{draft,xdata,ohlc,ydata}.py` `main` hands its argv to the builder behind
  `--registry <committed registry>` (unchanged when the argv names one); `register`, `DRAFT_MODULES`, "DEPRECATED"
  kept; test `tools/test_field_registry.py` (+35 lines).
- S-R2: `tools/prepare_research_fields_engine.py` `same_panel` refuses a statistic that is None for a `PANEL_KEYS`
  key; N-1 (optional): `SHARE_KEYS` 0-without-shares is not compared; test
  `tests/fixtures/research_fields/test_vendor_engine_path.py` (+23).

In the DIRTY-2 commit `5c53c2c9` (C++ and one Python wrapper):
- Clock primitives `include/.../fields/session_clock.hpp:43-157`, `src/research/fields/session_clock.cpp:29-129`:
  `SessionCalendar` (Python Calendar: NYSE rule sessions around the role, marks, `usable_from`, stats),
  `LatestRows` (Python Latest pointer: per-column last row passed), `WindowedSums<T>` (Python Windowed, events in
  numpy's stable order). `asof_series.hpp` was checked first (header comment :22-25 says why it is another rule).
- Run payload `include/.../fields/run_payload.hpp:21-43`, `src/research/fields/run_payload.cpp:35-72`: this run's
  `shares_out.f64` pinned by SHA-256 (`BuildSpec::shares_out`, `FileRequest`, `build_spec.hpp:28-53`).
- Short-side stage readers `include/.../fields/sources/short_side.hpp:28-87`, `src/research/fields/sources/
  short_side.cpp:64-152`: `kFtdStage` / `kRegshoStage` / `kSecurityMasterStage` contracts, `stage_year_files`
  (reader-side seal), four separate whole-file decoders `read_ftd_rows`, `read_regsho_lists`, `read_threshold_rows`,
  `read_finra_names` on `PinnedStage::table`.
- Holdings kinds `include/.../fields/holdings_fields.hpp:47-200`: `ftd_shares_ratio21` (`src/research/fields/
  holdings_ftd.cpp:44-257`: `FtdIndexer` with a `std::map` of settlement days, a full (dates+1) x lines cumulative
  matrix, `ftd_row`, `build_ftd_shares_ratio21`, `parse_ftd_kind` / `build_ftd_kind`) and `regsho_threshold_days63`
  (`src/research/fields/holdings_regsho.cpp:73-352`: `RegshoIndexer`, `index_listing_rows`, `regsho_row`,
  `build_regsho_threshold_days63`, kinds); specs `src/research/fields/holdings_specs.cpp:15-113` (registry spec_text
  copies).
- Registry: kind table 11 -> 13 rows (`src/research/fields/registry.cpp:235-249`), stage pins
  (`registry.cpp:353-365`), `SharedSources` / `BuildContext` (`registry.hpp:68-84`); spec keys `ftd`,
  `regsho_threshold`, `security_master`, `shares_out` (`research_fields_cli.cpp:166-170`, `:224-248`, `:394-406`).
- A3 review S-2: `build_entries(spec, role, to_build, planned)` builds on the union of every planned field
  (`research_fields_cli.cpp:109-120`, `:424`, `:462`) and reuse rule 6 (`reuse.hpp:22-31`, `reuse.cpp:201-247`,
  `:307-317`: a vendor price field is reused only from a panel with this build's `sessions_before_role`).
- A3-3: `DigestCache` (`reuse.cpp:165-178`), one digest per distinct input per decision, `inputs_digested` in the
  reuse record (`reuse.cpp:331`).
- Python engine route for the two holdings kinds (`tools/prepare_research_fields_engine.py`: `HOLD_ROUTES`,
  `formula_of`, `build_fields(stages=, shares_out=)`, `stage_reads`, `holdings_entry`, `holdings_producers`,
  `engine_path` binding) - flag-absent (no `--engine-exe`) path unchanged.
- Tests: `tests/research/research_fields_a4_clock_test.cpp` (6 TESTs), `research_fields_a4_reuse_test.cpp` (3),
  `research_fields_holdings_test.cpp` (7: fixture identity x5, planted-leak probes x2), registry test kind list
  13; CMake tails `atx-engine/CMakeLists.txt:551-561`, `tests/CMakeLists.txt:539-546`.
- Fixture `tests/fixtures/research_fields_holdings/` (synthetic, T2's re-dated stages, role 2023-05-01..2023-12-29;
  expected payloads of all nine holdings fields from the EXISTING Python builder), generator
  `make_holdings_fixture.py`, pytest `test_holdings_fixture.py` (2 tests). Scope note there: `sv_offexchange_share126`
  and `exch_up_365d` stay Python.

### What compiles / is tested: UNKNOWN as committed

- Receipts in `build-equity/`: `p9-a4-a` (Debug, 6 dirty entries, exit 0) and `p9-a4-b` (Debug, 24 dirty entries,
  exit 0) both from uncommitted trees on `1e4904b7`; neither is evidence for `5c53c2c9`. The only kept test log,
  `a4-evidence/gtest-p9-a4-a-whole.log`, is 94 / 95 with `ResearchFieldsA4Reuse.RebuiltPriceFieldUsesTheWholeBuilds
  Panel` red (1600 vs 1261; the committed test now expects 1600). No log for `p9-a4-b`. No pytest log. Next tag `p9-a4-c`.

### Missing against `r7-dispatch.md` A4 steps 2-5 (at `5c53c2c9`)

1. Owner's bar: ONE reader interface for the short-side sources (four ad-hoc decoders now), bounded memory (whole
   year files decoded; FTD keeps a (dates+1) x lines matrix), no per-row allocation (text columns kept as strings).
2. Planted-leak probes with mutation teeth (TEETH-RESTORE tool) for the new refusals / rules (S-2 rule 6, A3-3,
   the probes' clock); none run.
3. Root merge (`685e238b`), Debug build + fields gtests on the merged committed head, Release build + gtests,
   fixture pytest, Python suites (`scripts/tests` seeds 0 / 1, stage chain) since `prepare_research_fields_engine.py`
   changed.
4. The `kind: engine` flips of `ftd_shares_ratio21` and `regsho_threshold_days63` in `tools/field_registry.json` (own
   commits, at the end), the TRAIN identity argv for root.
5. S-6 boundary statement (`coskew_60m`, `xrd0_ttm` stay Python: both rows `kind: python`, verified), report
   `task-A4-report.md`.

---

## Earlier (2026-10-05) - OWNER STOP, read phase (superseded by the section above)

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
