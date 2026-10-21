# L3e-xcal sprint note — a cross-source shape certificate in the status composer (2026-10-20)

Lane `L3e-xcal`, branch `feat/vol-L3e-xcal-20261020` on base `be379ad4` (the W2b root), Python only, no build.
Code commit `ff9643c4`. Evidence: `C:\atx-cache\quote-benchmark\L3e-xcal-20261020-c6\RESULTS.md` (+ `sha256.txt`).

## 1. What and why

The status-routed composites are the product that reaches 92-98 % coverage, and they violate the plan's §1.2
hard gate across sources: every single-source policy is clean on its own served rows, but routing by status
mixes sources inside one chain and across the expiries of one underlier, and the served set jointly breaks the
no-arbitrage conditions (C6: dev200 18 convex + 1,755 calendar, dev200b 37 + 2,391; every break pairs
`ladder-point` with `m6`; LEDGER 2026-10-18 `| benchmark |`). The composer said so in its receipt ("rows from
different models are not jointly arbitrage-checked"). This lane adds the certificate as an opt-in routing rule.

`atx-vol/tools/compose_predictions_by_status.py --calendar-certificate off|demote|refuse --dataset <cohort dir>`:

- `off` (default): today's output byte for byte (dev200 C6 `afc66bb9…`, dev200b `10e5a190…` reproduced; receipts
  identical once the four run-identity lines — `tool_sha256`, `tool`, `created_utc`, `predictions` — are restored).
- After the status routing, the served price set (status `ok` / `ok_price_only` with a finite `predicted_price`,
  the scorer's `PRICE_SERVED_STATUSES`) is tested with exactly the served-shape conditions of `check_served.py` /
  `quote_benchmark._shape_breaks` (tol 1e-6): monotone and convex per chain and side, calendar across every pair
  of expiries of one (underlier, root, snapshot) at a strike and side served by both. The composer reproduces the
  scorer's counts to the digit (pairs 227,632 / 221,409; breaks 18 + 1,755 / 37 + 2,391).
- A break whose nodes come from ONE source is that source's own business (counted as single-source, untouched).
  A break whose nodes come from different sources acts on the node with the LARGEST `source_index`: every served
  row of that expiry side (`chain_id`, `is_call`) routed to that source is `demote`d to the next input in the
  ordered list that serves it (status + restriction), or — when none does, or under `refuse` — refused: status
  `calendar_arbitrage` (calendar) / `arbitrage_failure` (monotone, convex), price and vol blanked,
  `source_model` / `source_index` kept. Passes repeat until no cross-source break remains; bounded by the number
  of inputs (each pass removes one source from every offending expiry side).
- Reads only status, `predicted_price` and the quote geometry (`quotes.parquet` key columns: row_id, chain_id,
  underlier, root, snapshot_ns, expiry_days, strike, is_call); never bid / ask, never `labels.parquet` (a test
  moves the labels away and asserts identical output). When `--receipts` are given, the dataset's
  `quotes.parquet` must hash to the receipts' `quote_parquet_sha256`.
- Receipt block `calendar_certificate`: mode, `pairs_checked`, `cross_source_pairs`, monotone pairs / convex
  triples checked, breaks found by kind / cross-vs-single / source pair, `passes`, `rows_demoted` and
  `rows_refused` (by source pair and by status), `residual_breaks` (by kind; cross-source residual is 0 by
  construction, single-source residual is the inputs' own), served rows before / after.

## 2. Numbers (C6 = ladder-point -> m6, re-composed from the existing CSVs by sha256; no fit)

| Cohort | Arm | sha256 | Coverage | MAE $ | MAE/spread | Containment | p99 $ | Vol MAE bp | Violations m/c/cal | Rows refused |
|---|---|---|---|---|---|---|---|---|---|---|
| dev200 | off (= C6) | `afc66bb9` | 93.68 % | 0.2258 | 0.1436 | 98.34 % | 1.968 | 1,592 | 0 / 18 / 1,755 | — |
| dev200 | **refuse** (= demote: 2 inputs) | `ad705c6c` | 88.76 % | 0.2199 | 0.1097 | 99.24 % | 1.879 | 1,540 | **0 / 0 / 0** | 3,148 (`ladder-point+m6`, 204 expiry sides) |
| dev200b | off (= C6) | `10e5a190` | 98.44 % | 0.2352 | 0.1110 | 98.95 % | 1.877 | 1,616 | 0 / 37 / 2,391 | — |
| dev200b | **refuse** (= demote) | `b35a44d7` | 92.13 % | 0.2316 | 0.1056 | 99.59 % | 1.818 | 1,566 | **0 / 0 / 0** | 3,934 (`ladder-point+m6`, 238 expiry sides) |

Violations are 0 in every shape class on both cohorts by BOTH `check_served.py` and the scorer's `class_strata`;
containment >= 98.90 % in every class (the §1.2 98.5 % bar, which `off` missed in four classes). Cost (ruling (a):
a lost row is a refusal): 4.92 / 6.31 pts of coverage = 67 % / 69 % of what m6 contributed; the refused rows were
the LESS accurate ones (MAE, MAE/spread, p99, containment, vol MAE all improve). Against the single-source
ladder the certified composite keeps +1.78 / +1.88 pts of coverage at the ladder's accuracy with 0 violations.
`demote` equals `refuse` on C6 because there is no third input; the unit test exercises demote on three inputs.
Coverage targets (>= 96 % / 98.8 %) stay unmet by every arm. Development numbers only; nothing promoted.

## 3. Tests (`atx-vol/python/tests/test_compose_predictions_by_status.py`, 11 -> 20)

Fixture: one underlier, three expiries; `a` serves expiries 1 and 3, `b` serves expiry 2 with a calendar crossing,
`c` serves expiry 2 cleanly; the only put row shares the breaking chain and pins the (chain, side) unit.
`off` byte-identical to the plain run; `refuse` -> b's call rows `calendar_arbitrage`, blank price / vol, label
kept, put row untouched, residual 0; `demote` -> expiry 2 re-routed to `c`; two-input demote == refuse bytes;
single-source crossing untouched and reported; cross-source convex break refused (`arbitrage_failure`) / demoted;
labels moved away -> identical output; receipt fields; argument and geometry guards (mode without dataset,
dataset with off, uncovered row, chain mismatch, missing `predicted_price`).
Fail-on-revert: the calendar pair test disabled (blob `12300040`) -> 4 failed / 16 passed; restored (blob
`c5a8162e`, committed as `ff9643c4`) -> 20 passed.
Run: `"C:/Program Files/Python312/python.exe" -E -P -B -X utf8 -m pytest --noconftest -q -p no:cacheprovider
atx-vol/python/tests/test_compose_predictions_by_status.py` (polars 1.40.1 and pytest 9.0.2 live in the user
site-packages, which `-I` excludes; `-E -P -B` keeps the isolation minus the user-site exclusion).

## 4. Traps and notes

- The composer's receipt embeds FOUR run-identity fields (`tool_sha256` = sha256 of the file itself, `tool`
  path, `created_utc`, output `predictions` path) and is written CRLF by Python text mode on Windows: prove
  parity by replacing those four lines at the byte level (`l3e_steps.py parity`), never by re-dumping the JSON.
- `-X utf8` is needed with this interpreter: the console code page is cp1252 and polars frames print box glyphs.
- The certificate covers monotone / convex as well as calendar: the acceptance demands 0 convex breaks, and
  cross-source convex breaks exist (18 / 37) that only a certificate testing them can clear. On C6 every targeted
  expiry side carried a calendar break, so no `arbitrage_failure` refusal was needed.
- Unit = expiry SIDE (`chain_id`, `is_call`): all three conditions are per side, so refusing the other side would
  lose coverage without buying any certificate. The brief's whole-expiry unit refuses at least as many rows.
- The calendar test covers ALL served expiry pairs at a strike (the scorer's definition), not adjacent only.

## 5. Next

C6d (divsmooth -> ladder -> m6) is the three-input case where `demote` differs from `refuse` (not measured here:
the brief named C6). The C7 consolidation can take the `refuse` arm as its certified baseline and route the
refused m6 expiry sides to a source that is calendar-consistent with the ladder (or fit the ladder's refusals).
