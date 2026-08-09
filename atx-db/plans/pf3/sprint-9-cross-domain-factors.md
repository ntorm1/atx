# Sprint PF3-S9 — Cross-domain factors + unified factor namespace

**Goal:** integrate the already-built non-fundamental analytic surfaces into ONE factor namespace with consistent keys, units, and sign conventions, run them through the S7 PIT factor engine, and assemble the unified cross-domain panel. Five domains are in scope, each already materialized as its own typed, point-in-time surface: **price/liquidity** (`equity_price_metrics` — adjusted returns / realized vol / trailing momentum / dollar-volume + Amihud liquidity / max drawdown / market-relative beta / idiosyncratic vol / daily cross-sectional percentile ranks), **estimate revisions** (`est_surprise` + `est_consensus` snapshots), **13F flow** (`thirteenf_concentration_metrics` — HHI, top-holder concentration, holder-count and HHI change), **short-interest** (`short_interest_metrics` over the FINRA feed — days-to-cover, short-to-ADV, period-over-period change), and **insider** (`insider_transaction_metrics` — issuer-day net-buy, cluster-buy, plan-sale-ratio diagnostics). Each currently lives in its OWN schema with its OWN keys and units; S9's job is to project every one into the S7 factor catalog as first-class factor definitions and emit a unified `(security_id, as_of_date, factor_id, value)` surface consistent with S8's fundamental families — **not** to re-derive any domain's analytics. Reserved migrations 0160–0163.

**Mandate / Owns:** NEW `db/factors/cross_domain.py` (per-domain factor mappers + the unified-namespace assembly), the cross-domain catalog rows seeded into the S7 `factor_definitions` surface, and `db/tests/test_cross_domain_factors.py`.

**Must NOT touch:** the source analytic modules — **READ** `db/equity_price_metrics.py`, `db/thirteenf_concentration_metrics.py`, `db/short_interest_features.py`, `db/insider_metrics.py`, and the `est_surprise` / `est_consensus` estimate surfaces; do **not** re-architect, re-key, or re-derive any of them (13F concentration math, insider cluster diagnostics, short-interest feature SQL, and price analytics are all landed and correct). Do not modify the S7 factor engine itself (`db/factors/catalog.py` / `engine.py` / `cross_section.py`) — S9 *registers definitions into* it and *calls* it, it does not extend it. Do not touch the export panel (`db/factor_panel.py`, `v_factor_panel`) — that is **S10**. The fundamental factor families are **S8**; S9 unifies alongside them, it does not re-emit them.

**Depends on:** **PF3-S7** (the factor framework, PIT compute engine, and cross-sectional rank/zscore/winsorize/neutralize operators every cross-domain factor is routed through), **PF3-S8** (the fundamental factor families whose namespace and `(security_id, as_of_date, factor_id, value)` shape S9's cross-domain factors must be consistent with), and **pf2/pf1** for the source metric surfaces themselves (the five domain tables, their as-of readers, and their PIT columns). Sequential **after S8, before S10** — S9 shares the `db/factors/` package with S7/S8 and must not run concurrently with them in the same tree; S10 materializes the union of S8 + S9 as the backtest panel.

---

## Baseline / where the cycles go

The warehouse already computes rich per-domain analytics; what it lacks is *one* factor surface over them. Measured 2026-07-04 against `atx-impl/db`.

1. **Rich per-domain analytic surfaces exist — but each in its OWN schema, keys, and units.** `equity_price_metrics` is keyed `(security_id, trade_date)` and reports fractions / annualized fractions (`momentum_21d`, `realized_vol_60d`, `beta_60d`, `idiosyncratic_vol_60d`, `amihud_illiquidity_21d`, `max_drawdown_126d`) plus its own daily cross-sectional percentile ranks (`*_cs_pct_rank`). `thirteenf_concentration_metrics` is keyed `(security_id, cusip, report_period)` and reports HHI / concentration percentages and quarter-over-quarter changes (`value_hhi`, `top_10_holder_value_pct`, `holder_count_change`). `short_interest_metrics` is keyed `(security_id, settlement_date)` in FINRA settlement units (`si_days_to_cover`, `si_short_to_adv`, `si_change_ratio`). `insider_transaction_metrics` is keyed `(security_id, signal_date, window_days)` in share/dollar counts (`net_purchase_value`, `is_cluster_buy`, `plan_sale_value_ratio`). `est_surprise` is keyed `(security_id, measure_code, fiscal_year, fiscal_period)`. Five key grains, five unit systems, no common factor key.

2. **No unified cross-domain panel.** There is no single surface that answers "give me every cross-domain factor for this security on this date." A consumer today must join five heterogeneously-keyed tables by hand, reconcile their as-of grains, and normalize units per column — exactly the assembly S9 makes into a governed factor namespace.

3. **Cross-sectional treatment is inconsistent across domains.** `equity_price_metrics` ships its own daily percentile ranks for a handful of columns (`daily_return`, `momentum_21d`, `realized_vol_20d`, `dollar_volume`, `amihud_illiquidity_21d`); the 13F, short-interest, insider, and estimate surfaces ship raw levels with **no** cross-sectional standardization at all. A factor store cannot mix a pre-ranked price column with a raw short-interest level and call them comparable — every domain must pass through the *same* S7 cross-sectional operators to be namespace-consistent.

4. **The existing `v_alpha_daily_panel` is price-centric, not multi-domain.** The landed alpha panel view (from the partial signal layer, `db/alpha_research.py`) is built off price/return alphas; it is not the union of price + estimate + 13F + short-interest + insider factors that a cross-domain backtest needs. It is a precedent for the *shape*, not the multi-domain coverage.

**Already good — do not regress:** the per-domain PIT surfaces — `equity_price_metrics`, `thirteenf_concentration_metrics`, `short_interest_metrics`, `insider_transaction_metrics`, and `est_surprise` — are landed, catalogued, PIT-columned, and each has an as-of reader (`equity_price_metrics_asof`, `est_surprise_asof`, `est_consensus_asof`). S9 reads them **as-is** through those readers; it does not re-key, re-scale, or recompute any of them, and it must not perturb their existing catalog/quality rows.

---

## PIT / determinism + production contract

Clauses **(A)** bitemporal no-lookahead, **(C)** offline/no-network tests, **(D)** determinism + provenance, **(E)** schema-as-contract, **(I)** panel PIT-safety, and **(J)** semantic contract apply in full.

- **(A)** Every cross-domain factor row inherits its PIT availability **from its own source surface** — the price factor's `available_at` is the bar's, the 13F factor's is the filing's (a ~45-day lag past `report_period`), the short-interest factor's is the FINRA dissemination timestamp, the insider factor's is the Form 3/4/5 acceptance timestamp, the estimate factor's is the consensus/surprise availability. A cross-domain factor keyed at `(security_id, as_of_date)` reads each source only where `available_at ≤ as_of_date`. No blanket "available on the observation date" assumption is permitted (see Risks).
- **(J) / (S2)** Every domain's factor declares a consistent **unit** and **sign convention** in the S2 semantic contract before it lands: e.g. higher-is-more-bullish for insider net-buy and estimate-revision momentum, higher-is-more-bearish for short-interest days-to-cover and 13F crowding — with sign captured as contract metadata so downstream neutralization/combination is direction-correct. A check fails if a factor value violates its declared unit/sign domain.
- **(D)** Each per-domain mapper is a pure `compute_*` transform (source rows in → long `(security_id, as_of_date, factor_id, value)` DataFrame out), unit-tested without DuckDB; same inputs + params → same rows; each emitted factor row records its source-metric lineage.
- **(E)** Every cross-domain factor lands with a `factor_definitions` catalog row and a `table_catalog` / `field_catalog` entry in the same migration; the drift check fails on any uncatalogued factor or surface.
- **(B)** Migrations **0160–0163** only, schema/index/catalog split across numbers, timestamped DB+WAL backup before any live apply, never renumbering a landed migration: **0160** price/liquidity factor rows (catalog + assembly hook), **0161** estimate-revision + 13F-flow factor rows, **0162** short-interest + insider factor rows, **0163** the unified-namespace assembly surface + its indexes and catalog.

---

## Tasks

### S9-0 — Price/liquidity factor integration

**Root cause:** `equity_price_metrics` holds the densest, most backtest-relevant analytics in the warehouse (momentum / vol / liquidity / beta / idio-vol / drawdown), but they live in a price-specific schema with price-specific column names and a partial, hand-picked set of internal cross-sectional ranks — they are not addressable as catalogued factors in the S7 namespace, and their standardization is inconsistent with every other domain.

**Fix:** in `db/factors/cross_domain.py`, map the `equity_price_metrics` columns into S7 factor-catalog rows via seeded `factor_definitions` (momentum from `momentum_21d`/`momentum_126d`, volatility from `realized_vol_20d`/`realized_vol_60d`, liquidity from `avg_dollar_volume_21d`/`amihud_illiquidity_21d`, risk from `beta_60d`/`idiosyncratic_vol_60d`, drawdown from `max_drawdown_126d`/`pct_from_high_252d`). Each factor reads the source via `equity_price_metrics_asof` and is routed through the S7 cross-sectional operators so its rank/zscore is produced the *same way* as every other domain — the surface's own `*_cs_pct_rank` columns are used as a cross-check, not as the canonical factor. Migration **0160** seeds the catalog rows and the assembly hook.

**PIT:** (A) `available_at` carried from the bar via the as-of reader; (D) pure mapper unit-tested off a fixture frame.

**Accept:** the price/liquidity factors appear in the S7 factor catalog with declared units/signs; on a fixture slice each emits `(security_id, as_of_date, factor_id, value)` rows; the engine-produced cross-sectional rank agrees with the surface's native `momentum_21d_cs_pct_rank` within tolerance on the same cross-section.

### S9-1 — Estimate-revision + 13F-flow factors

**Root cause:** the estimate surfaces (`est_surprise`, `est_consensus`) and the 13F concentration surface (`thirteenf_concentration_metrics`) carry real signal — earnings surprise, consensus-revision momentum, institutional conviction/crowding — but in fiscal-period and quarterly-report grains with no cross-sectional standardization and no factor-catalog presence.

**Fix:** add estimate factors (standardized earnings surprise from `est_surprise`, and revision-momentum from the change in `est_consensus` snapshots) and 13F-flow factors (conviction/crowding from `value_hhi` / `top_10_holder_value_pct` / `effective_holder_count_value`, and flow from `value_hhi_change` / `holder_count_change`), each read via its as-of reader (`est_surprise_asof` / `est_consensus_asof` and the 13F as-of surface), projected onto the `(security_id, as_of_date)` grain and routed through S7 cross-sectional operators. Migration **0161** seeds the catalog rows.

**PIT:** (A) estimate `available_at` from the consensus/surprise availability; 13F `available_at` from `filing_date` (the ~45-day post-`report_period` lag), never `report_period` itself. (D) pure mappers.

**Accept:** estimate-revision and 13F-flow factors land in the catalog with correct sign (surprise/revisions higher-is-bullish; crowding sign declared and documented); a 13F factor at an `as_of_date` before its `filing_date` emits **no** row (lag respected); pure-transform tests green.

### S9-2 — Short-interest + insider factors

**Root cause:** `short_interest_metrics` (days-to-cover, short-to-ADV, period-over-period change) and `insider_transaction_metrics` (net-buy, cluster-buy, plan-sale-ratio) are strong stand-alone screens but sit in settlement-date and issuer-day grains with bespoke units and no factor-namespace membership.

**Fix:** add short-interest factors (`si_days_to_cover` / `si_short_to_adv` as crowding/squeeze pressure, `si_change_ratio` / `si_short_position_1p_change_ratio` as short-flow) and insider factors (`net_purchase_value` / `net_purchase_shares` net-buy, `is_cluster_buy` + `cluster_purchase_value` cluster signal, `plan_sale_value_ratio` as 10b5-1-adjusted sell pressure), read via each surface's as-of reader, projected onto `(security_id, as_of_date)`, and routed through the S7 operators. Migration **0162** seeds the catalog rows.

**PIT:** (A) short-interest `available_at` from the FINRA dissemination timestamp on the settlement row; insider `available_at` from the Form acceptance timestamp — both per-row, not blanket. (D) pure mappers.

**Accept:** short-interest and insider factors land with declared units/signs (days-to-cover higher-is-bearish; insider net-buy higher-is-bullish); each emits `(security_id, as_of_date, factor_id, value)` on a fixture slice; a short-interest factor respects the bi-monthly settlement availability; tests green.

### S9-3 — Unified namespace assembly + consistency gates

**Root cause:** even with all five domains catalogued, without an assembly step and a consistency gate they remain five parallel factor streams; nothing guarantees they share one key shape, one namespace, and one unit/sign discipline — which is precisely the precondition S10 needs to materialize a single panel.

**Fix:** the unified-namespace assembly in `db/factors/cross_domain.py` unions the S9-0/1/2 domain factors (alongside the S8 fundamental families already in the catalog) into ONE surface with the canonical `(security_id, as_of_date, factor_id, value)` shape, plus a consistency gate asserting: (a) every emitted `factor_id` resolves to exactly one catalog definition, (b) every factor carries a declared unit + sign in the S2 contract, and (c) no two domains collide on a `factor_id`. Migration **0163** creates the assembly surface, its indexes, and its catalog rows.

**PIT:** (A) assembled rows preserve each source factor's `available_at`; (I) the assembly ranks only within the as-of cross-section; (E)/(J) unit/sign consistency is contract-checked.

**Accept:** a single unified surface assembles across all five cross-domain domains on the proof slice; every `factor_id` is unique and catalogued with unit + sign; the consistency gate is red on a fixture with a duplicate `factor_id` or a unit/sign-less factor; the shape matches S8's fundamental-factor shape exactly.

---

## Sequencing & expected compounding

**S9-0 → S9-1 → S9-2 → S9-3.** Land the densest, best-understood domain first (price/liquidity — it already has partial cross-sectional ranks to validate the S7 operator path against), then the fiscal/quarterly-grain domains (estimates + 13F) whose PIT lags are the trickiest, then short-interest + insider, then the assembly + consistency gate that unions them. Compounding: once S9-3's unified namespace lands, the factor store holds **one** namespace spanning fundamental (S8) + cross-domain (S9) factors on a single `(security_id, as_of_date, factor_id, value)` shape — which is *exactly* what S10 materializes as `v_factor_panel` and exports to partitioned Parquet/Arrow. S9 is the last content step before the panel; getting the key shape and unit/sign discipline right here is what makes S10 a materialization rather than a reconciliation.

---

## Risks / guardrails

- **Per-domain availability lag is the sharpest hazard.** Each domain has its OWN lag and it must be PIT-correct per-domain, never a blanket assumption: 13F is a ~45-day post-`report_period` filing lag (use `filing_date`, not `report_period`); short-interest is bi-monthly FINRA settlement + dissemination; estimates are effectively real-time as consensus/surprise arrives; insider is Form-acceptance-timestamped; price is same-day bar availability. A single "available on the observation date" shortcut silently leaks the slowest domains.
- **Consistent sign convention across domains (S2).** Mixing higher-is-bullish (insider net-buy, estimate revisions) with higher-is-bearish (short-interest days-to-cover, 13F crowding) without a declared sign makes any downstream combination direction-wrong. Every factor carries its sign in the S2 semantic contract, gated by (J).
- **Do not re-architect the source modules.** The 13F concentration math, insider cluster diagnostics, short-interest feature SQL, and price analytics are landed and correct — S9 reads them through their as-of readers and *maps* them; it does not re-key, re-scale, or recompute them. Any temptation to "fix" a source unit belongs in that module's own sprint, not here.
- **Stay in 0160–0163.** No edits to any landed migration, no S7-engine or S10-panel changes; the four reserved numbers split schema / index / catalog per clause (B), each preceded by a timestamped DB+WAL backup.

---

## Bench / acceptance

- Every one of the five domains (price/liquidity, estimate-revision, 13F-flow, short-interest, insider) lands in ONE factor namespace with consistent keys and declared units/signs.
- The unified panel assembles across all domains on the proof slice into the canonical `(security_id, as_of_date, factor_id, value)` shape, matching the S8 fundamental-factor shape.
- Per-domain PIT availability is correct: a 13F/short-interest/insider factor emits no row before its source `available_at`; the lag tests are green.
- The consistency gate is green on the live namespace (unique `factor_id`, unit + sign present for every factor) and red on planted fixtures (duplicate `factor_id`, missing unit/sign).
- `python -m pytest atx-impl\db\tests\test_cross_domain_factors.py -q` green, and full `python -m pytest atx-impl\db\tests -q` green before commit.
- **Live-DB smoke** recorded in the ledger: per-domain factor row counts, unified-namespace factor count, distinct-`factor_id` count, and the `run_id`, on the operator-run proof slice.
- `db/PARITY_GAP.md` status updated (cross-domain factors now unified into one namespace); a `WAREHOUSE_PARITY_TRANCHES.md` row appended (start/end SHA, domains, verification commands, live smoke with exact counts + run_id, caveats/next → PF3-S10 panel export).

**Process:** own worktree off `main` via `atx-impl/scripts/new_db_worktree.sh new|finish sprint-9-cross-domain-factors`, merged at sprint end; controller `superpowers:subagent-driven-development` (fresh implementer + reviewer per task; TDD + verification-before-completion). Never `git add -A` (stage explicit paths); never push unless asked. New module ⇒ new `test_*.py`. Commit trailer EXACTLY `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
