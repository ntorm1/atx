# Sprint PF3-S4 — PIT universe membership + historical price backfill

**Goal:** build a point-in-time US common-equity **universe membership** surface — a governed answer to "who is in the investable set as of date D" (common equity, actively listed, passing a price/liquidity screen) — and drive a historical price-bar **backfill** (2014→present) through the PF3-S1 backfill DAG, so the warehouse carries a **dense price×fundamental overlap** instead of a proof slice. Today `equity_daily_bars` is 2012–2014 while `companyfacts` fundamentals are 2017–2026, so the PIT join the multiples and factors depend on is nearly empty — pf2-S9 valuation multiples emit almost no rows. S4 densifies the price side and lays a PIT membership screen over it, turning that near-empty overlap into a real cross-section the ratio engine (S6), the factor engine (S7–S9), and the pf2-S9 multiples can finally condition on. A **caveat**: an implicit universe already exists — `db/universes.py`'s `UniverseMembershipDataset` writes a `universe_memberships` snapshot (`us_liquid_equity_v1`, a trailing-liquidity screen). S4 does **not** duplicate it; it **reconciles** to it — extends the liquidity-only screen into a governed common-equity/listing/liquidity PIT surface, keeps the existing snapshot as a compatibility input, and never forks a second parallel universe. Reserved migrations **0140–0143**.

**Mandate / Owns:** NEW `db/universe.py` (the governed PIT membership builder + screen logic), the historical price-bar backfill wired through `db/pricing_bulk.py` **as a client of** the PF3-S1 DAG, a universe as-of reader in `db/asof.py`, and `db/tests/test_universe.py`. The overlap-density surface (the evidence that multiples/factors now have inputs) is owned here as a catalogued view.

**Must NOT touch:** the PF3-S1 backfill **engine** internals (`db/backfill.py`, the `DatasetOrchestrator` windowed/resumable machinery) — S4 is a **client** of that engine, it does not re-architect it. The factor engine (PF3-S7's `db/factors/`) is downstream and untouched. Corporate-actions / adjustment logic (`db/daily_adjustments.py`, `db/corporate_action_metrics.py`, adjusted-bar semantics) is **read**, not re-architected — S4 consumes adjusted bars, it does not redefine adjustment. The existing `db/universes.py` snapshot builder is reconciled to, not deleted or rewritten in place.

**Depends on:** PF3-S1 (the windowed/chunked/resumable backfill DAG — the price backfill **rides** it and inherits clause (H) safety); PF3-S2 (schema-contract v2 — the new membership + watermark tables land under the semantic contract); pf1's identifier spine (`security_master`, `security_ids_for_symbols` — recycled-ticker and share-class disambiguation) and pf2's pricing / corporate-actions surfaces (`equity_daily_bars`, `equity_price_metrics`, adjusted bars). **First content sprint** of PF3; runs **sequentially before** PF3-S5/S6, which share the `fundamental_*`/pricing surfaces and build directly on the dense overlap S4 produces.

---

## Baseline / where the cycles go

Measured 2026-07-04 against `atx-impl/db`. The warehouse has a *proof slice* of prices and a *fundamentals archive* that barely overlap it, plus an *implicit* universe that is a liquidity screen rather than a governed membership.

1. **`equity_daily_bars` is a proof slice, not a backfill.** It holds ≈3.18M rows for **2012–2014** across ≈8,206 securities — enough to prove the adjusted-bar pipeline (`db/pricing_bulk.py` → `equity_daily_bars` → `db/equity_price_metrics.py`) but **not** a real historical price panel. `BulkBarsOptions` even defaults `start_date=dt.date(2015, 1, 1)` — the loader exists and is chunked/idempotent, but no multi-year backfill has been run against it.

2. **The price×fundamental overlap is near-empty.** `companyfacts` fundamentals run **2017–2026**; the bars run 2012–2014. The intersection is essentially nil, which is precisely why `db/valuation_multiples.py` (pf2-S9) emits so few rows — a per-security-day multiple needs *both* a price and a fundamental on the same date, and almost no security-day has both. Every downstream cross-sectional factor inherits this emptiness: no overlap ⇒ no cross-section ⇒ no signal.

3. **Universe membership is implicit and partial, not a governed PIT screen.** `db/universes.py::UniverseMembershipDataset` (dataset_id `universe_memberships`) builds `us_liquid_equity_v1` — a *trailing-liquidity* screen (`min_price=5.0`, `min_dollar_volume=10_000_000`, `lookback_days=20`) written to the `universe_memberships` table keyed `(universe_id, security_id, effective_date)` with **REPLACE** semantics (`_replace_memberships`). It is a snapshot-per-effective-date liquidity filter, not a governed membership that also screens **share type** (common equity vs preferred/ADR/warrant/ETF) and **listing status**, and its replace-in-place write is not an interval history. A trailing-liquidity screen is a component of the answer, not the answer.

4. **The price backfill was deferred out of pf2 by the proof-slice posture.** pf2 deliberately proved *depth* on a ~1-year slice and left the multi-year historical price load for PF3 — it is called out in the PF3 design (current-state fact 3) as owned here. S4 is where that deferral is paid down.

**Already good — do not regress:**
- **Adjusted-bar semantics.** `equity_daily_bars` is split/dividend-adjusted; `db/daily_adjustments.py` / `db/corporate_action_metrics.py` own that math. S4 consumes it and must not perturb the adjustment contract.
- **The `equity_price_metrics` derived surface.** `db/equity_price_metrics.py` turns adjusted bars into the PIT analytics surface (returns, realized vol, momentum, ADV, Amihud illiquidity) the liquidity screen and later factors read. Its `available_at` discipline stays intact.
- **The corporate-action factors** and the pf2 pricing lineage generally — the backfill *feeds* these surfaces with more history; it does not change their shape.

---

## PIT / determinism + production contract

Clauses **(A)–(H)** all bear on this sprint; **(H)** is the load-bearing one for the backfill.

- **(A) No lookahead.** The universe reader gates on the membership valid window **and** `available_at ≤ as_of_ts`: a membership decision made on date D (e.g. a screen that first passes on D) is invisible to a query as-of D−1. Delisted names remain in history — the reader returns the set that was investable *as of* the query date, never the survivor set.
- **(H) Backfill-safe.** The price backfill obeys clause (H) *by construction*, because it runs through the PF3-S1 engine: windowed (per year/month partition), chunked, resumable (per-partition watermark), and idempotent (re-running a completed window is a no-op; a partial window resumes without duplication). S4 supplies the windows and the source; the engine supplies the safety.
- **(D) Determinism.** The membership screen is a pure `compute_*` transform (bars/metrics/security-master in → long membership DataFrame out), unit-tested without DuckDB; same inputs + same screen params → same intervals.
- **(B)/(F) Migrations.** Strictly **0140–0143**, schema/index/view split per the pf1-S5g/S5k WAL precedent, each new table/view catalogued in the same migration, DB+WAL backup before any live apply:
  - **0140** — `universe_membership` table (the governed interval-keyed PIT surface) + its `table_catalog`/`field_catalog` seed.
  - **0141** — price-backfill **watermark / partition-metadata** table (per year/month partition progress the S1 engine reads/writes for this backfill) + catalog.
  - **0142** — the **overlap / coverage** view (`v_price_fundamental_overlap`) proving the join is now dense.
  - **0143** — **indexes** on the membership + watermark tables and any catalog rows.

---

## Tasks

### S4-0 — PIT common-equity universe membership

**Root cause:** the only universe today is `db/universes.py`'s trailing-liquidity snapshot (`us_liquid_equity_v1`) keyed `(universe_id, security_id, effective_date)` with REPLACE semantics — it screens liquidity but not **share type** or **listing status**, and it stores per-date snapshots rather than a membership *history*, so there is no governed, interval-keyed answer to "who was investable as of D."

**Fix:** NEW `db/universe.py` building a governed `universe_membership` table keyed `(security_id, valid_from, valid_to)` — an **interval** history, not a per-date snapshot — where a row asserts "security S was a universe member from `valid_from` to `valid_to`." Membership criteria, each a pure predicate: **common-equity share type** (exclude preferred/ADR/warrant/unit/ETF via the identifier-spine security type), **active listing** (listed, not yet delisted as-of the interval), and a **price/liquidity screen** (reusing the `equity_price_metrics` ADV/price signals the existing `us_liquid_equity_v1` screen already computes, so the liquidity logic is shared, not re-implemented). **Reconcile** to `universe_memberships`: the existing snapshot becomes a compatibility input / component (its liquidity verdicts feed the screen), and the two surfaces are explicitly related in the catalog — no second parallel universe is forked. Migration **0140**.

**PIT:** (A) intervals are as-of; a name that delists closes its interval at the delist date and stays in history (no survivorship). (D) the screen is a pure transform, unit-tested without DuckDB.

**Accept:** `universe_membership` emits interval rows over the slice; a security that fails the common-equity screen (e.g. a preferred/ADR) is absent while its common-equity sibling is present; a delisted name is present for dates before its delist and absent after; re-running the builder on the same inputs reproduces identical intervals.

### S4-1 — Historical price-bar backfill through the S1 DAG

**Root cause:** `equity_daily_bars` is a 2012–2014 proof slice; the loader (`db/pricing_bulk.py`, source `bulk_bars_2015plus`, `security_ids_for_symbols` resolution, 200k-row chunks) is idempotent but has never been driven over the full window, so the price panel does not overlap the 2017–2026 fundamentals.

**Fix:** wire a **windowed 2014→present** backfill of daily bars through the PF3-S1 backfill DAG, with `db/pricing_bulk.py` as the **injectable source** (the archive path stays behind a file option per clause C — no vendor network in tests). Partition by **year/month**; record a **per-partition watermark** (migration **0141**) so the engine can skip completed partitions and resume partial ones. **PROVE** resumability + idempotency on a **bounded slice** in-module (a few partitions from fixture bars): a completed window re-runs as a no-op, a partial window resumes without duplicating rows. **Do not** run the full multi-year archive backfill in-module — that is the operator job (clause H / data-posture); the module proves the mechanism, the operator runs the archive.

**PIT:** (H) windowed/chunked/resumable/idempotent via the S1 engine; per-partition watermark is the resume anchor. (C) offline — fixture/injected bars, no network.

**Accept:** the backfill runs over a bounded fixture slice, writes bars, and records per-partition watermarks; a second run over the same partitions is a no-op (0 net new rows); a deliberately interrupted partition resumes to completion without duplicate `(security_id, trade_date)` rows; the full-archive run is documented as operator-run, not executed in pytest.

### S4-2 — Universe-as-of reader + dense overlap surface

**Root cause:** even with intervals landed, callers need a single PIT reader ("the universe as of D"), and the whole sprint's justification — that the overlap is now dense — must be *queryable evidence*, not an assertion.

**Fix:** add `universe_membership_asof(as_of_date, …, store=None)` to `db/asof.py`, extending the existing `universe_asof` (`asof.py:1553`) pattern — read-only connect, valid-window + `available_at ≤ as_of_ts` gating, no lookahead — resolving the interval history to the membership set as-of a date. Add an **overlap-density** surface, `v_price_fundamental_overlap` (migration **0142**), a catalogued view over `equity_daily_bars`/`equity_price_metrics` × the fundamentals surface × `universe_membership`, exposing, per period, the count of universe security-days that have **both** a price and a fundamental — the direct proof that multiples (pf2-S9) and factors (S7–S9) now have inputs.

**PIT:** (A) the reader gates on availability; an as-of date before an interval's `available_at` excludes it. (B) 0142 view catalogued with its own `table_catalog`/`field_catalog` rows.

**Accept:** `universe_membership_asof` returns the correct as-of set (delisted names included historically, excluded after delist; screen failures excluded); `v_price_fundamental_overlap` shows a materially non-empty overlap over the backfilled window on the slice, and a sample `valuation_multiples` / price-conditioned factor computation emits real rows where before it emitted ~0.

### S4-3 — Universe coverage + quality gates + catalog

**Root cause:** a membership surface silently drifts if nothing asserts that every security which *has* a price and a fundamental also has a **membership decision** (in-or-out), and the sprint's invariants need to be gate-ready for PF3-S12's orchestrator.

**Fix:** register a **coverage** check in `db/quality.py` reusing the existing `QualityResult`/`_table_exists` machinery: every security that appears in **both** the priced and the fundamental surfaces over the window has an explicit `universe_membership` decision (member or screened-out with a `reason`), i.e. no priced-and-fundamental security is *undecided*. Author it `severity=critical` so PF3-S12 can gate it (clause G, incrementally). Add the membership + watermark **indexes** and any residual catalog rows (migration **0143**).

**PIT:** (C) fixtures with a planted priced+fundamental security lacking a membership row (red) and a fully-decided fixture (green). (G) check authored gate-ready.

**Accept:** the coverage check is green on the slice (0 undecided priced+fundamental securities) and red on a fixture with a planted undecided security; indexes present; every new table/view carries its catalog rows; existing `quality.py` checks unaffected.

---

## Sequencing & expected compounding

**S4-0 → S4-1 → S4-2 → S4-3.** S4-0 lays the governed interval membership (the *who*); S4-1 lands the price history (the *what* the who is screened on and the *what* fundamentals join against); S4-2 exposes both as an as-of reader and proves the overlap is dense; S4-3 gates coverage so the surface can be trusted downstream. The compounding is the whole point of PF3's content wave: **a dense price×fundamental overlap plus a PIT universe is the precondition** for pf2-S9 multiples to finally emit real rows, for the S6 ratio engine to have security-days to compute over, and for every S7–S9 cross-sectional factor to have a non-empty cross-section to rank within. Without S4, the factor store is a correct engine over an empty panel; with S4, it has real inputs.

---

## Risks / guardrails

- **Do not execute the full multi-year archive backfill in-module.** Prove resumability + idempotency on a bounded fixture slice; the full 2014→present archive load is a documented, resumable **operator** job (clause H / data-posture). Running the archive in pytest would violate clause C and the proof-slice posture.
- **Survivorship / lookahead is the classic universe trap.** Membership must be **as-of**: it must include names that were investable *historically* even if they later delisted, and it must never leak a future screen verdict backward. Building the universe only from currently-listed names would silently survivorship-bias every downstream backtest. The interval `(valid_from, valid_to)` history + the `available_at`-gated reader are the mitigation.
- **Recycled-ticker / share-class collisions.** A ticker reused across two issuers, or two share classes of one issuer, must be disambiguated by the **pf1 identifier spine** (`security_master`, `security_ids_for_symbols`) — the same precedent `db/pricing_bulk.py` already relies on — not re-solved here. Membership is keyed on `security_id`, never on `symbol`.
- **Reconcile, don't duplicate.** The existing `universe_memberships` snapshot and `universe_asof` reader stay; S4 extends them into the governed interval surface + `universe_membership_asof`. Do not fork a second, divergent universe definition.
- **Stay in the reserved range.** Migrations strictly **0140–0143**, schema/index/view split; never edit a landed migration; back up DB+WAL before any live apply (clause F).

---

## Bench / acceptance

- `universe_membership` is **PIT-queryable**: `universe_membership_asof` returns the correct as-of set with delisted names included historically and screen-failures excluded; a no-lookahead test on the reader is green.
- The price backfill is **resumable + idempotent on the slice**: a completed window re-runs as a no-op, a partial window resumes without duplicate `(security_id, trade_date)` rows, per-partition watermarks recorded.
- The **overlap is dense** enough that a sample multiple/factor emits real rows — `v_price_fundamental_overlap` shows a materially non-empty price×fundamental intersection over the window, and a sample pf2-S9 `valuation_multiples` / price-conditioned factor emits where it previously emitted ~0.
- Coverage check green on the slice (0 undecided priced+fundamental securities), red on the planted fixture; authored `severity=critical`, gate-ready for PF3-S12.
- `python -m pytest atx-impl\db\tests\test_universe.py -q` green, and full `python -m pytest atx-impl\db\tests -q` green in the worktree before commit.
- **Live-DB backfill smoke** is **operator-run** against the shared DB in the primary tree (backed up first, clause F), with pre/post `equity_daily_bars` row counts, backfilled partition count, membership row count, overlap density, and the `run_id` **recorded in the ledger** — the archive is not run in pytest.
- `PARITY_GAP.md` status updated (price backfill / PIT universe), and a `WAREHOUSE_PARITY_TRANCHES.md` row appended (start/end SHA, domains, verification commands, live smoke with exact counts + run_id, caveats/next → PF3-S5 fundamentals completeness).

**Process:** sprint runs in its own git worktree off `main` via `atx-impl/scripts/new_db_worktree.sh new|finish <slug>`; controller `superpowers:subagent-driven-development` (fresh implementer + reviewer per task; TDD + verification-before-completion). Never `git add -A` (stage explicit paths); never push unless asked. Commit trailer EXACTLY `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
