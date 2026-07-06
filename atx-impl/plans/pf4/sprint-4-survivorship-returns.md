# Sprint PF4-S4 — Survivorship-safe returns (delisting-return stitching for backtest forward returns)

**Track:** Data correctness (pf4 Track B). **Reserved migrations 0185–0188.** Successor to **PF4-S3** (S1–S10 hardening),
predecessor to **PF4-S5** (multi-universe). Runs **sequentially inside the Track-B group** (PF4-S4 → PF4-S5 → PF4-S6) —
all three touch the shared `fundamental_*` / pricing / universe / delisting surfaces and must run one-at-a-time in the
same worktree.

**Goal:** make backtest **forward returns survivorship-bias-free**. Today a security that delists inside a forward-return
horizon window is silently *dropped* from the surviving-only panel — the name vanishes rather than realizing its (often
catastrophic) terminal return, and IC / decile / long-short spread arithmetic (PF4-S1 signal-evaluation) is computed over a
survivor-biased cross-section. This sprint (1) **populates observed DLRET** (terminal delisting returns) per delisted
security-day from an **injectable** vendor/manual file and reconciles **DLSTCD** (the vendor delisting-reason code) against
the warehouse's own public delisting detection; (2) **stitches** the terminal DLRET into the forward-return series so a
name that delists mid-horizon carries its realized terminal return instead of a NaN-drop; (3) codifies a **deterministic
spinoff/merger terminal-return policy** for corporate-action-driven terminals; and (4) registers a **survivorship-bias DQC**
(`severity=critical`, gate-ready) that flags any forward-return panel dropping delisted names without a terminal return.

This sprint does **not** build the signal-evaluation surface (PF4-S1 owns `db/signal_eval.py` and the raw forward-return
builder); it makes that surface's inputs survivorship-safe. Scope is deliberately narrow — the terminal-return leg only.
IBES-licensed / international-IFRS / ESG feeds stay parked (pf5).

**Mandate / Owns:**
- `db/delisting.py` — extend with the observed terminal-return catalog builder, the **DLSTCD reconciliation** transform,
  the deterministic **spinoff/merger terminal-return policy** applier, and the pure **survivorship-safe forward-return
  stitching** transform + its refresh function. Do **not** redesign the existing `delisting_return_observations` loader,
  `refresh_delisting_events`, or `delisting_events_asof` — reuse them.
- NEW `db/migrations/bodies_0185_0188.py` (registered in `db/migrations/registry.py`).
- NEW `db/tests/test_delisting_returns.py`.

**Must NOT touch:**
- The **PF4-S1 signal-evaluation surface** (`db/signal_eval.py`) — S4 *reads* its raw forward-return output through a
  reconciled source name and *produces* a survivorship-safe replacement; it does not edit the evaluator. Where PF4-S1's
  landed forward-return table/view name differs from the placeholder `signal_forward_returns` used below, the S4 implementer
  reconciles to the landed name in **one** module-level constant (`RAW_FORWARD_RETURN_SOURCE`) and nowhere else.
- The `delisting_return_observations` loader surface, `delist_code_dim`, `delisting_events`, and the two asof readers
  (`delisting_events_asof`, `delisting_return_observations_asof`) — read-only here; extend only additively.
- Any landed migration (≤ 0184) or another sprint's reserved region. S4 appends **only** 0185–0188.
- The opt-in Shumway–Warther −30% imputation stays a research policy; it is **never** written as an observed or
  policy-sourced terminal return.

**Depends on:** **PF3-S5** (the `delisting_return_observations` injectable surface + `crsp_dlstcd` column + `delisting_events`
public proxy + `delisting_events_asof` observed-return enrichment — all landed), **PF4-S1** (the raw forward-return series the
stitching splices into; landed before S4 in the sequence), and the S4-dense price surface (`equity_price_metrics`,
`daily_return`/`adjusted_close`) that supplies the pre-delist partial return.

---

## Baseline / where the cycles go

Measured 2026-07-06 against `atx-impl/db`, `db/PARITY_GAP.md`, and the migration bodies.

1. **`delisting_return_observations` is a correct, empty ingestion surface.** `db/delisting.py` carries
   `DelistingReturnObservationOptions(source_file, provider="INJECTED", vendor_security_id_type="PERMNO")`,
   `load_delisting_return_observations`, `normalize_delisting_return_observations`, and vendor-id resolution through
   `security_identifier_history`. The table's `OBSERVATION_COLUMNS` already include `delisting_return` (DLRET),
   `delisting_return_ex_div` (DLRETX), `crsp_dlstcd` (**DLSTCD**), `vendor_delist_code`, `delist_amount`, `delist_price`,
   `successor_security_id`/`successor_vendor_security_id`, and PIT `delist_date`/`as_of_date`/`available_at`. `PARITY_GAP.md`
   lines 200/215/302 are blunt: *"the default DB has none loaded"* and *"live DLRET ... remain absent."* So the *ingestion
   surface* exists but there is **no per-delisted-security-day terminal-return catalog**, **no DLSTCD reconciliation**, and —
   critically — **the terminal return is never fed back into the forward-return series a backtest consumes.**

2. **`delisting_events_asof` already stitches observed DLRET into the *event* row, but nothing stitches it into a
   *forward-return* series.** `DELISTING_EVENTS_ASOF_SQL` (`db/asof/security.py`) coalesces the latest visible observation
   into `delisting_return` and sets `delisting_return_type='OBSERVED_SOURCE'`, `return_policy='observed_source'`,
   `return_confidence='high'` (test `test_observed_delisting_return_observations_enrich_asof_without_lookahead`). This is the
   **event-grain** terminal return. The **return-grain** survivorship fix — splicing that terminal return into the forward
   window a signal is scored over — does not exist. PF4-S1's forward-return builder is surviving-only: a name with no bar
   after its delist date produces a NULL/absent forward return and is dropped from the cross-section.

3. **No corporate-action terminal-return policy.** `db/corporate_actions.py` infers cash dividends only
   (`action_type='cash_dividend_inferred'`); there is no deterministic rule mapping a **merger** (cash/stock consideration →
   realized terminal return) or a **spinoff** (parent + spun child combined return via `successor_security_id`) to a terminal
   return. Merger/acquisition delists therefore also vanish, and the choice of terminal return is undocumented and
   non-reproducible.

4. **No survivorship gate.** There is no `severity=critical` check asserting that a forward-return panel does not drop
   delisted names. A survivor-biased panel would flow into PF4-S1 IC/decile arithmetic and PF4-S2 gating with no signal that
   the cross-section is biased.

**Already good — do not regress:**
- The injectable observed-return loader + PIT vendor-id resolution + `available_at`=delisting-confirmation discipline
  (`test_observed_delisting_returns_resolve_vendor_identifier_without_lookahead`). S4 **consumes** this loader; it does not
  rewrite it.
- `delisting_events_asof` / `delisting_return_observations_asof` PIT contracts and the `OBSERVED_SOURCE` event enrichment.
- `crsp_dlstcd` normalization (`COLUMN_ALIASES['dlstcd'] → 'crsp_dlstcd'`) and the `delist_code_dim` public-proxy dimension
  with `reason_category` — the reconciliation reads both, never edits them.

---

## PIT / determinism + production contract

Shared clauses **(A)** bitemporal / no-lookahead, **(B)** append-only catalogued migrations, **(C)** offline / no-network
tests, **(D)** determinism + provenance, **(E)** schema-as-contract, **(G)** quality-gated, **(I)** panel PIT-safety, and
**(J)** semantic contract all apply in full.

- **(A) + (I) No-lookahead — the load-bearing invariant.** A terminal delisting return is available **only at/after the
  delisting-confirmation timestamp**, never the delist event date. Every observed terminal-return row inherits
  `available_at` from `delisting_return_observations.available_at` (the confirmation/load timestamp). Every stitched
  forward-return row sets `available_at = max(raw_forward_return.available_at, terminal_return.available_at)` — so a reader
  gating on `available_at ≤ as_of_ts` cannot see the spliced terminal return before the window closes and the terminal was
  knowable. The stitching attributes the realized return to the panel **formation date** (`as_of_date`) but the row is not
  *visible* until its window resolves. A regression test plants a terminal return whose `available_at` is after the query
  timestamp and asserts it does not leak.
- **(B) Migrations.** Schema split from index across the reserved range: **0185** observed terminal-return catalog +
  DLSTCD-reconciliation table + terminal-return policy dimension (+ their `dataset_catalog`/`table_catalog`/`field_catalog`
  seeds); **0186** the `forward_returns_survivorship_safe` table + `v_forward_returns_survivorship_safe` view (+ catalog);
  **0187** indexes only; **0188** the survivorship coverage view + `quality_check_registry` rows. Forward-only, idempotent
  (`CREATE TABLE IF NOT EXISTS`, `INSERT OR REPLACE` catalog rows). **Catalog every new table/view in the same migration.**
  Never renumber or edit ≤ 0184; timestamped DB+WAL backup before any live apply (clause F, operator step).
- **(C) + (D) Offline + determinism.** Every `compute_*` transform is pure (pandas in → long DataFrame out), stable-sorted,
  unit-tested independent of DuckDB; the DLRET loader stays behind the existing injectable `source_file` — **no CRSP/vendor
  network in pytest**. Same inputs + params → byte-identical rows (a row-order-shuffle property test proves it).
- **(E) Schema-as-contract.** No new table lands without a `table_catalog` + `field_catalog` entry seeded in the same
  migration; the drift check fails on any uncatalogued table. Catalog `forward_returns_survivorship_safe`,
  `delisting_terminal_returns`, `delisting_code_reconciliation`, `terminal_return_policy_dim`, and both views.
- **(G) Quality-gated.** The survivorship-bias check is registered `severity='critical'`, `enabled=true`, `failure_status='failed'`
  in `quality_check_registry` — gate-ready for the PF4-S2 orchestrator halt gate. The DLSTCD-reconciliation coverage check is
  registered `severity='error'` (surfaces mismatches without necessarily halting).
- **(J) Semantic contract.** Terminal / forward returns are **fractions** (unit `fraction`, sign `signed`, scale `1`);
  `terminal_return` and `forward_return` may be negative (a −1.0 terminal = total loss). `crsp_dlstcd` is an integer code
  (categorical). A semantic check fails if a return column is stored outside its declared fraction domain (e.g. a percent).

---

## Tasks

### S4-0 — Observed DLRET terminal-return catalog per delisted security-day + DLSTCD reconciliation

**Root cause:** the injectable `delisting_return_observations` surface holds the raw vendor rows, but there is no
**per-delisted-security-day terminal-return catalog** the stitching and DQC can read uniformly, and no reconciliation of the
vendor **DLSTCD** (`crsp_dlstcd`) against the warehouse's own public `delist_code` detection — so a vendor "merger" (DLSTCD
2xx) silently disagreeing with the warehouse "exchange delete" proxy is invisible.

**Fix:** in `db/delisting.py` add:

- `compute_delisting_terminal_returns(observations: pd.DataFrame, events: pd.DataFrame, policy_dim: pd.DataFrame) -> pd.DataFrame`
  — a pure transform that collapses `delisting_return_observations` to **one terminal return per `(security_id, delist_date)`**
  (latest-visible observation wins, `ORDER BY available_at DESC, source_loaded_at DESC, delisting_return_observation_id DESC`,
  matching the `observation_candidates` ranking in `DELISTING_EVENTS_ASOF_SQL`), tags `terminal_return_source='observed'`, and
  carries `crsp_dlstcd`, `return_observation_id`, `successor_security_id`, and PIT `delist_date`/`as_of_date`/`available_at`.
- `compute_delisting_code_reconciliation(events: pd.DataFrame, terminal_returns: pd.DataFrame, code_dim: pd.DataFrame) -> pd.DataFrame`
  — joins the warehouse `delisting_events.delist_code` (+ `delist_code_dim.reason_category`) to the vendor
  `crsp_dlstcd` (coarse-mapped to a `vendor_dlstcd_family`: `2xx→merger`, `3xx→exchange`, `4xx→liquidation`, `5xx→dropped`)
  on `(security_id, delist_date)` and emits `reconciliation_status ∈ {match, mismatch, vendor_only, warehouse_only, unmapped}`
  with a `mismatch_reason`.
- `refresh_delisting_terminal_returns(store, options)` and `reconcile_delisting_codes(store, options)` materializers that read
  the landed tables, call the pure transforms, and replace-by-source (mirroring `refresh_delisting_events`).

**Migration 0185** creates `delisting_terminal_returns`, `delisting_code_reconciliation`, and the `terminal_return_policy_dim`
(seeded in S4-1) with full catalog seeds. Real DDL for the recon table:

```sql
CREATE TABLE IF NOT EXISTS delisting_code_reconciliation (
    reconciliation_id VARCHAR PRIMARY KEY,
    source VARCHAR NOT NULL,
    security_id VARCHAR,
    symbol VARCHAR,
    delist_date DATE NOT NULL,
    as_of_date DATE NOT NULL,
    available_at TIMESTAMP NOT NULL,
    warehouse_delist_code VARCHAR,
    warehouse_reason_category VARCHAR,
    vendor_crsp_dlstcd INTEGER,
    vendor_dlstcd_family VARCHAR,
    reconciliation_status VARCHAR NOT NULL,   -- match | mismatch | vendor_only | warehouse_only | unmapped
    mismatch_reason VARCHAR,
    delisting_event_id VARCHAR,
    delisting_return_observation_id VARCHAR,
    run_id VARCHAR,
    source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
    updated_at TIMESTAMP NOT NULL DEFAULT now()
);
```

and `delisting_terminal_returns`:

```sql
CREATE TABLE IF NOT EXISTS delisting_terminal_returns (
    terminal_return_id VARCHAR PRIMARY KEY,
    source VARCHAR NOT NULL,
    security_id VARCHAR NOT NULL,
    symbol VARCHAR,
    delist_date DATE NOT NULL,
    as_of_date DATE NOT NULL,
    available_at TIMESTAMP NOT NULL,          -- delisting-confirmation timestamp; never the event date
    terminal_return DOUBLE NOT NULL,          -- realized DLRET (fraction, may be negative)
    terminal_return_ex_div DOUBLE,
    terminal_return_source VARCHAR NOT NULL,  -- observed | policy | none  (imputed is never written here)
    terminal_return_policy VARCHAR,           -- FK-ish to terminal_return_policy_dim.policy_code when source='policy'
    crsp_dlstcd INTEGER,
    return_basis VARCHAR,
    successor_security_id VARCHAR,
    return_observation_id VARCHAR,
    is_latest_revision BOOLEAN NOT NULL DEFAULT true,
    run_id VARCHAR,
    source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
    updated_at TIMESTAMP NOT NULL DEFAULT now()
);
```

Each table seeds `dataset_catalog` + `table_catalog` + `_catalog_fields_for_tables(...)` + explicit `field_catalog` rows for
the return columns (unit `fraction`, semantic_type `measure`), then calls `_refresh_schema_contract_v2_pin(conn)` — exactly the
pattern in `bodies_0144_0147.py`.

**PIT:** terminal rows inherit `available_at` from the observation (confirmation timestamp); the recon `available_at` is the
`max` of the event's and observation's `available_at`. Resolution to `security_id` reuses the loader's existing
`security_identifier_history` PIT resolver — no new lookahead path.

**Accept:** on an injected CSV, `delisting_terminal_returns` holds exactly one observed terminal return per
`(security_id, delist_date)`; `delisting_code_reconciliation` flags a `mismatch` when the vendor DLSTCD family disagrees with
the warehouse `delist_code` reason category, `match` when they agree, and `vendor_only`/`warehouse_only` when only one side has
evidence. Empty-by-default with no file injected; no imputed value labelled observed.

**TDD (write first, in `db/tests/test_delisting_returns.py`):**

```python
def test_injected_dlret_file_populates_terminal_return_per_delisted_security_day(tmp_store, tmp_path):
    from db.delisting import (
        DelistingReturnObservationOptions,
        load_delisting_return_observations,
        refresh_delisting_events,
        refresh_delisting_terminal_returns,
    )
    _seed_security(tmp_store)
    _insert_listing_status(tmp_store, listing_status_id="ls-obs",
                           status="inactive", valid_from=dt.date(2024, 4, 1),
                           available_at=dt.datetime(2024, 4, 2, 12, 0))
    refresh_delisting_events(tmp_store)
    csv_path = tmp_path / "crsp_dlret.csv"
    csv_path.write_text(
        "PERMNO,security_id,TICKER,DLSTDT,DLSTCD,DLRET,DLRETX,available_at\n"
        "12345,SEC-TEST-DELIST,DLS,2024-04-01,233,-0.640000,-0.640000,2024-04-03T12:00:00\n",
        encoding="utf-8")
    load_delisting_return_observations(
        tmp_store, DelistingReturnObservationOptions(source_file=csv_path, provider="CRSP_SAMPLE"))
    rows = refresh_delisting_terminal_returns(tmp_store)
    assert rows == 1
    term = tmp_store.con.execute(
        "SELECT security_id, delist_date, terminal_return, terminal_return_source, crsp_dlstcd "
        "FROM delisting_terminal_returns").fetchone()
    assert term[0] == SECURITY_ID
    assert _date_value(term[1]) == dt.date(2024, 4, 1)
    assert term[2] == pytest.approx(-0.64)
    assert term[3] == "observed"
    assert term[4] == 233


def test_dlstcd_reconciliation_flags_vendor_vs_warehouse_mismatch(tmp_store, tmp_path):
    from db.delisting import (
        DelistingReturnObservationOptions, load_delisting_return_observations,
        reconcile_delisting_codes, refresh_delisting_events, refresh_delisting_terminal_returns)
    _seed_security(tmp_store)
    # warehouse public proxy detects a generic NASDAQ_DELETE (family: exchange/dropped) ...
    _insert_listing_status(tmp_store, listing_status_id="ls-recon", status="inactive",
                           valid_from=dt.date(2024, 4, 1), available_at=dt.datetime(2024, 4, 2, 12, 0))
    refresh_delisting_events(tmp_store)
    # ... but the vendor DLSTCD 233 is a MERGER (2xx family) -> mismatch
    csv_path = tmp_path / "crsp_dlret_merger.csv"
    csv_path.write_text(
        "PERMNO,security_id,DLSTDT,DLSTCD,DLRET,available_at\n"
        "12345,SEC-TEST-DELIST,2024-04-01,233,0.052000,2024-04-03T12:00:00\n", encoding="utf-8")
    load_delisting_return_observations(
        tmp_store, DelistingReturnObservationOptions(source_file=csv_path, provider="CRSP_SAMPLE"))
    refresh_delisting_terminal_returns(tmp_store)
    reconcile_delisting_codes(tmp_store)
    status, family, reason = tmp_store.con.execute(
        "SELECT reconciliation_status, vendor_dlstcd_family, warehouse_reason_category "
        "FROM delisting_code_reconciliation").fetchone()
    assert status == "mismatch"
    assert family == "merger"
    assert reason in ("exchange_delete", "DELISTED_OR_TRANSFERRED_UNKNOWN", None)
```

(`_seed_security`, `_insert_listing_status`, `_insert_vendor_identifier`, `_date_value` are copied from
`db/tests/test_delisting.py` into a small `_helpers` block at the top of the new test module — reuse, do not import private
test symbols.)

### S4-1 — Deterministic spinoff/merger terminal-return policy

**Root cause:** merger/acquisition and spinoff delists have no observed DLRET in many public-proxy cases, and no documented,
reproducible rule maps the corporate action to a terminal return — so those names either vanish or get an ad-hoc value.

**Fix:** seed a **policy-as-data** dimension `terminal_return_policy_dim` (migration **0185**, alongside S4-0) and add a pure
`apply_terminal_return_policy(events, corporate_actions, policy_dim) -> pd.DataFrame` in `db/delisting.py` that, for a delist
**without** an observed terminal return, derives a `terminal_return_source='policy'` row per the matched policy. The dimension
is the single source of truth; the applier is a deterministic lookup, never a hardcoded branch:

```python
TERMINAL_RETURN_POLICY_ROWS = (
    # policy_code, corporate_action_type, terminal_return_basis, combine_successor, default_return, is_observed_required, description
    ("merger_cash",        "merger",          "cash_consideration",   False, None,  False,
     "Cash-merger consideration vs last pre-delist adjusted close = realized terminal return."),
    ("merger_stock",       "merger",          "successor_reinvest",   True,  None,  False,
     "Stock-merger: proceeds reinvested into the successor security_id; terminal return chains to the successor path."),
    ("spinoff",            "spinoff",         "parent_plus_child",    True,  None,  False,
     "Spinoff: parent close plus when-issued child value; combined via successor_security_id."),
    ("liquidation",        "liquidation",     "final_distribution",   False, None,  True,
     "Liquidation: observed final cash distribution required; no default."),
    ("exchange_delete",    "exchange_delete", "observed_dlret",       False, None,  True,
     "Exchange delete: observed DLRET required; no policy default (public proxy stays UNOBSERVED)."),
    ("dropped_unresolved", "dropped",         "unresolved",           False, None,  True,
     "Unresolved drop: no policy terminal return; handled only if an observed/imputed value is supplied elsewhere."),
)
```

The applier is strictly deterministic and **does not invent a return**: for `merger_cash` it computes
`(cash_consideration / last_pre_delist_adjusted_close) - 1` from `corporate_actions` + `equity_price_metrics.adjusted_close`;
for `spinoff`/`merger_stock` it chains through `successor_security_id`; for `exchange_delete`/`dropped_unresolved`/`liquidation`
without evidence it yields **no** terminal-return row (the survivorship DQC then flags the uncovered name — it does not paper
over it with −30%). The −30% Shumway–Warther value is never emitted here.

**PIT:** each policy terminal row's `available_at = max` of its corporate-action `available_at` and the last-pre-delist bar's
`available_at`. No successor return is chained from a date after the successor's own `available_at`.

**Accept:** a fixture cash-merger with a known consideration and last close returns the hand-computed terminal return with
`terminal_return_source='policy'`, `terminal_return_policy='merger_cash'`, deterministically (documented, reproducible); an
`exchange_delete` with no observation yields **no** policy row (left for the DQC to flag).

**TDD:**

```python
def test_cash_merger_policy_terminal_return_is_deterministic(tmp_store):
    from db.delisting import apply_terminal_return_policy, load_terminal_return_policy_dim
    policy = load_terminal_return_policy_dim(tmp_store)  # reads the seeded dim
    events = pd.DataFrame([{ "security_id": SECURITY_ID, "symbol": "DLS",
        "delist_date": dt.date(2024, 4, 1), "as_of_date": dt.date(2024, 4, 1),
        "available_at": dt.datetime(2024, 4, 2, 12, 0), "corporate_action_type": "merger" }])
    actions = pd.DataFrame([{ "security_id": SECURITY_ID, "ex_date": dt.date(2024, 4, 1),
        "action_type": "cash_merger", "cash_amount": 21.0,
        "last_pre_delist_adjusted_close": 20.0, "available_at": dt.datetime(2024, 4, 1, 22, 0) }])
    out = apply_terminal_return_policy(events, actions, policy)
    row = out.iloc[0]
    assert row["terminal_return"] == pytest.approx(0.05)          # 21/20 - 1
    assert row["terminal_return_source"] == "policy"
    assert row["terminal_return_policy"] == "merger_cash"


def test_exchange_delete_without_observation_yields_no_policy_return(tmp_store):
    from db.delisting import apply_terminal_return_policy, load_terminal_return_policy_dim
    policy = load_terminal_return_policy_dim(tmp_store)
    events = pd.DataFrame([{ "security_id": SECURITY_ID, "symbol": "DLS",
        "delist_date": dt.date(2024, 4, 1), "as_of_date": dt.date(2024, 4, 1),
        "available_at": dt.datetime(2024, 4, 2, 12, 0), "corporate_action_type": "exchange_delete" }])
    out = apply_terminal_return_policy(events, pd.DataFrame(), policy)
    assert out.empty          # DQC flags the uncovered name; policy never invents -30%
```

### S4-2 — Delisting-return stitching into the forward-return series *(the big one)*

**Root cause:** PF4-S1's raw forward-return series is surviving-only. When a security delists inside a horizon window
{1,5,10,21,63}, the surviving-only builder produces a NULL/absent forward return and the name is dropped — so IC / decile /
long-short-spread arithmetic is computed over a survivor-biased cross-section.

**Fix:** add a **pure** transform + a **materialized** survivorship-safe forward-return table.

- `compute_survivorship_safe_forward_returns(forward_returns, delisting_cohort, *, source, run_id) -> pd.DataFrame` in
  `db/delisting.py`. `forward_returns` is the surviving panel (columns `security_id, symbol, as_of_date, horizon_days,
  forward_end_date, raw_forward_return, available_at`). `delisting_cohort` enumerates
  `(security_id, as_of_date, horizon_days, delist_date, terminal_return, terminal_return_source, return_observation_id,
  terminal_available_at)` for every panel formation date whose forward window `(as_of_date, forward_end_date]` contains a
  `delist_date` with a known terminal return. The transform:
  1. **geometrically splices** the pre-delist partial return (if the surviving row carries one) with the terminal DLRET:
     `forward_return = (1 + raw_forward_return) * (1 + terminal_return) - 1`; when the surviving row is absent/NULL (the pure
     NaN-drop case) `forward_return = terminal_return`;
  2. sets `is_delisted_in_horizon=True`, `is_stitched=True`, and `available_at = max(raw.available_at, terminal_available_at)`;
  3. **passes through** non-cohort surviving rows unchanged (`is_stitched=False`);
  4. stable-sorts by `(security_id, as_of_date, horizon_days)` and hashes a deterministic `forward_return_id`.

  Real code:

```python
FORWARD_RETURN_SS_COLUMNS = [
    "forward_return_id", "source", "security_id", "symbol", "as_of_date", "horizon_days",
    "forward_end_date", "raw_forward_return", "terminal_return", "forward_return",
    "is_delisted_in_horizon", "is_stitched", "delist_date", "terminal_return_source",
    "return_observation_id", "is_latest_revision", "available_at", "run_id",
]

def _stitch(raw, terminal):
    if terminal is None or pd.isna(terminal):
        return raw
    if raw is None or pd.isna(raw):
        return float(terminal)
    return float((1.0 + float(raw)) * (1.0 + float(terminal)) - 1.0)

def _forward_return_id(source, security_id, as_of_date, horizon_days):
    payload = "|".join(str(p) for p in (source, security_id, as_of_date, horizon_days))
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()

def compute_survivorship_safe_forward_returns(forward_returns, delisting_cohort, *,
                                              source=DEFAULT_FORWARD_RETURN_SS_SOURCE, run_id=None):
    base = forward_returns.copy() if forward_returns is not None else pd.DataFrame()
    cohort = delisting_cohort.copy() if delisting_cohort is not None else pd.DataFrame()
    if base.empty and cohort.empty:
        return pd.DataFrame(columns=FORWARD_RETURN_SS_COLUMNS)
    keys = ["security_id", "as_of_date", "horizon_days"]
    merged = base.merge(cohort, on=keys, how="outer", suffixes=("", "_c"))
    merged["is_delisted_in_horizon"] = merged["terminal_return"].notna()
    merged["is_stitched"] = merged["is_delisted_in_horizon"]
    merged["forward_return"] = [
        _stitch(r, t) for r, t in zip(merged.get("raw_forward_return"), merged.get("terminal_return"))
    ]
    merged["available_at"] = (
        pd.concat([pd.to_datetime(merged["available_at"]),
                   pd.to_datetime(merged["terminal_available_at"])], axis=1).max(axis=1)
    )
    merged = merged[merged["forward_return"].notna()].copy()
    merged["source"] = source
    merged["run_id"] = run_id
    merged["is_latest_revision"] = True
    merged = merged.sort_values(keys, kind="mergesort").reset_index(drop=True)
    merged["forward_return_id"] = [
        _forward_return_id(source, s, a, h)
        for s, a, h in zip(merged["security_id"], merged["as_of_date"], merged["horizon_days"])
    ]
    return merged[FORWARD_RETURN_SS_COLUMNS]
```

- `refresh_survivorship_safe_forward_returns(store, options)` reads the raw forward-return source (module constant
  `RAW_FORWARD_RETURN_SOURCE = "signal_forward_returns"` — **reconcile to PF4-S1's landed name here and only here**) and builds
  `delisting_cohort` in SQL from `delisting_terminal_returns` × the raw source's `(security_id, as_of_date, horizon_days,
  forward_end_date)` where `delist_date > as_of_date AND delist_date <= forward_end_date AND terminal.available_at <=
  <panel-decision ts for as_of_date>`, then calls the pure transform and replaces-by-source into
  `forward_returns_survivorship_safe`.

**Migration 0186** creates `forward_returns_survivorship_safe` (schema below) + a convenience latest-revision view
`v_forward_returns_survivorship_safe`, both catalogued. **Migration 0187** adds indexes:
`idx_forward_returns_ss_key(source, security_id, as_of_date, horizon_days)`,
`idx_forward_returns_ss_delisted(is_delisted_in_horizon, as_of_date)`,
`idx_delisting_terminal_returns_security_date(security_id, delist_date, available_at)`,
`idx_delisting_code_reconciliation_status(reconciliation_status, delist_date)`.

```sql
CREATE TABLE IF NOT EXISTS forward_returns_survivorship_safe (
    forward_return_id VARCHAR PRIMARY KEY,
    source VARCHAR NOT NULL,
    security_id VARCHAR NOT NULL,
    symbol VARCHAR,
    as_of_date DATE NOT NULL,
    horizon_days INTEGER NOT NULL,
    forward_end_date DATE,
    raw_forward_return DOUBLE,
    terminal_return DOUBLE,
    forward_return DOUBLE NOT NULL,
    is_delisted_in_horizon BOOLEAN NOT NULL,
    is_stitched BOOLEAN NOT NULL,
    delist_date DATE,
    terminal_return_source VARCHAR,
    return_observation_id VARCHAR,
    is_latest_revision BOOLEAN NOT NULL DEFAULT true,
    available_at TIMESTAMP NOT NULL,
    run_id VARCHAR,
    source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
    updated_at TIMESTAMP NOT NULL DEFAULT now()
);
```

**PIT:** `available_at = max(raw, terminal)`; the cohort SQL filters `terminal.available_at <= end_of_day(as_of_date)` **only
for visibility of the input**, but the *forward-return row itself* carries the later `available_at` so it is not consumable
until the window resolves — a lookahead test asserts a terminal planted with a future `available_at` is not spliced when read
before that timestamp.

**Accept:** a name delisting mid-horizon produces a stitched forward return equal to the geometric splice of its pre-delist
partial return and terminal DLRET (**not NaN-dropped**); a name with no surviving bar at all still produces a row equal to the
terminal DLRET; surviving names pass through unchanged; shuffling the input rows yields byte-identical output.

**TDD:**

```python
def test_forward_return_uses_stitched_dlret_when_delisting_midhorizon():
    from db.delisting import compute_survivorship_safe_forward_returns
    forward = pd.DataFrame([{ "security_id": "A", "symbol": "A", "as_of_date": dt.date(2024, 1, 2),
        "horizon_days": 21, "forward_end_date": dt.date(2024, 1, 31), "raw_forward_return": 0.02,
        "available_at": dt.datetime(2024, 2, 1, 22) }])
    cohort = pd.DataFrame([{ "security_id": "A", "as_of_date": dt.date(2024, 1, 2), "horizon_days": 21,
        "delist_date": dt.date(2024, 1, 20), "terminal_return": -0.50, "terminal_return_source": "observed",
        "return_observation_id": "obs-1", "terminal_available_at": dt.datetime(2024, 1, 22, 12) }])
    out = compute_survivorship_safe_forward_returns(forward, cohort, source="ss_test")
    row = out.iloc[0]
    assert row["is_stitched"] is True or row["is_stitched"] == True
    assert row["forward_return"] == pytest.approx((1.02) * (1 - 0.50) - 1.0)   # -0.49
    assert pd.notna(row["forward_return"])                                     # NOT NaN-dropped


def test_dropped_name_with_no_surviving_bar_still_gets_terminal_return():
    from db.delisting import compute_survivorship_safe_forward_returns
    cohort = pd.DataFrame([{ "security_id": "B", "as_of_date": dt.date(2024, 1, 2), "horizon_days": 5,
        "delist_date": dt.date(2024, 1, 4), "terminal_return": -0.90, "terminal_return_source": "observed",
        "return_observation_id": "obs-2", "terminal_available_at": dt.datetime(2024, 1, 6, 12) }])
    out = compute_survivorship_safe_forward_returns(pd.DataFrame(), cohort, source="ss_test")
    assert len(out) == 1
    assert out.iloc[0]["forward_return"] == pytest.approx(-0.90)


def test_stitching_is_deterministic_under_input_shuffle():
    from db.delisting import compute_survivorship_safe_forward_returns
    forward = pd.DataFrame([
        { "security_id": s, "symbol": s, "as_of_date": dt.date(2024, 1, 2), "horizon_days": h,
          "forward_end_date": dt.date(2024, 2, 1), "raw_forward_return": 0.01,
          "available_at": dt.datetime(2024, 2, 1, 22) }
        for s in ("A", "B", "C") for h in (1, 5, 21)])
    a = compute_survivorship_safe_forward_returns(forward, pd.DataFrame(), source="ss_test")
    b = compute_survivorship_safe_forward_returns(
        forward.sample(frac=1.0, random_state=7).reset_index(drop=True), pd.DataFrame(), source="ss_test")
    pd.testing.assert_frame_equal(a, b)
```

### S4-3 — Survivorship-bias DQC (`severity=critical`, gate-ready) + coverage view + DLSTCD-recon gate

**Root cause:** nothing asserts a forward-return panel is survivorship-safe, so a survivor-biased panel would silently flow
into PF4-S1 IC arithmetic and PF4-S2 gating.

**Fix:** **Migration 0188** adds the coverage view `v_delisting_return_coverage` (per delist cohort: counts of security-days
with `observed` / `policy` terminal returns vs missing, and the stitched-vs-dropped count) and registers two checks in
`quality_check_registry`:

- `survivorship_forward_return_drops_delisted_names` — **`severity='critical'`, gate-ready.** SQL counts delisted
  security-days that *should* be stitched (a terminal return exists whose `delist_date` falls in some panel formation date's
  horizon window) but are **absent or unstitched** in `forward_returns_survivorship_safe` (anti-join). Threshold `0.0`,
  comparator `le`, `failure_status='failed'` → **RED (halt)** when > 0, **GREEN** at 0.
- `delisting_code_reconciliation_unresolved` — **`severity='error'`.** Counts `reconciliation_status='unmapped'` rows
  (vendor DLSTCD with no family mapping / no warehouse code to compare) → surfaces gaps without necessarily halting. Legitimate
  `mismatch` rows are reported in the coverage view, not failed (a real vendor/proxy disagreement is signal, not a defect).

Registry rows follow the exact `INSERT OR REPLACE INTO quality_check_registry (...) VALUES (...)` shape from
`bodies_0144_0147.py` (`valuation_input_core_completeness`). The check SQL bodies live in `db/quality/` (a new
`checks_survivorship.py` module wiring `SqlQualityCheck(..., severity='critical')`), matching how the existing checks modules
register `SqlQualityCheck`s.

Anti-join check SQL (real):

```sql
-- survivorship_forward_return_drops_delisted_names : expect 0
SELECT count(*)::DOUBLE
FROM delisting_terminal_returns t
JOIN (SELECT DISTINCT security_id, as_of_date, horizon_days, forward_end_date
      FROM forward_returns_survivorship_safe) panel
  ON panel.security_id = t.security_id
 AND t.delist_date >  panel.as_of_date
 AND t.delist_date <= panel.forward_end_date
LEFT JOIN forward_returns_survivorship_safe f
  ON f.security_id = t.security_id
 AND f.as_of_date  = panel.as_of_date
 AND f.horizon_days = panel.horizon_days
 AND f.is_stitched
WHERE f.forward_return_id IS NULL   -- a delisted name in a formation window that was NOT stitched = survivorship drop
```

**PIT:** the coverage view and checks are pure reads over the populated surfaces — deterministic, no network.

**Accept:** the survivorship check is **RED** on a panel where a delisted security-day was dropped (no stitched row) and
**GREEN** once the stitched row is present; the recon `unmapped` check is quiet when every vendor code maps and fires on an
unmapped code; the coverage view reports observed/policy/missing counts per cohort.

**TDD:**

```python
def test_survivorship_dqc_red_on_dropped_delisted_names_green_when_stitched(tmp_store):
    from db.quality.checks_survivorship import survivorship_forward_return_check
    # seed one terminal return whose delist falls in a formation window ...
    tmp_store.con.execute(
        "INSERT INTO delisting_terminal_returns (terminal_return_id, source, security_id, delist_date, "
        "as_of_date, available_at, terminal_return, terminal_return_source) "
        "VALUES ('t1','ss','A', DATE '2024-01-10', DATE '2024-01-10', TIMESTAMP '2024-01-12 12:00', -0.5, 'observed')")
    # ... and a survivorship-safe panel that DROPS it (only a surviving name, no stitched row for A)
    tmp_store.con.execute(
        "INSERT INTO forward_returns_survivorship_safe (forward_return_id, source, security_id, as_of_date, "
        "horizon_days, forward_end_date, forward_return, is_delisted_in_horizon, is_stitched, available_at) "
        "VALUES ('f1','ss','Z', DATE '2024-01-02', 21, DATE '2024-01-31', 0.03, false, false, TIMESTAMP '2024-02-01 22:00')")
    red = survivorship_forward_return_check(tmp_store)      # runs the registered critical check
    assert red.status == "failed" and red.severity == "critical"
    # now stitch A in -> GREEN
    tmp_store.con.execute(
        "INSERT INTO forward_returns_survivorship_safe (forward_return_id, source, security_id, as_of_date, "
        "horizon_days, forward_end_date, terminal_return, forward_return, is_delisted_in_horizon, is_stitched, "
        "delist_date, available_at) VALUES ('f2','ss','A', DATE '2024-01-02', 21, DATE '2024-01-31', -0.5, -0.5, "
        "true, true, DATE '2024-01-10', TIMESTAMP '2024-01-12 12:00')")
    green = survivorship_forward_return_check(tmp_store)
    assert green.status in ("passed", "skipped")


def test_dlret_not_visible_before_delist_confirmation(tmp_store, tmp_path):
    # a terminal return whose available_at is AFTER the read timestamp must not be spliced/visible
    from db.delisting import (DelistingReturnObservationOptions, load_delisting_return_observations,
                              refresh_delisting_events, refresh_delisting_terminal_returns)
    from db.asof import delisting_return_observations_asof
    _seed_security(tmp_store)
    _insert_listing_status(tmp_store, listing_status_id="ls-la", status="inactive",
                           valid_from=dt.date(2024, 4, 1), available_at=dt.datetime(2024, 4, 2, 12, 0))
    refresh_delisting_events(tmp_store)
    csv_path = tmp_path / "late_confirm.csv"
    csv_path.write_text(
        "PERMNO,security_id,DLSTDT,DLSTCD,DLRET,available_at\n"
        "12345,SEC-TEST-DELIST,2024-04-01,233,-0.7,2024-04-05T12:00:00\n", encoding="utf-8")
    load_delisting_return_observations(
        tmp_store, DelistingReturnObservationOptions(source_file=csv_path, provider="CRSP_SAMPLE"))
    refresh_delisting_terminal_returns(tmp_store)
    db_path = tmp_store.path
    tmp_store.connection.close(); tmp_store.connection = None
    before = delisting_return_observations_asof(dt.date(2024, 4, 3),
        as_of_ts=dt.datetime(2024, 4, 3, 13, 0), db_path=db_path, symbols=("DLS",), providers=("CRSP_SAMPLE",))
    assert before.empty                       # terminal not knowable until 2024-04-05
```

### S4-4 — Live-DB smoke + ledger closeout

**Fix:** run the operator live-DB smoke against the shared `atx_impl.duckdb` (backed up first, clause F): inject a small
observed-DLRET sample, `refresh_delisting_terminal_returns`, `reconcile_delisting_codes`,
`refresh_survivorship_safe_forward_returns`, run the survivorship + recon checks, and record exact row counts + `run_id`.
Then update `db/PARITY_GAP.md` (Domain-5 DLRET lines 200/215 flipped to reflect the populated terminal-return catalog +
stitching + survivorship gate; Tranche-5 line 302 updated) and append a `WAREHOUSE_PARITY_TRANCHES.md` row (8 columns:
Tranche | Status | Start SHA | End SHA | Domains | Verification | Live DB Notes | Caveats / Next) with start/end SHA, the
verification commands, live smoke counts + `run_id`, and next → PF4-S5 multi-universe consumes the survivorship-safe returns.

**Accept:** `python -m pytest atx-impl\db\tests\test_delisting_returns.py -q` green **and** full `python -m pytest
atx-impl\db\tests -q` green in the worktree before commit; `PARITY_GAP.md` + `WAREHOUSE_PARITY_TRANCHES.md` updated; commit
trailer EXACTLY `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.

---

## Sequencing & expected compounding

**S4-0 → S4-1 → S4-2 → S4-3 → S4-4.** The observed terminal-return catalog + DLSTCD reconciliation land first (they define the
terminal-return grain the rest read). The spinoff/merger policy fills terminals for names without observed DLRET. The
stitching transform then consumes **both** sources to produce the survivorship-safe forward-return series. The DQC closes last,
once the survivorship-safe surface exists, so it verifies a real panel rather than crying wolf. **Compounding:** the
survivorship-safe forward returns feed **PF4-S1 signal evaluation** (rank-IC / decile / long-short spread become computed over
a survivorship-safe cross-section), gate through **PF4-S2** (the critical survivorship check joins the orchestrator halt gate),
and flow into **PF4-S5** (multi-universe) and **PF4-S7/S8** (release + SDK) so the shipped panel is survivorship-safe end to
end. It removes the single largest silent bias in every return-based factor score.

---

## Risks / guardrails

- **No-lookahead is the whole point — do not leak the terminal return.** `available_at` = delisting-confirmation
  (`delisting_return_observations.available_at`), never the delist event date; the stitched forward-return row carries
  `max(raw, terminal)` so it is not consumable before its window resolves. A leak here turns the survivorship *fix* into a
  lookahead *bug*. Regression-locked by `test_dlret_not_visible_before_delist_confirmation`.
- **Never invent a terminal return.** Observed DLRET and deterministic corporate-action policy only; an uncovered delist gets
  **no** terminal row and is flagged by the critical DQC — the −30% Shumway–Warther value stays research-only and is never
  written as observed or policy. Keep `terminal_return_source ∈ {observed, policy}` in the catalog; `imputed` is never persisted
  there.
- **DLSTCD reconciliation reports, it does not overwrite.** A vendor/proxy disagreement is *signal* surfaced in
  `delisting_code_reconciliation` (status `mismatch`) and the coverage view — never a silent overwrite of the warehouse
  `delist_code`. Only `unmapped` gaps fail (as `error`); `mismatch` is expected and non-failing.
- **Reconcile the raw forward-return source name in exactly one place.** `RAW_FORWARD_RETURN_SOURCE` is the single constant the
  S4 implementer aligns to PF4-S1's landed table/view; do not scatter the name through SQL. The pure stitching transform is
  unit-tested with in-memory DataFrames so it is provable independent of whatever PF4-S1 named its table.
- **Stay in lane 0185–0188.** No signal-eval edits (PF4-S1), no universe edits (PF4-S5); extend `delisting.py` additively;
  never edit a landed migration or another sprint's region; never `git add -A` (stage explicit paths).

---

## Bench / acceptance

- Observed DLRET populated per delisted `(security_id, delist_date)` from an injectable file; one terminal return per
  security-day; empty-by-default with no file injected; no imputed value labelled observed.
- DLSTCD reconciliation flags vendor-vs-warehouse `mismatch`, records `match`/`vendor_only`/`warehouse_only`, and fails only on
  `unmapped`.
- Deterministic spinoff/merger policy returns the hand-computed cash-merger terminal return; an uncovered `exchange_delete`
  yields no policy row.
- A name delisting mid-horizon produces a **stitched** forward return (geometric splice, **not** NaN-dropped); a fully-dropped
  name still realizes its terminal DLRET; surviving names pass through unchanged; shuffling input is byte-identical output.
- Survivorship DQC (`severity=critical`, gate-ready) **RED** on a panel that drops delisted names, **GREEN** when stitched;
  `available_at` no-lookahead proven.
- `python -m pytest atx-impl\db\tests\test_delisting_returns.py -q` green, and full `python -m pytest atx-impl\db\tests -q`
  green in the worktree before commit (run from `atx-impl/`, never from `db/`).
- **Live-DB smoke** recorded in the ledger: observed-DLRET rows injected, terminal-return catalog + reconciliation counts,
  survivorship-safe forward-return rows (stitched vs pass-through), survivorship + recon check results, and the `run_id`.
- `PARITY_GAP.md` updated (Domain-5 DLRET/survivorship lines + Tranche-5 status); a `WAREHOUSE_PARITY_TRANCHES.md` row appended
  (start/end SHA, domains, verification commands, live smoke with exact counts + `run_id`, caveats/next → PF4-S5).

**Process:** own git worktree off `main` via `atx-impl/scripts/new_db_worktree.sh new|finish <slug>`; controller
`superpowers:subagent-driven-development` (fresh implementer + reviewer per task; TDD + verification-before-completion). Never
`git add -A` (stage explicit paths); never push unless asked. Commit trailer EXACTLY `Co-Authored-By: Claude Opus 4.8 (1M
context) <noreply@anthropic.com>`. New module ⇒ new `test_*.py`; operator live-DB smoke runs against the shared DB in the
primary tree, backed up first (clause F).
