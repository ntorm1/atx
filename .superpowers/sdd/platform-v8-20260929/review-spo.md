# Review: the spo-v3 optimiser (solve_tracking and the spo-v3 rule)

Snapshot `d22b735a` (read with `git show d22b735a:<path>` in `C:/atx-wt/pool-2`). Read-only: nothing was built or
run, and no data was opened. FIX-2 (N-2 / E-37, P-2, T-4) is not merged at this commit.

Severity: **I** important, **M** medium, **m** minor. No I finding.

Binding rules applied: E-14, E-15, E-26, E-31 and E-37 (`progress.md`); plan section 9, R-6 (the registration);
`v8-prereg.md` rule 7.

---

## Answers to the brief's questions

### 1. Is the objective the registered one?

Derived from the code (`target_tracking.cpp:387-457`, `strategy_spo.cpp:1394-1423`). Per scored rebalance decision d
and book, over the optimized names j (members with a risk row: slot < 50 and a finite positive specific variance):

    minimize_w  (gamma/2) [ (B'(w - a) + e_out)' F_+ (B'(w - a) + e_out) + sum_j d_j (w_j - a_j)^2 ]
              + sum_j [ (s/H) |w_j - w0_j| + (eta_j/H) |w_j - w0_j|^{3/2} ] + sum_j b_j max(-w_j, 0)
    s.t.        max(lower_j, w0_j - t_j) <= w_j <= w0_j + t_j
                1'w = -net_fixed                       (book net 0)
                -.02 - beta_fixed <= beta'w <= .02 - beta_fixed

The terms:
- a = L x desired: `desired` is the shared target of `form_desired`, so E-26 holds (`nav_replay.cpp:785-796, 975`).
- e_out = X'w of the fixed positions with a risk row.
- F_+ = F with negative eigenvalues set to 0.
- s = half spread + commission of the primary S2 law.
- eta_j = impact_y sigma_j sqrt(NAV_post / ADV_j). ADV and sigma come from rows [d-63, d).
- H = 20.
- b_j = the book's short financing per session.
- t_j = .01 ADV_j / NAV_post (0 without ADV).
- lower_j = min(w0_j, 0) where the locate rule guards the name; -inf elsewhere.
- beta = Sigma m / m'Sigma m, with m the equal-weight portfolio of the optimized names.
- gamma = 20 / sqrt(252 a'Sigma a), taken at the first scored rebalance decision.

This is the registration (plan section 9) and the R-6 report's statement (section 2, "Problem as coded", and
section 5), with these known or declared departures:
- F_+ in the solve but F in the published terms (A-7, open).
- No gross constraint: a breach voids the run (A-9).
- Unpriced members are outside Sigma and keep their weight (declared).
- No long financing term (the registered form has only `b_i max(-w_i, 0)`).
- The store is not checked to be atx-risk-v1.1 (SPO-6).

**No discrepancy between the code and the R-6 report.**

### 2. Does the fixed point depend on worker count, iteration order or floating-point association?

**Not on worker count:**
- The replay refuses `--book-workers` above 1 and grids with any v7 extension (`strategy_nav_replay.cpp:2273-2276`,
  `:3185-3186`).
- The solver has no threads, RNG or clock.
- The primary book's bits do not depend on which other books run. The per-decision data (slice, names, beta) is
  read-only, the dual is per book, and gamma depends only on the aim and the slice, which every book shares.

**Mathematically:**
- The minimiser is unique: d > 0 makes F_+ plus diag(d) positive definite.
- A warm and a cold dual agree to 1e-8 (`book_target_tracking_test.cpp:271-285`).

**Bitwise:** the returned w depends on three things:
- The name order: ascending instrument index, fixed.
- The warm dual: every later decision inherits it through w0 and the dual.
- The build ISA: dev is SSE2; `rel-avx2` permits FMA contraction.

Same exe and argv give the same bytes. Identities and the cell must use the same exe. No finding.

### 3. Can the carried dual make a scored decision depend on a warm-up decision?

**No.**
- `warm_up_step` (`strategy_spo.cpp:1052-1063`) moves only the book and the shadow (aim-partial-v5, `spent` unused
  under v5). It never touches `book.dual`.
- The first scored `plan_tracking` therefore finds `book.dual` empty and solves cold (`:1499-1503`).
- Later duals come only from scored solves. `begin_run` clears the books (`:1557`).
- The warm-up reaches the scored window only through the book, which is the design and is tested bit for bit against
  the plain aim-partial-v5 warm start (`strategy_spo_v3_test.cpp:389-454`), and through the shared hold-band state,
  which the parent carries identically.
- No warm-up decision reads the risk store:
  - `plan()` dispatches before `prepare` (`:1254`).
  - `hold()` only moves the shadow.
  - The decide path refuses `d < decision_begin` (`strategy_nav_replay.cpp:3531`).

### 4. Can the net or beta limit be reported met when it is not?

**Not for the plan.**
- `limits_met` is computed on the returned w after the restoration (`target_tracking.cpp:460-463`): both band
  excesses must be within 1e-12.
- The returned z is always finite and inside its box (`name_prox` clamps and maps a NaN input to the dead zone), so
  the NaN-reads-as-in-band path of `band_excess` cannot be reached through w.
- The rule's partition (optimized names plus fixed positions) covers every instrument (`strategy_spo.cpp:1178-1192`).
- The locate block after the rule is a no-op on the solver's plan, because the floors are the same.
- `BookStats` counts every scored row with `limits_met` false (`strategy_spo_v3.cpp:66`), including converged solves
  whose restoration failed.

`limits_met` is a property of the plan. The traded book's beta is never recorded, which matches E-31's wording
(`limits_unmet`). The accounting is right; enforcement is missing (SPO-1).

### 5. Are gamma's annualisation and the S_prior scaling consistent with the risk store's units?

**Yes.**
- The store is daily variance in decimal return units: "forecast at the session's close for the next session"
  (`strategy_risk_verb.cpp:944-951`), with invariant D < 1 (`strategy_risk_model.hpp:98-100`).
- sigma_aim = sqrt(252 a'Sigma a) is applied once (`strategy_spo.cpp:35, 1373`).
- The objective is per session: costs are divided by H, borrow is per session (`per_session`, 365/252 / day count),
  and Sigma is daily.
- The implied annual Sharpe of the aim is therefore 252 gamma sigma_d^2 / (sqrt(252) sigma_d) = gamma sigma_aim =
  S_prior = 20, exactly.
- The ceiling 1.0 daily matches the v1.1 invariant, so the clamp half of the tripwire cannot fire on a v1.1 store.

**Unpinned by any test** (T-4, open in FIX-2).

### 6. Can the iteration cap or the tripwire hide a non-converged solve on a scored decision?

**Hide: no.**
- Every scored row records `converged`, `limits_met`, the residuals and the iterations.
- `report_only.<book>` counts `unconverged` and `limits_unmet`.
- Warm-up decisions no longer solve, so nothing unscored is filtered out.
- The cap returns the last z, restored, and flagged unconverged. There is no early exit (E-31 satisfied).

**But two gaps remain:**
- The tripwire never acts on E-31's invalidity condition (SPO-1).
- `--spo-tol` / `--spo-iters` can redefine "converged" on the cell's argv (SPO-5).
- A non-finite iterate would read as converged (SPO-3, reachability unverified).

### 7. Look-ahead from the risk store?

**None found.**
- Only row d is read (`RiskStore::read(d)`, `strategy_spo.cpp:734-769`). `check_axes` binds the store's sessions to
  the replay's.
- Row d is the forecast made at the close of d. The model's own timing rule is "every quantity at t reads rows <= t"
  (`strategy_risk_model.hpp:50`; model internals not re-read here).
- The other inputs are point in time:
  - The decision liquidity uses rows [d-63, d) (`strategy_cost_v2.cpp:158-162`).
  - The E-26 ADV cap uses rows <= d.
  - Presence and membership are read at row d.

---

## Findings

### SPO-1 (M) E-31's invalidity condition is not enforced: the tripwire reads "clear" and the run publishes returns

**Where:**
- `atx-impl/src/strategy_spo_v3.cpp:111-129` (`count_trips`: clamps and gross breaches only), `:302-316`
  (`tracking_tripwire`), `:318-323` (`status`), `:99-107` (`limits_unmet` only inside `report_only`).
- `atx-impl/src/strategy_nav_v7.cpp:463-469`: `capture` voids only on `rows_tripwire()`.
- `atx-impl/src/strategy_nav_replay.cpp:2660-2665`: the console prints each book's net Sharpe.
- `scripts/specs/v8/r6-spo-v3.json`: the description's "Before any return" names the identities and
  `tripwire.status == clear` only.

**What is wrong:**
- Ruling E-31: `limits_unmet > 0` on scored decisions of the primary book makes the run invalid. The counts must be
  read before any return.
- The tripwire mechanism exists so that a void run has no NAV or return file and no return statistic on the console.
  E-31's condition is not part of it.
- A run with an unmet net or beta limit on the primary book:
  - exits 0
  - writes every NAV and return file
  - prints S2's net Sharpe in the same invocation
  - records `spo_v3.tripwire.status: "clear"`
- The only trace is `report_only.<primary>.limits_unmet`. The r6 template tells the operator to read the status.
- P-2 (FIX-2) adds `limits_unmet == 0` to the scorecard. The scorecard reads after the run, so it does not restore
  "before any return".

**Failing scenario:** one scored decision of `modeled-1bn-stale5-v1+swap-fin-v1` whose beta band is out of reach of
that session's trade limits (R-6 report section 10).
- It runs to the 2,000 cap and its restoration fails, so `limits_met` is 0.
- The run then completes, the console shows the S2 net Sharpe, the status reads clear, and the cell's returns have
  been seen before anyone reads the invalidity.

**Smallest fix:**
1. In `tracking_tripwire` / `tracking_tripwire_json`, treat `limits_unmet > 0` on the primary book's scored rows as
   void. The status names E-31, and the check does not depend on `--specific-ceiling-void`.
2. nav_v7 passes the primary label: `book_label` of `nav_primary_scenario_index`.
3. Bytes change only on runs E-31 already invalidates.
4. Add `limits_unmet == 0` to the r6 template's pre-return text.

**Blocks:** the R-6 cell (E-31 as declared).

**Verified** by reading.

### SPO-2 (m) The E-14 traded-book correlation is lagged one decision; FIX-AB's look-ahead reason does not hold

**Where:**
- `atx-impl/src/strategy_spo.cpp:1444-1452`: `aim_correlation_traded` = corr(`current` at DECIDE d, `aim_d`).
- `atx-impl/src/strategy_spo_v3.cpp:86-96`.
- `task-FIX-AB-report.md` Deviations, A-4.

**What is wrong:**
- `current` at d is the book of decision d-1's fills plus one session of drift (cadence 1 in the v7.1/v8 cells). The
  value therefore measures tracking plus the aim's own change from d-1 to d.
- A perfect tracker reads about the aim's decision-to-decision autocorrelation, not 1.
- The .9 threshold (E-14) was set from the prototype's plan-level corr(w, w_aim) of .96.
- Under the B0c warm start, the first scored row measures the aim-partial-v5 warm-up book. That is one row in about
  750; negligible.
- FIX-AB says the post-fill pairing "would need a one-decision look-ahead". It does not:
  - corr(book DECIDE read at the next decision, aim_d) uses nothing unknown at that decision.
  - It is a diagnostic, not an input.
- The timing is disclosed in `traded_correlation_unit`, so this is a definition question, not a hidden defect.

**Effect:** the bias is downward, so the criterion is stricter. Its size is about 1 minus the aim's one-session
autocorrelation. Unverified; it is small for a slow aim.

**Smallest fix:**
- Keep the previous aim per book and record the post-fill pairing beside the current one.
- The PM states which pairing E-14 reads before the cell.

**Blocks:** nothing until ruled.

**Verified** (code). **Unverified** (magnitude).

### SPO-3 (m, reachability unverified) The convergence test is NaN-blind

**Where:** `atx-engine/src/book/target_tracking.cpp:451` (`primal = std::max(primal, std::abs(x - z))`), `:93-94`
(`shrink` returns 0 for a NaN), `:455-456`, `:470-471`, `:381-383`.

**What is wrong:**
- If the x-update ever produced a NaN, `std::max` would keep the old residual.
- `name_prox` would map the NaN into the dead zone, giving z = w0 and step 0.
- The solve would report `converged` at iteration 1 with a no-trade plan, and limits met if the book was already
  in its bands.
- Its NaN dual is refused only at the book's next solve, so the run's last decision would publish such a row as
  converged.

**Reachability:** the inputs are validated finite and the capacitance is checked positive definite. Only overflow
reaches this.

**Smallest fix:** refuse a non-finite x, or write the update as `if (!(r <= primal)) primal = r;`.

**Blocks:** nothing.

### SPO-4 (m) E-37 is not in the code, and two hazards wait for its implementation

**Where:**
- `atx-impl/src/strategy_nav_v7.cpp:726-727`: spo-v3 still refuses `--capacity-curve` (known N-2, FIX-2).
- `strategy_spo.cpp:1557` (`begin_run`).
- `strategy_nav_v7.cpp:39, 383, 463-469, 813-814`.
- `strategy_spo.cpp:1198-1233`.

**Hazard (a): pass state leaks into the published blocks.**
- `Engine::begin_run` clears the books and the date cache but keeps `tracking_rows`, `calibration` and `timing`.
- In a capacity pass, the capacity books' rows would join `spo_diagnostics.csv`, the summary, `report_only` and
  `count_trips`.
- `capture` reruns `rows_tripwire()` after the capacity pass, when the main pass's NAV files already exist. A
  capacity-book gross breach would leave through `if (code != 0) return code` with no `v7_extras.json`. The void
  promise breaks.

**Hazard (b): the capacity books are not planned at their own NAV.**
- `plan_tracking` prices every book with `State::s2`, the primary S2 law.
- It caps trades at p ADV / NAV at the base NAV.
- A capacity book at multiple m would be the $1bn plan executed with m-scaled impact and participation. Unlike
  aim-partial-v6's capacity pass (`:352-362`), it is not the NAV-m tracker.
- FIX-2's brief ("the spo-v3 book at each multiple") does not say which reading E-37 means.

**Smallest fix:**
- Key rows by pass, or clear them for non-Main passes and keep the Main tripwire.
- The PM rules on (b).

**Blocks:** the E-29 / E-37 report on R-6 (with N-2).

**Verified** by reading.

### SPO-5 (m) The solver's stopping rule can be loosened on a registered cell's argv

**Where:** `atx-impl/src/strategy_nav_v7.cpp:92-96` (not in `refused_with_v3`), `:695-696`;
`strategy_spo.cpp:407-408` (tolerance up to 1e-3, 1..100000 iterations); `strategy_spo_v3.cpp:202`.

**What is wrong:**
- H, S_prior, p and beta are refused. The engine's declared tolerance and cap are not.
- With `--spo-tol 1e-3` every solve reads `converged` and `unconverged` is 0. E-31's counts then say nothing.
- The values appear only in the parameters block, not in the tripwire or the report.

**Smallest fix:** refuse both with spo-v3 (R-6 report section 10 already calls `--spo-iters` a different cell), or
mark a non-default value in the tripwire status.

**Verified** by reading.

### SPO-6 (m) Model version unchecked; zeroed covariance entries unreported under spo-v3

**Where:**
- `atx-impl/src/strategy_spo.cpp:666-671` (`RiskStore::open`) and `:761-763`.
- The manifest's `"model"` key is at `strategy_risk_verb.cpp:917`.
- `strategy_spo.hpp:310-345` (`TrackingRow`).

**Version check:**
- `open` admits any complete `atx.risk-model/v1` and never reads `"model"`.
- The registration names atx-risk-v1.1. Only root's SHA pin binds the version.
- A v1.0 store with 9e12 entries would void through the clamp. One without such entries would run silently.

**Zeroed covariance entries:**
- NaN factor-covariance entries are set to 0 and counted in `nan_covariance_entries`, but no v3 column carries the
  count.
- The zeroed entries lower sigma_aim, which raises gamma, and remove the factor risk of the exposed names, unseen.

**Smallest fix:** refuse `model != "atx-risk-v1.1"` under spo-v3, and add `nan_covariance_entries` to the row.

**Verified** (code). Whether the 4-year store has NaN entries on forecast rows is **unverified**.

---

## Checked, no finding

- **Solver algebra:**
  - The x-update constant `c0 = B W W'(B'a - e_out) + gamma d a`, with W W' = gamma F_+.
  - The Woodbury apply and the capacitance I + W'B'Delta^-1 B W.
  - The 9-case KKT choice: signs at lo/hi, equality rows, violation in row units, fixed tie order.
  - Over-relaxation and the scaled dual.
  - The 3/2-power prox: root `2q / (b + sqrt(b^2 + 4q))`; borrow shift `b / rho`; clamp after the 1-D argmin.
  - The restoration's minimum-norm 2 x 2 step, with net change -e0 and beta change -e1.
  - The external-gap sign.
  - The layout: group 1 + slot, 62 = 1 + 50 + 11.
- **Box always nonempty under v3:** upper = inf and lower = min(w0, 0) <= w0.
- **A-2:** gamma on the first scored rebalance decision; `calibration.session` in the scored window; the
  `warm_up` key only with a warm-up. `x.decision_begin` is not shifted by the warm start
  (`strategy_nav_replay.cpp:1231-1238`).
- **E-26:**
  - The aim is `L x shared.desired` from `form_desired`.
  - The ADV cap uses rows <= d and the initial NAV (E-15).
  - The rule id records the shaping.
  - spo-v1/v2 refuse `--hold-band` / `--adv-hold-q` for any value and any position. The replay parser has no `=`
    form, so the refusal cannot be bypassed that way. Grids are refused with the extension.
- **E-15:** the tracker's own trade limit uses NAV_post. E-15 governs only the shaping cap, which the aim inherits.
- **`Engine::last_aim`:** in memory only, never published. It is set on scored rebalance decisions, and is stale
  through warm-up and non-rebalance decisions (test seam only).
- **Iteration-cap telemetry:** `Timing.unconverged` counts exactly `!converged`, which is "stopped at the cap". It is
  console only.
- **Other refusals:**
  - The nine registered flags, before or after `--rule`.
  - `--spo-alpha` (only `implied-aim`, v3 only, once).
  - A second `--rule`.
  - A per-name rate.
  - `--emit-holdings` with the void on.

---

## Table of findings

| ID | sev | file:line | blocks | verified | one line |
|---|---|---|---|---|---|
| SPO-1 | M | `strategy_spo_v3.cpp:111-129, 302-334`; `strategy_nav_v7.cpp:463-469` | R-6 cell | yes | E-31's `limits_unmet > 0` never voids; status reads clear and returns are printed/written |
| SPO-2 | m | `strategy_spo.cpp:1444-1452` | ruling before the cell | code yes, size no | E-14 traded correlation lags one decision; FIX-AB's look-ahead reason is wrong |
| SPO-3 | m | `target_tracking.cpp:451, 93-94` | - | reachability no | NaN x-update would read as converged |
| SPO-4 | m | `strategy_spo.cpp:1557`; `strategy_nav_v7.cpp:383, 726, 814` | E-37 report (with N-2) | yes | capacity pass would leak rows into the tripwire after NAV exists; S2 law and base trade limit at every multiple |
| SPO-5 | m | `strategy_nav_v7.cpp:92-96, 695-696` | - | yes | `--spo-tol` / `--spo-iters` accepted on spo-v3, redefining convergence |
| SPO-6 | m | `strategy_spo.cpp:666-671, 761-763` | - | code yes | v1.1 not checked; zeroed covariance entries not in the v3 row |

Counts: I 0, M 1, m 5. Earlier findings still open on this code: A-7, A-8, A-9 (m; part 1); T-3 (spo-v2 pin
placeholder; part C item 7); T-4 (no independent solver reference; FIX-2); N-2 (E-37; FIX-2).

## Coverage

| file (at `d22b735a`) | lines read | notes |
|---|---|---|
| `atx-engine/include/atx/engine/book/target_tracking.hpp` | 1-148 (all) | |
| `atx-engine/src/book/target_tracking.cpp` | 1-482 (all) | SPO-3 |
| `atx-impl/src/strategy_spo.hpp` | 1-465 (all) | |
| `atx-impl/src/strategy_spo.cpp` | 1-60, 390-429, 640-1567 | v1/v2 FISTA (60-640) and v1/v2 outputs (1569-1797) not read |
| `atx-impl/src/strategy_spo_v3.hpp`, `strategy_spo_v3.cpp` | all | SPO-1 |
| `atx-impl/src/strategy_nav_v7.cpp` | 1-836 (all) | SPO-1, SPO-4, SPO-5 |
| `atx-impl/src/strategy_nav_replay.cpp` | 780-918, 1098-1140, 1226-1262, 2191-2276, 2645-2670, 3330-3420, 3531-3566 | |
| `atx-impl/src/strategy_target_replay.cpp` | 260-420 (`update_weights`, aim-partial-v5) | |
| `atx-impl/src/strategy_cost_v2.cpp` | 132-177 | |
| `atx-impl/src/strategy_risk_model.hpp` | 1-110 | timing and units only; model code not read |
| `atx-impl/src/strategy_risk_verb.cpp` | grep: manifest layout 917-951 | |
| `atx-impl/tests/strategy_spo_v3_test.cpp` | 270-521 | the E-26 tests and parse test not read |
| `atx-engine/tests/book/book_target_tracking_test.cpp` | 195-300 | |
| `scripts/specs/v8/r6-spo-v3.json`, pitch config R-6 entry | all / grep | |
| Reports | R-6 (all), FIX-AB (all), review-w1-A (all), review-w1-T T-3/T-4, FIX-2 brief, progress.md 260-573, plan section 9 R-6 | |
| Not read | `strategy_spo_test.cpp`, `strategy_spo_pin_test.cpp`, `strategy_spo_v3_pin_test.cpp` (beyond 30-50), `strategy_spo_digest.hpp`, risk model internals | |
