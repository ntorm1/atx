# Task C-2 report: report-only columns at the traded horizon

Lane C, branch `feat/platform-v8-c-20260929` (pool-9). Status: DONE.

## What was built

- `atx-impl/tools/horizon_stats.py` (new, numpy only, importable by the diagnostics lane):
  `HORIZON_THETA = 0.05` (equals the fitter's `AIM_THETA`, the reference construction theta),
  `IC_THETA_HORIZONS = (1..63)`, `theta_weights(horizons, theta) -> ndarray` (theta (1-theta)^(h-1)),
  `ic_theta(m, horizons, theta) -> float | None` (sum over h = 1..63 of theta (1-theta)^(h-1) m(h), truncated, not
  renormalised; None when m has another length or any m(h) is undefined),
  `theta_book_returns(q, forward, theta) -> (f_theta, live)` (b(d) = (1-theta) b(d-1) + theta q(d) from a flat start,
  the aim-partial rule of strategy_target_replay.cpp; f_theta(d) = sum_i b(d)_i forward(d)_i; NaN where b is flat).
- Fitter (`fit_composition_weights.py`):
  - every factor record carries `f_theta_unsigned` (the theta-averaged book of the unsigned neutralized standalone
    book q_k against the record's forward returns r(d+2)); the record key gains `horizon_fingerprint`, the producer
    fingerprint of `theta_book_returns` in horizon_stats.py, so an edit there invalidates the records.
  - `--report-f-theta`: every admission row gains `f_theta` (mean of s_k * f_theta over live TRAIN decisions) and
    `f_theta_hac_t` (Newey-West t, Bartlett lag 5, the veto's estimator); s_k = 0 gives None. admission.json gains a
    `report_only` block (definition, theta, window, "gates nothing, selects nothing, weights nothing (v8-prereg rule
    8)"); admission.csv appends the two columns. The columns are computed after every verdict; no check, order or
    weight reads them. Without the flag admission.json / .csv bytes are unchanged.
- Card (`alpha_report_card.py`):
  - `--ic-theta`: each card gains `horizon = {theta, h: [1, 63], ic_theta, weights_sum, definition}` from its own decay
    curve m(h) (`decay.ic`, the lagged one-day rank IC already in the card); the index row gains `ic_theta`.
  - `--marginal-ic PATH [--marginal-ic-sha256 SHA]`: reads contract K6 (`marginal_ic.json`: a list of rows or
    `{"candidates": [rows]}`, each `{id, ic21, ic21_hac_t, marginal_ic21, marginal_hac_t, max_abs_rho,
    max_rho_member}`, extra keys ignored, malformed or duplicate rows refused); each card gains `marginal_ic` (the row,
    or `{"status": "absent from marginal_ic.json"}`), the index row `marginal_ic21`, the index inputs the file's SHA.
  - When an admission row carries `f_theta` / `f_theta_hac_t` (fitter run with `--report-f-theta`), the card copies
    them into `card.admission`.
  - Without the switches (and with an admission.json written without `--report-f-theta`) card bytes are unchanged:
    `test_cards_byte_identical_to_the_pre_store_card` still passes against the v8-base card.
  - `size_groups` / `ff12_groups` lost their `Inputs` annotation so the card store's producer closure no longer
    includes the input-loading class (it is keyed by the field pins instead).

Pre-registration: the declaration that neither column gates or selects is already in `v8-prereg.md` on the
integration branch (root commit 553be716, rule 8, and the final list: "Report-only columns ic_theta, f_theta, marginal
IC and diagnostics G-1..G-3 gate nothing and select nothing"). Lane C did not edit root's file. Nothing has read either
column on real data.

Tests (`atx-impl/tools/test_horizon_stats.py`, synthetic):
`test_ic_theta_matches_hand_value` (closed-form geometric series and a literal loop, weights sum 1 - .95^63, None on a
short or undefined curve), `test_theta_book_returns_hand_case`, `test_f_theta_is_report_only` (v4-prior-v1: every
pre-existing admission key and row value, counts, admitted order and CSV cells identical with and without the flag;
the weights file differs only in `provenance.admission_sha256`; the store serves the same bytes; f_theta and its HAC
t equal a direct recomputation from `Context.build` + `horizon_stats` exactly), `test_f_theta_is_report_only_on_the_v3_
screen` (oriented by the screen sign), `test_card_ic_theta_matches_hand_value_of_its_decay_curve`,
`test_marginal_ic_rows_are_copied`, `test_switches_off_leave_the_cards_unchanged`,
`test_admission_f_theta_is_copied_and_bad_k6_refused`.

Test run (`"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider`): test_horizon_stats.py +
test_fit_composition_weights.py + test_fit_composition_weights_store.py: 108 passed; test_horizon_stats.py +
test_fit_composition_weights_store.py + test_alpha_report_card_store.py + test_alpha_report_card.py +
test_book_monitor.py: 50 passed.

## How root verifies

1. Identity with the flags off is the C-1 check (task-C-1-report.md steps 1-3): admission.json equal after dropping
   `inputs.script_sha256` and `inputs.context_sha256`, admission.csv byte-identical, cards `diff -r` empty.
2. Report-only on real data (TRAIN only): rerun the v7.1 fit argv with `--report-f-theta --work-dir
   build-equity/fit-work --output build-equity/mega-weights-v71-ew-c2`, then
   ```
   "C:/Program Files/Python312/python.exe" -c "import json,sys; a,b=[json.load(open(p+'/admission.json')) for p in sys.argv[1:3]]; strip=lambda d:[{k:v for k,v in r.items() if k not in ('f_theta','f_theta_hac_t')} for r in d['candidates']]; print('rows equal:', strip(a)==strip(b)); print('verdicts equal:', all(a[k]==b[k] for k in ('admitted','counts','sign_conflicts','rules'))); print('extra keys:', sorted(set(b)-set(a)))" build-equity/mega-weights-v71-ew-c1a build-equity/mega-weights-v71-ew-c2
   ```
   Expected: `rows equal: True`, `verdicts equal: True`, `extra keys: ['report_only']`.
3. Cards: rerun the v7.1 card argv with `--ic-theta` (and `--marginal-ic <F-2 output>/marginal_ic.json` once F-2 has
   run) into a new output; every card gains `horizon` (and `marginal_ic`) and nothing else changes.

## Deviations from the brief

- Both columns are behind switches (`--report-f-theta` on the fitter, `--ic-theta` / `--marginal-ic` on the card), so
  the accepted admission and card bytes stay identical when they are off (lane identity rule).
- `f_theta` is reported as two columns, `f_theta` (mean daily factor return of the theta-averaged sleeve book) and
  `f_theta_hac_t`, as the brief's "(... HAC t)" reads; statistics over live TRAIN decisions, oriented by s_k like
  the other admission columns.
- `ic_theta` is not renormalised by the truncated weight sum (0.9605); the card records `weights_sum` beside it.
- The K6 container is not fixed by the contract text; the card accepts a top-level list or `{"candidates": [...]}`
  (lane F had not committed K6 when this was written).
- No edit to `v8-prereg.md` (root's file): the declaration is already there (rule 8).

## Cross-lane edits

None.

## Open risks

- K6 container shape: if F-2 writes another container (for example one file per candidate), `load_marginal_ic`
  needs a small change; the per-row keys are the contract's.
- Every factor record now stores its f_theta series (about one more series per record); computing it costs one pass
  over the decisions per candidate (measured 0.05 s on a synthetic 1,000 x 6,000 book; about 2.5 s for 48 cold records,
  nothing on a store hit).
