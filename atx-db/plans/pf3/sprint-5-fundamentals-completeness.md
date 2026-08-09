# Sprint PF3-S5 — Fundamentals completeness (valuation inputs the factor store needs)

**Goal:** close the remaining **CORE** parity gaps the factor store depends on — **float & treasury shares** plus
basic/diluted share classes, the **enterprise-value components** (market cap + total debt + preferred + minority
interest − cash & equivalents), and **observed DLRET terminal delisting returns** — so that every canonical valuation
input is populated on the proof slice and **EV is computable per security-day** with full lineage. This sprint does not
build ratios or factors; it completes the *inputs* those engines read. Scope is deliberately narrow — CORE fundamentals
+ valuation inputs only; ESG, supply-chain, and licensed-estimate feeds stay parked (PF4+). Reserved migrations
**0144–0147**.

**Mandate / Owns:** `db/shares_outstanding.py` (float/treasury + share-class extension), NEW `db/enterprise_value.py`
(the per-security-day EV surface + as-of reader), `db/delisting.py` (populate observed DLRET terminal returns through
the existing injectable `delisting_return_observations` loader), `db/tests/test_enterprise_value.py`.

**Must NOT touch:** the ratio/metric **ENGINE** — PF3-S6 (`db/fundamental_ratios.py`, NEW `db/metric_engine.py`)
consumes these inputs and owns EV/EBITDA, P/E, and the valuation catalog; do not compute ratios here. Do not touch the
**factor engine** — PF3-S7 (`db/factors/`). The **standardization engine** (pf2-S3) is *read-only* here — extend it only
via new canonical item→concept mappings if a required EV component is not yet standardized, never by editing its rule
surface. Do not edit any landed migration (≤ 0143) or another sprint's reserved region.

**Depends on:** **PF3-S4** (dense PIT universe + historical price backfill → the market-cap leg of EV has real
price×shares overlap), **pf2-S3** (standardized `fundamental_*` items → total debt, preferred, minority interest, cash
pulled by canonical name), and **pf2-S9** (the valuation-multiples scaffold that currently emits few rows and will
consume the completed EV surface). Sequential **after PF3-S4, before PF3-S6** — all three touch the shared
`fundamental_*` / pricing surfaces and must run one-at-a-time in the same tree.

---

## Baseline / where the cycles go

The valuation half of the fundamentals warehouse is thin: share counts exist but only in their simplest form, EV has no
home, and the terminal-return surface is a schema with no rows. Measured 2026-07-04 against `atx-impl/db` and
`db/PARITY_GAP.md`.

1. **`shares_outstanding_history` is thin — ~342 rows, no float/treasury split.** `PARITY_GAP.md` line 198 records the
   surface as `shares_outstanding_history` (**342**) ← public SEC XBRL `fundamental_statement_points`, carrying only
   `shares_outstanding`, `shares_basic_avg`, `shares_diluted_avg` (loader `shares_outstanding.py`, `SHARE_COUNT_METRICS =
   ("shares_outstanding", "shares_basic_avg", "shares_diluted_avg")`). Line 206 pins the gap explicitly: *"it is not yet a
   CRSP daily `SHROUT` equivalent and does not cover **float, treasury shares**, exchange-sourced daily shares, or
   split-factor-integrated daily market-cap restatements."* Float (shares available to trade, excluding closely-held) and
   treasury (repurchased, non-outstanding) are the two legs a correct market cap and a correct EV both need, and neither is
   present.

2. **No `enterprise_value` surface — EV components are scattered and EV is not computable.** There is no table that
   assembles EV per (security_id, as_of_date). Its inputs live in different places — market cap wants price (Domain 6,
   dense only after S4) × diluted shares (`shares_outstanding_history`); total debt, preferred stock, minority interest,
   and cash & equivalents are standardized `fundamental_*` items (pf2-S3) — but nothing joins them into a single
   lineage-traced EV row. The design record confirms this: *"Float/treasury shares, EV components, observed DLRET terminal
   returns, and some share-class detail are Partial/Missing"* and *"EV computable per security-day"* is the sprint's own
   goal metric. This is also *why pf2-S9's valuation multiples emit so few rows* — EV/EBITDA has no denominator.

3. **Observed DLRET terminal returns are Missing — the surface exists but holds zero rows.** `PARITY_GAP.md` line 182
   describes `delisting_return_observations` as *"an injectable observed-return surface for CRSP-like
   `DLRET`/`DLRETX`/`DLAMT`/`DLPRC` rows"* with `delisting_events_asof` enriching events only with observations visible at
   the query timestamp — and `delisting.py` already carries `DelistingReturnObservationOptions(source_file,
   provider="INJECTED", vendor_security_id_type="PERMNO")`. But line 186 is blunt: *"No populated observed **`dlret`**
   terminal-return evidence in the default DB; Shumway-Warther-style -30% unresolved-delete imputation exists only as an
   opt-in research policy, not as a claimed source value,"* and line 200 confirms *"the default DB has none loaded."*
   Tranche 5 (line 287) reads: *"Public delisting proxy plus injectable observed-return surface exists; no populated
   DLRET."* An unpopulated terminal-return surface biases every survivorship and return study downstream — delisted names
   simply vanish rather than realizing their (often catastrophic) final return.

4. **Share-class detail is partial.** `shares_outstanding_history` tracks basic/diluted *averages* but does not distinguish
   multiple common-share classes (A/B/C) or reconcile per-class outstanding to a consolidated count — a gap for dual-class
   issuers where the traded class is not the whole float.

**Already good — do not regress:**

- **PIT SEC XBRL share counts.** `refresh_shares_outstanding_history` materializes share counts from normalized
  statement points with `effective_date`/`as_of_date`/`available_at`, accession + revision metadata,
  `is_latest_revision`, an as-of reader, watermarks, and quality checks. S5-0 *extends* this loader with new metrics;
  it does not rewrite the existing three.
- **Adjustment factors.** `adjustment_factor_history` + `daily_adjustment_factors` (event-level and daily PIT
  split/total-return factors, reconciled in `corporate_action_factor_reconciliation`) stay untouched — the EV
  market-cap leg reads adjusted price through the existing S4/Domain-6 surfaces.
- **The delisting evidence surface.** `delist_code_dim` + `delisting_events` (PIT public delisting evidence from
  `listing_status_intervals`, nullable `delisting_return`, explicit `is_return_imputed`, `return_policy`) and the
  injectable `delisting_return_observations` loader + `delisting_events_asof` reader are correct as designed. S5-2
  *populates* observations through the existing loader; it does not redesign the surface or promote the opt-in −30%
  imputation into a claimed source value.

---

## PIT / determinism + production contract

Shared clauses **(A)** bitemporal/no-lookahead, **(B)** append-only catalogued migrations, **(C)** offline/no-network
tests, **(D)** determinism + provenance, **(E)** schema-as-contract, and **(J)** semantic contract all apply in full.

- **(J) Semantic contract.** Every new column declares its unit, sign, and scale per PF3-S2's contract: share counts are
  **shares ≥ 0** (float ≤ shares_outstanding as an invariant; treasury ≥ 0), and the EV components carry explicit signs —
  **cash & equivalents SUBTRACTS**, debt/preferred/minority-interest ADD. A check fails if a value violates its declared
  unit/sign domain.
- **(A) Bitemporal.** Float/treasury rows inherit `available_at` from their source filing exactly like the existing share
  counts; each EV row sets `available_at = max(input.available_at)` across its price, share, and fundamental legs;
  observed DLRET rows set `available_at` to the **delisting-confirmation** timestamp (never the delisting event date) so
  no terminal return is visible before it was knowable.
- **(B) Migrations.** Schema split from index across the reserved range: **0144** float/treasury share metrics +
  share-class detail (+ its `table_catalog`/`field_catalog` seed); **0145** `enterprise_value` table + as-of view;
  **0146** observed-DLRET population support (any dimension/index the injectable loader needs); **0147** the
  completeness-coverage catalog + indexes. Never renumber or edit ≤ 0143; timestamped DB+WAL backup before any live apply.
- **(C)/(D)** All `compute_*` transforms are pure (pandas in → long DataFrame out), unit-tested independent of DuckDB; the
  DLRET loader stays behind an injectable `source_file` — no CRSP/vendor network in pytest, live smoke operator-run and
  ledgered.

---

## Tasks

### S5-0 — Float & treasury shares + share classes

**Root cause:** `shares_outstanding_history` covers only current/basic-avg/diluted-avg counts (`SHARE_COUNT_METRICS`,
342 rows); float, treasury, and per-class outstanding — the inputs a correct market cap and EV need — are absent
(`PARITY_GAP.md` line 206).

**Fix:** extend `db/shares_outstanding.py` to materialize float shares, treasury shares, and per-common-class
outstanding alongside the existing counts, sourced PIT from the standardized statement points (pf2-S3 item mappings; add
mappings for `TreasuryStockShares`/entity-public-float concepts only if missing). New `share_count_type` values
(`float`, `treasury`, `class_a`/`class_b`…) extend the existing enum without disturbing the three landed metrics.
Migration **0144** adds the columns/rows + catalog seed. Enforce the invariants: `treasury ≥ 0`, `float ≤
shares_outstanding`, all counts ≥ 0.

**PIT:** float/treasury rows inherit `available_at`, accession, revision metadata, and `is_latest_revision` from source
exactly like the current metrics; the as-of reader returns them without lookahead.

**Accept:** on the proof slice, float and treasury populate for issuers that disclose them; the invariants hold; the
existing three metrics and their row counts are unchanged; `refresh_shares_outstanding_history` remains idempotent.

### S5-1 — Enterprise value engine *(the big one)*

**Root cause:** EV has no surface — its components are scattered across price × shares (Domain 6) and standardized
`fundamental_*` items, and nothing assembles them, so EV is not computable and pf2-S9's EV/EBITDA emits almost nothing.

**Fix:** NEW `db/enterprise_value.py` computing **EV = market cap + total debt + preferred equity + minority interest −
cash & equivalents** per (security_id, as_of_date), where market cap = PIT adjusted price (S4-dense Domain 6) × diluted
shares (S5-0), and debt/preferred/minority/cash are pulled by canonical name from standardized items. Materialize an
`enterprise_value` table (migration **0145**, + as-of view) with each component stored as its own signed, unit-tagged
column **and** the assembled EV, every row **lineage-traced** to the exact source items/formula/vintage that produced
it. Ship a pure `compute_enterprise_value(...)` transform, an as-of reader mirroring the
`formula_registry_asof`/`delisting_events_asof` pattern, watermarks, and quality checks.

**PIT:** `available_at = max(input.available_at)` across the price, share, and fundamental legs; the as-of reader gates
on the valid window **and** `available_at ≤ as_of_ts`; cash strictly subtracts (sign enforced by the S2 semantic
contract).

**Accept:** EV computes on the proof slice for every security-day with a price × diluted-share × the required
fundamental components; each EV row resolves its full component lineage; a fixture with a known price/share/debt/cash
set returns the hand-computed EV; sign discipline is unit-tested (flipping cash's sign fails the semantic check).

### S5-2 — Observed DLRET terminal returns

**Root cause:** `delisting_return_observations` is a correct injectable surface but holds **zero rows** in the default
DB (`PARITY_GAP.md` lines 186/200/287); downstream survivorship/return studies see delisted names vanish rather than
realize their final return.

**Fix:** populate observed DLRET terminal returns through the existing injectable loader in `db/delisting.py`
(`DelistingReturnObservationOptions`, `source_file`, `provider`, `vendor_security_id_type="PERMNO"`), landing CRSP-like
`DLRET`/`DLRETX`/`DLAMT`/`DLPRC` rows from an operator-provided file into `delisting_return_observations`, with
security-id resolution to warehouse `security_id`. Migration **0146** adds any dimension/index the population needs.
`delisting_events_asof` continues to enrich events only with observations visible at the query timestamp — no change to
that reader's contract. The opt-in Shumway–Warther −30% imputation stays a research policy, never a claimed source
value.

**PIT:** observed rows set `available_at` to the **delisting-confirmation** timestamp (no lookahead to the event date);
resolution to `security_id` respects the identifier-history `available_at`.

**Accept:** on an injected fixture file, observed DLRET rows land and `delisting_events_asof` surfaces them for events
whose confirmation ≤ as-of; with no file injected the surface stays empty and nothing regresses; no imputed value is
ever labelled as observed.

### S5-3 — Completeness gates + coverage catalog

**Root cause:** nothing asserts that *every* canonical valuation input is actually populated on a slice, so a
silently-empty component (e.g. preferred equity never mapped) would leave EV quietly under-computed with no signal — and
the sprint's own goal is "no core-item stubs remain."

**Fix:** register a gated completeness check (severity=critical, clause G, gate-ready for PF3-S12) that, over the
proof-slice window, asserts each canonical valuation item (float, treasury, diluted shares, market cap, total debt,
preferred, minority interest, cash, EV) is populated for the universe cross-section, plus a **core-item stub detector**
that flags any canonical item whose table exists but is effectively empty. Migration **0147** adds the coverage catalog
+ indexes. The check reads the canonical item list as data, not a hardcode.

**PIT:** pure read over the populated surfaces + universe membership → deterministic rows; the stub detector is a
count/coverage read, no network.

**Accept:** the completeness gate is green on the populated slice and red on a fixture where one canonical component is
stripped; the stub detector fires on a schema-present/zero-row table and stays quiet on a populated one.

---

## Sequencing & expected compounding

**S5-0 → S5-1 → S5-2 → S5-3.** Float/treasury and share classes land first because the EV market-cap leg needs a correct
diluted-share denominator. The EV engine then assembles the full component set into a lineage-traced per-security-day
surface. Observed DLRET lands next — orthogonal to EV but part of "complete valuation inputs." The completeness gate
closes last, once every input is populated, so it verifies a real slice rather than crying wolf. **Compounding:**
completing these inputs unblocks the **FULL ratio/metric catalog (PF3-S6)** — valuation ratios finally have denominators
— and the **value/quality factor families (PF3-S8)**; it makes **pf2-S9's EV/EBITDA emit real rows** at last; and it
removes the survivorship bias that would otherwise silently corrupt every return-based factor evaluated in PF3-S11.

---

## Risks / guardrails

- **Float ≠ shares outstanding — do not conflate them.** Float excludes closely-held/restricted shares; treating
  shares_outstanding as float overstates tradable size and market cap. Store them as distinct `share_count_type` rows with
  the `float ≤ shares_outstanding` invariant enforced.
- **EV sign discipline — cash SUBTRACTS.** The classic EV bug is adding cash (or dropping its sign). Enforce it
  structurally via the PF3-S2 semantic contract (cash column declared negative-contribution) plus a unit test that fails
  on a flipped sign — not by convention alone.
- **Observed DLRET must be PIT.** `available_at` = delisting-confirmation, never the event date; no terminal return may be
  visible before it was knowable, or the survivorship fix itself becomes a lookahead leak. Keep imputed and observed
  strictly separated — the −30% policy is research-only, never a source value.
- **Stay in lane 0144–0147.** No ratios (PF3-S6), no factors (PF3-S7); extend the standardization engine only via additive
  item mappings; never edit a landed migration or another sprint's region.

---

## Bench / acceptance

- Float and treasury shares populated on the proof slice with the share-count invariants holding; the three landed metrics
  and their counts unchanged.
- **EV computable and lineage-traced per security-day** on the dense (S4) slice; a fixture returns the hand-computed EV;
  cash-sign discipline unit-tested and contract-enforced.
- Observed DLRET terminal returns populated from an injectable file; `delisting_events_asof` surfaces them PIT-safely;
  empty-by-default with no file injected; no imputed value labelled observed.
- Completeness gate green on the populated slice, red on a stripped-component fixture; core-item stub detector correct
  both ways.
- `python -m pytest atx-impl\db\tests\test_enterprise_value.py -q` green, and full `python -m pytest atx-impl\db\tests -q`
  green in the worktree before commit.
- **Live-DB smoke** recorded in the ledger: float/treasury/share-class row counts, EV rows computed + component-coverage
  counts on the slice, observed-DLRET rows injected, completeness-gate result, and the `run_id`.
- `PARITY_GAP.md` updated (Domain-6 share-count-limits + Domain-5 DLRET rows flipped to reflect the new coverage; EV
  surface added); a `WAREHOUSE_PARITY_TRANCHES.md` row appended (start/end SHA, domains, verification commands, live smoke
  with exact counts + run_id, caveats/next → PF3-S6 ratio catalog consumes EV).

**Process:** own git worktree off `main` via `atx-impl/scripts/new_db_worktree.sh new|finish <slug>`; controller `superpowers:subagent-driven-development` (fresh implementer + reviewer per task; TDD + verification-before-completion). Never `git add -A` (stage explicit paths); never push unless asked. Commit trailer EXACTLY `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`. New module ⇒ new `test_*.py`; operator live-DB smoke runs against the shared DB in the primary tree, backed up first (clause F).
