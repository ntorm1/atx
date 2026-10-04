# Re-review 1: lane AL-SIG fix round 1 (`f0db9a4f..7811f421`)

Reviewer: alsig-review (scoped re-review, read-only, no subagents). Branch `feat/p9-al-sig-20261003`, pool-13. The fix
commits are `39d8548e` (R1), `343efbc7` (Russell ids, S1), `67401122` (S2) and `309507fb` (rulings), plus the report
commit `7811f421`. The round-0 review is `task-AL-SIG-review.md` at `1c649035`.

## Verdict

**APPROVE.** R1 is fixed on every build path, and its new gtest has teeth. There are no new Required findings.
- Every Suggested finding (S1-S6) is resolved or answered by a ruling.
- F1, the lane's own find, is verified against house code from git history.
- The new findings N1-N4 are all Suggested.
- N1 is a house-stage point-in-time defect that root must carry before any real `russell_recon` build. It does not
  block this lane: the string is withdrawn (ALSIG-5) and the field is unbuilt.

## How this re-review was done

**Constraints.** No build, and no edit to code. I did not touch `C:/atx` or any `atx-db/` working tree. I opened no real
data. House atx-db docs and code were read only through `git show` / `git grep` on `main`, `feat/tier1-v3-warehouse`,
`0c35357e` and `21d6c4ac`.

**Diff.** `git diff f0db9a4f 7811f421 -- atx-engine scripts`, with the parquet excluded, was written to
`alsig-rr1-fixdiff.txt` in this directory and read. The report and review markdown were read separately. The file was
deleted after the review.

**Build receipt `p9-alsig-c`** (`build-equity/mega-p9-alsig-c-*`):
- `ExitCode` 0. The exe SHAs are `bb646215…73d6` and `2a78cf2c…0b0a`, as reported.
- `Source` `1c649035`, `DirtyEntries` 21. `git diff --name-only 1c649035 309507fb` also lists exactly 21 files.
- The receipt is time-stamped 20:18:08. The four fix commits are 20:20:10-20:20:40. `309507fb..7811f421` touches only
  the report.
- The log (UTF-16, converted to scratch) shows every changed TU compiled: `registry.cpp`, `research_fields_cli.cpp`,
  `russell_recon_field.cpp`, `gia_13f_field.cpp`, and `sources/{stage,russell_proxy,thirteenf_filings,
  thirteenf_holdings}.cpp`, plus the 4 AL-SIG test TUs and the registry and manifest test TUs.
- The only diagnostics are clang-cl's `/MP` notice and the two third-party databento `getenv` deprecations.
- Every added C++ line is 100 columns or less (scanned).

**pytest.** Run in pool-13 with `PYTHONDONTWRITEBYTECODE=1 ... -B -m pytest -q -p no:cacheprovider`:
- `scripts/tests/test_p9_al_sig_registrations.py scripts/tests/test_wave_queue.py`: `15 passed`.
- `atx-engine/tools/test_field_registry.py atx-engine/tools/test_no_new_python_builder.py`: `39 passed`.
- `research_cycle.py candidates validate --dir scripts/specs/p9/candidates`: exit 0, `candidates: 1 valid`.

**Fixture regeneration.**
- Command: `make_al_sig_fixture.py --out <sprint dir>/alsig-rr1-fixture`, exit 0, stdout
  `{"gia_13f": 2072, "russell_recon": 132}`.
- `cmp` against the committed fixture: 29 of 29 generated files are byte-identical. The committed fixture holds only
  two other files, `.gitattributes` and the generator.
- The scratch tree was deleted by name.

**Registry fingerprints.**
- `formula_sha256` was recomputed in Python from each row's `spec_text` as `sha256(canonical({definition, field,
  revision: 1}))`, the layout of `field_spec.cpp:160`.
- At both `f0db9a4f` and `7811f421`, both rows match their committed fingerprint:
  - gia_13f: `2e693083…` at both;
  - russell_recon: `cf77e5c0…` before, `c6c4b1aa…` after.

## Per-finding status

| finding | status | evidence |
|---|---|---|
| **R1** (Required): stage pin not checked on `--reuse` | **RESOLVED** | See check (1) |
| S1: `kIndexesStage` rule unchecked | RESOLVED | `russell_proxy.hpp:32-33` requires `russell-proxy-v1`; `stage.cpp:40-51,59-61`. The list semantics are reviewed in check (2) and N3 |
| S2: null CUSIP mapped | RESOLVED (comment wording, N2) | `thirteenf_filings.cpp:231` skips an empty map CUSIP; `thirteenf_holdings.cpp:130-131` never maps an empty holding CUSIP. `RowScreen` (`research_fields_al_sig_thirteenf_test.cpp:161-166`) plants `{"", 999}` in the map, so the old code would return 999. The test has teeth |
| S3 / ALSIG-4: `hf_crowd` ruling not recorded | RESOLVED | `hf_crowd.json` notes; `test_blocked_registration` (`test_p9_al_sig_registrations.py:139`). See check (4) |
| S4 / ALSIG-5: SR_c half of the bar not argued | RESOLVED by ruling | See check (4) |
| S5: commit misattribution | RESOLVED | `git show --stat f1b3c792` deletes `scripts/specs/p9/candidates/gia_13f.json` (37 lines); `34e12223` touches only the registration pytest. The report text now says so |
| S6: no `source_sample_end` evidence | RESOLVED | The report cites lit 3.1 F1 / F2 and the round-0 reviewer's verification (2014, 2016) |
| F1 (lane-found): wrong Russell index ids | VERIFIED | See check (2) |

## Checks

### (1) R1: every requested stage pin is checked before any reuse or build

- **What changed.** `check_stage_pins(spec)` (`registry.cpp:240-254`) opens every requested stage with
  `PinnedStage::open`. That function reads only the manifest, which is cheap, then compares its SHA-256 with the pin and
  checks the schema, `status`, the rules and the `files` object. It does this whether or not a planned field reads the
  stage.
- **Call sites.**
  - `build_fields` (v1, plain): `research_fields_cli.cpp:372`, after `plan_fields` and before `build_entries`, and
    `:375`, after the role re-pin.
  - `build_registry_fields` (v2): `:396`, before `reuse_payloads` (`:399`) and before anything is written to
    `output_dir`, and `:413`, after the last field.
- **Search for a path that still skips it.** I found none.
  - `reuse_payloads` and `build_entries` have no caller outside these two functions.
  - `is_known_key` admits `reuse` only for a v2 spec, so the plain path cannot reuse.
  - `parse_build_spec` only plans, and builds nothing.
  - The Python engine path (`prepare_research_fields_engine.build`, `:143-152`) forwards no `thirteenf` / `indexes` key
    and cannot route the two rows (ALSIG-3).
  - The fully-reused case (`to_build` empty, so no kind opens a stage) is exactly what the pre-reuse check covers.
  - The re-check after the build bounds a manifest that changes during the run.
- **Flag-absent identity.** A spec with no stage key does no I/O, so the identity holds by construction.
- **The new gtest has teeth.** `ReuseRefusesAWrongStagePin` is at `research_fields_al_sig_fixture_test.cpp:241-278`.
  - `build_into` creates `dir/<tag>` before the build.
  - Without the fix, cases b and c succeed: reuse rule 5 passes and nothing is opened. Case d also succeeds, because
    its mis-pinned `thirteenf` is read by no planned field.
  - A fix that only checked after the build would fail `fs::is_empty(dir/tag)`, because `reuse_payloads` copies into
    `output_dir`.
  - A fix that only pinned the stages that planned fields read would pass b and c but fail d.
  - The control case (e) shows the right pins still reuse 2 payloads.

### (2) `343efbc7`: house Russell ids and rule list

**Citation verified (read-only, from git history).**
- **`indexes.py` on `main`:**
  - `:202` `sets = {"R1000P": {}, "R2000P": {}, "R3000P": {}}`;
  - `:226` `"index_id": "SP500P"`;
  - `:251` `"rule": ["russell-proxy-v1", "sp500-proxy-v1"]`;
  - `:241-242`: the membership file's flag columns are `r1000` / `r2000` / `r3000` / `sp500`;
  - `:272`: `available_at` = rank date at `C.MARK_HOUR_UTC`, which is 22 (`common.py:35`).
- **`indexes.py` on `feat/tier1-v3-warehouse`:** carries the same ids and rule list (`:215`, `:264`).
- **Docs:** `0c35357e:atx-db/docs/datasets/indexes.md:15` says `` `index_id`: `R1000P`, `R2000P`, `R3000P`, `SP500P` ``,
  and `ALPHA_PANEL_MARKET.md:94` (still on the branch tip) names them per rule.
  - The later `21d6c4ac` rewrite of `indexes.md` no longer spells the ids. The code and `ALPHA_PANEL_MARKET.md` remain
    authoritative.
- **Effect of the old ids.** The lane says the old ids would have made the real stage refuse rather than read wrong
  values. That holds: `russell_snapshots` refuses when no usable row carries a declared id.

**Fixture.**
- `expected/gia_13f.f64` and `expected/russell_recon.f64` have the same blob ids at `f0db9a4f` and `7811f421`
  (`9bfb1367…`, `4e3812f7…`). `thirteenf/` and `role/` are unchanged.
- **`constituents.parquet`.** I decoded the old (`f0db9a4f`) and new files with pyarrow (synthetic fixture). Both have
  46 rows and the same columns. They are equal after mapping `r1000/r2000/r3000/sp500` to
  `R1000P/R2000P/R3000P/SP500P`.
- **`indexes/manifest.json`.** Only `rule` (string to list) and the parquet's `bytes` / `sha256` changed.
- **`counts.json`.** Only `pins.indexes` changed, and it equals `sha256(indexes/manifest.json)` (`3ae997fd…`).
- So only the stage files and the pin moved. The regeneration (above) reproduces them byte for byte.

**Is "a list that contains" safe?** Mostly. `names_rule` (`stage.cpp:40-51`) takes a string equal to the rule, or an
array holding it. It refuses a missing rule, a different string, and a list without it (`IndexesStageRule`,
`russell_test.cpp:106`). Two points follow.
- Against a stage built under a different Russell rule, the list check holds as far as house naming allows. House rule
  names are per family, so a re-ruled Russell proxy would carry `russell-proxy-v2` and not v1, and would be refused.
  Accepting other entries is the right semantics: an `sp500-proxy` change does not touch the `R1000P` / `R2000P` rows,
  and exact-list equality would force a needless re-registration.
- The residual hole is a list that names two Russell versions. `["russell-proxy-v1", "russell-proxy-v2"]` would be
  admitted, although the house writer never emits one. See N3.
- The rule check is weak in any case, and the SHA pin carries the real binding. The Task 2x rebuild changed Russell
  eligibility without bumping `russell-proxy-v1` (`git diff main feat/tier1-v3-warehouse -- .../indexes.py`:
  `listed_panel_sql`; `21d6c4ac` `indexes.md:3,7`: the FINRA exchange fallback).

### (3) Registry row: new fingerprint, still revision 1

**Correct; no bump was needed.**
- The house rule is at `atx-engine/tools/prepare_research_fields.py:526`: bump a revision "only for a change neither
  [the definition text nor the code] shows". Here the definition text changed (the `clock`, `definition` and
  `staleness` strings), so the fingerprint moved by itself, from `cf77e5c0…` to `c6c4b1aa…`. A payload under the old
  fingerprint can no longer be reused.
- The field was never built or published. Its arithmetic is unchanged; only the names of its input ids changed.
- **Diff scope.** The `field_registry.json` diff touches only the `russell_recon` row (`formula_sha256`, `clock`,
  `definition`, `staleness`). The `caveats` edit in `russell_recon_field.cpp:60-66` is correctly absent, because
  `caveats` is not spec text.
- **Fingerprint checks.** My independent recomputation matches at both commits. `CommittedRowsPinTheSpecs` (C++ text
  equals the row) is in the gtest run the lane reports: 33/33 under its filter.

### (4) ALSIG-5 / ALSIG-4 implementation

- **ALSIG-5: `rank(russell_recon)` is not queued.**
  - `russell_recon.json` has `status: "dropped"`, `wave: null`, and history `[proposed, dropped]`. The second entry is
    by "AL-SIG (PM ruling ALSIG-5)" and its note names the ruling.
  - `wave_queue.py:7-10,49`: `proposed -> dropped` is the documented withdrawal, and `dropped` is final
    (`NEXT` has no entry for it). `transition` therefore raises on any pin, and `emit` selects only `PINNED`.
  - `doc_problems` accepts a withdrawn `dropped` with no wave (`:96-98`), and `candidates validate` passes.
  - The notes argue both halves of the bar (rho about 0; SR_c cannot be argued a priori).
  - Pinned by tests: `test_string_held_by_the_bar_is_out_of_the_trial_queue` (`:86`) checks the status, that the status
    is final, that pinning raises, the ALSIG-5 note, and both halves. `test_the_lane_leaves_nothing_in_the_trial_queue`
    (`:98`) checks that no AL-SIG doc is proposed or pinned. `test_candidate_is_a_valid_queue_registration` now expects
    the `[proposed, dropped]` history.
- **ALSIG-4: `hf_crowd` cannot be queued.**
  - It lives in `scripts/specs/p9/blocked/` (schema `atx.p9-blocked-registration/v1`, status `blocked_on_data`). That
    status is not a queue status, so a `pin` on it raises in `transition`, and `doc_problems` refuses it.
  - `test_blocked_registration` asserts it is not in the queue and that its notes carry ALSIG-4.
- **ALSIG-6.** Already pinned by `test_string_excluded_by_the_bar_is_not_registered`, which checks that the `gia_13f` row
  exists.

### (5) New edits in A2's files

The edits are minimal.
- `registry.hpp`: +8 at the tail (a 6-line comment and one declaration).
- `registry.cpp`: +2 includes and one 15-line function at the tail.
- `research_fields_cli.cpp`: +4 call lines.

I checked overlap with A3's branch (`git diff 1239a5ff feat/p9-a3-20261003`):
- A3 edits `registry.hpp` at lines 11-70, `registry.cpp` around `kKinds` (from line 135), and `research_fields_cli.cpp`
  at `BuildContext ctx{..., plans}`, `is_known_key` and `price_source` parsing.
- None of these touches the tails or the four new call lines. The fix round adds no conflict surface beyond the merge
  notes of round 0.

## New findings

`path:line | tag (severity) | problem | fix`

**N1.** House `atx-db/src/atx_db/alpha_panel/indexes.py:275-279` (`main`; `:288-292` on `feat/tier1-v3-warehouse`),
consumed by `russell_recon_field.cpp:34-58` and `field_registry.json` (`russell_recon` row,
`non_pit_aspects: []`) | **Suggested; root carry, must be settled before any real `russell_recon` build or trade-filter
use** | The stage's row set looks ahead, so the field is not fully point-in-time.
- How it happens:
  - `intervals` drops a member line of reconstitution R when its security_master `last_session` falls before R's
    `effective_from` (`if end is not None and end < eff: continue`).
  - `last_session` is `max(tradingDate)` over the whole history (`security_master.py:140`), which is future information
    at the rank date.
  - Every kept row is stamped `available_at` = rank date 22:00 UTC.
- Effect: during the event window (rank date + 2 sessions to recon date - 1), a line that will delist before the
  effective date is missing from snapshot R. It reads NaN instead of its +1 / -1 / 0, so its NaN reveals a delisting
  4-7 weeks ahead (survivorship look-ahead).
- Scope:
  - The prior snapshot R' is unaffected, because its filter is known by the time R is visible.
  - The fixture and gtests cannot see this, since it is a property of the real stage.
  - It became live with F1: before the fix, the reader refused the real stage.
  - It does not block this lane: the defect is in house code that the lane may not touch, the string is withdrawn
    (ALSIG-5), and nothing is built in P9.
- This answers round-0 root item 7. `available_at` is the rank date at 22:00 UTC and the rank inputs are known by then,
  but the row set is not.
- Fix (either):
  - (a) the stage owner keeps the row and lets `effective_to` carry the delisting (the membership flags already do);
  - (b) the lane declares it in `russell_recon`'s `non_pit_aspects`, which changes the spec text, so the registry row
    must be re-derived.

**N2.** `thirteenf_filings.cpp:231`, `thirteenf_holdings.cpp:130` and report row S2 | Suggested (minor; comment
accuracy) | "House build_13f: null -> unmapped" is true only when the map holds no null CUSIP.
- House `research_fields_holdings.py:719` calls `pc.index_in(..., value_set=keys)` with the default `skip_nulls=False`,
  which matches a null holding CUSIP to a null map CUSIP.
- Checked on synthetic arrays with pyarrow 18.0.0: `index_in(['C1', None], value_set=[None, 'C1'])` gives `[1, 0]`.
- The engine's new behaviour (never map a null or empty CUSIP) is the right one. gia_13f is engine-only (DEC-5), so no
  parity gate is affected.
- Fix: reword the two comments, e.g. "stricter than house build_13f, whose index_in would match a null to a null map
  row". Optionally flag the house edge to its owner.

**N3.** `stage.cpp:40-51` | Suggested (optional) | `names_rule` admits a list that names the declared rule next to
another version of the same family, e.g. `["russell-proxy-v1", "russell-proxy-v2"]`.
- No house writer emits such a list, and the SHA pin still binds the exact manifest.
- Fix (optional): refuse a list that also holds another `<family>-v*` entry of the declared rule's family, with a gtest
  row in `IndexesStageRule`.

**N4.** Nits (no action needed for merge):
- The report's "three `check_stage_pins` call lines, one per call site" is wrong: there are four
  (`research_fields_cli.cpp:372,375,396,413`, two per build function).
- `stage.hpp:71` still reads "carrying a declared rule other than the contract's", while `stage.cpp:60` now says "does
  not name".
- The `russell_recon` history note says "never pinned without a new PM ruling". `dropped` is final, so even a new ruling
  cannot pin this document: a re-test is a new variant (`wave_queue.py:10`).

## For root

1. **K1 `--plan-only` discrepancy.** `progress.md` line 286 says "the K1 --plan-only root item is withdrawn". Ruling
   ALSIG-5 says "K1 --plan-only still runs", and the lane report (`task-AL-SIG-report.md` "For root") keeps it. The
   ruling governs, so correct the progress line.
2. **N1** before any real `russell_recon` build: either a stage fix by its owner (MKT / atx-db) or a `non_pit_aspects`
   declaration with a re-derived registry row.
3. **Unchanged from round 0, items 2-8:**
   - merge after A3, keeping both sides;
   - on the merged build, the gtest filter `ResearchFieldsAlSig*:ResearchFieldsRegistry*` now gives **33** passed;
   - `test_research_fields_engine_path.py` with `ATX_RESEARCH_FIELDS_EXE`, with no skip;
   - SQL2-CLS;
   - ALSIG-3.
