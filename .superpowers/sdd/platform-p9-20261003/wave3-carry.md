# Wave 3: binding rulings and carried findings per lane (doc agent for the PM, 2026-10-04)

A wave-3 dispatcher needs this file, `resume4-dispatch.md` and `briefs/brief-<ID>.md` only. Where a brief and this file
disagree, this file wins (it carries the later ledger rulings and what wave 2 actually built; see "Brief text
superseded"). Every lane: no TDD (implement, then the tests the brief names); builds only through
`scripts/research-build.ps1` (W2-BUILD); no real data; rules 1-10 of its brief as amended below. Each item is tagged
**required** (do it, or say in the report why it cannot be done) or *optional* (do it if in scope; else list it as not
done). "Carried minor" = a deferred review finding in the lane's files.

Drafted read-only at pool-2 head `b1406e14` (AL-SIG merged). Wave-2 lanes were still in fix rounds when this was
written (heads: E2 `f692ea36`, B2 `c25f9b24`, C2 `3eca79bf`, D2 `49af0a52`, S2 `f6c0e964`, AL-COMB `9692a73e`, COV
`e9fc3668`, T2 `dfa4218d`): re-check the cited lines at dispatch. Citation keys: `ledger :N` = `progress.md` line N
(362 lines at drafting); `status-4 :N`; `pool-N <file>:N` = that pool's sprint dir; `rr` = `resume-rulings.md`.
Items that need a PM ruling before dispatch carry "(Q<n>)" and are listed at the end.

## Lanes, pools, bases, slots

Base = the integration head after the wave-2 merges, the wave-2 gate, P9-B0 again and the W1-I2(b) parent re-pin
(status-4 :55-58; plan §2.1 "waves 2-3"). Root fills the SHA. 3b base = the head after the named 3b dependencies merge.
Slots follow plan §3.2 (wave 3: C3 1, D3 2, AL-CLOCK 3, AL-DATA 4, A4 5, SQL3 6; 3b: AL-CLOCK, COV-MV, SQL4).

| lane | wave | plan pool | proposed pool | hard dependencies (state at drafting) | brief | slot |
|---|---|---|---|---|---|---|
| C3 | 3 | 15 | 15, after C2 merges and its lease is released | C2 (fix round 1, unmerged); COV (fix round 2, unmerged; COV-3, Q12) | `brief-C3.md` | 1 |
| D3 | 3 | 16 | 20 (D2's warm tree; pool-16 holds YARCH WIP, ledger :50) (Q11) | D2, E2 (fix rounds, unmerged); AL-COMB after its ALCOMB-5 round (Q12) | `brief-D3.md` | 2 |
| AL-DATA | 3 | 20 | 21 or 23 (SQL1 / SQL2 merged; trees free after root deletes their builds) (Q11) | A3 merged (yes); landed data (none, ledger :16) | `brief-AL-DATA.md` | 4 |
| A4 (stretch) | 3 | 12 | 12 (A3 merged; root deletes the A3 build tree first) | A3 (yes), AL-SIG (yes, P18), T2 (merges last in wave 2) (Q13) | `brief-A4.md` | 5 |
| SQL3 | 3 | 17 | 17, after E2 merges | SQL1, SQL2 (yes); E2; B2's deletion commit (B2-8) (Q12) | `brief-SQL3.md` | 6, never under an in-flight wave |
| PRE | 3 | 13 | 13 (AL-SIG merged at `b1406e14`) | AL-COMB, AL-SIG, AL-CLOCK reports; B2, E2 merged; P1 order (Q14) | `brief-PRE.md` | doc, no merge conflict |
| AL-CLOCK | 3b | 19 | 19, after AL-COMB merges | C3 merged; DEC-16 (ruled, ledger :79); Y-5 read (done, R0-9) | `brief-AL-CLOCK.md` | 3b, first |
| COV-MV | 3b | at dispatch | 24 (COV's warm tree), after COV merges | COV (incl. round 2 R1b / R2b), C3 merged; funded (COV-7) | `brief-COV-MV.md` (new) | 3b, after AL-CLOCK |
| SQL4 | 3b | 18 | 18, after S2 merges | SQL3, S2, B2, C3, D3 merged; else P10 untouched (SQL-12) | `brief-SQL4.md` | 3b, with AL-CLOCK and COV-MV |

## Brief text superseded (this file wins)

**Every wave-3 brief (rules header).**
- Rule 1 "never build C++" and rule 7 "C++ tests are written, not run": superseded by W2-BUILD (rr :10-14) and
  `resume4-dispatch.md` :16-19 (a lane builds its own named targets through `research-build.ps1`, target-scoped,
  single-use tag `p9-<lane>-<letter>`, one build on the host at a time, memory gate, root's builds first).
- Rule 2 lease line `-MaxPool 20`: a pool above 20 needs `-MaxPool <its number>` (wave2-carry precedent); POOL-MAP
  (ledger :230): pool numbers are labels; the dispatcher records the real one.
- Rule 7 pytest line: add `-p no:cacheprovider`, an own `--basetemp`, `PYTHONDONTWRITEBYTECODE=1`; `scripts/tests` and
  any `atx-engine/tools` test (incl. `test_stage_chain.py`) in SEPARATE invocations (`resume4-dispatch.md` :20-21;
  pool-17 `task-E2-report.md` "Ledger candidates": the tools conftest window bind turns 31 scripts tests red).
- Each brief's "Pool / branch" line: the table above.

**Plan text left stale by the amendments** (ledger :229): header "6+1 cells" (now <= 8 hypothesis + <= 1 protocol
cell, DEC-18 as amended by COV-7 / COV-8); pool numbers in §2.1 / §3.3 for D3 (16) and AL-DATA (20); "DEC-1..DEC-20"
(DEC-21 = COV, DEC-22 = SQL, ledger :223); Appendix A `-MaxPool 20`.

**brief-SQL3.md** (checked against E2's tree `git -C C:/atx-wt/pool-17 show f692ea36:<path>` and SQL1 / SQL2 as
merged in pool-2 `b1406e14`).
1. "`scripts/wave_context.py` (wires the stage-chain hook)" and task 3 "`wave_context` passes the hook for its
   chains": the only `stage_chain.Chain(...)` construction is `scripts/research_wave.py:34-37` (`chain_of`).
   `wave_context.py:71-77` is `Wave.write_json`, the wave-result writer. Add `scripts/research_wave.py` to SQL3's files
   (E1 / E2's file, unowned in wave 3) (Q3).
2. "If E2 moved the binding or verdict writer into `scripts/cycle/`": the file writes did not move. Verdict:
   `scripts/cycle_verdict.py:126-147` (`keep_copy`, `write_verdict`); binding: `scripts/cycle_resume.py:183-191`
   (`write_binding`). E2's split moved only call sites (`write_verdict` wrapper, `admission_trials`, `copy_ledger`) into
   `scripts/cycle/gate.py:102-160` (pool-17 `task-E2-report.md:86-88`). A hook inside `cycle_verdict.py` /
   `cycle_resume.py` needs no `scripts/cycle/` file; the PM names `scripts/cycle/gate.py` only if the hook goes at the
   call site (recommend: it does not).
3. Task 4 writer list, by `classes.json` id: `run-receipt` `run_bounded_research.py:547`; `run-start` `:484`;
   `stage-receipt` `atx-engine/tools/stage_chain.py:284` (failed: `.failed-<k>.json`, `:147-149`); `cycle-binding`
   `cycle_resume.py:191`; `cycle-verdict` `cycle_verdict.py:134` (copy) and `:147`; `wave-result`
   `wave_stage_record.py:244` (through `Wave.write_json`); `wave-candidate` `wave_queue.py:136-141` (`write`);
   `wave-manifest` `wave_queue.py:344` (`emit`).
4. Task 1 "`atx-research-store ingest --catalog DB --class cls --path P`": the verb also takes `--root R` (default `.`
   = the process cwd) and `--classes FILE` (`store_cli.cpp:53`, `:316`). Pass `--root <root>` explicitly. `ingest`
   refuses (exit 3) a root whose absolute path holds a year token (SQL2-S7, below).
5. "`research_store_identity.check_one(DB, P, cls)`": the merged signature is `check_one(db, path, cls=None, *,
   root=None, registry=None)` (`atx-engine/tools/research_store_identity.py:310`); pass `root=`.
6. Task 1 `selection`: `record_store.RecordStore` selects `<root>/index.sqlite`, else `<root>/../index.sqlite`
   (`record_store.py:95-102`). The brief's `<d>/` and `<d>/*/` miss `<d>/../index.sqlite` when an argv directory is
   itself a store root. Mirror the accessor's two places (and keep `<d>/*/`).
7. Task 5 (spec keys for SQL4's index flags, "only if the PM names the file"): moot under SQL-11 (ledger :212, :229):
   no spec key and no `REUSE_NEUTRAL` entry in P9. Drop the task.
8. "Flag-absent identity (3) ... Real research cells may use the record-store index from this merge on": superseded
   by SQL-11 and wave2-carry: no P9 cell uses a SQLite cache index, also after SQL3; root's identity runs only, by
   direct argv, index deleted after the run.
9. "Cross-lane edits: none" and "Forbidden: every C++ file": SQL2-S7 is a C++ change in SQL2's catalog and SQL1-MON
   reaches `atx-impl/tools/book_monitor.py` (Q4, Q5). "Root verifies. No build" holds only if SQL3 stays Python-only.
10. "Needs merged: SQL1, SQL2, E2": add B2's deletion commit (B2-8, ledger :349: root moves the B2-R3 pin hunks into
    `cycle_verdict.py` / the `scripts/cycle/` split) and the root `classes.json` literals of the wave-2 merges
    (SQL2-CLS: `atx.research-wave/v2` from E2; `classes.json` `wave-manifest` lists only `atx.research-wave/v1` today,
    so a hooked `emit` of a v2 manifest needs it).
11. The suites line "`scripts/tests` under PYTHONHASHSEED 0 and 1, `atx-engine/tools`": two separate invocations.

**brief-SQL4.md** (checked against SQL2 as merged in pool-2 and B2 at pool-14 `c25f9b24`).
1. "**Merge slot:** 3b, alone" and "Cross-lane edits: none (wave 3b has no other lane)": SQL-12 (ledger :213, :229)
   puts SQL4 in 3b with AL-CLOCK and COV-MV. The CMake tails it shares with them (`atx-impl/CMakeLists.txt`,
   `atx-impl/tests/CMakeLists.txt`) are keep-both appends (P2).
2. Ledger head: the binding point is `default_ledger_head()` (`atx-engine/src/research/store/catalog/ingest_ledger.cpp:33`,
   today `return {};`), called by `store_cli.cpp:296` (catalog) and `:334` (ingest). B2's API: `ledger::chain_head(lines)`
   -> `ChainHead{head_sha256, lines, rule}`, rule `atx-ledger-chain-fold/v1`, plus `prefix_heads(lines)` for the
   prefix check (pool-14 `atx-engine/include/atx/engine/research/ledger/chain.hpp:40-72`). The seam comment's example
   `head.sha256` (`ingest_ledger.cpp:26-30`) names a field that does not exist (`head_sha256`).
3. `verify --ledger` "(new sub-option)": `cmd_verify` refuses any call without `--pins` (`store_cli.cpp:346-348`, usage
   `:56`). Add `store_cli.cpp` and `atx-engine/tests/research/research_catalog_cli_test.cpp` to SQL4's files; `--ledger`
   is already a value option (the `catalog` verb's).
4. "the catalog CMake block (link B2's ledger library)": the library is `atx-engine-research-catalog`
   (`atx-engine/CMakeLists.txt:400-431`); link `atx::engine-research-ledger` (B2; pool-14 `atx-engine/CMakeLists.txt:371-378`).
   The new `research_catalog_ledger_test.cpp` joins `atx-engine-research-catalog-tests`
   (`atx-engine/tests/CMakeLists.txt:465-468`): that block is in scope too.
5. Line anchors under **Read** (`strategy_ic_runner.cpp:583-596`, `:861`; `strategy_ic_result_cache.cpp:56-75`, the
   32 MiB slack at `:73-75`; `strategy_marginal_ic.cpp:1001-1048`) predate S2 / D2 edits (at S2's head the slack
   comment is `strategy_ic_result_cache.cpp:86`; `--pair-cache` parse still `:1129`). Re-locate by symbol at base.
6. Keep `ResearchCatalog.LedgerHeadSeamReceivesLinesInFileOrder` green (pool-23 `task-SQL2-report.md:211-218`).

**brief-C3.md** (checked against C1 / A3 merged, C2 at pool-15, COV at pool-24).
1. "Files in scope: the NAV files": name them: C1's set (`strategy_nav_replay.{cpp,hpp}`, `strategy_nav_replay_detail.hpp`,
   `strategy_nav_v7.*`, `strategy_vol_target.*`, `strategy_risk_target.*`, `strategy_cost_v2.*`,
   `atx-engine/src/book/replay_cost.cpp`), C2's new `strategy_nav_calibrate.{hpp,cpp}` (+ test) and its
   `strategy_target_replay.hpp` constants, `strategy_spo.{hpp,cpp}` with COV's RiskStore section (COV-3, plan §2.2 COV
   row), and plan §2.2's `atx-impl/src/config.*`, `scripts/wave_rules.py:110-119` (if root has not deleted it at C2's
   merge), `atx-impl/CMakeLists.txt`.
2. "the flag parser kept as a thin adapter producing the same NavSpec": C2 added `--calibrate-gross`,
   `--calibrate-tolerance`, capability `calibrate-gross-pm6-6`, and one leverage constant `aim_leverage_min/max`
   (`strategy_target_replay.hpp`); `risk_target::leverage_cap()` keeps JSON integers 1 / 2 so `--list-rules` bytes do not
   move (pool-15 `handoff-C2.md` rows S4, S7).
3. "cost-law registry ... trade-rate registry ... completing K-P9-7": E2 derives a cell's NAV flags only from a row's
   top-level `x-flag` and per-param `x-flag` in `params_schema.properties` (`scripts/wave_kinds.py:356-373`; C1's row
   shape `strategy_risk_target.cpp:331-351`); K-P9-7's `target` kind needs a registry slot COV-MV adds a row to.

**brief-D3.md** (checked against D2 at pool-20 `49af0a52`, E2 at pool-17).
1. "K-P9-8 weights v2" and gtest `WeightsV2.SingleRowEqualsV1Bytes`: P19 (ledger :42) names the schema
   `atx.dsl-composition-weights/v3` (`weights_schema_v3` in `atx-engine/include/atx/engine/research/composition/weights_schema.hpp`;
   the runner refuses v3 until D3, D2 report :31). Name the test `WeightsV3.SingleRowEqualsV1Bytes`; meaning unchanged.
2. "`strategy_ic_composition.cpp:409-420` (theme schedule)": now `IcComposition::schedule_theme_masses` `:384-406`,
   `IcStageKind::schedule` `:54`, `:276`; parse `strategy_ic_theme_tsmom.cpp`; kernel
   `research/composition/theme_tsmom.{hpp,cpp}`; rule row `strategy_ic_rules.cpp:487` (D2 head).
3. `atx-research-composition fit` exists (D2): `fit --factors DIR --spec FILE --output NEWDIR`
   (`atx-engine/src/research/composition/composition_cli.cpp:199-200`); D3 adds the walk-forward options to it.
4. The brief has no "Files in scope". Plan §2.2: the fitter, `composition_*.py` (now incl. D2's new
   `composition_engine_fit.py`), `research/composition/**`; the IC runner files and E2's kind files are needed too
   (Q7, Q8).
5. "`ThemeTsmom.ApplyTimeSleevesEqualRecorded` (on TRAIN fixtures)": synthetic fixtures only (brief rule 3).

**brief-A4.md** (checked against A3 + AL-SIG merged in pool-2).
1. "the 13F / FTD / RegSHO stage readers in C++ on A3's source interface": the 13F reader exists (AL-SIG; P18, ledger
   :41: A4 reuses it): `research/fields/sources/thirteenf.hpp` (+ `thirteenf_filings.cpp`, `thirteenf_holdings.cpp`),
   `sources/stage.hpp` (`PinnedStage`), `sources/parquet_columns.hpp`. FINRA as-of (`finra_asof_field.hpp`) and
   `asof_series.hpp` exist (YARCH). Build FTD / RegSHO in that style; check `asof_series.hpp` before adding a "Latest"
   clock (`fields/clock.hpp` is the calendar-day / seal header, another meaning).
2. **Read** gains AL-SIG's report (pool-13 `task-AL-SIG-report.md`) and A3's handoff item 4 (pool-12 `handoff-A3.md:210-215`,
   the A1-SHIM recipe).
3. "fixture byte identity vs the Python builders": T2 re-dated the SEC / holdings fixture modules (five expected values
   moved with fixture rows, ledger :254; T2-GOLD-4 frozen SIC fixture, ledger :238): identity runs on T2's merged fixtures.

**brief-AL-DATA.md.**
1. "the C++ builder kind on A3's sources": the Form ADV join is to 13F filer CIK: also AL-SIG's 13F reader (above).
2. "a registry row and the registration file": Lazy Prices and Form ADV crowding are already registered
   `blocked_on_data` in `scripts/specs/p9/blocked/lazy_prices.json` / `hf_crowd.json` (`atx.p9-blocked-registration/v1`):
   amend those; never a second string or id.
3. "Root verifies: K1 plan of each string": only for strings whose data landed (none expected, ledger :16).

**brief-AL-CLOCK.md.**
1. "theta_f = .12945 (Y-5's fast rate, half-life 5)": the built two-speed rule is `two_speed_fast_theta(C) = 1 - 2^(-C/5)`
   at rebalance cadence C (`atx-engine/include/atx/engine/book/two_speed.hpp:21-23`, `:45-46`; PM8-16 #4); .12945 is its
   cadence-1 value. Slow rate `two_speed_slow_theta` .05 (`:39`). (Q16)
2. "registration file with K-P9-11 keys": as AL-COMB did: `scripts/specs/p9/rules/<id>.json`
   (`atx.p9-rule-registration/v1`), not `candidates/` (`wave_queue` validates candidates as `atx.wave-candidate/v1`;
   pool-19 `task-AL-COMB-report.md:290`); the template carries no `rules` key and no `change.flags` (ALCOMB-1, rr :54).
3. `strategy_target_replay.cpp:345-399` still is the aim-partial-v5 per-name rate step (checked); two-speed refusals
   `:167-175`.

**brief-PRE.md.**
1. **Read** gains: D-COV's printed numbers (P9-V predictions; plan §3.2 PRE row, §3.4 step 6), AL-COMB's Q5 diagnostic
   numbers (q, q_lit; plan :589-590), the registration dirs `scripts/specs/p9/{candidates,rules,blocked,diagnostics,templates}/`,
   COV-6 / COV-7 / COV-8 (ledger :198-200), and this file's PRE section.
2. "Blind: no 2020-2023 statistic beyond the public lines of `progress.md` and the status files": WQ-EXPLORE
   (`wq-explore-report.md`, `wq-explore/`, `build-equity/wq-explore/`) and the PLOT-1 png are not public lines: unread (WQ-1).
3. Its dispatch point (P1, ledger :24: "PRE dispatches after AL-CLOCK's report") conflicts with the wave-3 slot (Q14).

## All lanes

- **required** `resume4-dispatch.md` hard rules (:9-34), incl. no `atx-db/`, nothing dated >= 2024-01-01, trial
  ledger untouched (133 lines, `27e40f9f`), no 2020-2023 statistic, DISK-2 / SCRATCH-1 deletion by name.
- **required** Wave-3 gate (plan §4.2): every suite 0 failed; no known-red carried into wave 3 (the wave-2 gate is
  "0 failed, no exception", M1a-RED ledger :216; status-4 :55-56).
- **required** E1-REUSE-a2 (ledger :153): every P9 cell manifest pins exes (`lock --exes` -> `exes_sha256`); templates
  leave the pin to root.
- **required** E1-STALE (ledger :141): never merge under a running wave.
- **required** CMake tails (P2, ledger :25): append one line / block at the end of the owning list, list it as a
  cross-lane edit; root keeps both. Wave-3 appenders: C3 (restructures `atx-impl/CMakeLists.txt`), D3, A4, SQL4,
  AL-CLOCK, COV-MV.
- **required** SQL2-CLS (rr :34): new `atx.<name>/v<n>` literals are listed in the report; root adds them to
  `classes.json`.
- **required** ALCOMB-1 (rr :54) + E2's rule derivation: rules enter a cell only through the `atx.research-wave/v2`
  `rules:` block; templates carry no `rules` / `change.flags`; a NAV row needs `x-flag` + per-param `x-flag`
  (`scripts/wave_kinds.py:356-373`) (C3, AL-CLOCK, COV-MV).
- **required** W1-I1 (ledger :314): new candidate / rule ids <= 24 chars; <= 18 if the cell runs `--eval-mode
  audit-exact` at pool-2's path depth (pool-17 `handoff-E2.md:75`).
- **required** (root) W1-I2 / I-2 (ledger :315; `wave1-whole-review.md:76-80`): a lane that changes the NAV exe (C3,
  AL-CLOCK, COV-MV) makes W1-I2(a) refuse every rule cell until root re-pins the lineage parents to the wave-3 P9-B0
  references; that re-pin precedes the first P9 cell.
- **required** Blind reads: never open `wq-explore-report.md`, `wq-explore/`, `build-equity/wq-explore/` (WQ-1, ledger
  :343) or `docs/plans/*equity-curve*.png` (PLOT-1, ledger :337).
- *optional* (root) M1d-NOTE-3 (ledger :280): per-candidate cache hit / miss strings stay on the P9-B0 volatile list.

## C3 (pool 15)

- **required** W1-MIN M-1 (ledger :316; `wave1-whole-review.md:84-93`): one 1-vs-4 `--book-workers` byte test with
  `--rule aim-partial-v6 --cost-v2 --capacity-curve --adv-hold-q` before any spec sets `--book-workers`.
- **required** P13 NAV half (ledger :36): the NAV exe refuses a present-and-different or malformed `seal.exclusive_end`
  in the manifests it reads; absent = legacy, accepted and logged. Copy S2's IC half (`fields_seal` in `bind_fields`,
  gtest `IcFieldsSeal.ManifestSealMustBeThisBuildsOrAbsent`; pool-18 `task-S2-report.md:27`).
- **required** COV-3 (ledger :195; plan §3.2 note): C3 inherits `strategy_spo.*` with COV's RiskStore read path; the
  move-only split and the core split move it unchanged; `CovStore.*` stays green; NavSpec's `--risk-model` takes a
  container-bearing store (no code: the flag is unchanged).
- **required** C2's surface in NavSpec (pool-15 `handoff-C2.md` S4, S7; rr C2-1): `--calibrate-gross`,
  `--calibrate-tolerance`, capability `calibrate-gross-pm6-6`, `strategy_nav_calibrate.*`, `aim_leverage_min/max`;
  calibration record stays in `summary.json`; C2-6 / C2-7 semantics unchanged (ledger :358-359).
- **required** K-P9-7 completion: cost-law and trade-rate rows in C1's row shape with `x-flag`, per-param `x-flag`,
  `incompatible[]`; a `target` kind slot for COV-MV's row (plan §2.2 COV-MV row).
- **required** P14 / G-P2 (ledger :37; pool-17 `task-E2-report.md:73`): C3's cost-law rows make
  `scripts/specs/p9/kinds/new-cost-model.json` runnable (today refused "in neither table"); its flip out of
  `SCHEMA_ONLY_FIXTURES` (`scripts/tests/test_wave_kinds.py:57`, `:193`, `:362`) is owned per Q8.
- **required** Identity: X-5 and Y-F0 NAV via `--spec` byte-identical to argv (brief), and Release = Debug NAV on X-5
  after the split (G-P3 NAV half, plan §4.3 phase 3; M1c ledger :241).
- **required** (root, if still present) delete `scripts/wave_rules.py:110-119` after root's C2 gm-pair identity (plan
  §2.2; pool-15 `task-C2-review.md` "What root must check" 7; G-P5).
- *optional* S2-FIX1 (5) "noted for C3" (ledger :325): per-worker as-of scratch in the IC memory admission
  (`strategy_ic_admission.cpp:330-339`; pool-18 `task-S2-review.md:111-118`) sits in an IC-runner file (Q1).
- *optional* PM7-35 Python copy `wave_rules.py:50-51` retirement after root's identity vs B1's predicate (P9 ruling,
  ledger :32; pool-17 `task-E2-report.md:75-77`): no wave-3 owner (Q6).
- *optional* C1-SPO leftover: `v7_extras.json` `capacity_spo_v3.rule` has no direct gtest pin (pool-15
  `task-C2-report.md:127`).
- *optional* C2 review S6: the calibrate tape (`strategy_nav_replay.cpp:3481-3496`) can turn construction reuse off
  under `--max-bytes`; keep `construction_reused` in the summary through the split.
- *optional* W1-MIN M-5 (`wave1-whole-review.md:118-123`): correct the `atx-impl/CMakeLists.txt` /O2 comment of D1's
  block when the core split touches it.

## D3 (pool 20 proposed)

- **required** D2-3 (rr :68; pool-20 `task-D2-report.md:29`, `:156`): delete `composition_*.py` once ew-theme-aim-v2 and
  the resid diagnostics have a C++ path; D3 replaces them with a document builder over `composition_fit.json`; deletion
  only after root's identity (plan §0.6).
- **required** G-P5 rows: the six rows of `scripts/tests/test_no_python_mirror.py` that D2 re-assigned to lane D3
  (D2 report :168) are retired with the deletion.
- **required** D2-5 (ledger :301): `horizon_stats.py` stays (P10, named G-P5 exception); D3 does not delete it.
- **required** P19 (ledger :42): K-P9-8 = `atx.dsl-composition-weights/v3`; v1 bytes unchanged when the kind is absent.
- **required** P14 / G-P2: the `walk-forward` kind leaves `SCHEMA_ONLY` (`scripts/wave_kinds.py:53`, `:243-249`;
  `wave_stage_kinds.py:10`, `:68`, `:102`; fixture `scripts/specs/p9/kinds/walk-forward.json`; `test_wave_kinds.py:548`).
- **required** AL-COMB identity (ALCOMB-4 ledger :268, ALCOMB-5 :285; pool-19 `task-AL-COMB-report.md:315-324`):
  mom-volman-v1 shares theme-tsmom's `theme_schedule` block and parse and its fixed-TRAIN schedule is the registered
  design: with walk-forward absent, mom-volman, theme-tsmom and theme-cov-hedge outputs stay byte-identical.
- **required** D2's engine-fit surface (D2 report :61): `--engine-fit` refuses ew-theme-v1 / -aim-v1 / -aim-v2 / -v6,
  the train orientation and era pools; a C++ ew-theme-aim-v2 fit is the deletion's precondition.
- **required** D2-R2 / D2-8 (ledger :328, :353): engine-mode `parent_check` semantics unchanged (P12 vs the parent).
- *optional* S2-FIX1 (5) (as-of scratch in `strategy_ic_admission.cpp:330-339`) if the PM moves it here (Q1).
- *optional* D2-7 (ledger :352): the fitter-wide cross-module fingerprint is P10, not D3.
- *optional* (root) D2 S-5 (D2 report, S-5 row): a pre-D2 parent fit is refused; refit it (0 trials) before P9-W.

## A4 (pool 12; stretch: moves to P10 first if pools are short)

- **required** A1-SHIM, moved here by A3-SHIM (ledger :270): route `prepare_research_fields_{draft,xdata,ohlc,ydata}.py`
  `main` through the registry entry; keep `register`, `DRAFT_MODULES`, "DEPRECATED" (pool-12 `handoff-A3.md:210-215`).
- **required** S-R2 (ledger :321; pool-12 `task-A3-rereview-1.md:144`): `prepare_research_fields_engine.py:268` refuses
  statistics whose `PANEL_KEYS` are all `None`.
- **required** A3 review S-2 (A3-FIX1 ledger :288; status-4 :66): price-field co-build dependence under `--reuse`: put
  the history extent (or co-requested price set) in the reuse key, or build the union from every requested plan.
- **required** A3-3 (rr :71-72; status-4 :66): per-field re-hash under `--reuse` (accepted in A3, noted for wave 3).
- **required** P18 (ledger :41): reuse AL-SIG's 13F reader; no second 13F decoder.
- **required** Identity: TRAIN payload SHA-256 per moved field = the current manifest's; each `kind: engine` flip in its
  own commit after its TRAIN identity (A3 flip precedent, ledger :269, :305; OD-A3-6F :361); seal counts read from the
  engine receipts (A3-FIX1).
- **required** S-6 boundary (A3-FIX1 ledger :288): `coskew_60m` and `xrd0_ttm` stay Python in P9.
- *optional* OD-A3-SEAL (ledger :360, non-gating): page-level / sorted-file pruning of sealed rows (TickerHistory3: 262
  row groups, none prunable, `root-wave2-merge-report.md:198-203`).
- *optional* A3 review S-4: `session_calendar_pin()` recomputed per price field (`vendor_fields.cpp:268`, `:296`).
- *optional* A3 concern 3 (pool-12 `task-A3-report.md:365-368`): union panel keeps bars on ~1,600 pre-role rows.
- *optional* A3 concern 4 (`task-A3-report.md:369-371`): engine `source_checks` per field, not per group.
- *optional* A3 re-review N-1 (`task-A3-rereview-1.md:152-158`): make `same_panel` None-aware for the shares keys.
- *optional* ALSIG-3 (`root-wave2-merge-report.md:342-344`): `ENGINE_FIELDS` does not route `gia_13f` / `russell_recon`.
- *optional* Carried A1 / A2 minors (pool-12 `task-A3-report.md:177-178`): `--engine-exe` Python twins, freeze test
  checks names only, `first_session` format; dev-shared reuse hashes the exe not the DLL, TRAIN alternative check,
  `.pending` pin, receipt-before-manifest test, `strategy_live.cpp:424` `code_sha256`.
- *optional* W1-MIN M-4 configure-time `git_sha` if the PM moves it here from SQL3 (Q5).

## AL-DATA (pool 21 / 23 proposed; gated)

- **required** OD-P9-4..6 not executable (ledger :16): build nothing unless data already landed; for each family a
  one-page design in the report.
- **required** ALSIG-7 (ledger :307): write the request to the atx-db owner to fix the Russell indexes stage look-ahead
  (`atx-db` `indexes.py:275-279`: a member dropped before the effective date is stamped available at the rank date, so
  NaN reveals a delisting 4-7 weeks early); `russell_recon` feeds no trial until fixed; the lane never touches `atx-db/`.
- **required** ALSIG-4 (ledger :258): `hf_crowd` (Form ADV crowding, F2 [46]) stays blocked and is never queued without
  a new PM ruling on the rho bar.
- **required** Amend `scripts/specs/p9/blocked/{lazy_prices,hf_crowd}.json`; no duplicate id across queue dirs (a
  duplicate refuses the whole catalog run; pool-23 `task-SQL2-rereview-1.md:110-115`).
- **required** K-P9-11 keys; `source_sample_end` from the paper, unverifiable rows left out (ALSIG-1, rr :60-61); ids
  <= 24 chars (W1-I1).

## SQL3 (pool 17)

- **required** SQL2-S7 (ledger :304; pool-23 `task-SQL2-rereview-1.md:98`): narrow the `--root` year-token check
  (`seal_guard.cpp:120-122`; callers `catalog.cpp:739`, `:752`, `cache_index.cpp:226`, `store_cli.cpp:266`, `:317`,
  `:464`) to the part at or below the repo root. C++ in SQL2's files (Q4).
- **required** SQL2-S7 / N3 (`task-SQL2-rereview-1.md:110-115`): decide whether `candidate` is keyed by path (a
  re-queued id becomes legal; a `since = 2` catalog column) or stays keyed by id with the refusal documented (Q4).
- **required** SQL1-MON (ledger :235; pool-21 `task-SQL1-report.md:326-336`): `book_monitor.py --fit-work` must not
  under-report on an indexed work dir (enumerate through `RecordStore`, or refuse an indexed dir) (Q5).
- **required** W1-MIN M-4 (ledger :316; `wave1-whole-review.md:108-117`): configure-time `engine_git_sha` / fields
  `git_sha` (`atx-engine/CMakeLists.txt:291-319`) invalidates field reuse on every reconfigure; the fix sits in CMake /
  `research/fields/reuse.cpp:140-186`, outside SQL3's Python files (Q5).
- **required** Path-length margin (M1d-RED-2 ledger :279; status-4 :65; `wave1-whole-review.md` I-1): the hook and the
  `store` block add no path depth; record index paths repo-relative. The cache-path refusal itself is E2's (W1-I1) and
  root's measurement (S2-4, ledger :296).
- **required** SQL-6 / SQL-2 / SQL-11: the `store` block records the selection; JSON stays the authority; the hook never
  changes an exit code; no P9 cell uses a cache index.
- *optional* E2-4 (ledger :283): the double-reclaim lock race lives in `scripts/run_bounded_research.py:186-308`
  (`HostClaims`, SQL3's file), not in an AL-CLOCK file (Q2).
- *optional* SQL2 notes N1 / N2 (`task-SQL2-rereview-1.md:100-109`): a refused ingest leaves a partial catalog and
  renames refuse until `--rebuild`; the hook's warning line names `catalog --rebuild`.
- **required** (root) M2A-4 (ledger :346): the real-tree catalog run is root's, at each wave gate "store" step; SQL3's
  identity procedure uses the tiny-world.

## PRE (pool 13): prereg lines and inputs

- **required** Timings off for every pinned run (W1-MIN M-3, ledger :316; `wave1-whole-review.md:100-107`).
- **required** mom-volman fixed-TRAIN schedule limit for P9-W and OD-3 (ALCOMB-4, ledger :268; pool-19
  `task-AL-COMB-report.md:319-324`); theme-cov-hedge has no such limit.
- **required** P9-L 30-day exposure (S2-FIX1 (3), ledger :325; pool-18 `task-S2-review.md:92-99`): under
  `--label-terminal` the delisting cause may be classified up to 30 days after T: truncate imputation 30 days before TRAIN
  end or state the exposure.
- **required** P9-L inputs (pool-18 `task-S2-rereview-1.md:62-68`; `task-S2-report.md` concern 2): no pre-fix
  `imputed-v1` IC-result entry reaches P9-L; an imputed terminal return is never guard-checked: the ruling says so.
- **required** Paired-test fallback (B2-R1 ledger :310, B2-6 :347, B2-7 :348): a pair with 1 - rho < 1e-6 prints "na";
  root measures rho of the X-5 / Y-F0 pairs first; the prereg names the fallback for a P9 pair below 1e-6;
  rounding-boundary print lines are listed, not auto-accepted.
- **required** Ids <= 24 chars, <= 18 under audit-exact at pool-2 depth (W1-I1 ledger :314; pool-17 `handoff-E2.md:75`);
  root measures the longest real cache path before any cell (S2-4 ledger :296).
- **required** Not queued: `hf_crowd` (ALSIG-4 :258), `rank(russell_recon)` (dropped: ALSIG-5 :264, non-PIT ALSIG-7
  :307, ALSIG-8 :308), `rank(gia_13f)` (not registered, ALSIG-2 rr :62), `lazy_prices` (blocked_on_data); nothing from
  WQ-EXPLORE (WQ-1 :343).
- **required** P9-S content (plan §5.2 :903): after the line above AL-SIG has no queueable string and no AL-DATA data
  landed, so P9-S has no string (Q15).
- **required** P9-V (COV-6 ledger :198): registered; runs only if P9-X is the Y-1 form, else lapses at 0; predictions =
  D-COV's print (plan §3.4 step 6), read per status-4 :57 (Release after Debug = Release container identity, COV-7
  :294; per-rank eigen b, pooled with and without the lowest rank, atx-cov-v1 read against v1.1; pool-24
  `task-COV-rereview-1.md:255-262`).
- **required** P9-MV (COV-7 ledger :199): dropped at 0 if COV-MV is not merged and APPROVE before the P9 cells start;
  the registration names the store pin it reads (Q10).
- **required** Ceilings (COV-8 ledger :200; DEC-18): N_c <= 71, N_tot <= 252; <= 8 hypothesis + <= 1 protocol cell;
  <= 10 strings; 0 mined campaigns.
- **required** G-P11 / COV-6 (ledger :224, :273; pool-24 `task-COV-review.md:239-240`): the P9 role
  `train-2020-2023-lo3` has `universe.delisting.returns_applied: false`: no as-of exception; root re-checks before
  every COV-reading run (S9 / COV-9, ledger :354).
- **required** Q5 diagnostic: q and q_lit printed before P9-H is registered (plan :589-590; `task-AL-COMB-report.md:241-256`).
- **required** E1-REUSE-a2 (ledger :153) and W1-I2(b) (:315): cell manifests pin exes; parents re-pinned to the P9-B0
  references (after wave 2, again after wave 3) before the first P9 cell.
- **required** DEC-16 (ledger :79): info-clock vs Y-5 and mom-volman vs Y-2 / Y-1 ruled distinct; AL-CLOCK's
  restatement argument recorded.
- **required** SQL-11 / SQL-12 (ledger :212-213): no P9 cell uses a SQLite cache index; SQL4's identity runs join the
  wave-3 set only if SQL4 merges before the cells.
- **required** COV-5 (ledger :197): every `atx-cov-v1` parameter fixed; a change after D-COV is a new recipe id (P10).
- **required** M1c-RED / C2-5 (ledger :244, :291): Release `norm-score-v1` books are evidence only after C2's fix;
  `rel` / `rel-avx2` are not identity surfaces.
- *optional* B2 review S4 (pool-14 `handoff-B2.md` S4 row): `atx-research-eval --trial` / `--history` are trusted-caller
  flags; PRE's E[max] / DSR arithmetic uses the N `nav_summ` derives; house DSR uses the bisection `norm_ppf` (B2-9 :350).

## AL-CLOCK (pool 19; 3b)

- **required** DEC-16 (ledger :79): constants are Y-5's and [29]'s, no new constant; theta_f per Q16.
- **required** K-P9-7 row shape (`x-flag`, per-param `x-flag`, `incompatible[]`); rule file in `scripts/specs/p9/rules/`;
  template per ALCOMB-1.
- **required** W1-MIN M-1 pattern: 1-vs-4 `--book-workers` byte test with the rule on.
- **required** Shares C3's registry / NavSpec files with COV-MV (one row + one parser entry each): append-only;
  AL-CLOCK merges first, COV-MV rebases (plan §3.2) (Q9).
- **required** (root) W1-I2: the NAV exe changes; parent re-pin before the cells.
- *optional* E2-4 (ledger :283) "parked for AL-CLOCK / E-follow-up": the code is in SQL3's `run_bounded_research.py` (Q2).

## COV-MV (pool 24 proposed; 3b; brief `briefs/brief-COV-MV.md`)

- **required** COV-7 (ledger :199): funded, 0 trials; P9-MV dropped at 0 if not merged and APPROVE before the P9 cells.
- **required** COV-4 (ledger :196): a decision at d reads store row d only (the same index as `BookScaler::estimate`
  `strategy_risk_target.cpp:141` and spo `strategy_spo.cpp:1097`); its reviewer traces it.
- **required** COV-9 / R2b (ledger :354; pool-24 `task-COV-rereview-1.md:170-190`): the u32 geometry wrap fixed in COV's
  round 2 before COV-MV consumes `factor_risk_view` (COV-MV is its planned zero-copy consumer).
- **required** `book::factor_variance` `check_model` (`atx-engine/src/book/risk_target.cpp:33-35`, pre-existing) compares
  `covariance.size()` with a wrapping `k*k`: guard if reused (K is bounded at 8,193 on container paths, pool-24
  `task-COV-report.md:495`).
- **required** COV-5: reads the store, never writes it; no `atx-cov-v1` parameter moves.
- **required** Blind: cov-design §5.2 asks the PM, not the lane, to read the spo-v1 / v2 v7 cell outcomes.
- **required** The registered rule's gross / priced-set reading is fixed in the brief before any code (blind; Q17).
- **required** (root) S9 role check (COV-9) and W1-I2 parent re-pin before P9-MV.

## SQL4 (pool 18; 3b)

- **required** SQL-7 / SQL-9 (ledger :206, :208): the ledger index head comes only from B2's `chain_head` /
  `prefix_heads`; the JSONL ledger stays the authority; B2's null-prev fix (`b295fa4f`, pool-14 `handoff-B2.md` S2 row)
  makes `chain_head` fold a `prev_sha256: null` line as the reader does.
- **required** SQL-12 (ledger :213): 3b with AL-CLOCK and COV-MV; not merged and APPROVE before the P9 cells -> P10
  untouched.
- **required** SQL-6 / SQL-11: explicit flags only; no P9 cell passes them; root deletes the index after its runs.
- **required** SQL1-FIX1 (3) (ledger :240): `open_cache` never creates; `atx-research-store cache init` is the only
  creator (SQL1 report :442).
- **required** D2's pair-cache source re-pin (P8, no bump; D2 report "P8" row): `MarginalIc.PairCacheSourcesPinned`
  holds at D2's value; SQL4 adds no bump.
- *optional* SQL1 parked minors (ledger :248; pool-21 `task-SQL1-rereview-1.md:159-210`): 9 (`record_store` damaged row
  never heals), rest of 11 (BUSY retry, `GetDriveTypeW` branch, concurrent creator, `since > 1` migration tests), N2-N6.

## Questions for the PM

- Q1 S2-FIX1 (5) is "noted for C3" but lives in `strategy_ic_admission.cpp:330-339` (IC runner, D-lane file): recommend
  move it to D3 (or keep it a root cold-u measurement note).
- Q2 E2-4's lock race (`run_bounded_research.py:186-308`) is "parked for AL-CLOCK", whose files it is not: recommend
  keep it parked (one runner per host, ledger :283); optional for SQL3, which owns the file.
- Q3 Does SQL3 also own `scripts/research_wave.py` (the only `Chain(...)` construction)? Recommend yes (unowned in wave 3).
- Q4 SQL2-S7 + N3 are C++ in SQL2's catalog (`seal_guard.cpp`, `tables_records.hpp`, `ingest_*.cpp`) but brief-SQL3
  forbids C++: recommend SQL3 takes `seal_guard.cpp` + its test (S7, with a root build) and keeps candidates keyed by id
  in P9 (N3 refusal documented; path key P10).
- Q5 SQL1-MON needs the fitter (`stored_factor_series`, D3's; SQL lanes may not touch it) and `book_monitor.py` (no
  owner); W1-MIN M-4 needs CMake / `reuse.cpp`: recommend SQL3 owns `book_monitor.py` and makes it refuse an indexed
  `--fit-work` dir (fail closed), M-4 goes to A4 (fields reuse owner) or P10.
- Q6 PM7-35 Python copy (`wave_rules.py:50-51`) has no wave-3 owner: recommend C3 (already owns `wave_rules.py:110-119`)
  after root's identity vs B1's predicate.
- Q7 brief-D3 lists no files: recommend fitter, `composition_*.py`, `research/composition/**`, `strategy_ic_runner.cpp`,
  `strategy_ic_composition.*`, `strategy_ic_admission.cpp`, `strategy_ic_detail.hpp`, `strategy_ic_rules.*`,
  `strategy_ic_theme_tsmom.*`, and the kind files of Q8; none of SQL3's.
- Q8 `scripts/wave_kinds.py`, `wave_stage_kinds.py`, `scripts/tests/test_wave_kinds.py`, `scripts/specs/p9/kinds/**`:
  D3 (walk-forward) and C3 (new-cost-model flip) both need them: recommend D3 owns them; C3's fixture flip is a root
  post-merge item (E2-2/3 precedent, rr :58-59).
- Q9 AL-CLOCK and COV-MV (both 3b) each add a row + parser entry in C3's registry / NavSpec files: recommend
  append-only cross-lane edits, AL-CLOCK merges first, COV-MV rebases onto it.
- Q10 Which store P9-MV pins (the `atx-cov-v1` container store from D-COV, or the v1.1 store of record): recommend
  `atx-cov-v1` (the eigen adjustment is the cell's reason, cov-design §5.2), ruled in PRE before any read.
- Q11 Pools: D3's planned 16 is unavailable and AL-DATA's 20 is D2's warm tree: recommend D3 -> 20, AL-DATA -> 21 or 23,
  COV-MV -> 24; C3 / SQL3 / SQL4 / AL-CLOCK wait for C2 / E2 / S2 / AL-COMB to merge and release 15 / 17 / 18 / 19.
- Q12 Unlisted hard dependencies: C3 on COV (COV-3: it moves `strategy_spo.*`), D3 on AL-COMB (same rule files), SQL3
  on B2's deletion commit (B2-8 hunks in `cycle_verdict.py`): recommend add all three to plan §3.2.
- Q13 A4's inputs (A3, AL-SIG) are merged; only T2 (re-dated SEC / holdings fixtures, merges last in wave 2) blocks it:
  recommend dispatch A4 at the T2 merge, not earlier.
- Q14 P1 dispatches PRE only after AL-CLOCK's report (3b), but PRE is a wave-3 lane: recommend dispatch PRE with wave 3
  (B2, E2 merged), leaving P9-C, P9-MV and P9-V predictions as placeholders closed by a short second round after
  AL-CLOCK's / COV-MV's reports and D-COV.
- Q15 P9-S has no queueable string (gia_13f excluded, russell_recon dropped, hf_crowd / lazy_prices blocked, no AL-DATA
  data): recommend PRE records P9-S as lapsed at 0 trials, N_c unchanged.
- Q16 AL-CLOCK's literal theta_f .12945 is the cadence-1 value of Y-5's `two_speed_fast_theta(C)`: recommend the rule
  uses the function (Y-5's registered rule at any cadence), else refuses cadence != 1.
- Q17 COV-MV's gross reading: recommend the priced block is rescaled to the desired target's gross over the priced
  names, unpriced names keep their weights (total gross = desired gross), as written in `brief-COV-MV.md`.
- Q18 AL-DATA as an early, doc-only lane (no data landed; designs + the ALSIG-7 atx-db request): recommend dispatch it
  now from the current head on a free pool; it writes no code.

## PM answers to Q1-Q18 (binding; this section wins over "Questions for the PM"; full text in `progress.md`, rulings W3-EARLY, W3-1..W3-18, D2-9)

- Dispatch rule (W3-EARLY): a lane starts when every hard dependency is merged and verified in root, from root's head
  at that moment; it merges root's wave-2 gate head into its branch (git merge, never rebase) before its report.
- Q1 (W3-1): S2-FIX1 (5) moves to D3, optional.
- Q2 (W3-2): E2-4 lock race stays parked (P10); optional for SQL3.
- Q3 (W3-3): SQL3 owns `scripts/research_wave.py`.
- Q4 (W3-4): SQL3 takes `seal_guard.cpp` + its test (C++, built under W2-BUILD); candidates stay keyed by id (path key P10).
- Q5 (W3-5): SQL3 owns `book_monitor.py`: refuse an indexed `--fit-work` dir, fail closed, fitter untouched. W1-MIN
  M-4 -> A4 optional, else P10.
- Q6 (W3-6): PM7-35 copy `wave_rules.py:50-51` is C3's to delete, after root's identity vs B1's predicate.
- Q7 (W3-7): D3 files: the fitter, `composition_*.py`, `research/composition/**`, `strategy_ic_runner.cpp`,
  `strategy_ic_composition.*`, `strategy_ic_admission.cpp`, `strategy_ic_detail.hpp`, `strategy_ic_rules.*`,
  `strategy_ic_theme_tsmom.*`, the kind files of Q8. D3 also carries, required (D2-9): the missing D2 S-1 test, the S-2
  refusal in `check_factor_series`, strict type equality in engine-mode `parent_check` (int vs equal float must not pass).
- Q8 (W3-8): kind files are D3's; C3's fixture flip is a root post-merge item.
- Q9 (W3-9): AL-CLOCK and COV-MV edits to C3's registry / NavSpec are append-only; AL-CLOCK merges first.
- Q10 (W3-10): P9-MV pins the `atx-cov-v1` store from D-COV.
- Q11 (W3-11): pools: C3 15, D3 20, A4 12, SQL3 17, SQL4 18, AL-CLOCK 19, COV-MV 24; PRE and AL-DATA use no pool.
- Q12 (W3-12): hard dependencies added: C3 on COV, D3 on AL-COMB, SQL3 on B2's deletion commit.
- Q13 (W3-13): A4 at the T2 merge; if the host is saturated then, A4 moves to P10 whole.
- Q14 (W3-14): PRE in two rounds (round 1 once B2 and E2 are APPROVE; round 2 after AL-CLOCK, COV-MV, D-COV, wave-3 P9-B0).
- Q15 (W3-15): P9-S lapses at 0 trials; N_c, K_a unchanged; nothing from WQ-EXPLORE.
- Q16 (W3-16): AL-CLOCK uses `two_speed_fast_theta(cadence)`.
- Q17 (W3-17): COV-MV gross rule as written in `brief-COV-MV.md`.
- Q18 (W3-18): AL-DATA now, doc-only, no pool, output `al-data/` in the root sprint dir.
- Added after drafting (COV-11, required for COV-MV): add an a = 2 over-correction control to the band test (COV
  re-review 2, S10); COV-MV checks per-row group values itself, the container reader does not (S11).
- Added after drafting (B2-10, required for PRE and every P9 cell template): each P9 cell spec / v2 manifest names
  `exes.eval`; verify refuses a v2 cell without it.
