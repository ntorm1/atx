# Brief: task W0-2

Plan: docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md (sections 3, 4 and 6.3 bind every task). Rules: .superpowers/sdd/platform-v8-20260929/lane-rules.md

### Task W0-2: role and fields for 2020-2023

**Files:**
- Create: `scripts/specs/v8/base-lo1.json`, `scripts/specs/v8/base-lo3.json`
- Create: `atx-impl/tools/compare_window_overlap.py`, `atx-impl/tools/test_compare_window_overlap.py`

**Interfaces:**
- Consumes: W0-1; A-3 stage inputs (if A-3 has not landed, root runs the builder commands by hand from the L9 report).
- Produces roles `build-equity/train-2020-2023-lo1`, `build-equity/train-2020-2023-lo3` and fields dirs
  `...-lo1-fields-v9`, `...-lo3-fields-v9` (63 fields, same list as `scripts/specs/v71.json` `fields.list`).
- Produces `compare_window_overlap.py --old DIR --new DIR --kind {signal,daily_ic,field} --out report.json`: aligns by
  (session, instrument id); prints max absolute difference, count of unequal cells, and `bit_identical: true|false`.

- [ ] **Step 1:** rebuild the identity bridge with `--seal 2024-01-01`; pin its manifest SHA in both specs.
- [ ] **Step 2:** build the roles: `prepare_recent_research.py role --start 2018-06-01 --score-start 2020-01-01 --end 2024-01-01`
  with the lo1 rule and the lo3 rule (`--universe linked-operating-v3`, `--sic-events`, `--delisting`). Keep the start date,
  so the panel's first date and every ResearchFast reseed point are unchanged.
- [ ] **Step 3:** `atx-equity-strategy-ic --plan-only` on each role. Record `required_bytes`. If it exceeds the cap in force,
  stop and apply the OD-2 ruling or switch to H-1.
- [ ] **Step 4:** build fields v9 on both roles through the bounded runner. Record seconds and peak MiB.
- [ ] **Step 5:** run the u pass of library v7.1 on the lo1 role, cold cache. Run `compare_window_overlap.py` against
  `mega-v71-train-u-1` for the cached signal payloads and `train_daily_ic.csv`, on sessions before 2022-09-30 (labels at
  h 63 mature in both).
- [ ] **Step 6:** write the result to the ledger under ruling W0-a (below), then commit the specs.

**Ruling W0-a (declare before step 5):** if the overlap is bit-identical, the 4-year cells continue the v7 ledger without
comment. If it differs by less than 1e-9, the difference is disclosed and the 4-year values are the reference from now on.
If it differs by more, the re-base stops and the cause is found -- a silent change in old values would make every paired
comparison uninterpretable -- cost if wrong: one day.

**Acceptance (root):** both roles admitted; fields v9 built with coverage on member cells within .02 of the 3-year build for
every field; overlap report committed.

