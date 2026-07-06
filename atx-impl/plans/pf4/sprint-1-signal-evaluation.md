# Sprint PF4-S1 — Signal-evaluation surface (IC / decay / quantile spread / turnover / crowding / breadth + gated DQC)

**Track:** Close pf3 (Track A). **This sprint IS the carried-forward PF3-S11** — the signal-evaluation surface
that pf3 scoped but never built. It is renumbered **PF4-S1**, its reserved migrations move from the abandoned
pf3 range **0168–0171** to **0176–0179** (pf3 landed through 0167 in code; pf4 starts at 0176 per the ROADMAP),
and its downstream consumer is now **PF4-S2** (panel gating + factor observability), not "S12".

**Goal:** score every factor in the exported panel for SIGNAL. The warehouse now *produces* factors (pf3 S8/S9)
and *exports* a PIT-safe, schema-hashed panel (pf3 S10 — `db/factor_panel.py`, the `v_factor_panel` view), but
nothing tells a researcher which of those factors actually carries alpha. This sprint builds that verdict layer
as **NEW `db/signal_eval.py`**: rank information coefficient (rank-IC) + IC information-ratio + t-stat +
sign-consistency over the horizon ladder {1, 5, 10, 21, 63}; IC-decay across that ladder; quantile/decile
long-short spread + per-decile monotonicity + hit-rate; factor turnover + rank autocorrelation; factor-to-factor
correlation + crowding; per-date breadth — plus two factor-level data-quality checks (**leakage** t+0 probe and
**coverage**) registered `severity=critical` and **gate-ready** so PF4-S2 can halt a bad panel build.

**Goal metric (sprint is done when):** every factor in the proof-slice panel receives IC / IC-decay / quantile
spread / turnover / crowding / breadth rows; a zero-signal fixture → rank-IC ≈ 0; a persistent-but-fading fixture
→ monotone-decaying IC profile; a monotone fixture → monotone deciles + positive long-short spread; a random-walk
fixture → ~uniform deciles + high turnover; two near-duplicate factors → mutually crowded; a planted leaky factor
→ leakage DQC **RED**, a properly-lagged factor → **GREEN**; a sparse factor → coverage DQC **RED**; every
`compute_*` transform is deterministic (row-order-shuffled input → byte-identical output); the full offline suite
is green from `atx-impl/`.

**Mandate / Owns:** NEW `db/signal_eval.py` (the pure IC / IC-decay / quantile-spread / turnover /
correlation-crowding / breadth transforms, their DuckDB persistence, and per-factor evaluation manifests); the
two factor-level DQC checks registered inside the (pf3-S3-decomposed) `db/quality/` package reusing
`QualityResult`; NEW `db/migrations/bodies_0176_0179.py` (four migrations) wired through
`db/migrations/registry.py`; a minimal append-style hook in `db/quality/_runner.py` mirroring the pf3-S10
`factor_panel_export_contract` block; NEW `db/tests/test_signal_eval.py`. The evaluation surfaces land under
migrations **0176–0179** with their own `table_catalog` / `field_catalog` rows (clause E) and reuse the
alpha-backtest manifest shape (`backtest_id`-style deterministic hash id + `params_json` + `source` + `run_id`)
established by `db/alpha_research.py::_build_manifests` — extended to per-factor grain.

**Must NOT touch:** the panel *materialization* — pf3-S10 owns `db/factor_panel.py`, the `v_factor_panel` /
`v_factor_panel_wide` views, and the Parquet lake export; PF4-S1 **reads** the exported panel read-only and never
rewrites it or its `schema_sha256`. The factor engine, factor families, and cross-domain assembly (pf3 S7–S9, the
`db/factors/` package) are frozen inputs — PF4-S1 does not add, redefine, or re-neutralize a factor. Critically,
**no forward-return lookahead may leak back into the factor values themselves**: forward returns exist here only
as a scoring *target*, never as a factor *input*. Do not edit a landed migration (≤ 0175) or another sprint's
reserved region (PF4-S2 owns 0180–0183). Do not promote the DQC checks to an actual orchestrator halt — authoring
them `severity=critical` and gate-**ready** is the deliverable; wiring the halt is **PF4-S2**'s job.

**Depends on:** pf3-S10 (the exported factor panel + its schema-contracted PIT views — the object under
evaluation; `v_factor_panel`, `read_panel_asof`). Forward returns are derived from `equity_daily_bars` /
`equity_price_metrics`. **Reconciliation vs the pf3 audit:** the ROADMAP records that the dense price backfill
(pf3-S4 / now PF4-S6) never ran — `equity_daily_bars` holds only ~3.18M rows for 2012–2014 while fundamentals are
2017–2026, so the live price×fundamental overlap is effectively empty. PF4-S1 therefore proves every scorer on
**injected offline fixtures** (clause C); the live per-factor counts become meaningful only once PF4-S4
(survivorship-safe returns) and PF4-S6 (dense backfill) land. This is stated honestly in the closeout ledger row.
Sequential **after** the pf3 S1–S10 code (all landed); PF4-S1 is the first pf4 sprint and the last content sprint
before PF4-S2 gates and observes the surface it produces.

---

## Baseline / where the cycles go

Measured 2026-07-06 against `atx-impl/db`. Factors can be built and shipped, but there is no honest, per-factor
verdict on whether any of them predicts returns.

1. **Factors are produced and exported but NOT scored.** pf3 S8/S9 emit fundamental + cross-domain factor
   families into one namespace; pf3 S10 exports them as `v_factor_panel` + partitioned Parquet. Nothing then asks
   *does this factor predict forward returns* — no per-factor rank-IC, no IC-decay-across-horizons, no turnover.
   A zero-signal factor is indistinguishable from the best factor in the panel: both are just columns.
2. **The only IC/spread machinery that exists is per-ALPHA, not per-FACTOR.** `db/alpha_research.py`
   (`AlphaResearchDataset._build_manifests`, lines 333–461) runs a backtest over composite *alpha expressions* —
   `corr(signal_value, forward_return) AS rank_ic` (line 373), `hit_rate` (line 388), a top/bottom-quantile
   long-short (lines 264–303), forward return via
   `lead(close, ?) OVER (PARTITION BY security_id ORDER BY trade_date)` over `equity_daily_bars` (line 349),
   written to `alpha_backtest_manifests`. Right *shape*, wrong *grain*: it scores a handful of hand-authored
   alphas, aggregates one number per alpha, and computes no IC decay, no factor turnover, no crowding. There is
   **no per-factor evaluation surface** over the exported panel.
3. **No crowding / correlation view exists to detect redundant factors.** The word "crowding" already appears in
   `db/short_interest_metrics.py` but means *short-interest days-to-cover crowding* — a different concept. There
   is no factor-to-factor correlation matrix and no notion of a factor being redundant. A dozen near-collinear
   value factors could ship as "twelve signals" with no signal that they are one.
4. **There is no gated factor DQC.** A factor that is accidentally leaky (its lag dropped upstream, so it
   correlates with the contemporaneous return) or effectively empty (present for a tiny fraction of the universe)
   would pass every existing check and ship silently. Leakage and coverage are asserted nowhere at factor grain.

**Already good — do not regress:**
- **The exported panel and its schema-hash.** `v_factor_panel` (long) + `v_factor_panel_wide` +
  `db/factor_panel.py::read_panel_asof` are the fixed object under evaluation; PF4-S1 reads them read-only and
  must not perturb the export contract or `PANEL_CONTRACT_SHA256`.
- **`equity_daily_bars` / `equity_price_metrics` as the forward-return input.** The canonical price surface; PF4-S1
  derives horizon forward returns from it rather than re-deriving prices.
- **The alpha manifest pattern.** `alpha_research.py`'s manifest discipline (`_hash_id` deterministic id +
  `json_dumps(params)` + `source` + `run_id`, persisted via `insert_frame` with a DELETE-then-insert idempotent
  `_replace_rows` transaction) is the template the PF4-S1 evaluation manifests reuse — extended to per-factor
  grain, not reinvented.

---

## PIT / determinism + production contract

ROADMAP clauses **(A)** bitemporal / no-lookahead, **(C)** offline no-network tests, **(D)** determinism +
provenance, **(E)** schema-as-contract, **(G)** quality-gated, **(I)** panel PIT-safety, and **(J)** semantic
contract apply in full. The load-bearing constraint of this sprint is that **the evaluation itself must be
honest**: forward returns are used ONLY as a scoring target (t+1..t+h) and are never fed back into any factor
value.

- **(A)/(I)** Every score is computed cross-sectionally *within a single as-of cross-section* and only then
  aggregated across dates. Forward returns are strictly future-dated relative to the factor's `as_of_date`; the
  factor value at `as_of_date` may use only inputs with `available_at ≤ as_of_date` (already enforced by
  `v_factor_panel`). No pooled (date-mixing) correlation that would let one date's future leak into another's
  score.
- **(D)** All `compute_*` scorers are pure (pandas panel + forward-return frames in → long DataFrame out),
  stable-sorted, unit-tested independent of DuckDB; identical inputs + params produce byte-identical rows; every
  persisted row records `factor_id`, horizon, date window, `source`, and `run_id` lineage plus a manifest
  `eval_id`.
- **(E)/(B)** Migrations **0176** (IC + IC-decay + eval-manifest surface), **0177** (quantile/decile spread +
  turnover), **0178** (correlation/crowding + breadth), **0179** (factor-DQC catalog + registry seeds + indexes).
  Given the 1-migration-per-task-group budget, **each migration seeds its tables *and* their
  `table_catalog`/`field_catalog` rows *and* their indexes in the same reserved number**, strictly within
  0176–0179, never editing a landed migration, and calls `_refresh_schema_contract_v2_pin(conn)` last so the
  schema-as-contract drift check stays green. Timestamped DB+WAL backup precedes any live apply (clause F).
- **(C)** Every test runs against an in-memory / template-copy DuckDB (`tmp_store` fixture) with fixture factors
  and fixture forward returns — including a deliberately-planted leaky factor and a deliberately-sparse factor.
  No network. Live proof-slice counts are operator-run and recorded in the ledger.
- **(G)** The two factor DQC checks (leakage, coverage) are authored `severity=critical` and seeded into
  `quality_check_registry` so they run through the standard `run_warehouse_quality_checks` /
  `evaluate_quality_gate` path and PF4-S2 halts a run on a red factor-DQC result.
- **(J)** Every emitted metric column declares its unit/sign/scale via `field_catalog` (correlations are
  dimensionless in [-1, 1]; returns are decimal fractions; counts are non-negative integers).

---

## Interfaces contract (real signatures — implement exactly)

`db/signal_eval.py` module-level constants and public surface:

```python
IC_HORIZONS: tuple[int, ...] = (1, 5, 10, 21, 63)
DEFAULT_N_QUANTILES: int = 10
SOURCE_NAME: str = "atx-impl signal evaluation engine"
DEFAULT_UNIVERSE_ID: str = "us_common_equity_liquid_v1"

# clause-G gated DQC check names (registered in quality_check_registry by migration 0179)
LEAKAGE_DQC_CHECK_NAME: str = "factor_leakage_tplus0"
COVERAGE_DQC_CHECK_NAME: str = "factor_coverage_asof_universe"
DEFAULT_LEAKAGE_ABS_CORR_THRESHOLD: float = 0.10   # |corr(factor, t+0 return)| above this ⇒ leaky
DEFAULT_COVERAGE_MIN_FRACTION: float = 0.50        # factor must cover ≥ 50% of the as-of universe

@dataclass(frozen=True)
class IcResult:
    ic: pd.DataFrame        # factor_id, horizon, mean_rank_ic, ic_std, ic_information_ratio,
                            #   ic_tstat, sign_consistency, n_dates, mean_names
    ic_decay: pd.DataFrame  # factor_id, horizon, ladder_position, mean_rank_ic, decay_ratio
    per_date: pd.DataFrame  # factor_id, as_of_date, horizon, rank_ic, n_names   (intermediate; not persisted)

# ---- pure transforms (pandas in → long DataFrame out; no DuckDB) ----
def compute_forward_returns(prices: pd.DataFrame, horizons: Iterable[int] = IC_HORIZONS) -> pd.DataFrame: ...
    # prices: security_id, as_of_date, close  →  long: security_id, as_of_date, horizon, forward_return
def compute_information_coefficient(panel: pd.DataFrame, forward_returns: pd.DataFrame, *,
                                    horizons: Iterable[int] = IC_HORIZONS) -> IcResult: ...
def compute_quantile_spread(panel: pd.DataFrame, forward_returns: pd.DataFrame, *,
                            n_quantiles: int = DEFAULT_N_QUANTILES,
                            horizons: Iterable[int] = IC_HORIZONS) -> pd.DataFrame: ...
def compute_turnover(panel: pd.DataFrame, *, n_quantiles: int = DEFAULT_N_QUANTILES) -> pd.DataFrame: ...
def compute_factor_correlation(panel: pd.DataFrame) -> pd.DataFrame: ...
def compute_crowding(correlation: pd.DataFrame) -> pd.DataFrame: ...
def compute_breadth(panel: pd.DataFrame, universe_counts: pd.DataFrame | None = None) -> pd.DataFrame: ...
def compute_leakage(panel: pd.DataFrame, same_day_returns: pd.DataFrame, *,
                    threshold: float = DEFAULT_LEAKAGE_ABS_CORR_THRESHOLD) -> pd.DataFrame: ...
def compute_coverage(panel: pd.DataFrame, universe_counts: pd.DataFrame, *,
                     min_fraction: float = DEFAULT_COVERAGE_MIN_FRACTION) -> pd.DataFrame: ...

# ---- panel read (read-only) + persistence + orchestration (DuckDB) ----
def load_panel_for_eval(store, *, start_date=None, end_date=None,
                        factor_ids: Iterable[str] | None = None) -> pd.DataFrame: ...   # SELECT ... FROM v_factor_panel
def evaluate_panel(store, *, forward_returns: pd.DataFrame | None = None,
                   n_quantiles: int = DEFAULT_N_QUANTILES, horizons: Iterable[int] = IC_HORIZONS,
                   universe_id: str = DEFAULT_UNIVERSE_ID, run_id: str | None = None) -> dict[str, int]: ...
def factor_leakage_report(store, *, panel=None, same_day_returns=None,
                          threshold: float = DEFAULT_LEAKAGE_ABS_CORR_THRESHOLD) -> dict[str, object]: ...
def factor_coverage_report(store, *, panel=None, universe_counts=None,
                           min_fraction: float = DEFAULT_COVERAGE_MIN_FRACTION) -> dict[str, object]: ...
def signal_eval_dqc_results(store, *, registry, requested_checks, requested_datasets, checked_at) -> list[QualityResult]: ...
```

Reuse from the codebase (do not reinvent): `from .connection import DEFAULT_DB_PATH, connect`;
`from .warehouse import insert_frame, json_dumps, quality_check`; the `_hash_id(prefix, *parts)` id-hashing
idiom and the `_replace_rows`/`store.transaction()` DELETE-then-`insert_frame` idempotent write pattern from
`db/alpha_research.py`; `from .quality import QualityResult` for DQC results.

---

## Tasks

### PF4-S1-0 — Information-coefficient surface (rank-IC + IC decay + eval-manifest) — migration 0176

**Root cause:** the exported panel is unscored — there is no per-factor rank-IC and no view of how predictive
power decays with horizon. The only IC in the tree is `alpha_research.py`'s per-alpha
`corr(signal_value, forward_return)`: wrong grain, no decay.

**Files:**
- NEW `db/signal_eval.py` — module scaffold + `IC_HORIZONS`/constants/`IcResult`, `compute_forward_returns`,
  `compute_information_coefficient`, `load_panel_for_eval`, `_hash_eval_id`, `_build_ic_manifest`,
  `persist_factor_ic`, and the `_replace_rows` helper.
- NEW `db/migrations/bodies_0176_0179.py` — start the file; add migration **0176**
  (`_pf4_s1_ic_surface`) creating `factor_eval_manifest`, `factor_ic`, `factor_ic_decay`, their
  `table_catalog`/`field_catalog` rows, indexes, and `_refresh_schema_contract_v2_pin`.
- EDIT `db/migrations/registry.py` — import + append `_MIGRATIONS_0176_0179`.
- NEW `db/tests/test_signal_eval.py` — start the file with the IC tests.

**Interfaces / DDL (migration 0176 — real SQL, no placeholders):**

```python
# db/migrations/bodies_0176_0179.py
"""PF4-S1 migration bodies: signal-evaluation surface (IC / decay / quantile / turnover / crowding / breadth / DQC)."""
from __future__ import annotations
import duckdb
from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

def _pf4_s1_ic_surface(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS factor_eval_manifest (
            eval_id VARCHAR PRIMARY KEY,
            factor_id VARCHAR NOT NULL,
            eval_kind VARCHAR NOT NULL,
            universe_id VARCHAR NOT NULL,
            start_date DATE,
            end_date DATE,
            horizon_days INTEGER,
            n_quantiles INTEGER,
            evaluation_days BIGINT,
            factor_row_count BIGINT,
            params_json VARCHAR NOT NULL,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            created_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS factor_ic (
            eval_id VARCHAR NOT NULL,
            factor_id VARCHAR NOT NULL,
            horizon INTEGER NOT NULL,
            mean_rank_ic DOUBLE,
            ic_std DOUBLE,
            ic_information_ratio DOUBLE,
            ic_tstat DOUBLE,
            sign_consistency DOUBLE,
            n_dates BIGINT,
            mean_names DOUBLE,
            universe_id VARCHAR NOT NULL,
            start_date DATE,
            end_date DATE,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS factor_ic_decay (
            eval_id VARCHAR NOT NULL,
            factor_id VARCHAR NOT NULL,
            horizon INTEGER NOT NULL,
            ladder_position INTEGER NOT NULL,
            mean_rank_ic DOUBLE,
            decay_ratio DOUBLE,
            universe_id VARCHAR NOT NULL,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (table_name, layer, entity, grain, description, natural_key_json, pit_notes, updated_at) VALUES
        ('factor_eval_manifest','control','factor_eval_manifest','eval_id',
         'Per-factor signal-evaluation manifest (one row per factor/eval-kind/params run) mirroring the alpha-backtest manifest shape.',
         '["eval_id"]',
         'Manifest lineage only; evaluation reads v_factor_panel read-only and never rewrites factor values.', now()),
        ('factor_ic','metric','factor_ic','factor_id,horizon,universe_id,run_id',
         'Per-factor aggregate rank-IC over the horizon ladder: mean rank-IC, IC information ratio, IC t-stat, sign-consistency.',
         '["factor_id","horizon","universe_id","run_id"]',
         'Rank-IC computed cross-sectionally per as_of_date then aggregated across dates; forward returns strictly t+1..t+h.', now()),
        ('factor_ic_decay','metric','factor_ic_decay','factor_id,horizon,universe_id,run_id',
         'Per-factor rank-IC decay profile across the horizon ladder with decay ratio vs the shortest horizon.',
         '["factor_id","horizon","universe_id","run_id"]',
         'Decay = mean rank-IC per horizon ordered by the ladder; no cross-date pooling.', now())
        """
    )
    for stmt in (
        "CREATE INDEX IF NOT EXISTS idx_factor_ic_factor_horizon ON factor_ic(factor_id, horizon)",
        "CREATE INDEX IF NOT EXISTS idx_factor_ic_decay_factor ON factor_ic_decay(factor_id, ladder_position)",
        "CREATE INDEX IF NOT EXISTS idx_factor_eval_manifest_factor_kind ON factor_eval_manifest(factor_id, eval_kind)",
    ):
        conn.execute(stmt)
    _catalog_fields_for_tables(conn, ("factor_eval_manifest", "factor_ic", "factor_ic_decay"))
    _refresh_schema_contract_v2_pin(conn)

MIGRATIONS: list[Migration] = [
    Migration(version=176, name="pf4_s1_ic_surface", up=_pf4_s1_ic_surface),
    # 0177, 0178, 0179 appended by later tasks in this same sprint
]
```

`db/migrations/registry.py` edit (append only, keep versions ascending):
```python
from .bodies_0176_0179 import MIGRATIONS as _MIGRATIONS_0176_0179
MIGRATIONS = [ ..., *_MIGRATIONS_0164_0167, *_MIGRATIONS_0176_0179 ]
```

`compute_information_coefficient` semantics (implement exactly):
- Merge `panel` (`security_id, as_of_date, factor_id, value`) with `forward_returns`
  (`security_id, as_of_date, horizon, forward_return`) on `(security_id, as_of_date)`.
- **Per `(factor_id, as_of_date, horizon)`**: Spearman rank correlation between `value` and `forward_return`
  across the names in that cross-section (skip cross-sections with < 3 non-null pairs → `rank_ic` NaN, excluded).
  Implement Spearman as Pearson correlation of the *ranks* (`scipy`-free: `Series.rank()`), so no new dependency.
- **Aggregate across dates per `(factor_id, horizon)`**: `mean_rank_ic = mean(rank_ic)`,
  `ic_std = std(rank_ic, ddof=1)`, `ic_information_ratio = mean_rank_ic / ic_std`,
  `ic_tstat = ic_information_ratio * sqrt(n_dates)`,
  `sign_consistency = mean(sign(rank_ic) == sign(mean_rank_ic))`, `n_dates`, `mean_names`.
- **IC-decay**: order horizons ascending → `ladder_position = 1..len(horizons)`;
  `decay_ratio = mean_rank_ic(h) / mean_rank_ic(shortest horizon)` (NaN-safe; 1.0 at the shortest).
- Stable-sort every output by `["factor_id", "horizon"]` and reset_index → determinism (clause D).

**TDD steps (failing test first — real code in `db/tests/test_signal_eval.py`):**

```python
from __future__ import annotations
import datetime as dt
import numpy as np
import pandas as pd
import pytest
from db.signal_eval import (
    IC_HORIZONS, IcResult,
    compute_forward_returns, compute_information_coefficient,
    load_panel_for_eval, evaluate_panel,
)

def _dates(n: int, start="2020-01-01") -> list[dt.date]:
    d0 = pd.Timestamp(start)
    return [(d0 + pd.Timedelta(days=7 * i)).date() for i in range(n)]

def _panel(factor_id: str, values_by_date: dict) -> pd.DataFrame:
    rows = []
    for as_of, sec_vals in values_by_date.items():
        for sec, val in sec_vals.items():
            rows.append({"security_id": sec, "as_of_date": as_of, "factor_id": factor_id, "value": float(val)})
    return pd.DataFrame(rows)

def test_zero_signal_factor_has_rank_ic_near_zero() -> None:
    rng = np.random.default_rng(7)
    dates = _dates(40)
    secs = [f"S{i}" for i in range(30)]
    # factor and forward returns are independent random draws -> no relationship
    panel_rows, fr_rows = [], []
    for d in dates:
        for s in secs:
            panel_rows.append({"security_id": s, "as_of_date": d, "factor_id": "noise", "value": rng.normal()})
            for h in IC_HORIZONS:
                fr_rows.append({"security_id": s, "as_of_date": d, "horizon": h, "forward_return": rng.normal()})
    result = compute_information_coefficient(pd.DataFrame(panel_rows), pd.DataFrame(fr_rows))
    assert isinstance(result, IcResult)
    mean_ic_h1 = result.ic.loc[result.ic["horizon"] == 1, "mean_rank_ic"].iloc[0]
    assert abs(mean_ic_h1) < 0.05

def test_persistent_but_fading_factor_has_monotone_decaying_ic() -> None:
    rng = np.random.default_rng(11)
    dates = _dates(60)
    secs = [f"S{i}" for i in range(40)]
    panel_rows, fr_rows = [], []
    for d in dates:
        base = {s: rng.normal() for s in secs}
        for s in secs:
            panel_rows.append({"security_id": s, "as_of_date": d, "factor_id": "fade", "value": base[s]})
            for h in IC_HORIZONS:
                decay = 0.9 ** (IC_HORIZONS.index(h))         # weaker signal at longer horizons
                fr_rows.append({"security_id": s, "as_of_date": d, "horizon": h,
                                "forward_return": decay * base[s] + 0.25 * rng.normal()})
    decay = compute_information_coefficient(pd.DataFrame(panel_rows), pd.DataFrame(fr_rows)).ic_decay
    profile = decay.sort_values("ladder_position")["mean_rank_ic"].to_numpy()
    assert np.all(np.diff(profile) <= 1e-9)                    # non-increasing rank-IC across the ladder
    assert profile[0] > profile[-1]

def test_compute_ic_is_order_invariant() -> None:
    rng = np.random.default_rng(3)
    dates = _dates(20); secs = [f"S{i}" for i in range(15)]
    panel_rows, fr_rows = [], []
    for d in dates:
        for s in secs:
            v = rng.normal()
            panel_rows.append({"security_id": s, "as_of_date": d, "factor_id": "f", "value": v})
            for h in IC_HORIZONS:
                fr_rows.append({"security_id": s, "as_of_date": d, "horizon": h, "forward_return": v + rng.normal()})
    p, fr = pd.DataFrame(panel_rows), pd.DataFrame(fr_rows)
    a = compute_information_coefficient(p, fr).ic
    b = compute_information_coefficient(p.sample(frac=1.0, random_state=99).reset_index(drop=True),
                                        fr.sample(frac=1.0, random_state=1).reset_index(drop=True)).ic
    pd.testing.assert_frame_equal(a.reset_index(drop=True), b.reset_index(drop=True))

def test_compute_forward_returns_from_prices() -> None:
    prices = pd.DataFrame({
        "security_id": ["S"] * 5,
        "as_of_date": _dates(5),
        "close": [100.0, 110.0, 121.0, 133.1, 146.41],
    })
    fr = compute_forward_returns(prices, horizons=[1])
    r = fr.sort_values("as_of_date")["forward_return"].dropna().to_numpy()
    assert np.allclose(r, [0.10, 0.10, 0.10, 0.10])           # 10% per step, last row NaN dropped

def test_evaluate_panel_persists_ic_rows_per_factor(tmp_store) -> None:
    # v_factor_panel is empty on a fresh template; evaluate_panel with an injected panel/returns still writes rows.
    # (Full base-table seeding is exercised in the DQC integration test; here we assert the persistence contract.)
    counts = evaluate_panel(
        tmp_store,
        forward_returns=None,
        run_id="rid-ic",
    )
    assert "factor_ic" in counts and "factor_ic_decay" in counts
```
1. Add the imports and the tests above → `python -m pytest db\tests\test_signal_eval.py -q` fails at import
   (`db/signal_eval.py` does not exist) — **observe red**.
2. Implement `db/signal_eval.py` (constants, `IcResult`, `compute_forward_returns`,
   `compute_information_coefficient`, `load_panel_for_eval` selecting
   `security_id, as_of_date, factor_id, value, available_at FROM v_factor_panel` with optional filters,
   `_hash_eval_id`, `_build_ic_manifest`, `persist_factor_ic`, `evaluate_panel` orchestrator that reads the panel
   via `load_panel_for_eval` (or uses the injected panel), derives `forward_returns` from `equity_daily_bars`
   when not injected, runs IC, and persists via the idempotent `_replace_rows` transaction).
3. Add migration 0176 + registry wiring (regenerate the `tmp_store` schema template by deleting
   `atx-impl/.pytest_cache/db_schema_templates/` if a stale template is cached).
4. `python -m pytest db\tests\test_signal_eval.py -q` → **observe green**.

**PIT:** rank-IC is computed cross-sectionally per `as_of_date` then averaged — no pooled cross-date leakage;
forward returns are strictly t+1..t+h; the transform is pure and deterministic.

**Verification (cwd: `atx-impl/`):**
- `python -m pytest db\tests\test_signal_eval.py -q`
- `python -c "from db.migrations.registry import MIGRATIONS; print(max(m.version for m in MIGRATIONS))"` → `179`
  after all four tasks; `176` after this task.
- `python -c "from db.connection import connect; from db.migrations._runner import apply_pending_migrations"` sanity
  (import-only) must not raise.

**Gates:** migration 0176 applies cleanly on a fresh template; the three IC tests + forward-return test + persist
test pass; the determinism (order-invariance) property test passes; `factor_ic`/`factor_ic_decay`/
`factor_eval_manifest` appear in `table_catalog` and `field_catalog` (asserted in PF4-S1-4's catalog sweep).

**Accept:** every factor in the panel receives a rank-IC row and an IC-decay row across all horizons; zero-signal
→ rank-IC ≈ 0; persistent-but-fading → monotone-decaying IC profile; identical inputs reproduce identical rows.

---

### PF4-S1-1 — Quantile / decile spread + turnover — migration 0177

**Root cause:** there is no per-factor decile long-short spread, hit-rate, or turnover. `alpha_research.py` has a
top/bottom-quantile long-short but only per composite alpha and as a single aggregate; factor-level turnover /
rank-autocorrelation does not exist.

**Files:**
- EDIT `db/signal_eval.py` — add `compute_quantile_spread`, `compute_turnover`, `_build_quantile_manifest`,
  `persist_quantile_spread`, `persist_turnover`; extend `evaluate_panel` to run + persist them.
- EDIT `db/migrations/bodies_0176_0179.py` — add migration **0177** (`_pf4_s1_quantile_turnover`) + append to
  `MIGRATIONS`.
- EDIT `db/tests/test_signal_eval.py` — add the quantile/turnover tests.

**Interfaces / DDL (migration 0177 — real SQL):**
```python
def _pf4_s1_quantile_turnover(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS factor_quantile_spread (
            eval_id VARCHAR NOT NULL,
            factor_id VARCHAR NOT NULL,
            horizon INTEGER NOT NULL,
            n_quantiles INTEGER NOT NULL,
            quantile INTEGER NOT NULL,
            mean_forward_return DOUBLE,
            mean_factor_value DOUBLE,
            n_obs BIGINT,
            long_short_spread DOUBLE,
            long_short_hit_rate DOUBLE,
            decile_monotonicity DOUBLE,
            universe_id VARCHAR NOT NULL,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS factor_turnover (
            eval_id VARCHAR NOT NULL,
            factor_id VARCHAR NOT NULL,
            n_quantiles INTEGER NOT NULL,
            top_decile_turnover DOUBLE,
            bottom_decile_turnover DOUBLE,
            mean_rank_autocorrelation DOUBLE,
            n_rebalances BIGINT,
            universe_id VARCHAR NOT NULL,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (table_name, layer, entity, grain, description, natural_key_json, pit_notes, updated_at) VALUES
        ('factor_quantile_spread','metric','factor_quantile_spread','factor_id,horizon,quantile,universe_id,run_id',
         'Per-factor per-decile mean forward return (monotonicity) plus denormalized top-minus-bottom long-short spread, hit-rate, and decile monotonicity.',
         '["factor_id","horizon","quantile","universe_id","run_id"]',
         'Quantile buckets formed within the as-of cross-section only; forward returns future-dated.', now()),
        ('factor_turnover','metric','factor_turnover','factor_id,universe_id,run_id',
         'Per-factor top/bottom-decile membership churn rebalance-to-rebalance plus factor rank autocorrelation.',
         '["factor_id","universe_id","run_id"]',
         'Turnover compares consecutive as-of dates in forward chronological order; no lookahead.', now())
        """
    )
    for stmt in (
        "CREATE INDEX IF NOT EXISTS idx_factor_quantile_spread_factor ON factor_quantile_spread(factor_id, horizon, quantile)",
        "CREATE INDEX IF NOT EXISTS idx_factor_turnover_factor ON factor_turnover(factor_id)",
    ):
        conn.execute(stmt)
    _catalog_fields_for_tables(conn, ("factor_quantile_spread", "factor_turnover"))
    _refresh_schema_contract_v2_pin(conn)
```
Append `Migration(version=177, name="pf4_s1_quantile_turnover", up=_pf4_s1_quantile_turnover)` to `MIGRATIONS`.

`compute_quantile_spread` semantics: per `(factor_id, as_of_date, horizon)` assign each name a quantile bucket
`1..n_quantiles` via `pd.qcut(value.rank(method="first"), n_quantiles, labels=False) + 1` (rank-first breaks ties
deterministically); require ≥ `n_quantiles` names per cross-section else skip that date. Per-date top-minus-bottom
= mean_return(quantile == n) − mean_return(quantile == 1). Aggregate across dates: per-quantile
`mean_forward_return`/`mean_factor_value`/`n_obs`; `long_short_spread = mean(top_minus_bottom per date)`;
`long_short_hit_rate = mean(top_minus_bottom > 0)`; `decile_monotonicity = Spearman(quantile index, per-quantile
mean_forward_return)` (via ranks). Emit one row per `(factor_id, horizon, quantile)` with the three summary
columns denormalized across a factor/horizon's decile rows. Stable-sort by
`["factor_id","horizon","quantile"]`.

`compute_turnover` semantics: for each `factor_id`, order distinct `as_of_date` ascending; per consecutive pair
compute top-decile membership set churn `1 − |A∩B| / |A∪B|` (Jaccard-complement) and bottom-decile likewise;
`mean_rank_autocorrelation` = mean over consecutive pairs of Spearman corr between the two dates' factor ranks on
the intersecting names. Aggregate to `top_decile_turnover`, `bottom_decile_turnover`,
`mean_rank_autocorrelation`, `n_rebalances`. Stable-sort by `["factor_id"]`.

**TDD steps (real test code — append to `db/tests/test_signal_eval.py`):**
```python
from db.signal_eval import compute_quantile_spread, compute_turnover

def test_monotone_factor_has_monotone_deciles_and_positive_spread() -> None:
    dates = _dates(50); secs = [f"S{i}" for i in range(50)]
    panel_rows, fr_rows = [], []
    for d in dates:
        for i, s in enumerate(secs):
            v = float(i)                                    # perfectly ordered factor
            panel_rows.append({"security_id": s, "as_of_date": d, "factor_id": "mono", "value": v})
            fr_rows.append({"security_id": s, "as_of_date": d, "horizon": 1, "forward_return": v / 100.0})
    spread = compute_quantile_spread(pd.DataFrame(panel_rows), pd.DataFrame(fr_rows), n_quantiles=10, horizons=[1])
    deciles = spread.sort_values("quantile")["mean_forward_return"].to_numpy()
    assert np.all(np.diff(deciles) > -1e-9)                 # monotone non-decreasing decile returns
    assert spread["long_short_spread"].iloc[0] > 0
    assert spread["long_short_hit_rate"].iloc[0] > 0.9
    assert spread["decile_monotonicity"].iloc[0] > 0.99

def test_random_walk_factor_has_flat_deciles_and_high_turnover() -> None:
    rng = np.random.default_rng(5)
    dates = _dates(60); secs = [f"S{i}" for i in range(40)]
    panel_rows, fr_rows = [], []
    for d in dates:
        for s in secs:
            v = rng.normal()                                # re-drawn each date -> unstable ranking
            panel_rows.append({"security_id": s, "as_of_date": d, "factor_id": "rw", "value": v})
            fr_rows.append({"security_id": s, "as_of_date": d, "horizon": 1, "forward_return": rng.normal()})
    p, fr = pd.DataFrame(panel_rows), pd.DataFrame(fr_rows)
    spread = compute_quantile_spread(p, fr, n_quantiles=10, horizons=[1])
    assert abs(spread["long_short_spread"].iloc[0]) < 0.05
    turnover = compute_turnover(p, n_quantiles=10)
    assert turnover["top_decile_turnover"].iloc[0] > 0.5    # membership churns hard for a random walk
    assert abs(turnover["mean_rank_autocorrelation"].iloc[0]) < 0.2

def test_stable_factor_has_low_turnover() -> None:
    dates = _dates(30); secs = [f"S{i}" for i in range(40)]
    panel_rows = [{"security_id": s, "as_of_date": d, "factor_id": "stable", "value": float(i)}
                  for d in dates for i, s in enumerate(secs)]
    turnover = compute_turnover(pd.DataFrame(panel_rows), n_quantiles=10)
    assert turnover["top_decile_turnover"].iloc[0] < 1e-9   # ranking never changes -> zero churn
    assert turnover["mean_rank_autocorrelation"].iloc[0] > 0.99

def test_compute_quantile_spread_is_order_invariant() -> None:
    rng = np.random.default_rng(21)
    dates = _dates(15); secs = [f"S{i}" for i in range(30)]
    rows, fr = [], []
    for d in dates:
        for s in secs:
            v = rng.normal()
            rows.append({"security_id": s, "as_of_date": d, "factor_id": "f", "value": v})
            fr.append({"security_id": s, "as_of_date": d, "horizon": 1, "forward_return": v + rng.normal()})
    p, f = pd.DataFrame(rows), pd.DataFrame(fr)
    a = compute_quantile_spread(p, f, n_quantiles=5, horizons=[1])
    b = compute_quantile_spread(p.sample(frac=1.0, random_state=8).reset_index(drop=True),
                                f.sample(frac=1.0, random_state=9).reset_index(drop=True), n_quantiles=5, horizons=[1])
    pd.testing.assert_frame_equal(a.reset_index(drop=True), b.reset_index(drop=True))
```
1. Append tests + imports → run `python -m pytest db\tests\test_signal_eval.py -q` → **red** (functions missing).
2. Implement `compute_quantile_spread`, `compute_turnover`, persistence, `evaluate_panel` extension + migration
   0177 + registry already wired (same bodies file). Delete stale schema-template cache.
3. Run → **green**.

**PIT:** quantile buckets are formed within the as-of cross-section only; forward returns are future-dated;
turnover compares consecutive as-of dates in forward chronological order with no lookahead.

**Verification (cwd: `atx-impl/`):** `python -m pytest db\tests\test_signal_eval.py -q`;
`python -c "from db.migrations.registry import MIGRATIONS; print(max(m.version for m in MIGRATIONS))"` → `177`.

**Gates:** migration 0177 applies cleanly; monotone/random-walk/stable + order-invariance tests pass;
`factor_quantile_spread`/`factor_turnover` catalogued.

**Accept:** decile spread, per-decile returns, hit-rate, and turnover emit per factor; monotone fixture → monotone
deciles + positive spread; random-walk fixture → ~uniform deciles + high turnover; deterministic.

---

### PF4-S1-2 — Correlation / crowding + breadth — migration 0178

**Root cause:** no factor-to-factor correlation matrix and no crowding score, so redundant / collinear factors
ship as if independent; and no cross-sectional breadth view telling a researcher on how many names a factor is
actually defined each date.

**Files:**
- EDIT `db/signal_eval.py` — add `compute_factor_correlation`, `compute_crowding`, `compute_breadth`,
  `persist_correlation_crowding`, `persist_breadth`; extend `evaluate_panel`.
- EDIT `db/migrations/bodies_0176_0179.py` — add migration **0178** (`_pf4_s1_correlation_breadth`).
- EDIT `db/tests/test_signal_eval.py` — add correlation/crowding/breadth tests.

**Interfaces / DDL (migration 0178 — real SQL):**
```python
def _pf4_s1_correlation_breadth(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS factor_correlation (
            eval_id VARCHAR NOT NULL,
            factor_id_a VARCHAR NOT NULL,
            factor_id_b VARCHAR NOT NULL,
            mean_correlation DOUBLE,
            mean_abs_correlation DOUBLE,
            n_dates BIGINT,
            universe_id VARCHAR NOT NULL,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS factor_crowding (
            eval_id VARCHAR NOT NULL,
            factor_id VARCHAR NOT NULL,
            max_abs_correlation DOUBLE,
            avg_abs_correlation DOUBLE,
            most_correlated_factor_id VARCHAR,
            n_peers BIGINT,
            universe_id VARCHAR NOT NULL,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS factor_breadth (
            eval_id VARCHAR NOT NULL,
            factor_id VARCHAR NOT NULL,
            as_of_date DATE NOT NULL,
            n_names BIGINT,
            n_non_null BIGINT,
            universe_size BIGINT,
            coverage_fraction DOUBLE,
            effective_breadth DOUBLE,
            universe_id VARCHAR NOT NULL,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (table_name, layer, entity, grain, description, natural_key_json, pit_notes, updated_at) VALUES
        ('factor_correlation','metric','factor_correlation','factor_id_a,factor_id_b,universe_id,run_id',
         'Pairwise cross-sectional factor-value correlation averaged over dates (ordered pairs a!=b) for redundancy analysis.',
         '["factor_id_a","factor_id_b","universe_id","run_id"]',
         'Correlations computed cross-sectionally per date then averaged; no date pooling.', now()),
        ('factor_crowding','metric','factor_crowding','factor_id,universe_id,run_id',
         'Per-factor crowding = max and average absolute correlation to the rest of the namespace, with the most-correlated peer.',
         '["factor_id","universe_id","run_id"]',
         'Derived from factor_correlation; a factor highly correlated with many others is crowded/redundant.', now()),
        ('factor_breadth','metric','factor_breadth','factor_id,as_of_date,universe_id,run_id',
         'Per-date cross-sectional breadth: non-null name count, as-of universe size, coverage fraction, effective breadth.',
         '["factor_id","as_of_date","universe_id","run_id"]',
         'Breadth is an as-of coverage measure over the as-of universe.', now())
        """
    )
    for stmt in (
        "CREATE INDEX IF NOT EXISTS idx_factor_correlation_a ON factor_correlation(factor_id_a, factor_id_b)",
        "CREATE INDEX IF NOT EXISTS idx_factor_crowding_factor ON factor_crowding(factor_id)",
        "CREATE INDEX IF NOT EXISTS idx_factor_breadth_factor_date ON factor_breadth(factor_id, as_of_date)",
    ):
        conn.execute(stmt)
    _catalog_fields_for_tables(conn, ("factor_correlation", "factor_crowding", "factor_breadth"))
    _refresh_schema_contract_v2_pin(conn)
```
Append `Migration(version=178, name="pf4_s1_correlation_breadth", up=_pf4_s1_correlation_breadth)`.

`compute_factor_correlation`: pivot to wide `(security_id, as_of_date) × factor_id`; per `as_of_date` compute the
factor×factor Pearson correlation over the names present; average each ordered pair `(a, b)`, `a != b`, over the
dates where both are defined on ≥ 3 common names; emit `mean_correlation`, `mean_abs_correlation`, `n_dates`.
Stable-sort by `["factor_id_a", "factor_id_b"]`.
`compute_crowding`: from the correlation frame, per `factor_id_a` → `max_abs_correlation`, `avg_abs_correlation`,
`most_correlated_factor_id = argmax |mean_correlation|`, `n_peers`. Stable-sort by `["factor_id"]`.
`compute_breadth`: per `(factor_id, as_of_date)` → `n_non_null`, `n_names`; join optional `universe_counts`
(`as_of_date → universe_size`) → `coverage_fraction = n_non_null / universe_size`; `effective_breadth` = names
count (placeholder for weight-based ENB — defined as `n_non_null` here, documented in `field_catalog`). Stable-sort
by `["factor_id", "as_of_date"]`.

**TDD steps (real test code — append):**
```python
from db.signal_eval import compute_factor_correlation, compute_crowding, compute_breadth

def test_near_duplicate_factors_are_mutually_crowded() -> None:
    rng = np.random.default_rng(2)
    dates = _dates(30); secs = [f"S{i}" for i in range(40)]
    rows = []
    for d in dates:
        base = {s: rng.normal() for s in secs}
        for s in secs:
            rows.append({"security_id": s, "as_of_date": d, "factor_id": "value_a", "value": base[s]})
            rows.append({"security_id": s, "as_of_date": d, "factor_id": "value_b", "value": base[s] + 0.01 * rng.normal()})
            rows.append({"security_id": s, "as_of_date": d, "factor_id": "indep",   "value": rng.normal()})
    panel = pd.DataFrame(rows)
    corr = compute_factor_correlation(panel)
    ab = corr[(corr["factor_id_a"] == "value_a") & (corr["factor_id_b"] == "value_b")]["mean_abs_correlation"].iloc[0]
    assert ab > 0.9
    crowd = compute_crowding(corr).set_index("factor_id")
    assert crowd.loc["value_a", "max_abs_correlation"] > 0.9
    assert crowd.loc["value_b", "max_abs_correlation"] > 0.9
    assert crowd.loc["indep", "max_abs_correlation"] < 0.5
    assert crowd.loc["value_a", "most_correlated_factor_id"] == "value_b"

def test_breadth_matches_known_fixture_coverage() -> None:
    dates = _dates(3); secs = [f"S{i}" for i in range(10)]
    rows = []
    for d in dates:
        for i, s in enumerate(secs):
            val = float(i) if i < 6 else None                # only 6 of 10 names defined
            rows.append({"security_id": s, "as_of_date": d, "factor_id": "sparse", "value": val})
    uni = pd.DataFrame({"as_of_date": dates, "universe_size": [10, 10, 10]})
    breadth = compute_breadth(pd.DataFrame(rows), uni)
    assert set(breadth["n_non_null"]) == {6}
    assert np.allclose(breadth["coverage_fraction"], 0.6)

def test_compute_correlation_is_order_invariant() -> None:
    rng = np.random.default_rng(31)
    dates = _dates(12); secs = [f"S{i}" for i in range(20)]
    rows = []
    for d in dates:
        for s in secs:
            rows.append({"security_id": s, "as_of_date": d, "factor_id": "a", "value": rng.normal()})
            rows.append({"security_id": s, "as_of_date": d, "factor_id": "b", "value": rng.normal()})
    p = pd.DataFrame(rows)
    a = compute_factor_correlation(p)
    b = compute_factor_correlation(p.sample(frac=1.0, random_state=6).reset_index(drop=True))
    pd.testing.assert_frame_equal(a.reset_index(drop=True), b.reset_index(drop=True))
```
1. Append tests → run → **red**. 2. Implement + migration 0178. 3. Run → **green**.

**PIT:** correlations computed cross-sectionally per date then aggregated (no date-pooling leakage); breadth is an
as-of coverage measure over the as-of universe; pure + deterministic.

**Verification (cwd: `atx-impl/`):** `python -m pytest db\tests\test_signal_eval.py -q`; max migration version →
`178`.

**Gates:** migration 0178 applies cleanly; crowding + breadth + order-invariance tests pass; three tables
catalogued.

**Accept:** the correlation matrix + crowding scores emit; two near-duplicate factors flagged mutually crowded;
per-date breadth matches the known fixture coverage.

---

### PF4-S1-3 — Factor DQC gated (leakage + coverage) — migration 0179

**Root cause:** nothing asserts factor-level data quality, so a leaky factor (its lag dropped upstream, so it
correlates with the same-day return) or an effectively-empty factor could ship into the panel silently.

**Files:**
- EDIT `db/signal_eval.py` — add `compute_leakage`, `compute_coverage`, `factor_leakage_report`,
  `factor_coverage_report`, `signal_eval_dqc_results`, `persist_factor_dqc`.
- EDIT `db/quality/_runner.py` — append a minimal hook after the existing `PANEL_EXPORT_GATE_CHECK_NAME` block
  (see below) that folds the two factor-DQC results into `schema_results`. (Append-style, mirrors the pf3-S10
  panel-export precedent; does not edit any prior check.)
- EDIT `db/migrations/bodies_0176_0179.py` — add migration **0179** (`_pf4_s1_factor_dqc`) creating
  `factor_dqc_result`, its catalog rows + indexes, and seeding `quality_check_registry` with the two checks.
- EDIT `db/tests/test_signal_eval.py` — add leakage/coverage + gate-wiring tests.

**Interfaces / DDL (migration 0179 — real SQL):**
```python
def _pf4_s1_factor_dqc(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS factor_dqc_result (
            check_name VARCHAR NOT NULL,
            factor_id VARCHAR NOT NULL,
            status VARCHAR NOT NULL,
            observed_value DOUBLE,
            threshold_value DOUBLE,
            severity VARCHAR NOT NULL,
            details_json VARCHAR,
            run_id VARCHAR,
            checked_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (table_name, layer, entity, grain, description, natural_key_json, pit_notes, updated_at) VALUES
        ('factor_dqc_result','control','factor_dqc_result','check_name,factor_id,run_id',
         'Per-factor data-quality-check outcomes (leakage t+0 probe, coverage) recorded for the gated factor DQC.',
         '["check_name","factor_id","run_id"]',
         'Leakage uses the same-day t+0 return purely as an adversarial probe, never as a scoring target or factor input.', now())
        """
    )
    conn.execute("CREATE INDEX IF NOT EXISTS idx_factor_dqc_result_check ON factor_dqc_result(check_name, factor_id)")
    conn.executemany(
        """
        INSERT OR REPLACE INTO quality_check_registry (
            check_name, dataset_id, table_name, severity, threshold_value,
            comparator, enabled, failure_status, source, updated_at
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, now())
        """,
        [
            ("factor_leakage_tplus0", "factor_panel", "v_factor_panel", "critical", 0.0, "eq", True, "failed", "pf4_s1"),
            ("factor_coverage_asof_universe", "factor_panel", "v_factor_panel", "critical", 0.0, "eq", True, "failed", "pf4_s1"),
        ],
    )
    _catalog_fields_for_tables(conn, ("factor_dqc_result",))
    _refresh_schema_contract_v2_pin(conn)
```
Append `Migration(version=179, name="pf4_s1_factor_dqc", up=_pf4_s1_factor_dqc)`.

`db/quality/_runner.py` hook (insert directly after the `for result in schema_results:` block's preceding
`PANEL_EXPORT_GATE_CHECK_NAME` append, before the final `for result in schema_results:` record loop):
```python
    from ..signal_eval import signal_eval_dqc_results
    schema_results.extend(
        signal_eval_dqc_results(
            store,
            registry=registry,
            requested_checks=requested_checks,
            requested_datasets=requested_datasets,
            checked_at=checked_at,
        )
    )
```

`compute_leakage`: merge `panel` with `same_day_returns` (`security_id, as_of_date, same_day_return` — the return
realized *at* `as_of_date`, i.e. contemporaneous, NOT future) on `(security_id, as_of_date)`; per `factor_id`
compute the pooled |Pearson corr| between `value` and `same_day_return`; `is_leaky = abs_corr > threshold`. Emit
`factor_id, abs_corr, threshold, is_leaky`. A correctly-lagged factor has ≈ 0 contemporaneous correlation.
`compute_coverage`: per `factor_id` mean over dates of `n_non_null / universe_size`; `is_undercovered =
coverage_fraction < min_fraction`.
`factor_leakage_report(store, ...)`: if `panel`/`same_day_returns` injected use them; else derive
`same_day_returns` from `equity_daily_bars` (`close / lag(close) OVER (PARTITION BY security_id ORDER BY
trade_date) - 1` aliased to `as_of_date = trade_date`) and `panel` from `load_panel_for_eval`. Returns
`{"violation_count": float(n_leaky), "rows": [...]}` matching the `factor_panel_export_gate_report` shape.
`signal_eval_dqc_results`: build the two reports, gate each with `_registry_allows_check(name, registry)` +
the same `requested_checks`/`requested_datasets` filter logic used for `PANEL_EXPORT_GATE_CHECK_NAME`, and return
`QualityResult(dataset_id="factor_panel", table_name="v_factor_panel", check_name=..., status="passed" if
violation_count == 0 else "failed", observed_value=violation_count, threshold_value=0.0, details={...},
severity="critical")` — 0/1/2 results. If `v_factor_panel`/`equity_daily_bars` are absent or empty, return the
check as `status="skipped"` (mirrors `warn_if_missing`) so an empty live DB does not spuriously halt.

**TDD steps (real test code — append):**
```python
from db.quality import QualityResult, evaluate_quality_gate, run_warehouse_quality_checks
from db.signal_eval import (
    LEAKAGE_DQC_CHECK_NAME, COVERAGE_DQC_CHECK_NAME,
    compute_leakage, compute_coverage,
)

def test_planted_leaky_factor_is_red_and_lagged_is_green() -> None:
    rng = np.random.default_rng(4)
    dates = _dates(40); secs = [f"S{i}" for i in range(30)]
    rows, sdr = [], []
    for d in dates:
        for s in secs:
            same_day = rng.normal()
            sdr.append({"security_id": s, "as_of_date": d, "same_day_return": same_day})
            rows.append({"security_id": s, "as_of_date": d, "factor_id": "leaky", "value": same_day})       # dropped lag
            rows.append({"security_id": s, "as_of_date": d, "factor_id": "lagged", "value": rng.normal()})  # independent
    res = compute_leakage(pd.DataFrame(rows), pd.DataFrame(sdr), threshold=0.10).set_index("factor_id")
    assert bool(res.loc["leaky", "is_leaky"]) is True
    assert bool(res.loc["lagged", "is_leaky"]) is False

def test_sparse_factor_fails_coverage_and_dense_passes() -> None:
    dates = _dates(10); secs = [f"S{i}" for i in range(20)]
    rows = []
    for d in dates:
        for i, s in enumerate(secs):
            rows.append({"security_id": s, "as_of_date": d, "factor_id": "dense", "value": float(i)})
            rows.append({"security_id": s, "as_of_date": d, "factor_id": "sparse",
                         "value": float(i) if i < 3 else None})    # 3/20 = 0.15 coverage
    uni = pd.DataFrame({"as_of_date": dates, "universe_size": [20] * len(dates)})
    cov = compute_coverage(pd.DataFrame(rows), uni, min_fraction=0.50).set_index("factor_id")
    assert bool(cov.loc["sparse", "is_undercovered"]) is True
    assert bool(cov.loc["dense", "is_undercovered"]) is False

def test_leakage_check_is_registered_and_critical(tmp_store) -> None:
    reg = tmp_store.con.execute(
        "SELECT severity, enabled FROM quality_check_registry WHERE check_name = ?",
        [LEAKAGE_DQC_CHECK_NAME],
    ).fetchone()
    assert reg is not None and reg[0] == "critical" and bool(reg[1]) is True

def test_red_factor_dqc_routes_to_halt(tmp_store) -> None:
    red = QualityResult(
        dataset_id="factor_panel", table_name="v_factor_panel",
        check_name=LEAKAGE_DQC_CHECK_NAME, status="failed",
        observed_value=1.0, threshold_value=0.0, details={"rows": []}, severity="critical",
    )
    gate = evaluate_quality_gate(tmp_store, "factor_panel", additional_results=[red])
    assert gate.decision == "halt"

def test_factor_dqc_included_in_sweep_when_panel_empty(tmp_store) -> None:
    # v_factor_panel is empty on a fresh template -> checks run and skip/ pass, never crash.
    results = run_warehouse_quality_checks(tmp_store, check_names=[LEAKAGE_DQC_CHECK_NAME, COVERAGE_DQC_CHECK_NAME])
    names = {r.check_name for r in results}
    assert LEAKAGE_DQC_CHECK_NAME in names and COVERAGE_DQC_CHECK_NAME in names
    for r in results:
        if r.check_name in {LEAKAGE_DQC_CHECK_NAME, COVERAGE_DQC_CHECK_NAME}:
            assert r.severity == "critical" and r.status in {"passed", "skipped"}
```
1. Append tests → run → **red** (functions + registry rows missing). 2. Implement pure functions, reports,
   `signal_eval_dqc_results`, the `_runner.py` hook, and migration 0179. Delete stale template cache. 3. Run →
   **green**.

**PIT:** the leakage check uses the same-day (t+0) return purely as an adversarial probe, never as a scoring
target, and never writes it back into any factor value; coverage is measured against as-of universe membership,
not a pooled roster.

**Verification (cwd: `atx-impl/`):**
- `python -m pytest db\tests\test_signal_eval.py -q`
- `python -m pytest db\tests\test_quality_smoke.py -q` (proves the runner hook did not regress the existing sweep)
- max migration version → `179`.

**Gates:** both checks registered `severity=critical` + enabled; a planted-red result routes to gate decision
`halt`; the empty-panel sweep runs the checks without crashing; existing `quality` checks unaffected;
`factor_dqc_result` catalogued.

**Accept:** leakage DQC RED on a planted leaky fixture + GREEN on a properly-lagged factor; coverage DQC RED on a
sparse fixture + GREEN on a well-covered factor; both authored gate-ready so PF4-S2 can halt on them.

---

### PF4-S1-4 — Closeout: full-suite green, live smoke, ledger + gap update

**Root cause / purpose:** a new module + four migrations + a shared-runner hook must leave the whole offline
suite green, be recorded honestly in the parity ledger (including the data-empty-live caveat), and update the gap
matrix — the append-only coordination surfaces the ROADMAP mandates per sprint.

**Files:**
- EDIT `db/PARITY_GAP.md` — mark factor-level signal evaluation as present (Domain: analytics / factor panel).
- EDIT `WAREHOUSE_PARITY_TRANCHES.md` (repo root `atx-impl/WAREHOUSE_PARITY_TRANCHES.md`) — append one tranche
  row.
- No code changes beyond docs; this task only runs verification + writes the ledgers + commits.

**Steps:**
1. **Full offline suite from `atx-impl/`** (never from `db/` — `db/calendar.py` shadows stdlib `calendar`):
   `python -m pytest db\tests -q` → must be green. If a pre-existing time-bomb / snapshot failure surfaces that is
   NOT caused by this sprint (the ROADMAP flags ~8 such failures owned by PF4-S3), record it verbatim in the
   ledger row as a pre-existing, out-of-scope failure and do NOT fix it here (PF4-S3 owns the re-green); this
   sprint's own `test_signal_eval.py` and the runner-hook smoke MUST be green.
2. **Catalog sweep (clause E):** assert the six new tables + manifest + DQC table are catalogued and the schema
   contract is re-pinned:
   ```
   python -c "from db.connection import connect, DEFAULT_DB_PATH; c=connect(DEFAULT_DB_PATH, read_only=True); import itertools; \
   rows=c.con.execute(\"SELECT table_name FROM table_catalog WHERE table_name IN ('factor_eval_manifest','factor_ic','factor_ic_decay','factor_quantile_spread','factor_turnover','factor_correlation','factor_crowding','factor_breadth','factor_dqc_result')\").fetchall(); print(sorted(r[0] for r in rows))"
   ```
   (Or assert it inside a `tmp_store` test — preferred, offline.) Add a
   `test_signal_eval_tables_are_catalogued(tmp_store)` that checks all nine table names appear in `table_catalog`
   and each has ≥ 1 `field_catalog` row.
3. **Live-DB smoke (operator-run, recorded — not executed autonomously):** on explicit operator go, on a bounded
   slice, run `evaluate_panel(store, run_id=<rid>)` and `run_warehouse_quality_checks(store,
   dataset_ids=["factor_panel"])`; record per-factor IC / decay / quantile / turnover / crowding / breadth row
   counts, the factor-DQC pass/fail tallies, and the `run_id`. **Expected on the current live DB: ~0 rows**
   because the price×fundamental overlap is empty (equity_daily_bars 2012–2014, fundamentals 2017–2026) — this is
   the honest reconciliation; meaningful counts arrive with PF4-S4 (survivorship returns) + PF4-S6 (dense
   backfill). Record the zero/near-zero counts + the caveat rather than asserting non-zero.
4. **Append `WAREHOUSE_PARITY_TRANCHES.md` row** (append-only; one row): columns matching the existing ledger —
   tranche name (`PF4-S1 signal-evaluation surface — per-factor IC/decay/quantile/turnover/crowding/breadth +
   gated leakage/coverage DQC`), status (`committed`), start SHA, end SHA, files touched (`db/signal_eval.py`
   (new), `db/migrations/bodies_0176_0179.py` (new), `db/migrations/registry.py`, `db/quality/_runner.py` (DQC
   hook), `db/tests/test_signal_eval.py` (new), `db/PARITY_GAP.md`), verification commands
   (`python -m pytest db\tests\test_signal_eval.py -q`; full `python -m pytest db\tests -q`), live smoke (exact
   counts + `run_id`, or the data-empty caveat), and caveats/next → **PF4-S2** (promote the leakage/coverage
   checks to orchestrator halt gates + add factor freshness/anomaly/lineage observability).
5. **Update `db/PARITY_GAP.md`**: in the analytics/factor row, note that per-factor signal evaluation (IC / decay
   / quantile spread / turnover / crowding / breadth) + gated factor DQC are now Built (fixture-proven;
   live-dense pending PF4-S6).

**Verification (cwd: `atx-impl/`):**
- `python -m pytest db\tests -q` (full offline suite green, or documented pre-existing PF4-S3-owned failures)
- `python -m pytest db\tests\test_signal_eval.py -q` (this sprint's suite green)
- `git status --porcelain` shows only the intended paths staged.

**Gates:** full suite green (modulo documented pre-existing failures); nine tables catalogued with field rows;
ledger row appended; gap matrix updated; commit staged with **explicit paths only** (never `git add -A`) and
trailer EXACTLY `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.

**Accept:** the sprint's suite + the full offline suite are green from `atx-impl/`; the ledger + gap files carry
the honest tranche record (including the data-empty-live caveat); the branch is ready to hand PF4-S2 a
gate-ready, fully-scored panel.

---

## Sequencing & expected compounding

**PF4-S1-0 → S1-1 → S1-2 → S1-3 → S1-4.** IC first: it is the load-bearing predictiveness measure every later
view references and the cheapest honest verdict on a factor (and it lands the module scaffold + eval-manifest +
forward-return builder every subsequent task reuses). Then quantile spread + turnover, the return-and-stability
view that turns rank-IC into economically legible long-short performance. Then correlation / crowding + breadth,
the redundancy-and-coverage view *over the now-scored set* — crowding is only meaningful once you know which
factors are worth keeping. DQC third-to-last: it gates the fully-scored surface. Closeout last. **Compounding:**
once every factor is scored (IC / decay / turnover / crowding / breadth) and factor DQC is gate-ready, **PF4-S2**
can wire the leakage/coverage checks as orchestrator halt gates and observe signal quality (freshness / anomaly /
lineage), and a researcher can select the factors that actually carry signal instead of guessing — which is the
entire point of the pf4 product.

---

## Risks / guardrails

- **The evaluation must not itself leak.** Forward returns are strictly future-dated and used only as the scoring
  target; a dropped lag in the scorer would silently manufacture predictive power. IC and every correlation are
  computed cross-sectionally per date and only then aggregated — never pooled across dates — so one date's future
  cannot leak into another's score. The leakage DQC's same-day return is a *contemporaneous* probe, never mixed
  into any factor input.
- **The leakage DQC is adversarial by design.** The planted-leaky fixture (factor value == same-day return) MUST
  flag RED; if the leakage check passes it, the check is broken. The planted-leaky and planted-sparse fixtures
  are the acceptance backbone.
- **Read the panel, do not rewrite it.** PF4-S1 never touches `v_factor_panel` / the lake export or its
  schema-hash, nor the pf3 S7–S9 factor engine/families; forward returns never re-enter a factor value.
- **Stay in lane.** Strictly migrations 0176–0179 in one new `bodies_0176_0179.py`; each migration seeds its own
  catalog rows + indexes and re-pins the schema contract; reuse the alpha-backtest manifest discipline
  (`_hash_id` + `params_json` + `source` + `run_id`) rather than inventing a parallel one; the `_runner.py` hook
  is append-style and mirrors the pf3-S10 panel-export block; never edit a landed migration (≤ 0175) or PF4-S2's
  0180–0183.
- **Determinism is a property test, not a hope.** Every `compute_*` has a row-order-shuffle test asserting
  `assert_frame_equal` after a stable sort. No Python `set` iteration or dict-order dependence in any id hash or
  output ordering.
- **Data-empty live is expected, not a failure.** The live smoke will report ~0 rows until PF4-S4/S6 densify the
  price overlap; record the caveat honestly rather than chasing non-zero counts.

---

## Bench / acceptance

- Every factor in the exported panel is scored on the proof slice: rank-IC + IC decay across the horizon ladder
  (S1-0), decile long-short spread + hit-rate + turnover (S1-1), per-date breadth (S1-2).
- The correlation / crowding surface emits a correlation matrix over the namespace and a per-factor crowding
  score, with two near-duplicate fixture factors flagged as mutually crowded (S1-2).
- The leakage DQC is RED on a planted leaky factor and GREEN on a properly-lagged factor; the coverage DQC is RED
  on a sparse factor and GREEN on a well-covered factor; both authored `severity=critical` and gate-ready, and a
  planted-red result routes `evaluate_quality_gate` to `halt` (S1-3).
- Every `compute_*` transform is deterministic under input row-order shuffles (property tests in S1-0/S1-1/S1-2).
- `python -m pytest db\tests\test_signal_eval.py -q` green, and full `python -m pytest db\tests -q` green from
  `atx-impl/` before commit (S1-4).
- **Live-DB smoke** recorded in the ledger: per-factor IC / decay / quantile / turnover / breadth counts on the
  slice, the crowding surface row counts, the factor-DQC pass/fail tallies, and the `run_id` — or, on the current
  data-empty live DB, the ~0-row counts + the reconciliation caveat (S1-4).
- `db/PARITY_GAP.md` status updated (factor-level signal evaluation now present); a
  `WAREHOUSE_PARITY_TRANCHES.md` row appended (start/end SHA, domains, verification commands, live smoke with
  exact counts + run_id or caveat, caveats/next → **PF4-S2** panel gating + observability).

**Process:** own git worktree off `main` via `atx-impl/scripts/new_db_worktree.sh new|finish <slug>`; controller
`superpowers:subagent-driven-development` (fresh implementer + reviewer per task; TDD +
verification-before-completion). Never `git add -A` (stage explicit paths); never push unless asked. New module ⇒
new `test_*.py`. `python -m pytest db\tests -q` green in the worktree from `atx-impl/` before every commit. Commit
trailer EXACTLY `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
