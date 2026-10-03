# Wave 2: binding rulings and carried findings per lane (PM, from progress.md)

Base: the P9 integration head after wave-1 merges + P9-B0 (filled at dispatch). Pools (remap ruling): E2 17, A3 12,
S2 18, B2 14, D2 20, C2 15, AL-COMB 19, AL-SIG 13, T2 21 (fresh). Every lane: no TDD (implement, then the tests the
brief names); no C++ build; no real data; rules 1-10 of its brief. "Carried minor" = a deferred review finding in the
lane's files; fix it if it is in scope, or say in the report why not.

## All lanes
- E1-REUSE-a2: every P9 cell manifest pins exes (`lock --exes` -> `exes_sha256`); a reused output is refused only
  against that pin; inherited outputs are judged by the parent's pin.
- E1-STALE: a reader/bundle code change refuses a resumed in-flight wave (exit 3); never merge under a running wave.
- SEAL-ALLOW is a Phase-0 root ruling only; lanes never open data.

## E2 (pool 17; inherits E1's files, P14)
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
- Carried E1 minors: N4 exe notes ignore the pin rule / mislabelled (`wave_stage_util.py:110`); HostClaims lock failure
  -> runner-error, not retryable; `exes_sha256` inherited by child specs / add-alpha copies (later waves refuse after a
  rebuild); content digests move with nested time values; summ/bundle/book log + receipt row order varies under a
  budget; receipt `attempt` / `build_type` 1 / null for most runs.

## A3 (pool 12)
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
- Carried C1 minors: config check skips a null risk store + risk-target parameter ranges; adv-hold + capacity runs 5
  lockstep passes; K-P9-7 marks `aim_leverage` required while defaulting it; C1-SPO gtest pins recipe + holdings, not
  summary / extras directly.

## AL-COMB (pool 19)
- P17: rebase on D2's head before merge; three listed cross-lane edits.
- D1 minor: two pytests pin 13 themes -> the first new theme breaks them (coordinate with D2).
- Between wave 2 and the P9 cells: root runs the AL-COMB zero-trial diagnostic (lit §8 Q5).

## AL-SIG (pool 13)
- P18: build the 13F and Russell-index source readers (K-P9-2 style); A4 reuses the 13F reader.
- Root runs K1 `--plan-only` of every AL-SIG string on the fields build that will carry P9-S.

## T2 (pool 21, fresh lease)
- `briefs/brief-T2.md`; T2-GOLD; A1-C.
