# Dataset `fundamentals_notes` (segments, notes items and segment features, point in time)

Code `src/atx_db/alpha_panel/fund_notes.py` (clock rule `notes-accepted-v1`, schema
`atx.alpha-panel.fundamentals-notes/v1`); registration stub `docs/datasets/fundamentals_notes.registry.json`.
Tier1-v4 task 14 (gap analysis section 2 item 12; plan S4.4). Input: stage `notes/` (SEC DERA *Financial Statement
and Notes* data sets 2019q1 .. 2026_08, landed by tier1-v3 lane NOTES, `notes_fetch.py`); no new source, no network.
Stage-level background (landing, item registry, reconciliation classes): `docs/ALPHA_PANEL_NOTES.md`.

## Files and keys

| file | rows (build 2026-10-03) | bytes | keys | content |
|---|---:|---:|---|---|
| `fundamentals_notes/segment_features.parquet` | 162,314 (8,947 CIKs) | 8,965,756 | (cik, period, adsh), unique; vintage key (cik, period) | per-filing segment features (this task) |
| `fundamentals_notes/segment_recon.parquet` | 46,321 (9,280 CIKs) | 1,544,567 | adsh | per original 10-K / 10-KT: segment revenue reconciliation |
| `fundamentals_notes/segments.parquet` | 2,483,640 (5,442 CIKs) | 21,631,671 | (adsh, metric, axis_kind, member, tag, ddate, qtrs, segments, uom) | segment measures on one business or geographic member |
| `fundamentals_notes/notes_wide.parquet` | 197,148 (11,354 CIKs) | 17,829,282 | adsh | current-period notes items per periodic filing |
| `fundamentals_notes/items/year=YYYY/items.parquet` | 18,470,845 (8 files, 2019-2026) | 125,791,550 | (adsh, tag, is_custom, segments, ddate, qtrs, uom, coreg) | long table of every registry-tag fact |
| `fundamentals_notes/coverage.json` | | | | gates per filing year, recon classes and rules, feature coverage on 10-Ks and on month-end panel cells |
| `fundamentals_notes/manifest.json` | | | | schema, code SHA-256, file SHA-256, notes input manifest SHA-256, rules, `gate_status` |

Filed 2019-01-02 .. 2026-08-31; `available_at` 2019-01-02 21:13 .. 2026-08-31 21:04 UTC; `segment_features` periods
2012-10-31 .. 2026-07-31 (late filers included). Every `segment_features` row has `clock_basis = accepted_utc`.
Forms: 10-Q 113,888; 10-K 41,242; 20-F 3,441; 10-Q/A 1,673; 10-K/A 1,591; 20-F/A 222; 40-F 151; 10-KT 70;
10-QT 21; 10-KT/A 8; 40-F/A 7. 94 rows inherited at least one block from an earlier vintage (79 amendments, 15
originals sharing a (cik, period)). Build: 357 s, native peak 0.58 GiB under a 0.8 GiB guard
(`.superpowers/sdd/tier1-v4/receipts/t14_fund_notes_full_r5.json`); manifest SHA-256 `1cc1132f2bd13b20...`.

## Point-in-time rule

* `available_at` (TIMESTAMP WITH TIME ZONE, UTC) is the filing's EDGAR acceptance time from the data set's SUB
  table (EDGAR America/New_York wall clock, DST-aware); when SUB has no acceptance time the floor is filed
  00:00 UTC + 46 h (`clock_basis = filed_plus_46h`). Every column of a row carries that clock. A row is visible at
  decision session d only if `available_at` < 22:00 UTC of session d-1 (GC1).
* Values are as filed in that accession; nothing is restated in place. An amendment (10-K/A, 10-Q/A) is its own
  accession and its own row with its own clock.
* `segment_features` is a vintage table keyed `(cik, period)`, ordered by `(available_at, adsh)`. A later vintage of
  the same `(cik, period)` that does not report a feature block inherits that block from the latest earlier
  vintage that does (blocks: segments, geography, customers, debt ladder; each lease / SBC column on its own;
  `carry_n` counts inherited blocks), so a partial amendment never blanks the original's values. The earlier row is
  unchanged.
* As-of macro (serving catalog; `<root>` = the alpha-panel build root):

```sql
CREATE OR REPLACE MACRO segment_features_asof(ts) AS TABLE
  SELECT * FROM read_parquet('<root>/fundamentals_notes/segment_features.parquet')
  WHERE available_at < CAST(ts AS TIMESTAMPTZ)
  QUALIFY row_number() OVER (PARTITION BY cik, period ORDER BY available_at DESC, adsh DESC) = 1;
-- latest annual features per issuer at decision session d (18-month staleness bound, as the coverage measure):
SELECT * FROM segment_features_asof(lake_cutoff(d)) WHERE is_annual AND period >= d - INTERVAL 548 DAY
QUALIFY row_number() OVER (PARTITION BY cik ORDER BY period DESC) = 1;
```

## `segment_features` columns

One row per periodic filing (10-K, 10-Q, 10-KT, 10-QT, 20-F, 40-F and amendments) with at least one feature.
Context: the filing's current period (`ddate = period`), its own flow duration (`qtrs = flow_qtrs`; a 10-Q falls
back to the discrete quarter `qtrs = 1` when it tags no year-to-date segment rows), the filer context (no
co-registrant) and the filer currency (the filing's most frequent monetary unit). Source of every value: SEC DERA
Financial Statement and Notes data set, NUM/DIM rows of the accession. Clock: `available_at` (above). Staleness:
annual blocks describe the fiscal year ending at `period` and stay current until the next 10-K (coverage counts
them for 548 days); flows in a 10-Q are year-to-date.

| name | type | unit | definition |
|---|---|---|---|
| cik | BIGINT | id | SEC CIK of the filer |
| adsh | VARCHAR | id | EDGAR accession |
| form | VARCHAR | code | EDGAR form |
| period | DATE | date | SEC balance-sheet date (month-end rounded) = fiscal period end |
| fy, fp | BIGINT, VARCHAR | label | SEC fiscal year / period label (FY, Q1..Q3, H1, M9) |
| filed | DATE | date | EDGAR filing date |
| available_at | TIMESTAMPTZ | UTC | acceptance clock (PIT rule above) |
| clock_basis | VARCHAR | rule | `accepted_utc` or `filed_plus_46h` |
| currency | VARCHAR | ISO 4217 | filer currency of the money values |
| flow_qtrs | INTEGER | quarters | duration of the filing's own flows (4 annual, 1/2/3 year-to-date) |
| is_annual | BOOLEAN | flag | `flow_qtrs = 4` |
| is_amendment | BOOLEAN | flag | form ends in `/A` |
| countryba | VARCHAR | ISO 3166 | SUB business-address country (blank read as US); the home market of `foreign_sales_share` |
| rev_consolidated | DOUBLE | currency | consolidated revenue of the context: first tier-1 revenue chain tag with a non-zero total |
| bseg_n_tagged | BIGINT | count | reportable business-segment members (`StatementBusinessSegmentsAxis`, not reconciling-like, not a roll-up / duplicate dropped by the reconciliation search) over every fact of the filing |
| bseg_n | BIGINT | count | business segments: `bseg_n_tagged`, or 1 for an annual filing with consolidated revenue and no tagged member |
| bseg_rev_n | BIGINT | count | reportable segments with positive revenue in the context |
| bseg_rev_tag | VARCHAR | tag | revenue tag of the segment rows (`mixed` when the search merged tags) |
| bseg_rev_rule | VARCHAR | rule | reconciliation-search rule that fixed the member set (annual; NULL = strict) |
| bseg_rev_hhi | DOUBLE | 0..1 | Herfindahl of segment revenue shares: sum (r_i / sum r)^2 over positive reportable segment revenues (>= 2 needed when >= 2 members are tagged); 1.0 for a single-segment annual filing with positive consolidated revenue |
| bseg_recon_class | VARCHAR | class | annual: the `segment_recon` class (`direct`, `with_reconciling`, `reconciled_search`, or a miss class) |
| geo_rev_n | BIGINT | count | geographic members (`srt:StatementGeographicalAxis`) with revenue in the context |
| foreign_sales_share | DOUBLE | 0..1 | 1 - domestic / consolidated revenue; domestic = member equal to `countryba`, US synonyms for a US filer, or `Domestic`; else the aggregate foreign member (`Foreign`, `International`, ...; `NonUs` / outside-US for a US filer) / consolidated revenue; NULL outside [-1%, 101%], else clipped to [0, 1] |
| foreign_basis | VARCHAR | rule | `domestic_member`, `foreign_member`, `out_of_range` |
| major_cust_n | BIGINT | count | single customers with revenue on `srt:MajorCustomersAxis` (alone or with a customer-concentration `ConcentrationRiskBy*` context; customer classes such as Residential, Commercial, Government-wide aggregates excluded) |
| major_cust_share | DOUBLE | 0..1 | largest single-customer revenue / consolidated revenue, in (0, 1] |
| dd1 .. dd5 | DOUBLE | currency | Compustat DD1-DD5: long-term debt principal due in year 1 .. 5 (`LongTermDebtMaturitiesRepaymentsOfPrincipal*`, fiscal or rolling variant) |
| dd_after5 | DOUBLE | currency | principal due after year 5 |
| dd_rem_fy | DOUBLE | currency | principal due in the remainder of the fiscal year (10-Q ladders) |
| dd_n_buckets | INTEGER | count | tagged buckets among dd1..dd5, dd_after5 |
| dd_sched_total | DOUBLE | currency | sum of the tagged buckets (missing = 0), plus `dd_rem_fy` when `dd1` is absent |
| dd1_share | DOUBLE | 0..1 | dd1 / (dd1 + .. + dd5 + dd_after5) |
| oplease_liab | DOUBLE | currency | operating lease liability (ASC 842), else current + noncurrent |
| oplease_rou | DOUBLE | currency | operating lease right-of-use asset |
| oplease_cost | DOUBLE | currency | operating lease cost of the period (flow, year-to-date in a 10-Q) |
| oplease_rate | DOUBLE | decimal | weighted-average operating lease discount rate |
| finlease_liab | DOUBLE | currency | finance lease liability, else current + noncurrent |
| mrc1 .. mrc5, mrcta | DOUBLE | currency | Compustat MRC1-MRC5 / MRCTA analog: undiscounted operating lease payments due in year 1 .. 5 / thereafter (ASC 842) |
| stkco | DOUBLE | currency | Compustat STKCO: share-based compensation (cash-flow add-back, else expense tags; year-to-date in a 10-Q) |
| sbc_unrecognized | DOUBLE | currency | unrecognized compensation cost of nonvested awards |
| carry_n | INTEGER | count | feature blocks inherited from an earlier vintage of the same (cik, period) |

## Segment revenue reconciliation (`segment_recon`, S4.4 gates)

Per original 10-K / 10-KT. Multi-segment: >= 2 reportable business-segment members over any fact of the filing.
S = one revenue value per business member (segment-only context preferred over `ConsolidationItems=OperatingSegments`,
current fiscal year, filer currency) for the chosen revenue tag; T = the same tag's non-dimensional total when
non-zero, else the first tier-1 chain tag with a non-zero total; E = tagged `ConsolidationItems` reconciling members
without a segment member.

* **Revenue tiers** (`revenue_tier`). Tier 1: `Revenues`, `RevenueFromContractWithCustomer(Ex|In)cludingAssessedTax`,
  `SalesRevenueNet`, ... (the S4.4 chain). Tier 2, only when no tier-1 tag has segment rows: industry formats
  `PremiumsEarnedNet`, `SupplementaryInsuranceInformationPremiumRevenue`, `InsuranceServicesRevenue`,
  `HealthCareOrganizationRevenue*`, `Regulated(AndUnregulated)OperatingRevenue`, `ElectricUtilityRevenue`, lessor
  lease revenue, `RevenueNotFromContractWithCustomer`, `RealEstateRevenueNet`. Tier 3, last: filer extension tags
  named as segment revenue or sales (`SegmentRevenue`, `NetSales`, ...) on a business-segment axis.
* **Strict classes** (`recon_class_strict`): `direct` (|S - T| <= 1% |T|), `with_reconciling` (|S +/- E - T| <= 1%),
  else a miss class (`segments_exceed_total`, `segments_below_total`, `double_count`, `scale_or_unit`, `no_total`,
  `zero_total`).
* **Search pass** on strict misses (`recon_class = reconciled_search`, `recon_rule`, `dropped_members`,
  `seg_members`): (1) S +/- one disclosed component (each ConsolidationItems member, their sum, the per-segment
  elimination sum: overlapping members such as `CorporateAndEliminations` and `IntersegmentElimination` were
  double-summed); (2) dedup: drop members equal (within 0.01%) to the sum of >= 2 smaller members (a group above
  its sub-segments on one axis) or repeating another member's value, roll-ups alone tried first; (3) reportable
  members only (corporate / intercompany / eliminations lines removed); (4) mixed tags: per member the
  highest-priority revenue tag it carries. The first rule that reconciles within 1% wins; the dropped members are
  excluded from `bseg_n` and `bseg_rev_hhi` of that filing.

## Gates (gap analysis item 12, GC9; manifest `gate_status`)

Measured by `python -m atx_db.alpha_panel.fund_notes build` (or `coverage`) into `coverage.json`. Gate years
2020-2025 (filing year of the 10-K). Denominator: original 10-K / 10-KT with >= 2 reportable business-segment
members over any fact (`multi_segment`).

* **Segment revenue for >= 90% of multi-segment 10-K filers: PASS** in every gate year (0.9027 .. 0.9254). With the
  tier-1 chain alone it would be 0.8827 .. 0.9022 (FAIL 2020-2024).
* **Segment sum within 1% of consolidated revenue on >= 95%: FAIL** in every gate year (0.9097 .. 0.9242; strict
  rule alone 0.8141 .. 0.8575). Tier-1 filings reconcile at 0.929 .. 0.939; the tier-2 / tier-3 filings that the
  first gate gains reconcile at 0.29 / 0.35 (insurer premiums, lessor revenue and extensions are not total segment
  revenue). Remaining misses 2020-2025: 517 `segments_below_total` (partial tagging, segments carried only on
  multi-axis contexts, banks whose fee revenue is compared with gross interest income), 182
  `segments_exceed_total` (untagged eliminations), 137 `double_count` (hierarchies that are not exact sums), 70
  `no_total`, 5 `scale_or_unit`.

| filing year | 10-Ks | multi-segment | with segment revenue | share (gate >= 0.90) | tier-1 only | reconciled 1% | share (gate >= 0.95) | strict rule only |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 2019 | 5,767 | 2,119 | 1,930 | 0.9108 | 0.8801 | 1,752 | 0.9078 | 0.8161 |
| 2020 | 5,572 | 2,074 | 1,894 | 0.9132 | 0.8877 | 1,723 | 0.9097 | 0.8141 |
| 2021 | 5,877 | 2,019 | 1,846 | 0.9143 | 0.8876 | 1,690 | 0.9155 | 0.8239 |
| 2022 | 6,525 | 2,014 | 1,843 | 0.9151 | 0.8908 | 1,685 | 0.9143 | 0.8340 |
| 2023 | 6,212 | 2,002 | 1,828 | 0.9131 | 0.8926 | 1,680 | 0.9190 | 0.8430 |
| 2024 | 5,790 | 1,943 | 1,754 | 0.9027 | 0.8827 | 1,621 | 0.9242 | 0.8575 |
| 2025 | 5,538 | 1,943 | 1,798 | 0.9254 | 0.9022 | 1,653 | 0.9194 | 0.8270 |
| 2026 | 5,040 | 1,725 | 1,581 | 0.9165 | 0.8904 | 1,453 | 0.9190 | 0.8286 |

Member_equity issuers (CIK of a panel `member_equity` cell that year):

| filing year | multi-segment | segment revenue share | reconciled 1% share |
|---|---:|---:|---:|
| 2019 | 1,362 | 0.9244 | 0.9055 |
| 2020 | 1,326 | 0.9253 | 0.9104 |
| 2021 | 1,296 | 0.9290 | 0.9120 |
| 2022 | 1,263 | 0.9311 | 0.9056 |
| 2023 | 1,261 | 0.9286 | 0.9137 |
| 2024 | 1,258 | 0.9277 | 0.9229 |
| 2025 | 1,231 | 0.9423 | 0.9181 |
| 2026 | 1,056 | 0.9403 | 0.9114 |

Classes (final; `reconciled_search` = strict miss fixed by the search pass):

| filing year | direct | double_count | no_total | reconciled_search | scale_or_unit | segments_below_total | segments_exceed_total | with_reconciling |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 2019 | 1,444 | 32 | 12 | 177 | 0 | 101 | 33 | 131 |
| 2020 | 1,391 | 28 | 13 | 181 | 1 | 104 | 25 | 151 |
| 2021 | 1,357 | 21 | 11 | 169 | 0 | 95 | 29 | 164 |
| 2022 | 1,371 | 26 | 11 | 148 | 1 | 92 | 28 | 166 |
| 2023 | 1,380 | 23 | 12 | 139 | 2 | 75 | 36 | 161 |
| 2024 | 1,339 | 20 | 11 | 117 | 1 | 71 | 30 | 165 |
| 2025 | 1,313 | 19 | 12 | 166 | 0 | 80 | 34 | 174 |
| 2026 | 1,164 | 14 | 12 | 143 | 0 | 69 | 33 | 146 |

Search rules:

| filing year | components | dedup | dedup+components | mixed_tags | mixed_tags+components | mixed_tags+dedup | reportable | reportable+components | reportable+dedup |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 2019 | 35 | 117 | 10 | 9 | 1 | 1 | 3 | 1 | 0 |
| 2020 | 36 | 122 | 11 | 7 | 0 | 2 | 3 | 0 | 0 |
| 2021 | 29 | 113 | 15 | 5 | 0 | 2 | 3 | 2 | 0 |
| 2022 | 26 | 95 | 17 | 5 | 0 | 2 | 3 | 0 | 0 |
| 2023 | 23 | 90 | 17 | 4 | 0 | 2 | 1 | 0 | 2 |
| 2024 | 27 | 72 | 13 | 1 | 1 | 1 | 1 | 0 | 1 |
| 2025 | 30 | 114 | 17 | 3 | 0 | 0 | 2 | 0 | 0 |
| 2026 | 27 | 97 | 15 | 1 | 0 | 0 | 2 | 1 | 0 |

## Measured coverage

**Panel cells (GC3).** Month-end session cells (last session of each month) of panel `member_equity` and of the
tier1-v4 Task 2 `universe` flags; a cell counts when its CIK (`identity/link_table_v3` valid that day, else the
cell row's CIK) sees an annual `segment_features` row with `available_at` < 22:00 UTC of the previous session and
`period` within 548 days (the latest period, then the latest vintage). Columns are non-null shares of all cells.

member_equity:

| year | cells | CIK linked | annual_row | bseg_n | bseg_rev_hhi | foreign_sales_share | major_cust_share | dd1 | oplease_liab | stkco |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 2020 | 27,558 | 0.9816 | 0.8922 | 0.8648 | 0.8097 | 0.3563 | 0.0535 | 0.4262 | 0.6522 | 0.8424 |
| 2021 | 29,100 | 0.9825 | 0.8678 | 0.8420 | 0.7931 | 0.3435 | 0.0488 | 0.4141 | 0.7711 | 0.8190 |
| 2022 | 27,861 | 0.9904 | 0.8975 | 0.8748 | 0.8244 | 0.3613 | 0.0485 | 0.4472 | 0.8118 | 0.8479 |
| 2023 | 27,601 | 0.9945 | 0.9085 | 0.8850 | 0.8345 | 0.3647 | 0.0483 | 0.4531 | 0.8502 | 0.8614 |
| 2024 | 27,284 | 0.9954 | 0.9043 | 0.8783 | 0.8244 | 0.3615 | 0.0491 | 0.4565 | 0.8531 | 0.8610 |
| 2025 | 26,563 | 0.9945 | 0.9011 | 0.8799 | 0.8249 | 0.3701 | 0.0512 | 0.4473 | 0.8540 | 0.8584 |

univ_all_common:

| year | cells | CIK linked | annual_row | bseg_n | bseg_rev_hhi | foreign_sales_share | major_cust_share | dd1 | oplease_liab | stkco |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 2020 | 39,599 | 1.0000 | 0.9741 | 0.9178 | 0.8551 | 0.3255 | 0.0554 | 0.3607 | 0.6541 | 0.9062 |
| 2021 | 44,368 | 1.0000 | 0.9354 | 0.8713 | 0.8158 | 0.3001 | 0.0524 | 0.3476 | 0.7812 | 0.8699 |
| 2022 | 44,704 | 1.0000 | 0.9613 | 0.8900 | 0.8376 | 0.3097 | 0.0501 | 0.3584 | 0.8039 | 0.8984 |
| 2023 | 42,741 | 1.0000 | 0.9807 | 0.9091 | 0.8563 | 0.3262 | 0.0511 | 0.3692 | 0.8777 | 0.9216 |
| 2024 | 42,032 | 1.0000 | 0.9812 | 0.9095 | 0.8514 | 0.3279 | 0.0534 | 0.3724 | 0.8957 | 0.9192 |
| 2025 | 41,062 | 1.0000 | 0.9849 | 0.9231 | 0.8587 | 0.3390 | 0.0565 | 0.3759 | 0.9048 | 0.9252 |

univ_r3000p:

| year | cells | CIK linked | annual_row | bseg_n | bseg_rev_hhi | foreign_sales_share | major_cust_share | dd1 | oplease_liab | stkco |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 2020 | 35,890 | 0.9487 | 0.9123 | 0.8751 | 0.8118 | 0.3296 | 0.0480 | 0.4077 | 0.6464 | 0.8516 |
| 2021 | 36,102 | 0.9562 | 0.9183 | 0.8769 | 0.8195 | 0.3298 | 0.0461 | 0.4140 | 0.7957 | 0.8597 |
| 2022 | 35,858 | 0.9705 | 0.9319 | 0.8902 | 0.8355 | 0.3381 | 0.0434 | 0.4238 | 0.8152 | 0.8763 |
| 2023 | 35,820 | 0.9744 | 0.9365 | 0.8927 | 0.8381 | 0.3479 | 0.0442 | 0.4255 | 0.8551 | 0.8871 |
| 2024 | 35,700 | 0.9773 | 0.9390 | 0.8937 | 0.8344 | 0.3484 | 0.0465 | 0.4299 | 0.8700 | 0.8904 |
| 2025 | 35,833 | 0.9744 | 0.9386 | 0.9008 | 0.8367 | 0.3548 | 0.0478 | 0.4325 | 0.8762 | 0.8882 |

**Original 10-K / 10-KT filings** (filing year; non-null share; `bseg_rev_hhi_multi_segment` = among multi-segment
filers). ASC 842 lease items start with fiscal years beginning after 2018-12-15 (10-Ks filed from 2020).

| filing year | 10-Ks | with_row | bseg_n | bseg_rev_hhi | bseg_rev_hhi_multi_segment | foreign_sales_share | major_cust_share | dd1 | dd_sched_total | oplease_liab | oplease_rate | finlease_liab | mrc1 | stkco |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 2019 | 5,767 | 0.9095 | 0.8389 | 0.7885 | 0.8674 | 0.2237 | 0.0473 | 0.2982 | 0.3364 | 0.0264 | 0.0049 | 0.0081 | 0.0137 | 0.7552 |
| 2020 | 5,572 | 0.9187 | 0.8424 | 0.7933 | 0.8703 | 0.2279 | 0.0481 | 0.3137 | 0.3525 | 0.6840 | 0.6010 | 0.2468 | 0.5680 | 0.7681 |
| 2021 | 5,877 | 0.8874 | 0.8030 | 0.7592 | 0.8777 | 0.2185 | 0.0476 | 0.3075 | 0.3352 | 0.6551 | 0.5843 | 0.2462 | 0.6229 | 0.7436 |
| 2022 | 6,525 | 0.8506 | 0.7591 | 0.7200 | 0.8779 | 0.2086 | 0.0420 | 0.2838 | 0.3090 | 0.6294 | 0.5537 | 0.2224 | 0.6146 | 0.7254 |
| 2023 | 6,212 | 0.8806 | 0.7838 | 0.7429 | 0.8786 | 0.2213 | 0.0438 | 0.2968 | 0.3216 | 0.7070 | 0.6207 | 0.2397 | 0.6671 | 0.7548 |
| 2024 | 5,790 | 0.9029 | 0.8071 | 0.7589 | 0.8626 | 0.2313 | 0.0480 | 0.3119 | 0.3351 | 0.7368 | 0.6465 | 0.2506 | 0.7028 | 0.7755 |
| 2025 | 5,538 | 0.9061 | 0.8238 | 0.7636 | 0.8682 | 0.2467 | 0.0520 | 0.3174 | 0.3378 | 0.7429 | 0.6517 | 0.2544 | 0.7057 | 0.7806 |
| 2026 | 5,040 | 0.8863 | 0.8054 | 0.7399 | 0.8638 | 0.2437 | 0.0506 | 0.3167 | 0.3369 | 0.7252 | 0.6411 | 0.2462 | 0.6855 | 0.7734 |

Distributions (annual originals filed 2020-2025): `bseg_n` p10 / median / p90 = 1 / 1 / 4 (max 20; 59% single
segment); `bseg_rev_hhi` 0.42 / 1.00 / 1.00 (min 0.097); `foreign_sales_share` 0.049 / 0.332 / 0.802 (8,531 filings;
basis: 8,331 domestic member, 391 foreign member, 10 out of range); `major_cust_share` 0.101 / 0.325 / 0.893 (1,899
filings); `dd1_share` 0.000 / 0.057 / 0.489; `oplease_rate` 0.030 / 0.051 / 0.092.

## Known limits

* **Gate 2 (segment sum within 1%) fails** at 0.91-0.92 (see Gates). The remaining misses need rules this pass
  does not have: segments tagged only with extra axes (`BusinessUnit`, `Geographical`, `LegalEntity`), partial
  member tagging, banks' composite segment revenue (net interest income + noninterest income; the consolidated net
  interest income is not landed), and untagged intersegment eliminations.
* **Major-customer coverage is low** (about 5% of member cells, 4-5% of 10-Ks). Only revenue amounts on
  `srt:MajorCustomersAxis` are landed; the common percentage disclosure (`ConcentrationRiskPercentage1` with
  `ConcentrationRiskByBenchmark` = revenue and `ConcentrationRiskByType` = customer) is kept by `notes_fetch` only
  when it also carries a business or geographic axis. Remedy: add `ConcentrationRisk` to
  `notes_fetch.ITEM_TAG_PREFIXES` and re-land the 40 data sets (15.5 GB download, about 1 h), then read the share
  from the percentage rows.
* **Foreign-sales share** (34-37% of member cells) needs a home-country or aggregate foreign member; region-only
  breakdowns (Americas, EMEA, ...) and domestic-only filers that tag no geography stay NULL.
* **10-Q segment rows** get no reconciliation or dedup pass: a roll-up member can bias a 10-Q `bseg_rev_hhi`; prefer
  annual rows (`is_annual`) or compare `bseg_rev_n` with `bseg_n`.
* **Single-segment default:** an annual filing with consolidated revenue and no tagged business member reads as one
  segment (`bseg_n` 1, HHI 1.0); a multi-segment filer that tags no segment axis is misread. `bseg_n_tagged` keeps
  the raw count.
* **IFRS filers** (`ifrs-full` segment axis and tags) are not mapped; their rows carry US-GAAP-tagged items only.
* **Values are as filed:** 873 rows have negative `stkco` (forfeiture reversals), a few have negative lease or
  ladder amounts; `oplease_rate` outside [0, 1] (percent-format filer errors, 143 rows) is NULL. Notes values are in
  the filer currency (`currency`), not converted.
* **Clock:** 9,182 rows have `available_at` before the `filed` date: filings accepted 17:00-22:00 ET after the
  EDGAR 17:30 cut-off carry the next business day as filing date, and the acceptance time (the dissemination time,
  the FSDS clock rule) is used. 2 rows were accepted more than 4 days after the filing date (date-adjusted
  filings); the later clock is kept.
* **Debt ladder in 10-Qs** is fiscal-year based (`dd_rem_fy` + year buckets), not rolling.
* **GC2:** no return statistic was computed; all measurements are value and coverage counts.
