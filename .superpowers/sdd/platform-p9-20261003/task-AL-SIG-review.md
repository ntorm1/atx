# Review: lane AL-SIG at `f0db9a4f` (base `1239a5ff`)

Reviewer: alsig-r (adversarial, read-only, no subagents). Branch `feat/p9-al-sig-20261003`, pool-13.

## Verdict

**Overall: CHANGES_REQUIRED.** There is one Required finding (R1). All other findings are Suggested.

- **Spec compliance: PASS.** Every brief, carry and ruling item is met (checked one by one below). R1 is a gap in the
  K-P9-2 pin contract on one code path, not a missing deliverable.
- **Task quality: CHANGES_REQUIRED (R1).** Apart from R1 the work is of high quality:
  - the readers are pure, typed decoders;
  - the closed-form gtests check behaviour;
  - both leak probes have teeth;
  - the fixture oracle is independent of the C++ and regenerates byte for byte.

## How this review was done

The lane deleted its build tree to save disk space, so I did not rebuild. I reviewed the code from source, the build
receipts and logs, and the pytest runs below. Rebuilding was not needed to decide the verdict, and the fix round for R1
rebuilds anyway.

**Diff.** `git diff 1239a5ff f0db9a4f`, binary fixtures excluded, read from a scratch file in this directory. The file
was deleted after the review.

**Build receipts.**
- `build-equity/mega-p9-alsig-{a,b}-*` were read.
- Run a stopped on `russell_recon_field.cpp(168,43) iso_date`.
- Run b exited 0, with exe SHA-256 `fcd568ba…24ca` and `8fcb2bee…86be`.
- In logs a + b, every lane TU compiled. That covers:
  - the 8 new library sources;
  - the 4 new test TUs;
  - `registry.cpp`, `research_fields_cli.cpp` and `research_fields_registry_test.cpp`.
- The only diagnostics are clang-cl's `/MP` unused-argument notice and third-party databento `getenv` warnings.
- The target links `atx_warnings` (`/W4 /permissive-`). `ATX_WERROR` defaults to ON (`CMakeLists.txt:257`) and
  `equity-dev` does not override it.
- So the `/W4 /WX` claim holds for clang-cl Debug.
- Every changed C++ line is 100 columns or less (scanned). The CMake tails exceed 100 columns, as the rest of those
  files do.
- Receipt b was built from `80074679` with 1 dirty entry. `f1b3c792` adds exactly one C++ line to `80074679` (the
  `clock.hpp` include). HEAD's C++ equals `f1b3c792`'s (`git diff --stat f1b3c792 f0db9a4f` shows only the report and
  the pytest).

**pytest.** Run in pool-13 with `PYTHONDONTWRITEBYTECODE=1`, `-B -p no:cacheprovider -rs` and explicit paths:
- `scripts/tests/test_p9_al_sig_registrations.py scripts/tests/test_wave_queue.py`: exit 0, `13 passed in 16.67s`.
- `atx-engine/tools/test_field_registry.py atx-engine/tools/test_no_new_python_builder.py`: exit 0,
  `39 passed in 30.02s`.
- `atx-engine/tests/fixtures/research_fields/test_research_fields_engine_path.py` without `ATX_RESEARCH_FIELDS_EXE`:
  exit 0, `10 passed, 1 skipped in 14.82s`. The skip is `:270 test_real_executable_identity`.
  - The lane reports `11 passed`, with nothing skipped, against its built exe. The counts agree: the one gated test is
    the flag-absent identity, which compares Python and engine byte for byte on si_shares, si_dtc and vol_126.
  - It is a real test, not a skip. It must be re-run by root on the merged build.

**Fixture regeneration.**
- Command: `python -B make_al_sig_fixture.py --out <pool-13 sprint dir>/alsig-rv-fixture`, exit 0, stdout
  `{"gia_13f": 2072, "russell_recon": 132}`.
- Compared with `cmp`: all 29 generated files are byte-identical to the committed fixture. That includes `expected/*.f64`
  and `counts.json`, so no expected value was edited to fit.
- The scratch directory was deleted.

**Citations (web, no data).**
- CHL 2015: NBER w19290 PDF text reads "for the 1996-2012 sample period", so `source_sample_end` 2012 is verified.
- Lazy Prices (JF 75(3)): "quarterly and annual filings ... from 1995 to 2014", so 2014 is verified.
- BHL 2022 (RFS 35(7)): "about 1,500 hedge funds from 2004 through 2016", so 2016 is verified.
- WYZ 2006: the lane's evidence comes from the 2012 CFR working-paper revision. It is moot, because the string is not
  registered (ALSIG-2).
- All four journal, volume and issue citations are correct.

## Point-in-time review (per builder)

**gia_13f.** No look-ahead found.
- V(P) is the maximum `available_at` over every filing of P filed by its deadline (`filing_date <= deadline`), notices
  included (`thirteenf_filings.cpp` `index_thirteenf`). This matches house `build_13f` (`research_fields_holdings.py`).
- Effective filings are a subset of those filings, so every position read is available by V(P).
- The skill of P uses excess returns of quarters P-7..P. Those need the positions and quarter-end implied prices of
  P-8..P.
- `GiaAccumulator::add` stamps P with a running maximum of V over every quarter with positions (V*(P) >= V(q) for every
  q read).
- `gia_row` reads P only when `V*(P) < date(t-1) 22:00 UTC`, `P <= date(t)` and `date(t) - P <= 150`. A later visible
  quarter wins, and a NaN at the anchor stays NaN.
- A quarter that is missing or sealed resets `prev_`, so no return spans a gap. The excess window is pruned to
  `(q+1-8, q+1]`.
- Seal:
  - a quarter whose V(P) is on or after 2024-01-01 is never used;
  - a holdings part whose filing quarter begins on or after the seal is never opened (house `source_part_is_sealed`).
- `filings.parquet` and `filing_checks.parquet` are decoded whole and filtered, as house `build_13f` does
  (`stage.table`, line 614). The FINRA reader set the same precedent.
- The 13F deadline comes from the stage (`deadline` column, the first SEC business day >= period + 45 d). The clock is
  `filing_date + 46 h`.

**russell_recon.** No look-ahead found relative to the stage's `available_at`.
- V(R) = max(`available_at` of R's rows, `available_at` of R''s rows). The stage sets `available_at` to the rank date at
  22:00 UTC.
- The window opens at the first session t with `date(t-1) 22:00 > V(R)`, which is rank date + 2 sessions (pinned by
  `PlantedLeakProbeHasTeeth`: `kRank2 + 2`).
- The window closes on the session before `recon_date`.
- Rows available on or after the seal are dropped and counted. The 2024 reconstitution in the fixture and in the
  gtests is dropped.
- The reconstitution dates bound only the window's end and cannot leak.

**Planted-leak probes have teeth.**
- gia: the plant changes quarter 4's weights.
  - If a later quarter's data leaked into an earlier quarter's value, the plant would surface at or before
    `V(Q4)`, and `EXPECT_GT(honest[changed], visible)` would fail.
  - The same-session mark variant is caught.
- russell: an exact first-change day is asserted, and the leaky variant is caught.
- Sealed part: `2024q1` is not opened (counted). The same filing pointed at `2023q4` or `1999q1` is opened and refused
  (zero SHA-256 or unlisted), so the predicate is what keeps the sealed part closed.

## Spec compliance, item by item

| item (brief / carry / ruling) | result |
|---|---|
| Carry P18: 13F and Russell readers in K-P9-2 style, reusable by A4 | met: `sources/{stage,parquet_columns,thirteenf,russell_proxy}`, public API in `thirteenf.hpp` |
| `gia_13f` kind + engine-only row (DEC-5), 13F from 2013q2, quarter-end plus filing-lag clock, skill from the manager's own past holdings returns | met (`gia_13f_field.*`, `gia_13f_skill.cpp`, `field_registry.json` row) |
| `russell_recon` kind + engine-only row, proxy rank-date clock, 2018-06+ | met |
| Planted-leak probe with teeth for every builder | met (above) |
| At most 10 strings, each with theme, tier, sign in the string, horizon class + half-life (PM8-5), citation, K-P9-11 keys | met: 1 queue string + 2 blocked. Horizon class and half-life sit in `notes` (no structured key exists) and are pinned by a regex test |
| `lazy_prices`, `hf_crowd` as `blocked_on_data` only | met (`scripts/specs/p9/blocked/`, not in the queue) |
| `rank(gia_13f)` not queued (ALSIG-2) | met. The candidates directory holds only `russell_recon.json`. No `scripts/` file other than the exclusion test and `hf_crowd`'s note names gia_13f |
| No trial run, no ledger appended | met. No ledger, trials or alpha-registry file is in the diff. The lane ran only `candidates validate`, which is read-only (wave_queue). The registration history says "trials 0" |
| ALSIG-1 (WYZ sample end) | met, and now moot |
| ALSIG-3 (`ENGINE_FIELDS` routing) | noted for root / A3 (report concern 1) |
| ALSIG-4 (`hf_crowd` blocked, not queued without a ruling) | the registration is held. Its text does not yet carry the ruling (S3) |
| ALSIG-FLIP / M1a-RED freeze | met. The registry diff only appends 2 rows and flips nothing |
| Flag-absent identity | met by construction: the new spec keys are optional; the parse_reuse refactor keeps messages, order and values (checked line by line); the 3 ported kinds are untouched; `engine_field_names()` is read only by tests. Evidence: lane gtests 68/70 (the M1a-RED pair only) and the real-exe pytest |
| CMake tails: one block each | met |
| Cross-lane edits listed | met (all 7 are listed) |
| No new Python builder (DEC-5) | met (the freeze test passes). The fixture's numpy oracle is test infrastructure, not a production rule |
| No `atx-db/`, nothing dated 2024 or later opened | met. The fixture is synthetic, and its 2024 rows and parts are poison or seal probes |

## Findings

`path:line | tag (severity) | problem | fix`

**R1.** `atx-engine/src/research/fields/research_fields_cli.cpp:394-408`, with `gia_13f_field.cpp:310,319` and
`russell_recon_field.cpp:206,215` | **Required (major)** | On the `--reuse` path the spec's stage pin is never checked.
- How it happens:
  - The kinds' `parse_*` only check that `spec.thirteenf` or `spec.indexes` is present.
  - `reuse_payloads` re-hashes `dir/manifest.json` on disk and compares it with the prior entry's recorded source.
  - `PinnedStage::open` is the only comparison against `StageRequest::manifest_sha256`, and it runs only inside
    `build_*_kind`. A reused field never reaches it.
- Effect: a spec that pins stage manifest X, run against a stage directory whose manifest is Y (equal to the prior
  build's), reuses silently and succeeds. The receipt then binds a spec that claims X to payloads computed from Y.
- A fresh build of the same spec is refused. The role pin, by contrast, is enforced on every path
  (`RoleAxes::load`, :392).
- `ResearchFieldsAlSigFixture.ReuseFollowsTheStagePin` (`research_fields_al_sig_fixture_test.cpp:209`) always passes
  the correct pin, so it cannot catch this.
- Fix: verify every requested stage pin before the reuse decision. Two options:
  - in `build_registry_fields`, open the requested stages into the shared sources before `reuse_payloads`;
  - or have the stage kinds' plans carry the pin and refuse a digest mismatch at plan or reuse time.
- Add a gtest: a wrong `manifest_sha256` with a valid `reuse` directory and an unchanged stage is refused, and nothing
  is reused or written.

**S1.** `atx-engine/include/atx/engine/research/fields/sources/russell_proxy.hpp:30` | Suggested (minor) |
`kIndexesStage` leaves `rule` empty, so it is not checked.
- The field's definition and clock depend on `russell-proxy-v1`: the rank-day rule, banding, and `available_at` =
  rank date 22:00 UTC. The fixture manifest carries that rule.
- House precedent pins a stage rule (`research_fields_holdings.py:65`, security_master).
- A re-ruled stage would be accepted with nothing but a new SHA pin.
- Fix: set the contract's rule to `"russell-proxy-v1"`.

**S2.** `atx-engine/src/research/fields/sources/thirteenf_holdings.cpp:125` and `thirteenf_filings.cpp:232` | Suggested
(minor) | A null CUSIP reads as `""` on both sides.
- If `cusip_map_pit` ever held a null-CUSIP row, null-CUSIP holdings would map to its security_id.
- House `build_13f` maps a null CUSIP to unmapped (`pc.index_in`, null to -1).
- Fix: skip empty CUSIPs when building the map, or treat an empty holding CUSIP as unmapped.

**S3.** `scripts/specs/p9/blocked/hf_crowd.json:20,29` | Suggested (minor) | Ruling ALSIG-4 came after the lane finished.
The registration does not say that it may not be queued without a new PM ruling on the bar. Fix: add the ruling to
`notes` or as a history entry.

**S4.** `scripts/specs/p9/candidates/russell_recon.json:20` | Suggested; **needs a PM ruling** | The notes argue only the
rho half of the lit §3.0 bar (rho about 0).
- On the other half they concede "small breadth ... gain is small (+.00 to +.02)", and net SR_c >= .4 is not argued.
- ALSIG-2 excluded gia_13f strictly by the bar. The brief names this string explicitly, and lit §3.1 F8 says
  "consider".
- Fix: either argue SR_c >= .4 from the mechanism in the notes, or record a PM ruling that registers it as the
  brief's named string or trade-filter input despite the SR half.

**S5.** `.superpowers/sdd/platform-p9-20261003/task-AL-SIG-report.md:15,31,68,102` | Suggested (minor) | The report
misattributes the commits.
- The deletion of `scripts/specs/p9/candidates/gia_13f.json` is in `f1b3c792` (the compile-fix commit), not
  `34e12223`.
- `34e12223` touches only `test_p9_al_sig_registrations.py`.
- The net tree is correct. Fix: correct the report text; do not rewrite history.

**S6.** `task-AL-SIG-report.md` (Evidence) | Suggested (minor) | The report gives no `source_sample_end` evidence for
`lazy_prices` (2014) or `hf_crowd` (2016). The reviewer verified both (above). Fix: add them to the evidence and the
ledger candidate.

## What root must check at merge

1. R1 fixed and re-reviewed.
2. Merge after A3 (ALSIG-FLIP). A3 edits the same five cross-lane files:
   - `build_spec.hpp`, `registry.hpp`, `registry.cpp` (`kKinds`), `research_fields_cli.cpp` (`is_known_key`) and
     `research_fields_registry_test.cpp` (`KindLookup`).
   - Keep both sides: `KindLookup`'s count and `want` list become the union in table order.
   - A3 adds `BuildContext::plans` after `sources`.
   - The `field_registry.json` tails also need both sides kept.
3. On the merged build:
   - `atx-engine-research-fields-tests --gtest_filter=ResearchFields*`: 0 failed (24 AL-SIG tests plus A3's fixed
     M1a-RED pair).
   - `test_research_fields_engine_path.py` with `ATX_RESEARCH_FIELDS_EXE` set, in its own session: no skip.
4. K1 `--plan-only rank(russell_recon)` on the fields build that carries P9-S.
5. Add `atx.p9-blocked-registration/v1` to `classes.json` (SQL2-CLS).
6. ALSIG-3: `prepare_research_fields_engine.ENGINE_FIELDS` does not route the two engine-only rows (root / A3).
7. The readers trust the stage's `available_at`, `cusip_map_pit` and `filing_checks.unit_factor`, as house Python does.
   Confirm from the house docs, not data, that the Russell proxy's `available_at` is the rank date at 22:00 UTC and that
   its rank inputs are known by then.
8. PM items:
   - S4 (does russell_recon pass the bar?);
   - whether the `gia_13f` field and kind stay while the string is excluded (report "Decisions");
   - report concern 5: the lane's `rm -f scratchpad/*.log *.out` in the shared scratchpad may have removed other
     agents' evidence files.
