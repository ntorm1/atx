# Lane B1 review

## Verdict
APPROVE

0 blocker, 0 major, 9 minor. Spec compliance: ✅

All three deliverables meet the brief, K-P9-4 and Rulings P1, P2, P9 and P12:
- the `factors` verb;
- `research/admission`: screen_v4 on `eval::hac`, the greedy pass, the PM7-35 predicate and the report-only traded
  horizon;
- the comparator pytest.

The C++ reads clean under clang-cl 18 `/W4 /permissive- /WX`. I found no unused names, sign mismatches, narrowing
brace inits, missing includes, `[[nodiscard]]` drops or lines over 100 columns in any new `.cpp` or `.hpp`.
- Each new `switch` covers every enumerator.
- Each anonymous-namespace constant and helper is used.

No existing TU or header changes. The dispatch adds one exact-match branch for `argv[1] == "factors"`, so every
existing verb keeps its bytes when the flag is absent. Every finding below is minor and none blocks the merge.

## Reviewed SHA
`01f20405c61bfdcfb3d3ec2141c39821fff860e6` (base `d7c1c520`). The tree was `C:/atx-wt/pool-14` on branch
`feat/p9-b1-20261003`, and HEAD was this SHA. `git status --porcelain` was empty before and after.

## Evidence
I ran the lane's pytest in pool-14 with `PYTHONDONTWRITEBYTECODE=1` and `-p no:cacheprovider`. It wrote no file.

```
"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider -rs atx-impl/tools/test_factor_series_admission.py
exit_code=0
.......s                                                                 [100%]
=========================== short test summary info ===========================
SKIPPED [1] atx-impl\tools\test_factor_series_admission.py:481: needs built atx-equity-strategy-targets and atx-research-admission
7 passed, 1 skipped in 0.70s
```

I also wrote two independent read-only checks. The scripts live in my scratchpad, outside every tree, and read the
committed fixture.
- `screen_spot.py`: my own transcription of `screen.cpp`, with sequential sums, the n <= lag branch and the greedy
  pass, run on `screen_v4_v1.json` against the fitter rows stored there.
  - exit_code=0: `decision mismatches 0 worst hac_t rel diff 8.861344385406817e-16`.
  - Both screens agree: every status, failed check, rank, `redundant_with`, `low_overlap_with` and
    `undefined_rho_with`.
- `repr_spot.py`: the placement logic of `python_float_repr`, fed numpy's shortest digits (unique, the `to_chars`
  analogue), checked against `repr()` on random bit patterns and edge cases.
  - exit_code=0: `checked 179939 mismatches 0`.

## Findings
path:line | severity | problem | required fix

- m1. `atx-engine/src/research/admission/admission_cli.cpp:247-253`, `:340-342` | minor | Candidates and the
  K-P9-4 series are matched by id and order only:
  - `read_factor_series` reads only the ids.
  - Nothing compares `cache_payload_sha256` with the factor manifest's `candidates[k].payload_sha256`.
  - Nothing checks `traded_horizon_sessions` or the `contract` and semantics keys.

  A candidates file from another library version with the same ids would screen without error. `admission.csv` would
  then print statistics under the wrong payload SHA. | Refuse when `meta[k].cache_payload_sha256` differs from the
  manifest's `payload_sha256`. Check `traded_horizon_sessions == kTradedHorizonSessions`. Add a refusal case to
  `CliScreensAFactorSeries`.
- m2. `atx-impl/src/strategy_factors_verb.cpp:300-306` | minor | The working-set charge, `cells x 57 B` + ring + 16
  MiB, leaves out two allocations, so `--max-bytes` is not an upper bound:
  - `ctx.rows`, 8 B per used row;
  - the `FactorTable`, 16 B per decision x candidate. At `kMaxCandidates = 20000` and about 1,000 decisions that is
    about 320 MB.

  | Add `8 x cells` and `16 x decisions x candidates` to `extra`. The index is already read before the role loads.
- m3. `atx-impl/src/strategy_factors_verb.cpp:23`, `:142`; report "Cross-lane edits" | minor | The report names only
  D1. `load_pinned_f64` is declared in D1's `strategy_ic_detail.hpp` but defined in S1-owned
  `strategy_ic_signal_cache.cpp:359`, and S1 edits that TU in wave 1. The TU also includes D1's header, which pulls
  in `strategy_ic_composition.hpp` and `strategy_ic_runner.hpp`. Both are D1's. | List S1 as a dependency as well.
  At each D1 and S1 merge, root rebuilds `atx-equity-strategy-targets` and reruns `FactorsVerb.*`.
- m4. `atx-impl/tools/test_factor_series_admission.py:399-401` | minor | In `compare_admission_csv`,
  `zip(header, ca, cb)` drops trailing cells when a row's cell count differs from the header's. That is the check root
  runs on X-5 and Y-S, and it could miss an extra or missing cell. | Report a difference whenever `len(ca)` or
  `len(cb)` differs from `len(header)`.
- m5. `atx-impl/src/strategy_factors_verb.hpp:8-42`; `admission_cli.cpp:350` | minor | K-P9-4 as coded differs from
  plan §2.3:
  - argv adds `--role-sha256`;
  - `DIR` is specified as `DIR/signals.json` (`atx.factor-signals/v1`);
  - the output adds `factor_h21.f64`, and the screen requires it, so a K-P9-4 directory without it is refused;
  - the fitter record's `f_theta_unsigned` is not carried, and the fitter's `--report-f-theta` path depends on it.

  The first three are justified, but D2 depends on all four. | The PM amends the K-P9-4 text before D2. Alternatively,
  the screen treats `factor_h21.f64` as optional and skips `traded_horizon.csv` when it is absent. Record
  `f_theta_unsigned` as out of K-P9-4 for D2.
- m6. `atx-engine/include/atx/engine/research/admission/sign_rule.hpp:33`, `:74-77` | minor | The default
  `WaveZeroKeptV1` is right per P9. But `gate_counts(kPm735SignRule, ...)` does not reproduce today's gate:
  - `research_cycle.py:1420` counts an admitted string only when `sign_agrees` holds;
  - the gate also has a `sign_agrees: false` option, which the predicate cannot express.

  When E2 moves the gate onto this predicate, an admitted string with runner sign 0 starts to count. | The PM records
  this as E2's identity substitution, or rules that the gate keeps `GatePriorV1`. `gate_counts` gains the gate's
  `sign_agrees` switch before E2 uses it.
- m7. `atx-impl/tools/test_factor_series_admission.py:224-238` | minor | `table_rows` re-types the fitter's
  admission-table assembly (fit:2249-2259) instead of calling it. If the fitter changes that assembly, the fixture's
  CSV golden would not move. Root's real-data `compare_admission_csv` against the fitter's own `admission.csv` is the
  backstop. | State in the docstring that the CSV golden pins only `screen_v4` and `admission_csv`, or call a factored
  helper when D2 opens the fitter.
- m8. `atx-impl/src/strategy_factors_verb.cpp:222-225`; `atx-engine/include/atx/engine/research/admission/screen.hpp:153`
  | minor | Two public entry points leave structural preconditions unchecked:
  - `factor_series` checks `row_begin.size()` but not `row_begin.back() == rows.size()`,
    `basis.size() == rows.size() x 4` or `rows[r] < instruments`. A hand-built `FactorContext` (a public aggregate)
    reads out of bounds.
  - `pair_correlation` (noexcept) indexes `a` and `b` without a check.

  | Return `InvalidArgument` in `factor_series` for a malformed context. Assert `a, b < candidates` in
  `pair_correlation`.
- m9. Report "How root verifies" step 2 | minor | The report says `ResearchAdmission.*` runs 17 tests. The three files
  hold 16 (7 + 4 + 5); `FactorsVerb.*` is 4. | Correct the count so root's expected-count check matches.

## Checked
- [x] `.agents/cpp/agent.md` §10 applied to the diff. No UB, narrowing or uninitialised state found, and lifetimes are
  sound:
  - `ScreenInput` spans and the `PriceExposureInput` temporary are borrowed only for the call.
  - The ring binds to role or fixture storage that outlives it.
  - `ATX_TRY` is used only with declarations and member targets.

  Each error path returns a `Result`, and the verbs refuse before publishing; an IO error after the directory is
  created leaves no manifest, as manifest-last intends. There is no concurrency, and every sort has a strict
  tie-break, so the output is deterministic.
- [x] The diff stays in scope:
  - new `research/admission/**` and its tests and fixtures;
  - new `strategy_factors_verb.{hpp,cpp}` and its test and fixture;
  - the one dispatch line;
  - one appended block in each CMake list. The two test lists are T1's, edited as Ruling P2 allows and declared in
    the report.

  The fitter and `strategy_ic_admission.cpp` are untouched. The read-only include of D1's `strategy_ic_detail.hpp` is
  declared (see m3).
- [x] The evidence in the report matches its claims. The pytest result reproduces, and the two independent checks
  above agree with the report's emulation numbers.
- [x] K-P9-4 semantics:
  - Decisions are `[score_begin, score_end - 2)`, as in `fit:623`.
  - q is centred tied ranks over used names with a finite signal, using `group_rerank.hpp`, whose fewer-than-2 case
    matches the fitter's. It is projected out twice on the K2 basis and divided by gross when live (> 1e-9 x entry).
  - f(d) = sum over used names i of q_i(d) r_i(d+2). The return is that of interval (d+1, d+2], from `export_decision`,
    with NaN mapped to 0, as `fit:1069-1070` does.
  - tau averages over all consecutive decisions, flat ones included, as `standalone_turnover` does.
  - Tolerance is 1e-12, because numpy reduces pairwise and its QR is not K2's Householder; the lane states this.
- [x] Look-ahead:
  - The basis at d reads sessions <= d, the label is r(d+2) and the signal row is d.
  - h21 sums forward rows d .. d+20 (intervals d+2 .. d+22) and is NaN past `score_end - 1`, so nothing at or after
    the seal is read.
  - The TRAIN mask uses the decision session, as `fit:2241` does.
  - The HAC runs over the compacted live TRAIN series, as in the fitter, with lag 5, Bartlett weights, divisor n and
    no correction.
  - `mean_inference` reproduces `newey_west_t` term for term when n > L. When n <= L, the lane's branch keeps the
    declared L, as the fitter does.
  - `FutureReturnDoesNotChangePast` has teeth for any label read later than d+2. A basis read up to d+2 is caught
    instead by `EqualsFixture` against the fitter's own Context.
- [x] P12: decisions and order are byte-equal and floats tie at 1e-12, relative above 1, in the gtests and in
  `compare_admission_csv`.
- [x] P9: `kPm735SignRule = WaveZeroKeptV1`, which is today's wave rule. `pm7_35` reproduces `wave_rules.sign_pm7_35`
  case for case.
- [x] The lane's additions beyond the brief:
  - `--role-sha256` is the same external pin K2 takes.
  - `signals.json`: K-P9-4 leaves the format of `DIR` open, and the index fixes the candidate order.
  - `factor_h21.f64` is report-only, as Ruling B1-H allows.
  - The `atx-research-admission` exe sits in owned paths and is how root runs the byte check.
  - v4-prior-v2 is one check, pinned to the fitter fixture. The X specs use v4-prior-v1.
  - The private Debug /O2 on the factors TU copies the IC TUs' idiom and changes no other TU.

  None of these changes identity. The contract-text drift is m5.
- [x] The comparator is not a tautology. Both fixtures are regenerated from the fitter's own `Context.build`,
  `factor_record`, `screen_v4` and `admission_csv` and compared float-exactly with the committed files, so they would
  catch fitter drift or a hand edit. The C++ is tied to those fixtures by the gtests. One part is self-referential:
  the h21 golden is a lane-written numpy reference, which the docstring says. It is report-only.
- [x] Blindness held. I opened only synthetic fixtures, specs, manifests and code: no 2020-2023 return, IC, Sharpe or
  NAV output, and nothing dated 2024-01-01 or later.
