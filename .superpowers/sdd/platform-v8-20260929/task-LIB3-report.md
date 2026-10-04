# Task LIB3 report: v9 prior-class library wave, draft (wave AG)

Lane LIB3, pool 11, branch `feat/platform-v8-lib3-20261001` from `67f04389`. Documentation only: no C++, no Python code,
no field build, no registry edit, no data read. Declared 2026-10-01 before any IC, TRAIN, return or NAV read of any
candidate; registration text for v9 (the v8 trial program is untouched: N <= 51, prereg rule 10).

## Status

| item | status | commit |
|---|---|---|
| Draft `docs/plans/2026-10-01-v9-library-draft.md` (10 candidates, field specs, add-alpha lines, rulings) | DONE | this commit |
| Report (this file) | DONE | this commit |

No draft registry file: the registry has no off-by-default entries (`add-alpha` registers directly), so, as LIB2 did,
the registration lives in the draft and this report. No registry default list or theme text changed.

## 1. Candidates

| # | id | theme | family | tier | status | DSL sha256 (16) | bars / slots / extra fields |
|---|---|---|---|---|---|---|---|
| C-1 | `stmom` | price_momentum | short-term momentum among heavily traded stocks (Medhat-Schmeling 2022, RFS) | B- | READY | `06dc6238d7e94089` | 41 / 5 / 1 |
| C-2 | `ind_leadlag` | price_momentum | big-firm industry return leads small firms (Hou 2007, RFS) | C+ | READY | `d2d9b4d24b474944` | 21 / 7 / 2 |
| C-3 | `nt_late` | filing_events | first NT 10-K / NT 10-Q (Bartov-Konchitchki 2017, AH) | B- | NEEDS-FIELD | `bafc4e3a93204e59` | 0 / 3 / 1 |
| C-4 | `earn_season` | reversal_seasonality | earnings seasonality (Chang-Hartzmark-Solomon-Soltes 2017, RFS) | B- | NEEDS-FIELD | `64a0be8f2c71efd1` | 0 / 5 / 2 |
| C-5 | `lazy_prices` | filing_events | 10-K text similarity (Cohen-Malloy-Nguyen 2020, JF) | B- | NEEDS-DATA | `8a175c874813e0c8` | 0 / 2 / 1 |
| C-6 | `tnic_mom` | price_momentum | text-based peer momentum (Hoberg-Phillips 2018, JFQA) | B- | NEEDS-DATA | `0be5696997a181a7` | 0 / 2 / 1 |
| C-7 | `conn_rev` | reversal_seasonality | connected-stock reversal (Anton-Polk 2014, JF) | C+ | NEEDS-DATA | `4a1cffe83a007531` | 0 / 3 / 1 |
| C-8 | `fund_fit` | ownership_flow | expected flow-induced trading (Lou 2012, RFS) | C+ | NEEDS-DATA | `46a02ba90aad79b1` | 20 / 3 / 1 |
| C-9 | `tax_book` | profitability_quality | tax-to-book income (Lev-Nissim 2004, TAR) | C+ | NEEDS-DATA | `1b76612cee2479bb` | 20 / 6 / 3 |
| C-10 | `iv_skew` | options_implied | implied volatility smirk (Xing-Zhang-Zhao 2010, JFQA) | C+ | NEEDS-DATA | `1dc4382ae845968f` | 20 / 4 / 2 |

Every candidate is a family the book does not hold (v7.1 members plus every v8.0-v8.2 registration). Each carries the
paper's definition and sign, citation and sample, post-2004 evidence (or the gap, marked), DSL, deviations, overlap by
construction [est] and a breadth reason in draft section 2. All pass the house budget by the mirror.

## 2. Fields to build and data asks

- **NEEDS-FIELD (an atx-engine producer on a stage already built):** F-L1 `nt_first_126` from `sec_filings/events.parquet`
  (NT 10-K / NT 10-Q rows with acceptance clock; the stage the K8 fields already pin; this lifts v8's OD-6 withdrawal of
  `nt_late`); F-L2 `earn_season_rank` from the fundamentals stage read for `ni_q` / `eps_consist_4y` (20 quarters,
  defined from about 2019Q4).
- **NEEDS-DATA (asks to atx-db):** finish the 10-K text landing (stopped at 7%) and rebuild `text/` and
  `classification_tnic/` (C-5, C-6); build `nport/` with an index-fund screen (C-7, C-8); build `fundamentals_notes/`
  (`tax_current`, C-9); a put-wing IV column (C-10; no source column is registered, v7 D8).
- Specs (stage, point-in-time rule, staleness, formula ids) in draft section 3.

## 3. What root runs to compile-check

- READY now: the `stmom` and `ind_leadlag` add-alpha lines (draft section 4) in a scratch tree against fields v9 or
  later; add-alpha step 3 runs K1: `build-equity/bin/atx-equity-strategy-ic.exe --plan-only --library <lib>
  --library-sha256 <sha> --train build-equity/train-2020-2023-lo3/manifest.json --train-sha256 <sha> --train-fields
  <fields dir> --train-fields-sha256 <sha>`. Expected rows: `stmom` 41 bars, 5 slots, extra `shares_out`;
  `ind_leadlag` 21 bars, 7 slots, extra `me_company`, `grp_ff49`.
- The other eight compile only once their field is in a manifest and the registry (K1 refuses unknown fields).
- Static figures come from a scratch mirror (not committed) that reproduces all 15 recorded rows of library-v8-draft
  section 4 and task-LIB2 section 1 (bars, slots, nodes, SHA) and `qmj_safety`'s 8 slots. K1 decides.

## 4. Open questions and rulings (draft section 6)

- LIB3-a: no theme text edits (theme texts are in generated library bytes); a new `cross_firm_links` theme is the
  alternative for C-2 / C-6.
- LIB3-b: `fund_fit` under `ownership_flow` ("informed" in its text) or withdraw.
- LIB3-c: `stmom`, `conn_rev` (and the one-month `ind_leadlag`) against library-v7-draft section 4's cost exclusion.
- LIB3-d: `stmom`'s components `rev_21`, `turnover_21` were IC-screened standalone by atx-db (I did not read results).
- LIB3-e: C+ members without a verified post-2004 test (`ind_leadlag`, `tax_book`, `fund_fit`, `conn_rev`): keep or
  withdraw at 0 trials.
- LIB3-f: roster 60 + 10 = 70 > 64.
- LIB3-g: `ind_leadlag` at the 7-slot limit.
- LIB3-h: paper definitions (Anton-Polk connection rule, Lou flow forecast, Hoberg-Phillips peer weights) copied before
  the builds; fallbacks declared now.

## 5. Deviations from the brief

- READY count is 2 of 10. Reason (finding): most new price / volume families are already measured on platform data
  (74 hypotheses in three pv libraries plus the atx-db characteristics screen, which includes the high-volume premium
  as `abn_turnover`); they are excluded under the prior-exposure rule. The earnings-announcement premium was READY (via
  `ea_window_pre5`) but is reported dead in the US after 2004 (Heitz-Narayanamoorthy-Zekhnini 2020, working paper).
- Evidence rule stricter than the v8 draft: the house literature note's CZ-own 2005-2024 statistics are not used
  (their window contains 2024); this lowered `tax_book` from v8's B to C+.

## 6. Hygiene and disclosures

- Read list and not-read list: draft section 9. Nothing under `build-equity/` was opened; no IC, card, fit or NAV.
- Disclosures under the hidden-data rule: (1) the v8 literature note prints CZ-own statistics for 2005-2024 (external,
  not platform data): seen, not used; (2) `ALPHA_PANEL_SEC.md` prints per-year Form 25 cause counts for 2024-2026 and a
  2024-06-30 filer count, and a status grep of `ALPHA_PANEL_OWNERSHIP.md` printed 13D parse counts for 2024-12..2026-09
  (filing-metadata counts, no return or signal statistic): used for nothing; (3) two sources (Muravyev-Pearson-Pollet
  2025, Heitz et al. 2020) read as abstracts with unverified sample ends, used only to lower or exclude.
- Web: definitions, signs, samples and published statistics only; every claim cites its source; unverified items are
  marked in the draft.

## Cross-lane edits

None.

## Open risks

- Six of ten candidates wait on atx-db builds; several definitions (C-6, C-7, C-8) must be copied from the papers before
  their fields are built.
- `stmom`, `ind_leadlag`, `tnic_mom`, `conn_rev` are one-month signals with high turnover [est].
- Sample years are unverified for Bartov-Konchitchki, Chang et al., Hoberg-Phillips and Lou (marked in the draft).
