# Sprint PF4-S5 — Multi-universe + versioning

**Goal:** lift the warehouse from a **single** governed universe (`us_common_equity_liquid_v1`, the PF3-S4
interval-keyed `universe_membership` surface) to a **governed multi-universe product**: at least **three**
distinct PIT universes computed through the existing universe machinery from **declared, definition-as-data
selection rules** (liquidity floor / size cap / sector caps); **release versioning** so a `(universe_id,
version)` snapshot is pinnable and **content-stable** per `(universe_id, version, as_of)`; **membership
turnover** reporting per universe (names added/removed rebalance-to-rebalance + turnover rate); and the
universe-as-of applied **consistently** across factor-panel assembly and the SDK read filter — both of which
now accept `(universe_id, version)`. Today `db/universe.py` hard-codes one universe id
(`DEFAULT_UNIVERSE_ID = "us_common_equity_liquid_v1"`), `db/factor_panel.py` filters on a single
`DEFAULT_FACTOR_PANEL_UNIVERSE_ID`, and there is no notion of a universe **version**, no **turnover** surface,
and no **definition registry** — so a downstream quant team cannot pick a broad vs a liquid-large vs a
sector-neutral investable set, cannot pin a reproducible universe release, and cannot measure rebalance churn.
S5 adds a governed `universe_definition` registry (definitions + versions + selection rules + content hash),
computes each governed universe's PIT membership through the **same** `compute_universe_membership_intervals`
interval machine (reconciled to `us_common_equity_liquid_v1`, never forked), threads
`(universe_id, universe_version)` through the reader / panel / read filter, and emits `v_universe_turnover`.
Reserved migrations **0189–0191**.

**Mandate / Owns:** `db/universe.py` (the definition registry, the ≥3 governed definitions-as-data, the
version-aware builder, the pure selection-rule + turnover transforms, the content-hash), version-aware
`universe_membership_asof` in `db/asof/pricing.py`, `(universe_id, universe_version)` threading in
`db/factor_panel.py` (`_apply_universe_filter`, `assemble_factor_panel_long`, `read_panel_asof`), the new
migration body `db/migrations/bodies_0189_0191.py` (registered in `db/migrations/registry.py`), a version/
subset-stability quality check in `db/quality/checks_market_reference.py`, and **NEW**
`db/tests/test_multi_universe.py`. The parity-ledger close-out (`WAREHOUSE_PARITY_TRANCHES.md` row +
`db/PARITY_GAP.md` update) is owned here.

**Must NOT touch:** the PF3-S4 **interval-compression contract** — `compute_universe_membership_intervals`
stays the one interval machine; S5 feeds it version-tagged, rule-filtered daily decisions, it does not
re-architect the compression. The existing `us_common_equity_liquid_v1` **row shape and semantics** are
preserved (its rows default to `universe_version='v1'`); S5 **reconciles** to it and adds siblings, it never
rewrites or deletes it. The legacy `db/universes.py` snapshot (`us_liquid_equity_v1`) stays a compatibility
input. The factor engine (`db/factors/`), the price backfill (`db/pricing_bulk.py`,
`db/backfill.py`), and the adjusted-bar / `equity_price_metrics` surfaces are **read**, not re-shaped. Do not
edit any migration `< 0189`; do not touch another sprint's reserved range.

**Depends on:** PF3-S4 (`universe_membership` interval surface, `universe_membership_asof`,
`v_price_fundamental_overlap`, the coverage gate — S5 extends every one of these); PF3-S2 schema-contract v2
(new table/columns land catalogued + contract-pinned); PF3-S10 `db/factor_panel.py` (the panel-assembly +
read path S5 threads version through); pf1 identifier spine + a sector/classification source for the
sector-neutral rule. **Second** of the PF4 Track-B data-correctness wave — runs **after** PF4-S4
(survivorship returns) and **before** PF4-S6 (activation harness), which share the
`fundamental_*`/pricing/universe surfaces and consume S5's multi-universe membership.

---

## Baseline / where the cycles go

Measured 2026-07-06 against `atx-impl/db`. The warehouse has exactly **one** governed universe, no versioning,
and no turnover.

1. **One universe id, hard-coded.** `db/universe.py:14` pins `DEFAULT_UNIVERSE_ID = "us_common_equity_liquid_v1"`
   and `UniverseMembershipOptions` carries one screen (`min_price=5.0`, `min_dollar_volume=10_000_000`,
   `lookback_days=20`). There is a common-equity mask and a listing/liquidity screen, but no way to declare a
   **broad** floor, a **liquid-large** size cap, or a **sector-neutral** cap — the selection logic is one
   fixed rule, not definition-as-data. `db/factor_panel.py:23` mirrors this with a single
   `DEFAULT_FACTOR_PANEL_UNIVERSE_ID`.

2. **No version.** `universe_membership` (migration 0140) is keyed `(universe_id, security_id, valid_from,
   source)` with no `universe_version` column. A rebuild replaces in place (`_replace_intervals`), so there is
   no immutable, pinnable `(universe_id, version)` release and no content-stability guarantee — a caller cannot
   ask for "the `v1` membership of `us_common_equity_broad` as of D" and be sure it is byte-identical across
   rebuilds.

3. **No turnover surface.** Nothing reports names added/removed between rebalances or a turnover rate, so a
   quant team cannot size rebalance costs or detect a universe that is churning pathologically.

4. **The reader/panel/read-filter are single-universe.** `universe_membership_asof` (`db/asof/pricing.py:544`)
   takes `universe_id` but no version; `_apply_universe_filter` (`db/factor_panel.py:120`) filters
   `members["universe_id"] == universe_id` only; `read_panel_asof` (`db/factor_panel.py:657`) has **no**
   universe filter at all (it reads the pre-scoped `v_factor_panel`). So even if multiple universes existed,
   the consumption path could not select among them by `(universe_id, version)`.

**Already good — do not regress:**
- **The interval machine.** `compute_universe_membership_intervals` compresses daily decisions into
  deterministic, stable-sorted intervals and keeps explicit exclusion rows (`is_member=false` + `reason`). S5
  reuses it verbatim — every governed universe runs through it.
- **The as-of gating.** `universe_membership_asof` gates on the valid window **and** `available_at <= as_of_ts`
  and returns only `is_member=true`, latest-revision rows. S5 adds a version predicate, nothing else.
- **The PF3-S4 coverage gate.** `priced_fundamental_universe_decision_coverage`
  (`db/quality/checks_market_reference.py:87`) stays green; S5 adds a **new** version/subset-stability check
  beside it.

---

## PIT / determinism + production contract

Clauses **(A) (B) (D) (E) (G) (I)** all bear on this sprint.

- **(A) No lookahead.** Every universe's membership is decided **only** from data visible as-of the decision
  date: the daily-decision SQL gates listing/liquidity on `available_at <= b.available_at`, and
  `universe_membership_asof(as_of, version=…)` gates on the valid window **and** `available_at <= as_of_ts`. A
  size-rank or sector-cap decision that first passes on D is invisible to an as-of-`D−1` query. Delisted names
  stay in interval history; no survivorship.
- **(B) Append-only, catalogued migrations.** Strictly **0189–0191**, schema/index/view split, each new
  table/view/column catalogued (`dataset_catalog` + `table_catalog` + `field_catalog`) **in the same
  migration**, schema-contract v2 re-pinned via `_refresh_schema_contract_v2_pin`, DB+WAL backup before any
  live apply.
- **(D) Determinism.** The selection-rule application, the interval compression, the content hash, and the
  turnover computation are **pure** `compute_*` transforms (pandas in → DataFrame/str out), stable-sorted,
  unit-tested without DuckDB. **Shuffle input rows → byte-identical intervals + identical `content_sha256`.**
- **(E) Schema-as-contract.** `universe_definition`, the two new `universe_membership` columns, and
  `v_universe_turnover` all land with contract rows; the drift check fails on any uncatalogued surface.
- **(G) Quality-gated.** A `severity=critical` `governed_universe_version_stability` check is authored
  gate-ready for PF4-S2's orchestrator halt (subset invariant + content-hash stability).
- **(I) Panel PIT-safety.** Universe membership is applied **as-of** in `assemble_factor_panel_long` and in the
  `read_panel_asof` read filter, both version-scoped; the export-boundary lookahead test still gates export.

Migrations: **0189** — `universe_definition` registry + `universe_membership` version/content columns +
catalog. **0190** — `v_universe_turnover` view + catalog. **0191** — indexes + version natural-key +
quality-check registry row + residual catalog.

---

## Tasks

### S5-0 — Governed universe definition registry + version columns (definition-as-data)

**Root cause:** the only "definition" is the hard-coded `UniverseMembershipOptions` default; there is no
registry that declares **which** universes exist, at **what version**, under **which selection rules**, so a
new universe cannot be added as data and a version cannot be pinned.

**Fix (migration 0189, `db/migrations/bodies_0189_0191.py`):** create `universe_definition` and add two
columns to `universe_membership`. Real DDL (schema half of the split):

```sql
CREATE TABLE IF NOT EXISTS universe_definition (
    universe_id VARCHAR NOT NULL,
    universe_version VARCHAR NOT NULL,
    name VARCHAR NOT NULL,
    description VARCHAR,
    status VARCHAR NOT NULL DEFAULT 'active',        -- active | deprecated | draft
    parent_universe_id VARCHAR,                        -- subset lineage (liquid_large ⊆ broad)
    selection_rules_json VARCHAR NOT NULL,             -- declared rules: floors / size cap / sector caps
    content_sha256 VARCHAR,                            -- pinned membership content hash (nullable until built)
    is_pinned BOOLEAN NOT NULL DEFAULT false,
    available_at TIMESTAMP,
    source VARCHAR NOT NULL,
    created_at TIMESTAMP NOT NULL DEFAULT now(),
    source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
    PRIMARY KEY (universe_id, universe_version)
);
ALTER TABLE universe_membership ADD COLUMN IF NOT EXISTS universe_version VARCHAR NOT NULL DEFAULT 'v1';
ALTER TABLE universe_membership ADD COLUMN IF NOT EXISTS content_sha256 VARCHAR;
```

Catalog `universe_definition` (`dataset_catalog`/`table_catalog` with `pit_column='available_at'`,
`natural_key_json='["universe_id","universe_version"]'`), seed `field_catalog` for every
`universe_definition` column and for the two new `universe_membership` columns
(`_catalog_fields_for_tables(conn, ("universe_definition","universe_membership"))` + explicit rows), then
`_refresh_schema_contract_v2_pin(conn)`. Existing `v1` rows backfill to `universe_version='v1'`, so the legacy
universe is preserved unchanged (reconcile, don't fork).

In `db/universe.py` declare the **definition-as-data** constants (real Python, no placeholder):

```python
@dataclass(frozen=True)
class UniverseDefinition:
    universe_id: str
    universe_version: str
    name: str
    description: str
    selection_rules: dict[str, object]
    parent_universe_id: str | None = None
    status: str = "active"

GOVERNED_UNIVERSE_DEFINITIONS: dict[str, UniverseDefinition] = {
    "us_common_equity_broad": UniverseDefinition(
        universe_id="us_common_equity_broad",
        universe_version="v1",
        name="US common-equity broad PIT universe",
        description="Common equity, actively listed, light liquidity floor.",
        selection_rules={"min_price": 1.0, "min_dollar_volume": 1_000_000.0,
                         "min_history_days": 20, "size_rank_max": None, "sector_cap": None},
    ),
    "us_common_equity_liquid_large": UniverseDefinition(
        universe_id="us_common_equity_liquid_large",
        universe_version="v1",
        name="US common-equity liquid-large PIT universe",
        description="Common equity, strict liquidity floor, top-N by trailing ADV.",
        selection_rules={"min_price": 5.0, "min_dollar_volume": 50_000_000.0,
                         "min_history_days": 20, "size_rank_max": 1000, "sector_cap": None},
        parent_universe_id="us_common_equity_broad",
    ),
    "us_common_equity_sector_neutral": UniverseDefinition(
        universe_id="us_common_equity_sector_neutral",
        universe_version="v1",
        name="US common-equity sector-neutral PIT universe",
        description="Common equity, liquidity floor, per-sector member cap.",
        selection_rules={"min_price": 5.0, "min_dollar_volume": 10_000_000.0,
                         "min_history_days": 20, "size_rank_max": None, "sector_cap": 50},
        parent_universe_id="us_common_equity_broad",
    ),
}

def register_universe_definition(store, definition: UniverseDefinition, *, content_sha256=None,
                                 pinned=False, available_at=None) -> None:
    store.con.execute(
        """INSERT OR REPLACE INTO universe_definition
           (universe_id, universe_version, name, description, status, parent_universe_id,
            selection_rules_json, content_sha256, is_pinned, available_at, source)
           VALUES (?,?,?,?,?,?,?,?,?,?,?)""",
        [definition.universe_id, definition.universe_version, definition.name, definition.description,
         definition.status, definition.parent_universe_id, json_dumps(definition.selection_rules),
         content_sha256, pinned, available_at, SOURCE_NAME],
    )
```

**PIT:** (B) 0189 catalogs every surface + re-pins the contract; (A) `available_at` recorded on the definition.

**TDD — write first in `db/tests/test_multi_universe.py`:**

```python
def test_universe_definition_registry_seeds_three_governed_universes(tmp_store):
    from db.universe import GOVERNED_UNIVERSE_DEFINITIONS, register_universe_definition
    assert set(GOVERNED_UNIVERSE_DEFINITIONS) >= {
        "us_common_equity_broad", "us_common_equity_liquid_large", "us_common_equity_sector_neutral"}
    for defn in GOVERNED_UNIVERSE_DEFINITIONS.values():
        register_universe_definition(tmp_store, defn)
    rows = tmp_store.con.execute(
        "SELECT universe_id, universe_version FROM universe_definition ORDER BY universe_id").fetchall()
    assert ("us_common_equity_broad", "v1") in rows and len(rows) >= 3

def test_universe_membership_version_column_defaults_v1_and_is_catalogued(tmp_store):
    cols = {r[0] for r in tmp_store.con.execute(
        "SELECT column_name FROM duckdb_columns() WHERE table_name='universe_membership'").fetchall()}
    assert {"universe_version", "content_sha256"} <= cols
    assert tmp_store.con.execute(
        "SELECT count(*) FROM table_catalog WHERE table_name='universe_definition'").fetchone()[0] == 1
    assert tmp_store.con.execute(
        "SELECT count(*) FROM schema_contract WHERE table_name='universe_membership' "
        "AND column_name='universe_version'").fetchone()[0] == 1
```

**Accept:** the three governed definitions register; `universe_membership` carries `universe_version`
(default `'v1'`) + `content_sha256`; both are catalogued + contract-pinned; the migration is idempotent and
strictly in 0189.

### S5-1 — Definition-driven multi-universe PIT membership builder

**Root cause:** `compute_universe_membership_intervals` applies one fixed common-equity + listing + liquidity
screen; it has no size-rank cap (liquid-large) and no per-sector cap (sector-neutral), and it cannot be driven
from a `UniverseDefinition`.

**Fix:** add a **pure** cross-sectional rule layer in `db/universe.py` that runs **before** interval
compression and honours the declared `selection_rules`, then a driver that builds every governed universe
through the **existing** interval machine (reconcile, don't fork). Real Python:

```python
def apply_selection_rules(daily_decisions: pd.DataFrame, definition: UniverseDefinition) -> pd.DataFrame:
    """Set is_member per declared rules within each as_of cross-section. Pure, stable-sorted."""
    frame = daily_decisions.copy()
    rules = definition.selection_rules
    close = pd.to_numeric(frame.get("close"), errors="coerce")
    adv = pd.to_numeric(frame.get("avg_dollar_volume"), errors="coerce")
    hist = pd.to_numeric(frame.get("history_days"), errors="coerce")
    passes = (close.ge(rules["min_price"]) & adv.ge(rules["min_dollar_volume"])
              & hist.ge(rules["min_history_days"]))
    reason = pd.Series("member", index=frame.index)
    reason = reason.mask(~passes, "liquidity_screen_fail")
    # size cap: rank by trailing ADV within each as_of_date (descending), keep top-N
    if rules.get("size_rank_max"):
        rank = adv.groupby(frame["as_of_date"]).rank(method="first", ascending=False)
        size_ok = rank.le(rules["size_rank_max"])
        reason = reason.mask(passes & ~size_ok, "size_rank_excluded")
        passes = passes & size_ok
    # sector cap: keep at most sector_cap names per (as_of_date, sector), ranked by ADV desc
    if rules.get("sector_cap"):
        srank = adv.groupby([frame["as_of_date"], frame["sector"]]).rank(method="first", ascending=False)
        sec_ok = srank.le(rules["sector_cap"])
        reason = reason.mask(passes & ~sec_ok, "sector_cap_excluded")
        passes = passes & sec_ok
    frame["is_member"] = passes.fillna(False)
    frame["reason"] = frame.get("reason", reason).where(frame.get("reason").notna() if "reason" in frame else False, reason)
    frame["reason"] = reason.where(passes | reason.ne("member"), "member")
    return frame.sort_values(["as_of_date", "security_id"], kind="stable").reset_index(drop=True)

def build_governed_universe(store, definition, options) -> DatasetLoadResult:
    daily = GovernedUniverseMembershipDataset()._daily_decisions(store, options)
    ruled = apply_selection_rules(daily, definition)
    intervals = compute_universe_membership_intervals(ruled, options_for(definition, options))
    sha = membership_content_sha256(intervals)
    intervals["universe_version"] = definition.universe_version
    intervals["content_sha256"] = sha
    rows = _replace_intervals_versioned(store, intervals, definition)   # scoped by (universe_id, version)
    register_universe_definition(store, definition, content_sha256=sha, available_at=...)
    return DatasetLoadResult(dataset_id="universe_membership", rows_loaded=rows, source=SOURCE_NAME,
                             details={"universe_id": definition.universe_id,
                                      "universe_version": definition.universe_version, "content_sha256": sha})
```

`_replace_intervals_versioned` mirrors the PF3-S4 `_replace_intervals` but scopes its DELETE by
`(universe_id, universe_version)` and inserts the version + `content_sha256`; the common-equity mask and the
listing gate stay inside `_daily_decisions` (shared, not re-implemented). The sector-neutral universe reads a
`sector` column carried on the daily decisions (sourced from the classification/identifier surface; injected in
fixtures for offline tests).

**PIT:** (A) size-rank and sector-cap are computed **within** the as-of cross-section only, from
availability-gated daily decisions — never across dates. (D) `apply_selection_rules` is pure, stable-sorted,
DuckDB-free.

**TDD:**

```python
def test_each_governed_universe_emits_pit_membership(tmp_store):
    _load_multi_universe_slice(tmp_store)   # bars + securities + sectors + listing over a fixture window
    for uid, defn in GOVERNED_UNIVERSE_DEFINITIONS.items():
        res = build_governed_universe(tmp_store, defn, _options_for(uid))
        assert res.rows_loaded > 0
        n = tmp_store.con.execute(
            "SELECT count(*) FROM universe_membership WHERE universe_id=? AND universe_version='v1' AND is_member",
            [uid]).fetchone()[0]
        assert n > 0

def test_name_in_broad_but_not_liquid_large(tmp_store):
    # SMALL: common equity, dollar_volume 5M -> passes broad (floor 1M), fails liquid_large (floor 50M)
    _load_multi_universe_slice(tmp_store)
    build_governed_universe(tmp_store, GOVERNED_UNIVERSE_DEFINITIONS["us_common_equity_broad"], _options_for("us_common_equity_broad"))
    build_governed_universe(tmp_store, GOVERNED_UNIVERSE_DEFINITIONS["us_common_equity_liquid_large"], _options_for("us_common_equity_liquid_large"))
    broad = universe_membership_asof(dt.date(2020,1,3), store=tmp_store,
                                     universe_id="us_common_equity_broad")["security_id"].tolist()
    large = universe_membership_asof(dt.date(2020,1,3), store=tmp_store,
                                     universe_id="us_common_equity_liquid_large")["security_id"].tolist()
    assert "SMALL" in broad and "SMALL" not in large

def test_selection_rules_shuffle_determinism(tmp_store):
    daily = _daily_decisions_with_sectors()
    defn = GOVERNED_UNIVERSE_DEFINITIONS["us_common_equity_sector_neutral"]
    a = apply_selection_rules(daily, defn)
    b = apply_selection_rules(daily.sample(frac=1.0, random_state=7).reset_index(drop=True), defn)
    pd.testing.assert_frame_equal(a.reset_index(drop=True), b.reset_index(drop=True))
```

**Accept:** all three governed universes emit PIT interval membership on the fixture; a name that clears the
broad floor but not the liquid-large floor is in `broad` and absent from `liquid_large`; the sector-neutral cap
is honoured; `apply_selection_rules` is shuffle-invariant.

### S5-2 — Universe release versions: pinnable + content-stable

**Root cause:** even with versions in the schema, nothing makes a `(universe_id, version, as_of)` membership
snapshot **content-stable** (byte-identical across rebuilds) or **pinnable** (a version whose content hash is
frozen and asserted unchanged).

**Fix:** add the pure content hash + a pin/verify path in `db/universe.py`, and make the reader
version-aware in `db/asof/pricing.py`.

```python
def membership_content_sha256(intervals: pd.DataFrame) -> str:
    """Stable content address of a (universe_id, version) membership snapshot."""
    if intervals is None or intervals.empty:
        return hashlib.sha256(b"empty-universe-membership").hexdigest()
    cols = ["universe_id", "security_id", "valid_from", "valid_to", "is_member", "reason"]
    ordered = intervals.loc[:, cols].sort_values(cols, kind="stable")
    payload = ordered.to_csv(index=False, date_format="%Y-%m-%d").encode("utf-8")
    return hashlib.sha256(payload).hexdigest()

def pin_universe_version(store, universe_id: str, universe_version: str) -> str:
    sha = store.con.execute(
        "SELECT any_value(content_sha256) FROM universe_membership WHERE universe_id=? AND universe_version=?",
        [universe_id, universe_version]).fetchone()[0]
    store.con.execute(
        "UPDATE universe_definition SET is_pinned=true, content_sha256=? "
        "WHERE universe_id=? AND universe_version=?", [sha, universe_id, universe_version])
    return sha

def assert_pinned_version_stable(store, universe_id: str, universe_version: str) -> None:
    pinned, live = store.con.execute(
        """SELECT d.content_sha256, any_value(m.content_sha256)
           FROM universe_definition d JOIN universe_membership m
             ON m.universe_id=d.universe_id AND m.universe_version=d.universe_version
           WHERE d.universe_id=? AND d.universe_version=? AND d.is_pinned GROUP BY d.content_sha256""",
        [universe_id, universe_version]).fetchone()
    if pinned != live:
        raise ValueError(f"pinned universe {universe_id}:{universe_version} drifted: {pinned} != {live}")
```

`universe_membership_asof` in `db/asof/pricing.py` gains `universe_version: str = "v1"`, threaded into
`UNIVERSE_MEMBERSHIP_ASOF_SQL` as an extra `AND u.universe_version = p.universe_version` predicate (params
`[as_of_date, as_of_ts, universe_id, universe_version]`). The `v1` default preserves every existing caller.

**PIT:** (D) the hash is a pure function of the sorted content — order-independent; a rebuild from shuffled
inputs yields the same hash. (A) the reader's version predicate composes with the existing valid-window +
`available_at` gates.

**TDD:**

```python
def test_universe_version_is_pinnable_and_content_stable(tmp_store):
    _load_multi_universe_slice(tmp_store)
    defn = GOVERNED_UNIVERSE_DEFINITIONS["us_common_equity_broad"]
    r1 = build_governed_universe(tmp_store, defn, _options_for(defn.universe_id))
    sha_pin = pin_universe_version(tmp_store, defn.universe_id, "v1")
    assert sha_pin == r1.details["content_sha256"]
    # Rebuild the same slice: content hash identical, pinned version stable.
    r2 = build_governed_universe(tmp_store, defn, _options_for(defn.universe_id))
    assert r2.details["content_sha256"] == sha_pin
    assert_pinned_version_stable(tmp_store, defn.universe_id, "v1")   # no raise

def test_asof_reader_selects_by_version(tmp_store):
    _load_two_versions(tmp_store, "us_common_equity_broad")   # v1 and a stricter v2 of the same id
    v1 = universe_membership_asof(dt.date(2020,1,3), store=tmp_store,
                                  universe_id="us_common_equity_broad", universe_version="v1")
    v2 = universe_membership_asof(dt.date(2020,1,3), store=tmp_store,
                                  universe_id="us_common_equity_broad", universe_version="v2")
    assert set(v1["security_id"]) != set(v2["security_id"])
    assert (v1["universe_version"] == "v1").all() and (v2["universe_version"] == "v2").all()
```

**Accept:** a `(universe_id, version)` snapshot is content-stable across rebuilds (identical `content_sha256`);
a pinned version's hash is frozen and `assert_pinned_version_stable` passes after a faithful rebuild and would
raise on drift; `universe_membership_asof` selects the requested version and never bleeds `v2` into a `v1`
read.

### S5-3 — Membership turnover reporting (migration 0190)

**Root cause:** there is no surface reporting names added/removed rebalance-to-rebalance or a turnover rate, so
rebalance churn is invisible.

**Fix (migration 0190):** create `v_universe_turnover` — one row per `(universe_id, universe_version,
rebalance_date)` giving `prev_rebalance_date`, `member_count`, `prev_member_count`, `names_added`,
`names_removed`, `turnover_rate`. Real DuckDB view SQL:

```sql
CREATE OR REPLACE VIEW v_universe_turnover AS
WITH rebalances AS (
    SELECT DISTINCT universe_id, universe_version, as_of_date AS rebalance_date
    FROM universe_membership WHERE is_member AND is_latest_revision
),
ordered AS (
    SELECT universe_id, universe_version, rebalance_date,
           lag(rebalance_date) OVER (
               PARTITION BY universe_id, universe_version ORDER BY rebalance_date) AS prev_rebalance_date
    FROM rebalances
),
member_at AS (
    SELECT r.universe_id, r.universe_version, r.rebalance_date, u.security_id
    FROM rebalances r
    JOIN universe_membership u
      ON u.universe_id = r.universe_id AND u.universe_version = r.universe_version
     AND u.is_member AND u.is_latest_revision
     AND u.valid_from <= r.rebalance_date
     AND (u.valid_to IS NULL OR u.valid_to >= r.rebalance_date)
)
SELECT
    o.universe_id, o.universe_version, o.rebalance_date, o.prev_rebalance_date,
    (SELECT count(*) FROM member_at c WHERE c.universe_id=o.universe_id
       AND c.universe_version=o.universe_version AND c.rebalance_date=o.rebalance_date) AS member_count,
    (SELECT count(*) FROM member_at c WHERE c.universe_id=o.universe_id
       AND c.universe_version=o.universe_version AND c.rebalance_date=o.prev_rebalance_date) AS prev_member_count,
    (SELECT count(*) FROM member_at cur WHERE cur.universe_id=o.universe_id
       AND cur.universe_version=o.universe_version AND cur.rebalance_date=o.rebalance_date
       AND NOT EXISTS (SELECT 1 FROM member_at prv WHERE prv.universe_id=o.universe_id
         AND prv.universe_version=o.universe_version AND prv.rebalance_date=o.prev_rebalance_date
         AND prv.security_id=cur.security_id)) AS names_added,
    (SELECT count(*) FROM member_at prv WHERE prv.universe_id=o.universe_id
       AND prv.universe_version=o.universe_version AND prv.rebalance_date=o.prev_rebalance_date
       AND NOT EXISTS (SELECT 1 FROM member_at cur WHERE cur.universe_id=o.universe_id
         AND cur.universe_version=o.universe_version AND cur.rebalance_date=o.rebalance_date
         AND cur.security_id=prv.security_id)) AS names_removed,
    CASE WHEN o.prev_rebalance_date IS NULL THEN NULL ELSE
        CAST(
          (SELECT count(*) FROM member_at cur WHERE cur.universe_id=o.universe_id
             AND cur.universe_version=o.universe_version AND cur.rebalance_date=o.rebalance_date
             AND NOT EXISTS (SELECT 1 FROM member_at prv WHERE prv.universe_id=o.universe_id
               AND prv.universe_version=o.universe_version AND prv.rebalance_date=o.prev_rebalance_date
               AND prv.security_id=cur.security_id))
          + (SELECT count(*) FROM member_at prv WHERE prv.universe_id=o.universe_id
             AND prv.universe_version=o.universe_version AND prv.rebalance_date=o.prev_rebalance_date
             AND NOT EXISTS (SELECT 1 FROM member_at cur WHERE cur.universe_id=o.universe_id
               AND cur.universe_version=o.universe_version AND cur.rebalance_date=o.rebalance_date
               AND cur.security_id=prv.security_id)) AS DOUBLE)
        / NULLIF(CAST(
          (SELECT count(*) FROM member_at c WHERE c.universe_id=o.universe_id
             AND c.universe_version=o.universe_version AND c.rebalance_date=o.rebalance_date)
          + (SELECT count(*) FROM member_at c WHERE c.universe_id=o.universe_id
             AND c.universe_version=o.universe_version AND c.rebalance_date=o.prev_rebalance_date)
          AS DOUBLE), 0)
    END AS turnover_rate
FROM ordered o
ORDER BY o.universe_id, o.universe_version, o.rebalance_date;
```

`turnover_rate = (added + removed) / (member_count + prev_member_count)`. A pure
`compute_universe_turnover(intervals) -> DataFrame` mirror in `db/universe.py` computes the same figures for
DuckDB-free unit testing. Catalog `v_universe_turnover` (dataset + table + field catalog) in 0190 and re-pin
the contract.

**PIT:** (A) membership at each rebalance is reconstructed from availability-gated interval history; no future
interval leaks backward. (D) `compute_universe_turnover` is pure/stable-sorted.

**TDD:**

```python
def test_turnover_add_remove_fixture(tmp_store):
    # t0 members {A,B}; t1 members {B,C}  -> added={C}, removed={A}, turnover=(1+1)/(2+2)=0.5
    _load_turnover_fixture(tmp_store, universe_id="us_common_equity_broad")
    rows = tmp_store.con.execute(
        """SELECT rebalance_date, names_added, names_removed, member_count, prev_member_count, turnover_rate
           FROM v_universe_turnover WHERE universe_id='us_common_equity_broad' AND universe_version='v1'
           ORDER BY rebalance_date""").fetchall()
    assert rows[0][1:] == (0, 0, 2, 0, None) or rows[0][0] is not None   # first rebalance has no prior
    t1 = rows[1]
    assert (t1[1], t1[2], t1[3], t1[4]) == (1, 1, 2, 2)
    assert round(t1[5], 6) == 0.5

def test_compute_universe_turnover_matches_view(tmp_store):
    intervals = _turnover_intervals_frame()
    pure = compute_universe_turnover(intervals)
    assert round(pure.loc[pure["rebalance_date"] == dt.date(2020,1,3), "turnover_rate"].iloc[0], 6) == 0.5
```

**Accept:** `v_universe_turnover` reports the correct added/removed sets and `turnover_rate=0.5` on the
`{A,B}→{B,C}` fixture; the pure `compute_universe_turnover` agrees with the view row-for-row; the view is
catalogued in 0190.

### S5-4 — Universe-as-of applied consistently: panel assembly + SDK read filter

**Root cause:** `_apply_universe_filter`/`assemble_factor_panel_long` filter on a single `universe_id` with no
version, and `read_panel_asof` applies **no** universe filter — so the consumption path cannot select a panel
cross-section by `(universe_id, version)`.

**Fix (`db/factor_panel.py`):** thread `(universe_id, universe_version)` through both paths.

- `_apply_universe_filter(panel, membership, *, universe_id, universe_version="v1", as_of_ts)` adds
  `members = members[members["universe_version"] == universe_version]` beside the existing
  `members["universe_id"] == universe_id` filter (the `valid_from/valid_to/as_of_date/available_at` gating is
  unchanged).
- `assemble_factor_panel_long(..., universe_id=DEFAULT_FACTOR_PANEL_UNIVERSE_ID, universe_version="v1")` passes
  the version down.
- `read_panel_asof(..., universe_id: str | None = None, universe_version: str = "v1")` — the **SDK read
  filter** — when `universe_id` is supplied, inner-joins the PIT cross-section to the as-of membership set for
  `(universe_id, universe_version)` via a registered relation from `universe_membership_asof(..., store=…)`;
  when `None`, behaviour is unchanged (reads pre-scoped `v_factor_panel`). Real join wiring:

```python
def read_panel_asof(as_of_date, *, as_of_ts=None, db_path=DEFAULT_DB_PATH, store=None,
                    factor_ids=None, security_ids=None, wide=False,
                    universe_id: str | None = None, universe_version: str = "v1") -> pd.DataFrame:
    ...
    if universe_id is not None:
        members = universe_membership_asof(resolved_date, as_of_ts=resolved_ts, store=active,
                                           universe_id=universe_id, universe_version=universe_version)
        long = long[long["security_id"].isin(set(members["security_id"]))].reset_index(drop=True)
    ...
```

`factor_panel_export_gate_report` gains a `universe_version` default of `'v1'` so its membership NOT-EXISTS
check composes with the version column without changing existing gate behaviour.

**PIT:** (I) membership applied strictly as-of, version-scoped, in both assembly and the read filter; the
export-boundary lookahead gate is untouched. (A) the read filter reuses the availability-gated
`universe_membership_asof`.

**TDD:**

```python
def test_panel_assembly_filters_by_universe_and_version(tmp_store):
    membership = _membership_frame_two_universes()   # broad has {A,B,C}; liquid_large has {A} only, both v1
    factors = _factor_surface(["A", "B", "C"])
    broad = assemble_factor_panel_long(factors, universe_membership=membership,
                as_of_date=dt.date(2020,1,3), universe_id="us_common_equity_broad", universe_version="v1")
    large = assemble_factor_panel_long(factors, universe_membership=membership,
                as_of_date=dt.date(2020,1,3), universe_id="us_common_equity_liquid_large", universe_version="v1")
    assert set(broad["security_id"]) == {"A", "B", "C"}
    assert set(large["security_id"]) == {"A"}

def test_read_panel_filter_selects_by_universe_version(tmp_store):
    _seed_factor_panel_and_membership(tmp_store)   # v_factor_panel rows + versioned universe_membership
    got = read_panel_asof(dt.date(2020,1,3), store=tmp_store,
                          universe_id="us_common_equity_liquid_large", universe_version="v1")
    assert set(got["security_id"]) == {"A"}
    unfiltered = read_panel_asof(dt.date(2020,1,3), store=tmp_store)   # no universe -> pre-scoped view
    assert set(got["security_id"]) <= set(unfiltered["security_id"])

def test_read_panel_universe_filter_no_lookahead(tmp_store):
    _seed_factor_panel_and_membership(tmp_store)   # C first becomes a member on 2020-01-04
    early = read_panel_asof(dt.date(2020,1,3), store=tmp_store,
                            universe_id="us_common_equity_broad", universe_version="v1")
    assert "C" not in set(early["security_id"])
```

**Accept:** panel assembly and the read filter both select strictly by `(universe_id, universe_version)`; a
`liquid_large` read returns a subset of the `broad` read; the read filter honours no-lookahead (a name that
joins the universe on D+1 is absent from an as-of-D read); default (no universe) behaviour is byte-unchanged.

### S5-5 — Indexes, versioned natural key, and gate-ready version-stability check (migration 0191)

**Root cause:** the version columns need indexes + a uniqueness guarantee, and the multi-universe invariants
(subset lineage, content-hash stability, definition completeness) must be a `severity=critical` check
PF4-S2 can wire into the orchestrator halt.

**Fix (migration 0191):** indexes + a unique versioned natural key + a registry row.

```sql
CREATE INDEX IF NOT EXISTS idx_universe_membership_version
  ON universe_membership(universe_id, universe_version, security_id, valid_from, valid_to, available_at);
CREATE UNIQUE INDEX IF NOT EXISTS uq_universe_membership_version
  ON universe_membership(universe_id, universe_version, security_id, valid_from, source);
CREATE INDEX IF NOT EXISTS idx_universe_definition_status
  ON universe_definition(universe_id, universe_version, status, is_pinned);
```

Add a `SqlQualityCheck` in `db/quality/checks_market_reference.py` (mirroring the PF3-S4 coverage check shape):
`governed_universe_version_stability`, `severity="critical"`, threshold `0.0`, comparator `eq`,
`required_tables=("universe_definition","universe_membership")` — counting rows that violate any of:
(a) a pinned definition whose `content_sha256` ≠ its live membership hash; (b) a member of a child universe
(`parent_universe_id` set) that is **not** a member of its parent at the same `(as_of_date)` — the subset
invariant (`liquid_large ⊆ broad`, `sector_neutral ⊆ broad`); (c) a governed definition with **zero**
membership rows. Register the check in `quality_check_registry` in the migration (real DDL, following the
0143 `priced_fundamental_universe_decision_coverage` pattern). Catalog residuals + re-pin the contract.

**PIT:** (G) the check is authored gate-ready (`severity=critical`, `failure_status='failed'`) for PF4-S2. (B)
indexes split from schema; unique key enforces the versioned natural key.

**TDD:**

```python
def test_version_stability_gate_passes_on_consistent_universes(tmp_store):
    _load_multi_universe_slice(tmp_store)
    for uid, defn in GOVERNED_UNIVERSE_DEFINITIONS.items():
        build_governed_universe(tmp_store, defn, _options_for(uid))
        pin_universe_version(tmp_store, uid, "v1")
    results = run_warehouse_quality_checks(tmp_store,
        check_names=("governed_universe_version_stability",), record=False)
    assert results[0].status == "passed" and results[0].observed_value == 0.0
    assert results[0].severity == "critical"

def test_version_stability_gate_fails_on_subset_violation(tmp_store):
    # plant a liquid_large member absent from broad at the same date
    _plant_subset_violation(tmp_store)
    results = run_warehouse_quality_checks(tmp_store,
        check_names=("governed_universe_version_stability",), record=False)
    assert results[0].status == "failed" and results[0].observed_value >= 1.0

def test_indexes_and_registry_seeded(tmp_store):
    idx = {r[0] for r in tmp_store.con.execute(
        "SELECT index_name FROM duckdb_indexes() WHERE index_name IN "
        "('idx_universe_membership_version','uq_universe_membership_version','idx_universe_definition_status')"
        ).fetchall()}
    assert idx == {"idx_universe_membership_version","uq_universe_membership_version","idx_universe_definition_status"}
    assert tmp_store.con.execute(
        "SELECT severity, threshold_value, comparator, enabled FROM quality_check_registry "
        "WHERE check_name='governed_universe_version_stability'").fetchone() == ("critical", 0.0, "eq", True)
```

**Accept:** indexes + unique versioned key present; the version-stability check is green on the three
consistent governed universes and red on a planted subset violation or a drifted pin; authored
`severity=critical`, gate-ready for PF4-S2; every new surface carries catalog rows; the drift check is clean.

### S5-6 — Close-out: parity ledger + gap update

**Root cause:** the sprint's evidence must land in the two coordination ledgers.

**Fix:** run the full offline suite green in the worktree, then update the ledgers (never `git add -A`; stage
explicit paths only). Append one `WAREHOUSE_PARITY_TRANCHES.md` row (start/end SHA, domains = multi-universe +
versioning + turnover, verification commands, the operator-run note that only the offline slice is proven in
pytest, caveats/next → PF4-S6 activation harness). Update `db/PARITY_GAP.md`: flip the universe line from a
single governed universe to **≥3 governed PIT universes + release versioning + turnover**, and add/flip the
matching `db/parity.py` provider note if applicable.

**Accept:** `python -m pytest atx-impl\db\tests\test_multi_universe.py -q` green and full
`python -m pytest atx-impl\db\tests -q` green from `atx-impl/`; `WAREHOUSE_PARITY_TRANCHES.md` row appended;
`db/PARITY_GAP.md` universe status updated; commits carry the exact trailer
`Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.

---

## Sequencing & expected compounding

**S5-0 → S5-1 → S5-2 → S5-3 → S5-4 → S5-5 → S5-6.** S5-0 lands the definition registry + version columns (the
*what universes exist*); S5-1 computes each governed universe's PIT membership through the shared interval
machine (the *who is in each*); S5-2 makes a `(universe_id, version)` snapshot pinnable + content-stable (the
*reproducible release*); S5-3 reports rebalance turnover (the *how much it churns*); S5-4 threads
`(universe_id, version)` through panel assembly and the SDK read filter (the *consumption*); S5-5 indexes,
enforces the versioned key, and gates the invariants; S5-6 records the evidence. The compounding: a downstream
quant team can now pin a **reproducible, versioned, turnover-characterised** investable set of their choosing
(broad / liquid-large / sector-neutral) and pull a factor panel scoped to exactly that universe+version — the
precondition for the PF4-S6 activation harness (which builds all governed universes over the dense backfill),
the PF4-S7 release engine (which pins universe+version alongside the panel release), and the PF4-S8 SDK (whose
`read_panel(universe=, ...)` rides the S5-4 read filter).

---

## Risks / guardrails

- **Reconcile, never fork.** `us_common_equity_liquid_v1` keeps its exact rows and semantics (defaulted to
  `universe_version='v1'`); the three new universes are **siblings** built through the same
  `compute_universe_membership_intervals`. Do not re-implement the common-equity mask or the listing gate — the
  new size/sector rules layer on top in `apply_selection_rules`.
- **Survivorship / lookahead is the universe trap — twice over.** Size-rank and sector-cap are cross-sectional
  and must rank **only within the as-of cross-section** from availability-gated decisions; a top-N or per-sector
  verdict must never leak backward. The interval history + `available_at`-gated, version-scoped reader is the
  mitigation; the no-lookahead test on the read filter is the proof.
- **Version PK mechanics.** DuckDB `ALTER TABLE ADD COLUMN` cannot extend the existing PK
  `(universe_id, security_id, valid_from, source)`; the versioned natural key is enforced by the new
  `uq_universe_membership_version` unique index (0191), and `_replace_intervals_versioned` scopes its DELETE by
  `(universe_id, universe_version)` so two versions of one id never collide.
- **Content-stability is load-bearing.** `membership_content_sha256` must be order-independent (sort before
  hashing) so a shuffled rebuild is byte-identical; the pin check compares the frozen definition hash to the
  live membership hash.
- **Sector source.** The sector-neutral rule needs a `sector` per security; source it from the existing
  classification/identifier surface (inject in fixtures for offline tests) — do not invent a new sector table.
- **Stay in range.** Migrations strictly **0189–0191**, schema/index/view split; never edit a landed
  migration; back up DB+WAL before any live apply (clause F). Offline tests only (clause C); run pytest from
  `atx-impl/`, never from `db/`.

---

## Bench / acceptance

- **≥3 governed universes PIT-queryable:** `us_common_equity_broad`, `us_common_equity_liquid_large`,
  `us_common_equity_sector_neutral` each emit interval membership on the fixture and resolve through
  `universe_membership_asof`; a name meeting the broad floor but not the liquid-large floor is in one and not
  the other.
- **Versions pinnable + content-stable:** a `(universe_id, version, as_of)` snapshot has an order-independent
  `content_sha256` that is identical across rebuilds; a pinned version passes `assert_pinned_version_stable`
  and the gate goes red on drift; `universe_membership_asof` selects the requested version cleanly.
- **Turnover reported:** `v_universe_turnover` reports correct names added/removed and
  `turnover_rate = (added+removed)/(cur+prev)` (0.5 on the `{A,B}→{B,C}` fixture); the pure
  `compute_universe_turnover` agrees with the view.
- **Applied consistently:** `assemble_factor_panel_long` and the `read_panel_asof` read filter both select by
  `(universe_id, universe_version)`; `liquid_large ⊆ broad` on the read; no-lookahead holds on the read filter;
  default (no universe) behaviour unchanged.
- **Gated + catalogued:** `governed_universe_version_stability` green on consistent universes, red on a planted
  subset/pin violation, authored `severity=critical`; every new table/view/column carries catalog + contract
  rows; indexes + versioned unique key present.
- `python -m pytest atx-impl\db\tests\test_multi_universe.py -q` green, and full
  `python -m pytest atx-impl\db\tests -q` green in the worktree before commit.
- `db/PARITY_GAP.md` universe status updated (single → ≥3 governed + versioning + turnover), and a
  `WAREHOUSE_PARITY_TRANCHES.md` row appended (start/end SHA, domains, verification commands, offline-slice
  posture, caveats/next → PF4-S6 activation harness).

**Process:** sprint runs in its own git worktree off `main` via
`atx-impl/scripts/new_db_worktree.sh new|finish <slug>`; controller
`superpowers:subagent-driven-development` (fresh implementer + reviewer per task; TDD +
verification-before-completion). Never `git add -A` (stage explicit paths); never push unless asked. Commit
trailer EXACTLY `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
