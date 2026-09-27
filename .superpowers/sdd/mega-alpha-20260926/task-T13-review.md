# Task T13 review — composition variant `mv-shrink-0.9-nonneg-netcost-v1`

Commit reviewed: `d38e7929` (pool-3, `feat(impl): netcost composition variant for the weight fit [mega T13]`).
Diff read from `review-T13.diff` (287 lines, single commit, two files). To resolve two concrete named
risks I read the live `atx-impl/tools/fit_composition_weights.py` at `C:/atx-wt/pool-3/...` for context
outside the diff (unchanged surrounding code) and `atx-impl/src/strategy_ic_runner.cpp` for the IC
runner's parser — see "Checks run" below. No commands executed, nothing else touched.

### Spec Compliance

- ✅ Spec compliant.
  - `mu_k = mean_TRAIN(s_k f_k) - c*tau_k`, `c = 0.0018`: `fit_weights()` computes `mu = factors.mean(axis=1)`
    (unchanged) then `mu - cost_drag` only when `cost_drag is not None`
    (fit_composition_weights.py:78-80 in the diff). `drag = NETCOST_C * taus[k] for k in active`
    (line 1073 / diff hunk `@@ -1049,49...`). Verified by hand against the new fixture (below).
  - Covariance/shrink/clip/normalize unchanged: `cov = np.atleast_2d(np.cov(factors, ddof=1))` still
    runs on the *original* (non-net) `factors`; `shrink_solution` (0.1*S+0.9*diag(S)) is untouched;
    clip-to-nonneg and `/= sum` are untouched (diff hunk starting `def fit_weights`).
  - Admission screen v3-admit-v1 and signs unchanged / cost enters only the weight fit: `drag` is
    computed after `eligible`/`active` are already fixed and is passed only into `fit_weights`; no
    admission/orientation/sign code is touched anywhere in the diff. The new test
    `test_netcost_changes_only_the_fit` directly checks this: admission.json/.csv identical between
    default and netcost runs, and `net["signs"] == gross["signs"]`.
  - `--composition` flag, old method default, byte-identical default output: `p.add_argument("--composition",
    default=RULE_ID, choices=COMPOSITIONS, ...)`. `provenance["rule"]` is now `args.composition`
    instead of the hardcoded `RULE_ID` constant, but on the default path that string is identical, and
    the `netcost_c`/`netcost_mu` keys are added only `if netcost:` — so the default-path document is
    structurally unchanged. Confirmed by the new test comparing byte-for-byte `default` vs. an explicit
    `composition=fcw.RULE_ID` run.
  - Method id + c recorded in provenance: `provenance.update(netcost_c=NETCOST_C, netcost_mu="...")`
    plus `provenance["rule"] = args.composition` (= `"mv-shrink-0.9-nonneg-netcost-v1"`).
  - Hand-computed 3-candidate fixture: `test_hand_case_cost_changes_the_weights` — re-derived it myself:
    e1/e2/e3 are mutually orthogonal ±1 patterns of equal norm, so S is diagonal with equal entries and
    Sh=S, giving `w_raw ∝ mu`. Gross mu=[.003,.002,.001] → [1/2,1/3,1/6] ✓. Net mu = mu − 0.0018·[.5,1.5,.2]
    = [.0021, −.0007, .00064] → clip the negative middle → [.0021/.00274, 0, .00064/.00274] ✓. Matches
    the test's asserted values exactly.

- ⚠️ Cannot verify from diff alone: the report's claim of a byte-diff against a real prior artifact
  (`1507a2d8`, "admission.csv byte-identical, JSON differs only in script_sha256-derived lines") is an
  out-of-band comparison I can't reproduce from this diff. The code-level argument above (provenance.rule
  unchanged on default path, no new keys added) already establishes byte-identity independent of that
  claim, and `script_sha256` is expected to change whenever the script's bytes change — routine, not a
  defect.

### Checks run (named risks outside the diff)

1. **Risk:** unit mismatch — does `taus[k]` used for `c*tau_k` match the "declared per-alpha turnover"
   (one-way, per-day, gross-1 book) used elsewhere for the 0.70 admission limit, or some other quantity?
   **Check:** `taus = [r["tau"] for r in records]` and `records[...]["tau"] = standalone_turnover(q)` is
   the exact same field read for `tau_over_limit`/`TAU_LIMIT` in the admission screen — same units, one
   source of truth. No mismatch.
2. **Risk:** the new/renamed provenance keys (`rule` value change, `netcost_c`, `netcost_mu`) could break
   the IC runner's strict JSON parsing of the weights file (constraint: "IC runner must still accept the
   weights JSON"). **Check:** read `composition_weights()` in `atx-impl/src/strategy_ic_runner.cpp:638`;
   its own comment states "Unknown keys are allowed," and the parser only validates `schema`,
   `library_sha256`, `weights` (finite, >=0, known ids), `train_manifest_sha256`, and `signs` — it never
   touches `provenance`. No risk.

### Strengths

- The netcost path is minimal and surgical: one optional parameter threaded through `fit_weights`, one
  boolean derived from the CLI flag, and additions gated strictly behind `if netcost:` so the default
  path's document construction is provably unchanged by inspection, not just by test.
- `test_hand_case_cost_changes_the_weights` is a genuine, independently-checkable hand computation (not
  a tautological re-implementation of the production formula), and it asserts the sign flip
  (`raw[1] < 0`) that makes the nonneg clip meaningful for this case.
- `test_netcost_changes_only_the_fit` is thorough: proves default==explicit-default (flag doesn't change
  default behavior), default==net for admission (cost never touches the screen), signs equality, absence
  of netcost keys on the gross path, presence and correctness of `cost_drag` per admitted candidate on
  the net path, and that the two composition's weights actually differ. It also confirms the incremental
  work cache is shared (`computed_this_run == 0` on the third run).
- Docstring, help text, and provenance comment all state the exact formula and the pre-registration
  timing (before any v3 measurement) — good selection-hygiene paper trail.

### Issues

#### Critical (Must Fix)
None.

#### Important (Should Fix)
None.

#### Minor (Nice to Have)
- `fit_composition_weights.py` docstring hunk (diff lines ~20-23): the netcost sentence is inserted
  mid-paragraph between "w /= sum(w)." and "Only admitted candidates...", so the two unrelated sentences
  now run together without a paragraph break. Purely a readability nit.
- No test exercises `--composition mv-shrink-0.9-nonneg-netcost-v1` together with `--screen none` (the
  T9 all-runner-oriented path); the docstring explicitly says composition applies to both screen modes,
  but only `v3-admit-v1` is exercised for netcost. Not required by the brief, but would close a coverage
  gap for the module's other supported mode.
- The pre-existing `runner_accepts(...)` (Python port of the C++ IC-runner parser, used elsewhere in
  this test file) is never run against a netcost-composition weights JSON. I independently confirmed via
  `strategy_ic_runner.cpp` that provenance is ignored so this is very low risk, but an explicit
  `runner_accepts` call on the netcost output would make that guarantee self-verifying in this file too.

### Assessment

**Task quality:** Approved

**Reasoning:** The change implements exactly the specified formula with the smallest possible surface
area, leaves the admission/sign/covariance/normalization code paths untouched, keeps the default output
byte-identical by construction (not just by test), and is backed by a correctly hand-verified 3-candidate
fixture plus a thorough integration test. No correctness, safety-rule, or spec-compliance issues found.
