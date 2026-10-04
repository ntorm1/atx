# Wave 2: binding rulings and carried findings per lane (PM, from progress.md)

A wave-2 dispatcher needs this file and `briefs/brief-<ID>.md` only. Where a brief and this file disagree, this file
wins (it carries the later ledger rulings; see "Brief text superseded" below). Every lane: no TDD (implement, then the
tests the brief names); no C++ build; no real data; rules 1-10 of its brief. "Carried minor" = a deferred review
finding in the lane's files; fix it if it is in scope, or say in the report why not.

## Lanes, pools, bases, slots

Base = the P9 integration head after the wave-1 merges + P9-B0 (root fills the SHA at dispatch), except SQL1 and SQL2
as stated. Pools follow the wave-2 remap ruling (pools 7 / 8 / 10 are not recovered; pool-16 holds foreign YARCH WIP and
is unavailable), COV-3 and SQL-7. Merge slots follow plan §3.2 as amended.

| lane | pool | base | brief | merge slot | state |
|---|---|---|---|---|---|
| E2 | 17 | wave-1 head + P9-B0 | `brief-E2.md` | 1 | to dispatch |
| A3 | 12 | wave-1 head + P9-B0 | `brief-A3.md` | 2 | to dispatch |
| S2 | 18 | wave-1 head + P9-B0 | `brief-S2.md` | 3 | to dispatch |
| B2 | 14 | wave-1 head + P9-B0 | `brief-B2.md` | 4 | to dispatch |
| D2 | 20 (D1's pool; planned 16) | wave-1 head + P9-B0 | `brief-D2.md` | 5 | to dispatch |
| C2 | 15 | wave-1 head + P9-B0 | `brief-C2.md` | 6 | to dispatch |
| AL-COMB | 19 | wave-1 head + P9-B0; rebases on D2's head before merge (P17) | `brief-AL-COMB.md` | 7 | to dispatch |
| AL-SIG | 13 (A2's pool; planned 20) | wave-1 head + P9-B0 | `brief-AL-SIG.md` | 8 | to dispatch |
| COV | 22, fresh lease, `-MaxPool 22` (COV-3) | wave-1 head + P9-B0 (C1, T1 merged) | `brief-COV.md`, section "Lane COV" | 9 | to dispatch |
| SQL1 | 23, fresh lease, `-MaxPool 23` | `20443022` (SQL1-EARLY) | `brief-SQL1.md` | 10, onto the wave-1 head (keep-both CMake tails or a rebase) | **running** (lane-sql1, since SQL1-EARLY) |
| SQL2 | 24, fresh lease, `-MaxPool 24` | SQL1's task-1 commit on top of the wave-1 head + P9-B0 (SQL-7, SQL1-EARLY) | `brief-SQL2.md` | 11, directly after SQL1 (rebased on it) | dispatch only after SQL1's task-1 commit (its API) lands |
| T2 | 21, fresh lease, `-MaxPool 21` | wave-1 head + P9-B0 | `brief-T2.md` | as the PM orders (test files only) | to dispatch |

SQL2's base needs both SQL1's task 1 and the wave-1 head, but SQL1's task-1 commit sits on `20443022`. Root builds that
combined base at dispatch and names its SHA.

## Brief text superseded (this file wins)

- `brief-COV.md` "Pool / branch" ("16 if the YARCH tree is released, else 7 or 8 after `-RecoverStale`, else a fresh
  lease with `-MaxPool 22`"): COV takes fresh pool-22 only (COV-3; pools 7 / 8 are not recovered).
- `brief-COV.md` heading "Lane COV-MV (optional ... only if the PM registers P9-MV)": COV-MV is funded (COV-7), wave
  3b. Not a wave-2 dispatch.
- `brief-SQL1.md` "base = the post-wave-1 P9-B0 head": the base is `20443022` (SQL1-EARLY). Its "Merge slot: after the
  plan's eight lanes (and T2 / COV as the PM orders)" becomes slot 10, after COV's 9. Its "Read: rulings
  SQL-1..SQL-9" extends to SQL-10, SQL-11, SQL-12 and SQL1-EARLY.
- `brief-SQL2.md` "(no real research cell uses it before SQL3's receipt block merges; ruling SQL-6, PQ-1)": SQL-11
  applies. No P9 cell uses a SQLite cache index; root's identity runs do, by direct argv, and root deletes the index
  after the run.
- Every brief's rule 2 lease line says `-MaxPool 20`. Pools 21-24 need `-MaxPool` set to their own number.

## All lanes

- E1-REUSE-a2: every P9 cell manifest pins exes (`lock --exes` -> `exes_sha256`); a reused output is refused only
  against that pin; inherited outputs are judged by the parent's pin.
- E1-STALE: a reader/bundle code change refuses a resumed in-flight wave (exit 3); never merge under a running wave.
- SEAL-ALLOW is a Phase-0 root ruling only; lanes never open data.
- **PY-HYG:** run pytest only with explicit suite or file paths. Never run bare `pytest` at a repo root: there is no
  root config, so it would walk `build-equity/`, `deps/`, `archive/` and `.superpowers/`. Before reporting, list your
  own python / pytest / atx child processes and kill any you left running.
- **M1a-RED field-flip freeze:** until A3's fix of the two `ResearchFields*` gtests (below) is merged and its root cause
  is known, no further field is flipped to `kind: engine` beyond `si_shares`, `si_dtc` and `vol_126`, the three whose
  TRAIN payloads were shown equal.
- **Wave-2 gate: 0 failed, with no known-red exception (M1a-RED).** The wave-1 gate let three pre-existing failures
  pass as named known-reds. They must be fixed in wave 2 (E2: the pytest; A3: the two gtests), each with its root cause
  stated and no expected value edited to fit. Root does not carry them past wave 2.
- CMake tails (P2): a lane that adds a test or source appends one line or block at the end of the owning list and
  lists it as a cross-lane edit; root keeps both sides of a tail conflict. Wave 2 has many appenders on
  `atx-engine/CMakeLists.txt`, `atx-engine/tests/CMakeLists.txt` and `atx-impl/tests/CMakeLists.txt`.

## E2 (pool 17; inherits E1's files, P14)
- **M1a-RED (required):** fix `scripts/tests/test_research_mine.py::test_fields_are_the_rule_applied_to_the_registry`,
  with its root cause stated in the report and no expected value edited to fit. It fails at `:119`
  (`assert set(EXCLUDED_BY_CLASS) <= set(v9) - read`; extra names `noa_lag4`, `ea_window_pre5`, `ins_cluster_buy`).
  Root's diagnosis (`root-wave1-merge-report.md` "Two pre-existing failures"): v8 commit `a5914373` added registry
  alphas (`atx-impl/strategies/alphas/registry.json`) that read those three fields, while the test's
  `EXCLUDED_BY_CLASS` still lists them as unread. The test pins prereg item 4 (the mined-fields rule). Any change to
  the constant must be derived from that rule, not from the failing set. If the fix needs a rule decision, stop and
  ask the PM.
- P14: E2 owns `wave_*.py` etc.; its `backtest_integrity.py` edits are cross-lane vs B2; kind fixtures 8 runnable + 2
  schema-only until wave 3.
- P9: retire the Python PM7-35 copy (`wave_rules.py:50-51`) after identity vs B1's C++ predicate. B1 note: under the P9
  default, `gate_counts` != `research_cycle.py:1420` gate -- runner-sign-0 admitted strings start counting when unified.
- D1-PSCHEMA: validate wave-manifest rule `params` against `params_schema` read from the exe's `--list-rules --json`
  capabilities (D2 corrects the schema in parallel).
- D1 deviation: E2 passes `--theme-registry` (built-in 13-theme default until then); D1 minor: fitter early theme-order
  refusal is vacuous until E2 (`fit_composition_weights.py:468,480`).
- **E1-N3 (required):** with pinned exes, a NAV-only cell two template levels down refuses its parent's legitimately
  reused output (`research_cycle.py:918-936` looks one level up) -> walk the whole template chain; test 3 levels.
- R0-7-EOL follow-up: add `.gitattributes` `scripts/specs/** eol=lf` (blobs unchanged; root re-checks pins after).
- A2 note: seal rule now in Python (A1) and C++ (A2) -> G-P5 allowlist row (with T1's guard).
- SQL-11: no spec key carries the SQLite index flags (`--candidate-cache-index`, `--pair-cache-index`) in P9. The keys
  and their `REUSE_NEUTRAL` status are P10. SQL3 (wave 3) takes over the writer files E2 leaves (plan §2.2 SQL3 row).
  If E2 moves a record writer into `scripts/cycle/`, say which file in the report.
- Carried E1 minors: N4 exe notes ignore the pin rule / mislabelled (`wave_stage_util.py:110`); HostClaims lock failure
  -> runner-error, not retryable; `exes_sha256` inherited by child specs / add-alpha copies (later waves refuse after a
  rebuild); content digests move with nested time values; summ/bundle/book log + receipt row order varies under a
  budget; receipt `attempt` / `build_type` 1 / null for most runs.

## A3 (pool 12)
- **M1a-RED (required):** fix the gtests `ResearchFieldsWriter.QuantilesPartitionLikeNumpy` and
  `ResearchFieldsVolumeMean.SumOrderIsNumpys` in `atx-engine-research-fields-tests`, with the root cause stated in the
  report and no expected value edited to fit. Root's notes (`root-wave1-merge-report.md` "(2) gtest"):
  - Quantiles fails at `research_fields_writer_test.cpp:139`, five times: `same_bits(x, 0.0)` is false for the
    quantiles of `{-0.0}`.
  - VolumeMean fails at `research_fields_volume_mean_test.cpp:120/133/135`: it gets 5e15, 1.25e15, 1.25e15 where the
    test expects 1/3, 0, 1/9. `trailing_mean.cpp:57` (`accepted = present && isfinite(v) && v >= 0.0`, since
    `eccf6338`) rejects the test's -1e16 as a negative volume, while the test expects negatives accepted.
  - Both are v8 code that no merge touched. The exe was first registered in CTest by T1.
  Decide whether the test or the code is wrong against numpy and the field's spec, and say why. These files are in
  scope by M1a-RED.
- **Field-flip freeze (M1a-RED):** none of A3's price / ohlc builder kinds is flipped to `kind: engine` in the registry
  ahead of that fix. Put the fix in its own commit, before any registry flip.
- A1-SHIM: deprecated field shims retire in wave 2 (route through the `--registry` entry).
- P13: refusing an absent seal is enabled only after root confirms every live manifest has a seal block.
- A1 merge state: `regenerate()` is the registry round-trip; engine-only rows (DEC-5) need no test edit.
- Carried A1 minors: `--engine-exe` silently computes un-routable engine twins in Python; freeze test checks file names
  only; Python accepts any `first_session` string while A2's C++ reader requires YYYY-MM-DD; report's root-diff omission
  (source_checks.v9 earn_season_rank).
- Carried A2 minors: `research_fields_cli.hpp:20` 115 cols; dev-shared reuse hashes exe not DLL (latent); TRAIN alt
  check "differs only in producer" wrong vs v15; test pins leftover `.pending`; no test for receipt-before-manifest;
  `strategy_live.cpp:424` needs top-level `code_sha256` absent from registry manifests.

## S2 (pool 18)
- P16: scope adds `atx-engine/src/alpha/*.cpp` + IC runner / cache / admission files (cross-lane vs D-lane files
  declared at dispatch); survival-label source = the role builder's imputed close.
- P13: C++ seal consumer, IC half. S1-PIN: S2 now owns `ic_screen.{hpp,cpp}` (S1's cross-lane edit accepted).
- Carried S1 minors: pair-cache source pin covers engine files only; `--verified-digests` can write unverified pairs to
  the persistent cache; pair cache loaded before the memory check + memory under-counted; `--exclude-self` uses a
  reconstructed composite; weak tests (tautological identity, counter-only digests, unreplicated >0.2); Release preset /
  verify list omit `atx-engine-w1-foundation-tests`; Release identity dir +41 chars near the 259 path limit; test-only
  `cache_identity` "must not be legacy" unenforced (`strategy_marginal_ic.hpp:69-72`); unowned comment
  `research_ic_fitness.cpp` says <= 11 regressors (now 34).

## B2 (pool 14)
- **Chain-head function (SQL-7, required):** `research/ledger` exposes a public C++ function. It takes the trial
  ledger's line texts in file order and returns the chain head under the existing chain rule, plus a name for that rule
  (SQL4 stores it as `ledger_state.head_rule`). It must work on any prefix: SQL4's `verify --ledger` checks that every
  head recorded in a verdict or wave result is the head of some prefix. It reuses the one chain rule
  `research/ledger` implements, never a second copy (G-P5). SQL2's catalog and SQL4's `ingest_ledger.cpp` call it and
  never re-implement it.
- SQL-9: the trial ledger stays hash-chained JSONL as the authority through P9. Nothing in B2 changes ledger bytes or
  the append format.
- P11: B2 writes the C++ gtests asserting T1's statistics tie fixture (`eval_tie/`), tolerance per T1's statement; G-P7.
- T1 minor: the eval_tie checker imports the Python statistics B2 deletes and no suite runs it -> keep the checker
  runnable (frozen copy or retarget) when deleting.
- K-P9-4a (B1's shipped factors bytes); P14 (`backtest_integrity.py` cross-lane with E2).
- Carried B1 minors: screen matches factor series by id only (no payload SHA / horizon check); `--max-bytes` omits
  used-row index + output table; `compare_admission_csv` ignores extra / missing cells; comparator re-types fitter table
  assembly; two public fns lack input-structure checks.

## D2 (pool 20)
- K-P9-4a: code against B1's shipped `factors` bytes (`factor.f64`, `tau.f64`, `factor_h21.f64`, `manifest.json`).
- D1-PSCHEMA: `params_schema` must describe K-P9-9 wave-manifest rule `params`; re-pin the fixture.
- P8 (group_rerank helper), P19 (`atx.dsl-composition-weights/v3`), CM-5 (fitter SHA out of output bytes, re-pin).
- P17: AL-COMB rebases on D2's head (merge slot after D2).
- Carried D1 minors: two_speed "not a theme" refusal untested (`strategy_ic_two_speed.cpp:42-44`); `--list-rules`
  matched anywhere in argv (`runner.cpp:811-819`); over-cap `--plan-only` prints a reloadable K1 plan
  (`runner.cpp:676-727`); two pytests pin today's 13 themes (`test_fit_composition_rule_plugins.py:157`,
  `test_composition_resid.py:268`).

## C2 (pool 15)
- P15: in-process calibration behind a wave-manifest key; absent = today's Python match stage.
- C1 merge state: per-book leverage, capacity lockstep, summary binds extras, exe identity in `summary.producer`
  (C1-PROD).
- COV shares the `atx-impl-strategy-target-tests` source list in `atx-impl/tests/CMakeLists.txt` (two lines; root
  resolves the text). COV owns `strategy_risk_model.*`, `strategy_risk_verb.cpp`, `tools/equity_strategy_risk.cpp` and
  the RiskStore section of `strategy_spo.*` in wave 2. C2 does not edit them.
- Carried C1 minors: config check skips a null risk store + risk-target parameter ranges (stays C2's; COV only
  guarantees `RiskStore::open` refuses a container whose root is not the manifest's); adv-hold + capacity runs 5
  lockstep passes; K-P9-7 marks `aim_leverage` required while defaulting it; C1-SPO gtest pins recipe + holdings, not
  summary / extras directly.

## AL-COMB (pool 19)
- P17: rebase on D2's head before merge; three listed cross-lane edits.
- D1 minor: two pytests pin 13 themes -> the first new theme breaks them (coordinate with D2).
- Between wave 2 and the P9 cells: root runs the AL-COMB zero-trial diagnostic (lit §8 Q5). It may read either risk
  store (K-P9-12).

## AL-SIG (pool 13)
- P18: build the 13F and Russell-index source readers (K-P9-2 style); A4 reuses the 13F reader.
- Root runs K1 `--plan-only` of every AL-SIG string on the fields build that will carry P9-S.

## COV (pool-22, fresh; slot 9)
- COV-1 / COV-2 / COV-5: 0 trials. Flag-absent is byte-identical (default recipe `atx-risk-v1.1`, no container,
  `--bias-families v1`). Every `atx-cov-v1` parameter (a = 1, 64 simulations, seed 7, half-life 504) is fixed in the
  brief and not changed after D-COV. A change is a new recipe id and a P10 note.
- COV-3: owns `strategy_risk_model.*`, `strategy_risk_verb.cpp`, `tools/equity_strategy_risk.cpp` and the RiskStore
  section of `strategy_spo.*` for wave 2; C3 inherits `strategy_spo.*` in wave 3. Contract K-P9-12 (`.atxcov`).
- **COV-4 (for the lane and its reviewer):** keep the as-of rule as today's consumers have it: block t uses sessions
  <= t, and a decision at d reads block d. The design's claim that no d-2 lag is needed is not accepted on the
  designer's word. The COV reviewer must trace, in the role builder and NAV replay, when a decision at d is formed and
  first traded, and show that block d holds nothing from after that point. The truncation tests
  (`CovTruncation.*`: delete / perturb every session > t, block t byte-identical) and the planted-leak probe are
  required.
- Release: a Release build of the `atx-cov-v1` producer is decided only if root measures the Debug run over 600 s, and
  then needs Release-vs-Debug byte identity on the synthetic fixture first.
- Cross-lane: one source line in `atx-engine/CMakeLists.txt`; two lines in `atx-impl/tests/CMakeLists.txt` (C2 shares
  the list).
- C1 merge state: `BookScaler::estimate` reads `RiskStore::read(d)`; reuse C1's vol-target replay fixtures for the
  identity test; never edit C1 / C2 files.
- Carried C1 minor in scope: "config check skips a null risk store" stays C2's; COV only guarantees `RiskStore::open`
  refuses a container whose root is not the manifest's.
- After the merge, root runs D-COV (plan §3.4 step 6), which prints P9-V's registered predictions. P9-V (COV-6) and
  P9-MV (COV-7) are registered cells, with N_c <= 71 and N_tot <= 252 (COV-8).

## SQL1 (pool-23; running since SQL1-EARLY)
- SQL1-EARLY: coded from `20443022` before wave 1 closed. It merges in wave 2 at slot 10 onto the wave-1 head, keeping
  both sides of CMake tail conflicts or rebasing. Root reviews it like any lane.
- SQL-4: no new SQLite dependency. Link the vendored `atx_sqlite3`, extend `atx/core/db` and fix its defects (W1-W8) in
  place. SQL1 owns `atx-core/src/db/sqlite.{cpp,hpp}` and `atx-engine/tools/record_store.py`. It never touches
  `vcpkg.json`, `CMakePresets.json` or any dependency line.
- SQL-5: table descriptions are compile-time C++ (DDL, bind / read, digest, schema JSON from one `constexpr`
  descriptor). There is no generated source file, no code-gen step and no Python generator. Python gets one generic
  accessor.
- SQL-10: Python reads the schema from each store's `store_info('schema_json')` row, not from a subprocess.
- SQL-6: the record store selects its index by file presence, which is allowed only because consumer bytes are
  identical either way. SQL-11: no P9 cell runs with a SQLite cache index. Only root's identity runs use one.
- SQL-2: JSON stays the authority for every pinned or chained artifact through P9.
- SQL-7: task 1 (descriptor machinery, core and cache descriptors, schema fixtures) is committed first. It is SQL2's
  base. Report its SHA to the PM as soon as it exists.

## SQL2 (pool-24; after SQL1's task 1)
- SQL-7: the base is SQL1's task-1 commit on top of the wave-1 head (see the lane table). SHA-256 values are recorded as
  `verified` (hashed by the catalog) or `declared` (stated by a manifest, file not opened). The catalog never opens
  sealed or large payloads, and it never follows `field-source` pins. The real-tree ingest is a root-only bounded run.
- SQL-9: ledger lines are ingested as an index. The chain head comes only from B2's chain-head function (SQL4 wires
  it); the catalog never re-implements the chain rule.
- SQL-11: SQL1's X-5 opt-in identity (index cold and warm) is root's run. Root deletes the index afterwards, and no P9
  cell uses it.
- SQL-8: G-P10 is printed at the freeze and gates nothing.
- SQL-4 / SQL-5 as for SQL1. Forbidden: `scripts/**`, every atx-impl file, `research_ledger.py`,
  `backtest_integrity.py`.

## T2 (pool 21, fresh lease)
- `briefs/brief-T2.md`; T2-GOLD; A1-C. Test files and fixtures only. Its gate is the `atx-engine/tools` suite at 0
  failed in one process under `PYTHONHASHSEED` 0 and 1, run with explicit paths (PY-HYG).
