# Task T8 — Library v3: short-interest, implied-vol, earnings-drift and size families on fixed v2

Declared 2026-09-27 by the root parent BEFORE any v3 measurement. Families are chosen from published
priors, not from data. Every v3 candidate is one admission trial in the ledger.

Owner worktree: `C:/atx-wt/pool-7` (library lane), new branch `feat/mega-alpha-library-v3-20260927` cut
from the root HEAD the controller names (v2 fix round 1 will already be imported there). Files you own:
`atx-impl/strategies/generate_*_v3.py`, `atx-impl/strategies/*_v3.json`, `atx-impl/strategies/*_v3.recipe.json`
(mirror the v2 file trio and naming), plus a synthetic fixture for the generator if v2 has one.
Do not edit v1/v2 library files, the runner, the engine or the NAV replay.

## Inputs you rely on

- Fixed v2 library: `atx-impl/strategies/generate_price_volume_ic96_v2.py` and its JSON/recipe. v3 =
  every v2 candidate byte-identical (same ids, same DSL text, same `dsl_sha256`) + the new families
  below. The candidate cache keys on `dsl_sha256`, so unchanged candidates must stay byte-identical.
- Extra PIT fields (loaded by the runner via `--train-fields` / `--validation-fields`, task T7), exact
  names: `si_shares`, `si_dtc`, `iv_atm_21d`, `iv_atm_63d`, `iv_atm_126d`, `earn_recent`, `shares_out`,
  `mktcap_lagged`, `size_grp`, `is_common`, `mkt_ret`. Read `atx-engine/tools/prepare_research_fields.py`
  for units and clocks (e.g. `si_*` visible only after FINRA dissemination; `earn_recent` semantics).
  Price fields as in v2 (`close` split-adjusted, `raw_close`, split-adjusted volume as v2 uses it).
- DSL grammar/operators: read the v2 generator and the engine alpha-DSL docs/headers it relies on.
  Use only operators the VM supports today.

## Data hazards (must be handled in the DSL, not assumed away)

- IV contains garbage rows: `iv_atm_126d` has member values up to 1.2e16 and IV maxima ~69 (units
  mixed / vendor errors). Every IV-based candidate must be robust: compute on a guarded IV (treat values
  outside [0.02, 5.0] as missing, if the DSL can express it) and/or use cross-sectional ranks so a
  single garbage cell cannot dominate. Say exactly which guard you used.
- `mktcap_lagged` covers only ~73-75% of members; prefer `shares_out * raw_close` for size (99.7%) and
  use `mktcap_lagged` only where it adds something.
- SI updates twice a month (FINRA settlement dates); IV daily. Missing -> NaN (candidate excludes the
  name that day); never fill with 0.

## Families (declared from priors; direction below is the prior, the runner still orients on TRAIN)

Keep the count modest: about 24-32 new candidates total. For each family use 2-4 variants that differ
only in a declared window/smoothing grid; no other free parameters.

1. **Short-interest ratio** `si_shares / shares_out` (prior: high SI -> lower returns; Asquith, Pathak
   & Ritter 2005; Boehmer, Huszar & Jordan 2010). Level (ranked), and level smoothed.
2. **Days to cover** `si_dtc` (prior: high DTC -> lower returns; Hong, Li, Ni, Scheinkman & Yan 2015).
3. **Change in SI ratio** over ~2 and ~4 FINRA cycles (~10 and ~21 sessions) (prior: rising SI ->
   lower returns).
4. **IV level** ATM 21d (prior: high IV -> lower returns, the IVOL anomaly analogue; Ang, Hodrick, Xing
   & Zhang 2006), ranked.
5. **IV term slope** `iv_atm_126d - iv_atm_21d` or ratio (prior: inverted term structure -> lower
   returns; Vasquez 2017; Jiang & Tian-style slope), ranked, guarded.
6. **IV change** over 5 and 21 sessions, smoothed (prior: sharp IV rise -> lower returns; An, Ang, Bali
   & Cakici 2014, ATM approximation).
7. **Implied minus realized vol** `iv_atm_21d - realized_vol_21d(close)` annualized consistently
   (prior: high IV-RV spread -> lower returns; Bali & Hovakimian 2009; Goyal & Saretto 2009).
8. **Earnings-announcement return drift**: market-relative return (`ret - mkt_ret`) accumulated over the
   announcement window flagged by `earn_recent`, held for ~20-60 sessions after (prior: positive drift;
   Chan, Jegadeesh & Lakonishok 1996; Brandt, Kishore, Santa-Clara & Venkatachalam 2008). Must be causal:
   only returns up to t-1 relative to the decision; check `earn_recent`'s clock in the producer.
9. **Size tilt** `-log(shares_out * raw_close)` ranked (prior: small > big, weak; one or two variants
   only). The construction neutralizes logADV63, so expect overlap; declare it anyway.

Turnover: the declared per-alpha ceiling is standalone `tau_k <= 0.70/day` (daily full rebalance,
neutralized gross-1 book). Any variant built on a daily-changing input (IV change, IV-RV) must include
DSL smoothing (ts_mean / decay) so it is plausibly below that; SI-based signals change slowly.

Borrow: the NAV replay's `swap-fin-v1` blocks new shorts in special-tier names (flags: mcap < $1bn,
raw price < $5, SI/shares_out > 10%, age < 365d). SI/size alphas that short heavily-shorted small names
will be partly blocked in the NAV run. Do not work around this in the DSL; just note which families lean
into it.

## Requirements

1. Generator writes the v3 library JSON + recipe deterministically (same bytes on rerun), with
   provenance (generator sha, v2 library sha it extends). Record each new candidate's family, prior
   direction, window, and fields referenced.
2. Every candidate is causal (uses data <= t-1 for the t decision, per the v2 convention) and uses only
   fields the producer provides.
3. `--check` (or the v2 equivalent) validates the library; report counts, the lookback, and which
   candidates reference extra fields.
4. Fixture: synthetic check that v2 candidates are byte-identical in v3 and new candidates parse (reuse
   the v2 fixture pattern). Root runs the native `--plan-only` compile.
5. Report: the family table (id range, formula, prior sign, citation, windows), IV guard used, expected
   turnover class per family, which families reference which fields.
