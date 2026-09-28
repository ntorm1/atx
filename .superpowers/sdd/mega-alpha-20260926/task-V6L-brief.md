# Task V6-L: alpha library v6 (`generate_fund_ic_v6.py` -> `fund_industry_ic_v6.json`)

**Pool:** C:/atx-wt/pool-5. First: `git status` clean, then `git checkout -B feat/mega-alpha-v6-l-20260927 e0dfb8c7`.
Work ONLY in pool-5. Never build, never run real data, never spawn subagents, never touch C:/atx or other pools.
Commit trailer: `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
**Model:** Opus 5.5. **Report:** `C:/atx-wt/pool-2/.superpowers/sdd/mega-alpha-20260926/task-V6L-report.md`. Reply < 15 lines.

**Read first:** the "## v6 revision" section of `.superpowers/sdd/mega-alpha-20260926/v4-prereg.md` (item V6-L is this
task; binding), `.superpowers/sdd/mega-alpha-20260926/v6-literature.md` sections 3 (per-theme proposed definitions), 4
(missing families) and 2, `v6-code-review-signal.md` findings I2, I3, I5 (double smoothing, 21-session blackout,
low_risk vs neutraliser). Then `docs/plans/2026-09-27-mega-alpha-scorecard.md` sections 5-6 (current 38 candidates and
verbatim DSL), `atx-impl/strategies/generate_fund_ic_v5.py` (+ v4) and `atx-impl/strategies/fund_industry_ic_v5.json`,
and the TRAIN fields-v6 manifest (40 fields; find it via `studies/v51_train.sh`) so you know which fields exist. The DSL
vocabulary: grep the evaluator (`vm.hpp`, `ts_ops.hpp` in atx-engine) for available ops (`decay_linear`, `ts_*`, rank ops)
and their NaN/window semantics before writing any string.

**Deliverables:**
1. `atx-impl/strategies/generate_fund_ic_v6.py` producing `atx-impl/strategies/fund_industry_ic_v6.json` deterministically
   (same JSON canonicalisation as v5; record its sha256 in the report). Library v6 = library v5.1 with these changes, ONE
   variant per hypothesis, every candidate prior-signed (sign from the literature, cited in the generator as a comment
   with author-year):
   - price_momentum: add residual momentum (12-1 momentum residual to the price-risk exposures available in the DSL;
     if not expressible, the closest expressible proxy, documented);
   - profitability_quality: add cash-based operating profitability (Ball-Gerakos-Linnainmaa-Nikolaev 2016);
   - value: add composite value (mean rank of bm, ep, cfp, ebit_ev) and intangible-adjusted value if R&D/SG&A fields
     exist (else document the missing field);
   - investment_issuance: add 5-year composite issuance / net external financing (Daniel-Titman 2006; Bradshaw et al.)
     if shares-outstanding history allows, else 1-year net share issuance;
   - reversal_seasonality: add Heston-Sadka multi-lag seasonality (mean of same-month returns at lags 12, 24, 36 months);
   - low_risk: REPLACE low_beta / low_ivol / low_max with betting-against-correlation (Asness-Frazzini-Gormsen-Pedersen
     2020) and scaled MAX (Novy-Marx-Medhat); keep lowvol_ind only if it is not spanned by vol63 (state the argument);
   - short_interest: add long-window FINRA shorting flow (63-126 session mean of short volume / volume);
   - earnings_momentum: add EAR-centred earnings momentum (return over the [-1,+1] window around the earnings reaction
     date, held) if an event-date field exists; else keep and document.
   - Smoothing: for every member whose signal uses `decay_linear(rank(x), 21)`, switch to `rank(decay_linear(x, 21))`
     ONLY when the review's 21-session blackout argument applies (the rank is masked to members; see vm.hpp:425-441,
     ts_ops.hpp:25-26); for the fast sleeves (standalone tau >= .08 in `build-equity/mega-weights-v51-ew/admission.json`)
     remove the 21-session decay entirely (construction theta does the smoothing). Do not touch other members.
   Roster size <= 48. Every DSL string must reference only fields in the fields-v6 manifest; if a hypothesis needs a
   field that does not exist, list it under "needs new field" in the report rather than inventing one.
2. A pure-Python check script `atx-impl/strategies/check_fund_ic_v6.py` that validates: unique ids, prior sign present,
   all field names exist in a given manifest path, no forbidden ops, roster <= 48, and prints the diff vs v5.1 (added /
   removed / changed ids). Run it (no real data needed: the manifest is metadata).
3. Report: table of all candidates (id, theme, prior sign, DSL, change vs v5.1, citation), the "needs new field" list,
   the sha256 of the JSON, the root command lines to run the u/admission pass with `studies/v51_train.sh`-style phases
   (the controller will wire them into `studies/v6_train.sh`), and concerns.

**Constraints:** never read any TRAIN return statistic to choose a definition or a sign (priors come from the literature
only); never read 2023+ data; do not edit fit_composition_weights.py, NAV C++, nav_summ.py or the v5/v5.1 JSON/generators.
