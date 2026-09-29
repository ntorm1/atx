# Platform code review v8: research iteration speed (task P1a)

Reviewer: Fable 5.1 (P1a, read-only), 2026-09-29. Tree `C:/atx` main @ `7fbfc379`. Nothing edited, built or run.

**Read**
- Sprint state `platform-20260928/`: code-review-v7.md, progress.md, plan-v7.md, task reports L1, L2, L8, L9, W2, W3, W5a,
  W5b, U2.
- Code: atx-impl/src/strategy_{ic_runner,ic_composition,nav_replay,price_exposures,target_replay}.cpp; atx-engine alpha
  (vm.hpp, streaming_engine.hpp, ts_sliding.hpp, panel.hpp) and src/data/strategy_data.cpp; atx-impl/CMakeLists.txt,
  tests/CMakeLists.txt, CMakePresets.json; scripts/research_cycle.py, run_bounded_research.py, specs/*.json;
  atx-engine/tools/prepare_research_fields.py, research_fields_{sec,holdings}.py, prepare_recent_research.py (CLI only);
  atx-impl/tools/fit_composition_weights.py, mega_report/data.py; atx-impl/strategies/generate_fund_ic_v*.py.
- Receipts (pool-2 `build-equity/`): the v7.1 cycle's six `*-run*/receipt.json`, the u/w `summary.json` stage timers of
  v6.1, v7rel, v7.0-lo3 and v7.1, 18 `mega-v7-*-receipt.json` build receipts, fit and card stderr logs.
- atx-db/ was not reviewed.

**Hygiene.** No file dated 2023 or later and no file named validation / VAL was opened (skipped by name:
`studies/v3_validation_once.sh`, `v4_validation_once.sh`). Every number below is a timing, a byte count or a line count.
Two disclosures: (1) plan-v7.md (mandated reading) carries one validation figure in prose; it is not used or repeated
here. (2) One `python -c` one-liner printed the key names and shape of the TRAIN role manifest (metadata only).

Effort: S < 1 day, M 1-3 days, L > 3 days. Value: H = at least halves a cycle or unblocks the 2012-2019 extension.

## Headline

1. **A library cycle now costs 134 s of compute, but only about 5 s of it is the new alpha.** v7.1 (4 new members on 44
   cached): ref 28.5 + u 23.0 + fit 21.9 + card 19.5 + w 14.9 + nav 26.0 s. The four VM evaluations took 1.7 s.
2. **Three stages redo finished work.** The fit computed 48 and reused 0 (fresh work dir per version). The card rebuilt
   all 48. The u pass spent 14.6 of 22.8 s loading and blending cached signals into a combined book nothing reads.
3. **The Release A/B was not inconclusive.** Stage timers separate CPU from disk: Release is faster on every CPU stage
   (composition 6.98 -> 2.20 s, vm 22.4 -> 17.5 s, ic 10.4 -> 8.4 s; -29% in total). Only the disk stages doubled, under
   four concurrent lanes. The Release exes are now stale (built 09-28 18:34) and the NAV A/B was never run.
4. **Adding an alpha is still a programming task.** v7.1 took a 560-line generator and a 355-line test for four alphas
   whose declarative content is about 190 lines, plus a 322-line spec (222 lines differ from v7.0's).
   The generators form a 7-deep import chain, each link re-derived and SHA-pinned at import.
5. **Extending TRAIN to 2012 breaks bit-identity of the current book under any single-role design.** The IC runner runs
   `ResearchFast`, whose sliding kernels reseed on a schedule counted from the panel's first date. A lookback-halo
   restart, or simply a longer role, changes low bits of the 2020-2022 signals. Era-sharded roles avoid this.
6. **The "64-field limit" is two limits.** The one at 63 of 64 is a manifest row count check (one line). The structural
   one (a u64 bitmask over referenced fields) is at 40 of 64.
7. **No test runs the real ladder.** research_cycle tests drive fake tools; four test suites each build their own
   synthetic panel; the three strategy gtest exes are not registered with ctest; no CI covers the platform.
8. **Research state lives in a CMake binary dir.** The trial ledger, 11 cache roots, every accepted cell and the
   untracked build script sit in gitignored `build-equity/` (1,208 entries). C: has 78 GB free (83% used).

## S1. Executive ranking

| # | id | finding | value | effort | files (line refs) |
|---|---|---|---|---|---|
| 1 | P-1 | No alpha registry: a generator + test + library + recipe + spec per version; Python mirror of the DSL parser | H | M | strategies/generate_fund_ic_v71.py:73-262,264-560; generate_fund_ic_v4.py:197-372; v70.py:272-384 |
| 2 | P-2 | Fit and card reuse nothing: work dir per version, records bound to the whole script's SHA | H | S | fit_composition_weights.py:242,926,1061,184-192; specs/v71.json:232; research_cycle.py:659,704 |
| 3 | P-3 | u pass blends a combined book nobody reads and loads every cached payload to do it | H | S | strategy_ic_runner.cpp:2020-2026,2220,2257-2262; research_cycle.py:478,508 |
| 4 | P-4 | Release exes not adopted although existing receipts show -29% CPU; tree stale; spec pins Debug paths | H | S-M | atx-impl/CMakeLists.txt:86-95; specs/v71.json:6-7,16-19; build-equity/mega-build.ps1 (untracked) |
| 5 | P-5 | Whole-panel evaluation: memory scales with dates x names; a halo restart is not bit-identical | H (v8) | M (shards) / L (blocks) | strategy_ic_runner.cpp:611-662,2033-2049,2099; ts_sliding.hpp:20-29,76-89; vm.hpp:1701-1710 |
| 6 | P-6 | Field caps: manifest row check at 63/64; referenced-field bitmask at 40/64 | H (next wave) | S | strategy_ic_runner.cpp:737,511,188 |
| 7 | P-7 | 22 of 63 fields are never reused (SEC, holdings modules); a fields build is 59-121 s | M | S-M | prepare_research_fields.py:501-505,525-528,2917-2923; research_fields_holdings.py:10 |
| 8 | P-8 | research_cycle gaps: no add-alpha, no L9 stage inputs, whole-repo dirty stop, manual dsr_n, cache seed by hand | H | M | research_cycle.py:100-102,597-635,936-952,591-594; run_bounded_research.py:91-93 |
| 9 | P-9 | NAV replay has no stage timers; exposures rebuilt from 253 sessions per decision; no construction lockstep | M | M | strategy_price_exposures.cpp:107-146; strategy_nav_replay.cpp:1014-1112 |
| 10 | P-10 | Four Python mirrors of C++ logic (C1 still open; two were added in v7) | M | M | fit_composition_weights.py:670-780; alpha_report_card.py; generate_fund_ic_v4.py:230-318; check_fund_ic_v6.py |
| 11 | P-12 | No end-to-end fixture; strategy gtests outside ctest; no CI | M | M | atx-impl/tests/CMakeLists.txt:54,84-111; scripts/tests/test_research_cycle.py:177-320 |
| 12 | P-14 | Research outputs, ledger and build script in the build dir; tools under a sprint folder; data in Downloads | M | S-M | .gitignore:12; specs/v70.json sources block; specs/v71.json:310,319 |
| 13 | P-11 | Report seal regex matches hex path parts; it also does not cover 2026 | L-M | S (owner gate) | mega_report/data.py:23,49 |
| 14 | P-13 | 160 KB files: a split pays in lane disjointness, not compile time | L-M | M | strategy_ic_runner.cpp; strategy_nav_replay.cpp |
| 15 | P-15 | Small caps and stale constants: workers <= 4 on 16 threads; generator bound 314 vs runner 336; caps in 5 places | L | S | strategy_ic_runner.cpp:2348,616-617; generate_fund_ic_v4.py:53 |

## S2. Measured timings (TRAIN 2020-2022, existing receipts)

**v7.1 cycle, one invocation** (first receipt 11:03:13 UTC, monitor written 11:05:29: 136 s wall).

| phase | wall s | peak MiB | what the time is | avoidable |
|---|---|---|---|---|
| ref (identity NAV on new fields) | 28.5 | n/a | a full NAV replay, needed only when the fields dir changes | all, when fields are unchanged |
| u (44 hits + 4 evaluations) | 23.0 | 767 | composition 9.23, cache_load 5.36, vm 1.74, ic 1.69, labels 0.71, load 0.50 | about 15 s (P-3) |
| fit | 21.9 | 320 | context 6.16; 48 records x 0.31 (computed 48, reused 0) | about 14 s (P-2) |
| card | 19.5 | 968 | 48 cards x 0.65-0.92 over 4 workers | about 15 s (P-2) |
| w (48 hits) | 14.9 | 506 | composition 8.00, cache_load 2.98, labels 0.70 | about 5 s (P-4) |
| nav | 26.0 | 347 | no stage timers | unknown (P-9) |
| monitor, summ, fields check | n/a | n/a | run "direct": no receipt, no timing | n/a |
| **sum of receipted phases** | **133.8** | 968 | | **target about 50 s** |

**Release vs Debug, same library (v6.1), cold cache, 39 evaluations** (`mega-v61-train-u-1` vs `mega-v7rel-train-u-1`).

| stage | Debug s | Release s | kind |
|---|---|---|---|
| vm | 22.36 | 17.55 | CPU |
| ic | 10.41 | 8.36 | CPU |
| composition | 6.98 | 2.20 | CPU |
| **CPU stages** | **39.75** | **28.11** | -29% |
| fields_load + fields_verify + cache_write | 29.94 | 63.97 | disk + scalar SHA, under 4 concurrent lanes |
| wall | 75.9 | 97.9 | |

**Builds** (mega-v7-* receipts, 3-4 jobs by free RAM). 1 TU + 1 link 16.3-16.9 s; 2 TUs 9.0-18.9 s; 6-12 TUs 23-54 s;
95-120 TUs 215-223 s; Release from cold 161 TUs 426 s. **Python tests**: 468 in 142 s (U2 report). **Fields**: v9 cold
121 s / 571 MiB; with reuse 58.7 s (W5a) and 69 s (W5b). **Hashing** after L1: 2.50 GB in 1.59 s (1.57 GB/s).
gtest run times are not receipted anywhere.

## S3. Findings in detail

### P-1 Alpha registry and one generator (H, M)

*Evidence.*
- 7 generators 3,726 lines, 7 tests 2,341 lines, checker 204 lines; 14 JSON files (libraries 28-38 KB, recipes 84-156 KB).
- Import chain v71 -> v70 -> v61 -> v6 -> v5 -> v4 -> v2 grammar (`generate_price_volume_ic96_v2.py`). Every link
  re-derives its parent's documents and asserts their SHA (generate_fund_ic_v71.py:274-284).
- Copy-paste per file: `_load` and `pinned_<parent>` (v71:264-284, same in v6:258, v61:90, v70:235); `validate`
  (v71:309-322, v61:128, v70:384); `main` with `--check`; the budget constants (v42:45, v5:50-51, v6:66-68, v61:45-47,
  v70:64-68, v71:61-63).
- A Python DSL parser, lookback and slot estimator mirror the C++ front end (v4:230-318; v70:272-384 adds the W2 ops),
  cross-checked against registry.cpp by regex (v4:344-369).
- In v71 the declarative content is lines 73-262 (4 fields, 4 DSL strings, 4 member blocks, theme text). The other 300
  lines are procedure.
- The library JSON is already registry-shaped: id, family, dsl, theme, tier, tier_rank, prior_sign, citation.
- Consumers read little of the recipe: the fitter takes theme, tier and prior_sign per id and the library pin
  (fit_composition_weights.py:332-383).

*Change.*
1. `atx-impl/strategies/alphas/registry.json` (`atx.alpha-registry/v1`): one entry per alpha {id, dsl, theme, tier,
   prior_sign, citation, prior_sign_source, form, notes{formula, domain, deviation}, added_in}; a `fields` table
   {name: formula_id, origin, producer, clock}; a `themes` table.
2. `atx-impl/strategies/libraries/<name>.json`: {id, parent, members (ordered ids), budget exceptions, prereg ref}.
3. `atx-impl/strategies/generate_library.py --library NAME [--check]` writes the library and a slim recipe
   (`atx.dsl-ic-experiment/v2`: library pin, parent pin, lineage rows, trials rule). Static validation comes from the
   exe: `atx-equity-strategy-ic --plan-only` gains a `candidates[]` array {id, dsl_sha256, num_slots,
   required_lookback, extra_fields} (strategy_ic_runner.cpp:2396-2399 prints library maxima only today).
4. v4..v71 generators and their outputs are frozen as legacy (tests kept). The registry is seeded by importing the 48
   entries of fund_industry_ic_v71.json.
5. Themes move out of the fitter source (fit_composition_weights.py:184-192) into the registry.

*Effect.* A new alpha is one registry entry (about 12 lines) and one member id. No Python is written or reviewed.

*Acceptance.* (a) `generate_library.py --library v71 --check` reproduces fund_industry_ic_v71.json byte for byte
(sha 787c802e...). (b) The exe plan rows equal the committed `static_validation` figures of the v71 recipe for all 48.
(c) The fitter on the slim recipe writes an admission.json byte-identical to mega-weights-v71-ew after dropping the
recipe pin.

### P-2 Fit and card reuse (H, S)

*Evidence.*
- `mega-weights-v71-ew-run1/stderr.log`: `fit: computed 48, reused 0`, context built in 6.16 s.
- The spec names a new work dir per version (specs/v71.json:232) and nothing seeds it; the candidate cache is seeded by
  a hand-run `cp -al` (L8 report, root command 3).
- Records are bound to `SCRIPT_SHA256` of the whole 2,144-line script (fit_composition_weights.py:242,926,1061). Any
  fitter edit empties the store, and the theme whitelist lives in that script (:184-192), so a new theme is such an edit.
- The card has no store: 48 cards in 18.98 s, peak 968 MiB, the largest Python peak in the ladder.

*Change.*
- One work root per role: `build-equity/fit-work/<role sha16>/`; same for the candidate cache. Content keys make this
  safe. `research_cycle.py` derives both when the spec omits them.
- Bind records to a producer fingerprint (AST closure of `factor_record`, `Context`, `PricePanel`,
  `neutralization_basis`, `centered_tied_ranks`), as prepare_research_fields.py:2748-2780 does, not to the file SHA.
- Card: store the per-candidate invariant block (IC by year, decay, size and FF12 splits, coverage, book pnl) under the
  signal key; recompute only the correlation block, which depends on the admitted set.

*Effect (est.).* fit 21.9 -> about 8 s; card 19.5 -> about 5 s.

*Acceptance.* A v7.1 re-run on the shared roots prints `computed 4, reused 44` after a v7.0 run and writes a
byte-identical admission.json; a comment-only fitter edit leaves `reused 48`; cards are byte-identical to mega-cards-v71.

### P-3 Incremental u pass (H, S)

*Evidence.*
- The u pass blends every candidate (strategy_ic_runner.cpp:2220) and scores the blend (:2257-2262). The ladder never
  reads the u-pass blend: the NAV takes the w pass's `train_combined.json` (research_cycle.py:508).
- A signal hit with an IC-result hit still loads its 52 MB payload (:2020-2026) only to feed the blend: 5.36 s for 44.
- v7.1 u pass: 22.8 s wall, of which vm + ic for the four new members is about 3.4 s.

*Change.* `--no-composition` on the IC exe: skip `IcComposition`, `__combined__` rows, planned targets and
`--save-combined`; with it, a candidate with both hits is not loaded. The summary records `composition: "skipped"`.

*Effect (est.).* u 23.0 -> about 6 s; admitted bytes fall by the composition plane (about 59 MB).

*Acceptance.* orientations.json and the member rows of train_daily_ic.csv are byte-identical to mega-v71-train-u-1
(the existing `json-rows` / `csv-rows` compares keyed on library ids); the fitter output is unchanged.

### P-4 Release executables (H, S-M)

*Evidence.* Table in S2. What blocks adoption:
1. The ledger read the A/B by wall time only (progress.md:453-457) and never re-measured.
2. build-equity-rel/bin exes date from 09-28 18:34 (pre-L1, W2, L3, L4); the Debug exes are from 09-29.
3. The ruling asked for a NAV A/B as well; only the u pass was run.
4. Specs pin `build-equity/bin/...` and the vcpkg `debug/bin` DLL path (specs/v71.json:6-7,16-19).
5. No identity canary exists (v7 C3): no pinned small role to run after a build tag.
6. `mega-build.ps1` is untracked, inside build-equity, with `$root` hard-coded to pool-2.
7. The Debug tree's /O2 list is kept by hand, twice (atx-impl/CMakeLists.txt:87-94). strategy_live.cpp (decide),
   strategy_holdings.cpp and strategy_runner.cpp are not on it.
- Hypothesis, not measured: the 3.2x on composition is the Debug STL (checked iterators, debug `std::sort`
  predicates), which per-source /O2 does not remove.
- `vm_identity` has no build type (strategy_ic_runner.cpp:144-146), so both builds share cache entries by design.

*Change.*
- Track the build script as `scripts/research-build.ps1 -Preset equity-rel`.
- Spec key `build: "equity-rel"` resolves the exe dir and DLL path; the receipt already records the exe SHA.
- Keep gtests in the Debug tree; build only the three research exes in Release.
- Canary = the tiny fixture of P-12, run after each build tag, outputs compared by SHA across Debug and Release.

*Acceptance.* On a quiet host: u, w and NAV of the v7.1 cell byte-identical between builds (daily CSVs, orientations,
train_combined); CPU stages at least 25% lower; then the default spec build becomes equity-rel.

### P-5 History extension: era shards first, date blocks second (H for v8)

*Where the limits are.*

| limit | value | location | binding for 2012-2022? |
|---|---|---|---|
| dates per role | 4,096 | strategy_ic_runner.cpp:616; strategy_data.cpp:91; nav :46; target :37; risk :39 | no (about 2,725) |
| instruments | 20,000 | same five places | no |
| instrument union in the role builder | 8,000 default | prepare_recent_research.py:920 | likely (5,627 for 4.6 years) |
| warm-up | score_begin >= 383 | strategy_ic_runner.cpp:616; strategy_data.cpp:100 | first score date about 2013-09 |
| lookback | <= score_begin - 63 (336 on this role) | strategy_ic_runner.cpp:617 | grows with warm-up |
| memory | cells x (72 + 8 S + 8 C + 1) + labels + composition | strategy_ic_runner.cpp:611-662 | yes |
| slots per program, DSL bytes, candidates, families | 64, 4,096, 256, 32 | :590, :583, :575 | no |
| bounded runner | 8,192 MiB, 600 s | run_bounded_research.py:75 | yes for one long role |

- Today: 6.50 M cells, S 8, C 6 -> 185 B/cell -> 1,481 MiB admitted of 1,536. One more slot or field is 50 MiB: the
  library is already at the wall.
- One role 2012-03..2022-12 (estimates): 3.6 GiB at the same 5,627 names, about 5.4 GiB at 8,500 names. The v7 figure
  (4 GiB) held names constant; the union grows with history.
- The card (968 MiB at 1,155 dates) is the next process to cross 1,536 MiB.

*Why a lookback halo is not enough.*
- The runner evaluates in `ResearchFast` (:45, :2037). Its sliding lanes accumulate shifted sums and rebuild "once every
  kReseedMul*d clean steps" (ts_sliding.hpp:20-29,76-89); the ts_sum slide is Neumaier-compensated from the column start
  (vm.hpp:1701-1710). The schedule is counted from the panel's first date.
- So a block restarted at t0 - halo differs from the whole-panel run in low bits. The documented contract is 1e-9, not
  identity. Recurrences (trade_when, hump, kalman) carry state from date 0 and differ materially.
- StreamingEngine is bit-exact only with state carried from the same first date (streaming_engine.hpp:26-33).
- The same holds for a longer role without blocks: moving date 0 from 2018-06 to 2012-03 re-anchors every lane. The
  2020-2022 signals, and the accepted cells built on them, would not reproduce byte for byte.

*Change, in two steps.*
- **P-5a era shards (M).** Roles E1 2014-2016 and E2 2017-2019 built by the existing CLI (`--start`, `--score-start`,
  `--end`), each with a 399-session warm-up and the geometry of today's role. The existing role is E3.
  - research_cycle: `roles: [E1, E2, E3]` runs fields -> u -> w -> nav per era on shared, role-keyed cache roots.
  - Fitter and nav_summ pool the eras: factor returns and daily net returns concatenated in date order, one admission,
    one Sharpe. Each era's NAV deploys afresh; nav_summ already excludes the deployment session.
  - No C++ change. Memory per process stays at today's level. E3 stays byte-identical, so v7.1 remains the reference.
- **P-5b date-blocked runner (L).** `--block-dates 256`:
  - borrow sub-spans of the date-major columns with `Panel::create_borrowed` (panel.hpp:97) over [t0 - H, t1),
    H = the program's `required_lookback`;
  - stream the payload, labels and composition by date;
  - evaluate in `AuditExact`, whose windowed kernels are independent of the panel start (vm.hpp:1704-1705), so a block
    equals the whole panel bit for bit at any history length;
  - refuse programs with recurrence ops in block mode, or run them through StreamingEngine with carried state;
  - new `vm_eval_mode` string, hence new cache keys. This is a one-time re-base, declared as an identity step.
  - Working set about (H + 256) x n x 136 B = 0.45 GB at H 336, independent of history. Candidates can then run in
    parallel.

*Acceptance.* 5a: the E3 cycle reproduces the v7.1 cell's S2 daily CSV SHA; E1 and E2 peak below 1,536 MiB; the pooled
summary over E3 alone equals mega-nav-v71-summ-n37.json. 5b: on the tiny fixture and on E3, block output equals
whole-panel `AuditExact` output byte for byte for every library member; AuditExact cold vm time is measured first (it
is unknown, and it decides whether 5b is affordable).

### P-6 Field caps (H at the next wave, S)

*Evidence.* `rows.size()>64` (strategy_ic_runner.cpp:737) counts manifest rows; fields-v9 has 63. The rows go into a
`std::map`; no bitmask indexes them. The u64 masks (:188, :511-518) index `Library::extra_fields`, the referenced
subset: 40 in v7.1. No other stage caps the row count (NAV, risk, live and fitter were checked).

*Change.* Raise the row cap to 1,024 now. When referenced fields approach 64, change `FieldPlan` masks to
`std::bitset<256>`. *Acceptance.* A 200-row manifest with 40 referenced fields is admitted; the field plan (capacity,
loads) of v7.1 is unchanged; a library referencing 65 fields still refuses until the bitset lands.

### P-7 Field reuse (M, S-M)

*Evidence.*
- At HEAD the builder's own six groups are keyed by an AST closure per group (prepare_research_fields.py:525-528,
  2748-2780). That is finer than the ledger's wording ("keys on the builder module's code sha", progress.md:235).
- The SEC and holdings module fields are never reused by design (:501-502; research_fields_holdings.py:10). Their
  group has no entry in `FIELD_PRODUCERS`, so :2921 marks them "producing code differs": 22 of 63 fields every build.
- A closure reaches shared helpers, so one helper edit recomputes a whole group (U2 concern 2: 32 issuer fields).

*Change.* Each module exports `PRODUCERS = {group: (entry functions)}` and its source; `producer_fingerprints` runs
over that module's AST. Stage inputs are already pinned per field (stage manifest SHA), which serves as the source
check. *Effect (est.).* A fields build that adds one field: 59-121 s -> about 10 s (hardlink plus hash).
*Acceptance.* Rebuilding fields-v9 with `--reuse` of itself reports reused 63, computed 0, and an identical `files`
map; a one-line edit in `build_ftd` recomputes only ftd_shares_ratio21.

### P-8 research_cycle: add-alpha and plumbing (H, M)

*Evidence.*
- No `add-alpha` verb (verbs at research_cycle.py:4-8). `INPUT_KEYS` (:100-102) has no SEC or holdings stage pins and
  `fields_step` (:597-635) passes none. A fields-v9 rebuild, and so V7-F, cannot run through the driver (L9 is
  design only).
- The clean check is the whole repository (:936-941; run_bounded_research.py:91-93), and every bounded phase repeats
  it. v7.0 took three invocations because lane reports landed in the sprint dir (progress.md:153).
- `summ.dsr_n` must be edited and committed when another cell is ledgered first (:591-594).
- Version names are embedded in about 20 spec paths; v70 -> v71 differs in 222 lines.
- The JSON scoring is a second, hand-built command (L8 report, root command 5).
- fields, check, monitor and summ run "direct": no receipt, no timing.

*Change.*
1. `add-alpha --id X --dsl "..." --theme T --tier B --prior-sign 1 --citation "..." --parent v71 [--name v72]`:
   validates through the exe plan (P-1), writes the registry entry, the library file, a prereg stub, and a spec
   derived from the parent's spec by name templates; then `lock`.
2. `run --screen`: u (new members only, P-3) -> fit -> card -> gate; prints the admission rows. `run` continues with
   w -> nav -> summ and writes `cycle_verdict.json`: admission, paired dSR with SE and CI, DSR N, PBO, per-phase
   seconds and peak MiB.
3. Stage inputs as in the L9 design: `sec_identity_bridge`, `earnings_calendar`, `insider`, `sec_filings`,
   `thirteenf`, `ftd`, `regsho_threshold`, `security_master`, `short_volume_ext`, `reuse_fields`.
4. Clean check scoped by pathspec to code and spec paths (atx-core, atx-tsdb, atx-engine, atx-impl, scripts,
   CMake files); ignored dirty paths are listed in the receipt.
5. `dsr_n: "ledger+1"`; `ref` skipped when the fields manifest SHA equals the parent's.
6. Every phase runs through the bounded runner so each has a receipt.

*Effect.* One command from idea to verdict; about 50 s of compute with P-2, P-3 and P-4.
*Acceptance.* `add-alpha` for one of the four v7.1 members on parent v70 yields a library whose entry is byte-identical
to the committed one; `run` reproduces the v7.1-style admission row; a report file written under `.superpowers/`
mid-run does not stop the cycle, while an edit under atx-impl/ does.

### P-9 NAV replay throughput (M, M)

*Evidence.* No timer in any NAV source file (zero `steady_clock` uses), so 26.0 s cannot be attributed. Each decision
recomputes 2 logs x 253 sessions x 5,627 names (strategy_price_exposures.cpp:107-146): about 2.1 bn logs per replay.
Scenario books run in lockstep on one thread (strategy_nav_replay.cpp:1014-1112); construction variants do not, so
each theta, dust or rule variant is a separate 26-47 s process.

*Change.* (1) Stage timers in summary.json (load, hash, exposures, construction, books, write) first. (2) A per-session
log-return ring: each session logged once. (3) `--construction-grid` sharing the loaded inputs and exposures across
variants. (4) Books on a DetPool after the shared decision.
*Acceptance.* All ten daily and events files of the v7.1 cell byte-identical; timers sum to within 5% of wall.

### P-10 Python mirrors of C++ (M, M)

Four mirrors, each a silent-divergence risk and a second place to edit:

| mirror | Python | C++ source of truth |
|---|---|---|
| exposures, neutraliser (QR vs Cholesky) | fit_composition_weights.py:670-780 | strategy_price_exposures.cpp; strategy_target_replay.cpp |
| labels, return guard, rank IC | alpha_report_card.py (`runner_check` reports the gap) | atx-engine factory/ic_screen.cpp |
| DSL parser, lookback, slot estimate | generate_fund_ic_v4.py:230-318; v70:272-384 | atx-engine alpha parser / typecheck |
| operator and policy tables | check_fund_ic_v6.py | registry.cpp |

*Change.* `atx-equity-strategy-targets exposures --role ... --output DIR` writes the per-decision basis and forward
returns in the fitter's Context layout; the fitter and card read it. The parser and checker mirrors go with P-1.
*Acceptance.* Fitter factors from the exported basis equal today's to 1e-12 on TRAIN, then the Python path is deleted.

### P-11 Report seal check (L-M, S, owner gate)

*Evidence.* `FORBIDDEN` (mega_report/data.py:23) searches the whole relative path (:49) for `2023|2024|2025`. A v2
cache path holds about 96 hex characters, so about 0.4% of paths trip it; a role SHA containing such a run would
refuse every candidate. The pattern has no 2026, although 2026 sessions now exist.
*Change (owner decides).* Split the path on `/ . _ -`; drop components that are pure hex of length 16 or 64 (after
the `fp_` / `ic1_` prefix); on the rest, refuse any decimal run that starts with a year >= 2023, and keep the
validation / VAL rule. *Acceptance.* `fp_2e2025f0.../droe...` is read; `...-2023-2024/`, `x_20250131.csv`,
`nav-2026/` and `VAL` are refused; the v7 pitch re-renders with 0 unavailable blocks.

### P-12 End-to-end fixture, ctest, CI (M, M)

*Evidence.* research_cycle `run` tests use fake tools (test_research_cycle.py:177-320). Synthetic panels are built
separately in test_fit_composition_weights.py:164, test_alpha_report_card.py:114, test_prepare_research_fields.py:203
and the C++ fixtures. Only `atx-impl-tests` is registered with ctest (tests/CMakeLists.txt:54); the three strategy
exes (243 tests in strategy_*_test.cpp) are run by hand. The only workflow is atx-db.yml.

*Specification.*
- `scripts/tests/fixtures/tiny_world.py`: deterministic role of 448 dates x 64 names (score_begin 384, seeded
  returns with planted signals), 3 extra fields with a manifest, a 4-member library, a registry and a spec `tiny.json`.
- `scripts/tests/test_cycle_e2e.py`: runs `research_cycle.py run tiny.json --root <tmp>` with the real exes when
  `ATX_EQUITY_BIN` is set; asserts golden SHAs of orientations.json, admission.json and the primary daily CSV, and a
  second run as a no-op.
- Needs `--no-git` (test roots only) in research_cycle.py and run_bounded_research.py.
- Register the strategy exes with `gtest_discover_tests` under a label `atx_equity_strategy`.

*Acceptance.* The e2e test runs in under 15 s and gives the same SHAs from Debug and Release exes (the P-4 canary).

### P-13 File splits (L-M, M)

One large TU plus link costs 16 s, so compile time is not the case for a split. Lane disjointness is: in v7, L3 and L4
both needed strategy_nav_replay.cpp and L1 held strategy_ic_runner.cpp alone (20 and 16 commits in nine days).

| file | seams (current lines) |
|---|---|
| strategy_ic_runner.cpp | library + field plan 176-610; admission + fields binding 611-777; weights 778-1015; signal cache 1017-1526; field residency 1527-1720; IC-result cache 1721-1990; score_role 2013-2342; CLI 2343-2568 |
| strategy_nav_replay.cpp | accounting 398-720; decision + plan 722-936; run_books 1004-1112; summary JSON 1114-1556; CSV + load 1558-1833; scenarios 1835-1950; publish + holdings + CLI 2097-2650; nav_decide 2654- |
| stage_equity_ic.cpp | not on the research-exe path; leave |

Do each split as one move-only commit at the start of the owning lane. *Acceptance.* Identical exe outputs on the
fixture and the v7.1 cell; the `dsl_vm_sources` and `ic_result_sources` pins updated in the same commit.

### P-14 Where research state lives (M, S-M)

- `build-equity/` holds objects and research outputs together: trials.jsonl (the DSR N ledger), 11 cache roots (v71:
  2.4 GB), fields dirs (v9: 3.1 GB), every accepted cell, and mega-build.ps1. It is gitignored.
- nav_summ.py and backtest_integrity.py, run by every cycle, live under `.superpowers/sdd/mega-alpha-20260926/studies/`
  (specs/v71.json:310).
- Sources are absolute paths on one machine, one of them in a Downloads folder (specs/v70.json sources block).

*Change.* A research root outside the build dir (spec key `out_root`); the ledger copied into the sprint dir after
each cycle; the two scripts moved to atx-impl/tools/; a `sources.json` with SHA pins resolved by `lock`; a
`cache gc --keep-referenced-by <specs>` verb on top of the existing `--cache-report`.

### P-15 Small caps (L, S)

- `--workers` <= 4 (strategy_ic_runner.cpp:2348) on a 16-thread host.
- Generators enforce 314 prior bars (generate_fund_ic_v4.py:53); the runner's bound on this role is 336.
- Shape caps are repeated in five files (table in P-5).
- The 5-field and 7-slot budgets are generator rules. The runner charges the library maximum, so one wide member
  raises the memory of every pass.

## S4. Contradictions and corrections to the v7 review and ledger

| v7 statement | finding |
|---|---|
| A5: Release timing inconclusive | Stage timers separate it: CPU stages -29%, composition 3.2x faster; disk stages doubled under load |
| A6: date blocks with a lookback halo; StreamingEngine is bit-exact | True only with state carried from the same first date. A halo restart, or a longer role, changes `ResearchFast` bits |
| A6: a 2010-2022 role needs about 4 GiB | Holds at constant names; the union grows (builder default cap 8,000): about 5.4 GiB estimated |
| A6: the 5-field budget and the 314-bar bound are the same limit | Neither is a runner limit: 314 is a generator constant (runner: 336); the runner has no per-candidate field cap |
| "64-field limit, 63 used" | Two caps: manifest rows (63/64, one line) and the referenced-field mask (40/64, structural) |
| W5b F1-F2: reuse keys on the module's code sha | Builder groups use per-group AST closures; the SEC and holdings fields are never reused at all |
| S3 estimate after L1 + L2: about 1 min per cycle | Measured 134 s: fit and card reuse nothing, the u pass blends, a ref NAV was added |
| C: 93% full | 83% used, 78 GB free |

Confirmed still open from v7: A8 (NAV), C1 (mirrors), C3 (canary), C7 (no full-stack test), C4 (configure-time
provenance, atx-impl/CMakeLists.txt:105-134).

## S5. Proposed v8 platform lanes

Lanes own disjoint files. Contracts declared before dispatch: (i) the exe plan `candidates[]` rows (B writes, A
reads); (ii) the exposures layout (D writes, C reads in a follow-up); (iii) `--no-git` semantics (A writes, E uses).

| lane | findings | owns | depends on |
|---|---|---|---|
| A registry + add-alpha + cycle plumbing | P-1, P-8, P-14 (paths) | atx-impl/strategies/alphas/, libraries/, generate_library.py (+test); scripts/research_cycle.py, run_bounded_research.py, scripts/tests/test_research_cycle.py, scripts/specs/ | contract (i) |
| B incremental IC runner | P-3, P-6, P-15, P-13 (ic split), plan rows | atx-impl/src/strategy_ic_runner.{cpp,hpp}, strategy_ic_composition.{cpp,hpp}, tools/equity_strategy_ic.cpp, tests/strategy_ic_*_test.cpp | none |
| C reuse in the Python tools | P-2, P-7 | atx-impl/tools/fit_composition_weights.py, alpha_report_card.py (+tests); atx-engine/tools/prepare_research_fields.py, research_fields_sec.py, research_fields_holdings.py (+tests) | none |
| D NAV throughput + exposures verb | P-9, P-10 (C++ side), P-13 (nav split) | atx-impl/src/strategy_nav_replay.{cpp,hpp}, strategy_price_exposures.{cpp,hpp}, strategy_target_replay.{cpp,hpp}, tools/equity_strategy_targets.cpp, tests/strategy_{nav_replay,price_exposures,target_replay}_test.cpp | none |
| E build, fixture, CI (root or one lane) | P-4, P-12, P-11 (owner gate), P-14 (script moves) | atx-impl/CMakeLists.txt, atx-impl/tests/CMakeLists.txt, scripts/research-build.ps1, scripts/tests/fixtures/tiny_world.py, scripts/tests/test_cycle_e2e.py, atx-impl/tools/mega_report/data.py | contract (iii) |

**Order.** E's fixture and B's split commit first (they give every other lane an identity check). Then A, B, C, D in
parallel. Merge order B -> C -> A -> D -> E.

**History extension (P-5).** 5a (era shards) is a sixth, data-gated lane after A lands: it owns
atx-engine/tools/prepare_recent_research.py, nav_summ.py pooling and the `roles:` loop contract with A. 5b (date blocks)
waits for a measurement of `AuditExact` cold vm time on the fixture and E3.

**Trials.** Every item here is identity work on existing books: no new trial. P-5a's E1 and E2 cells are new data and
need a pre-registration before any IC read.
