"""R4 qualification ledger, end to end: a synthetic month-end market evaluated by the real
R3b engine under the committed frozen split, persisted like a sealed store run, then
qualified under the frozen policy."""

from __future__ import annotations

import dataclasses
import datetime as dt
import json
import math
from pathlib import Path
from types import SimpleNamespace

import duckdb
import numpy as np
import pandas as pd
import pytest

from atx_db.research import evaluation as ev
from atx_db.research import qualification as rq

POLICY = rq.load_policy()
START = dt.date(2012, 1, 1)
MONTHS = 170  # formations 2012-01-31 .. 2026-02-28; the frozen holdout starts 2024-01
NAMES = 300
HOLDOUT_INDEX = 144  # 2024-01-31
STRONG, MODERATE = 0.22, 0.06
N_NOISE = 20
REPO = Path(__file__).resolve().parents[1]


def _month(offset: int) -> dt.date:
    total = START.month - 1 + offset
    return dt.date(START.year + total // 12, total % 12 + 1, 1)


class Market:
    """Month-end formations; ``returns[t]`` is realized over the month after formation t.

    Caps rise with the security index and every third name is on the NYSE (point in
    time), so the NYSE 20/50 breakpoints split micro / small / large at about 20% and
    50% of the names.
    """

    def __init__(self, returns: np.ndarray, months: int) -> None:
        self.returns, self.months, self.names = returns, months, returns.shape[1]
        rows = []
        for m in range(months):
            formation = _month(m + 1) - dt.timedelta(days=1)
            rows.append({"month_index": m, "month_start": _month(m), "formation_date": formation,
                         "cutoff": dt.datetime.combine(formation, dt.time(22)),
                         "entry_date": formation + dt.timedelta(days=1), "status": "formed",
                         "eligible_members": self.names})
        self.calendar = pd.DataFrame(rows)
        self.caps = np.exp(np.linspace(0.0, 6.0, self.names))
        self.nyse = np.arange(self.names) % 3 == 0

    def labels(self) -> pd.DataFrame:
        growth = np.vstack([np.zeros(self.names), np.cumsum(np.log1p(self.returns), axis=0)])
        entries = np.array(self.calendar.entry_date.tolist(), dtype="datetime64[D]")
        months = np.arange(self.months)
        return pd.concat([pd.DataFrame({
            "month_index": np.repeat(months, self.names), "security": np.tile(np.arange(self.names), self.months),
            "horizon_months": h, "forward_return": np.expm1(growth[months + h] - growth[months]).ravel(),
            "status": 0, "terminal": 0, "anchor_date": np.repeat(entries, self.names)})
            for h in ev.DEFAULT_HORIZONS], ignore_index=True)

    def maturity(self) -> pd.DataFrame:
        return pd.DataFrame([{"month_index": m, "horizon_months": h, "matured": True,
                              "expected_end": _month(m + h + 1) - dt.timedelta(days=1)}
                             for h in ev.DEFAULT_HORIZONS for m in range(self.months)])

    def context(self) -> pd.DataFrame:
        nyse = np.tile(self.nyse, self.months)
        return pd.DataFrame({"month_index": np.repeat(np.arange(self.months), self.names),
                             "security": np.tile(np.arange(self.names), self.months),
                             "market_cap": np.tile(self.caps, self.months), "is_nyse": nyse,
                             "venue_pit": True, "is_nyse_pit": nyse})

    def feature(self, feature_id: str, values: np.ndarray, sign: int, variants: tuple[str, ...],
                names: np.ndarray | None = None) -> ev.FeatureData:
        chosen = np.arange(self.names) if names is None else names
        frame = pd.concat([pd.DataFrame({"month_index": np.repeat(np.arange(self.months), len(chosen)),
                                         "security": np.tile(chosen, self.months), "variant": v,
                                         "value": values[: self.months][:, chosen].ravel()})
                           for v in variants], ignore_index=True)
        return ev.FeatureData(feature_id, sign, frame)

    def basis(self, features: dict[str, ev.FeatureData], variants: dict[str, tuple[str, ...]]) -> ev.BasisInputs:
        return ev.BasisInputs(
            basis="reconstructed", status=ev.BASIS_AVAILABLE, security_count=self.names, calendar=self.calendar,
            labels=self.labels(), maturity=self.maturity(), context=self.context(),
            controls=pd.DataFrame(columns=["month_index", "security", "control", "value"]),
            variants_by_feature=variants, load_feature=features.__getitem__,
            meta={"identity_basis": "current_ticker_unverified", "universe_basis": "us_listed_reconstructed_v1",
                  "classification_basis": "current_sic_backcast", "availability_basis": "conservative_filing_46h"},
            release_frames=True)


def _ar1(rng: np.random.Generator, periods: int, names: int, rho: float = 0.9) -> np.ndarray:
    values = np.empty((periods, names))
    values[0] = rng.standard_normal(names)
    for t in range(1, periods):
        values[t] = rho * values[t - 1] + math.sqrt(1.0 - rho * rho) * rng.standard_normal(names)
    return values


# A family like the real one: a few priced characteristics among many unpriced ones (the
# deflated-Sharpe benchmark and BH both depend on the whole family's composition).
MAIN_FEATURES = ("planted", "two_sided", "flipped", "size_split", "fades", "sparse",
                 *(f"noise_{i:02d}" for i in range(N_NOISE)))
SIGNS = {"two_sided": 0}


def _evaluated() -> tuple[ev.EvaluationTables, ev.EvaluationSpec, tuple[ev.CatalogFeature, ...]]:
    rng = np.random.default_rng(20260925)
    periods = MONTHS + 12
    values = {name: _ar1(rng, periods, NAMES) for name in MAIN_FEATURES}
    index = np.arange(NAMES)
    small = (index >= 0.2 * NAMES) & (index < 0.5 * NAMES)  # NYSE-PIT small bucket (20th..50th NYSE pct)
    holdout = (np.arange(periods) >= HOLDOUT_INDEX)[:, None]
    ones = np.ones((periods, NAMES))
    loading = {
        "planted": STRONG * ones,
        "two_sided": -STRONG * ones,  # two-sided hypothesis, realized negative
        "flipped": -MODERATE * ones,  # hypothesis +1, truth negative
        "size_split": np.where(small, -2.0, 2.0) * MODERATE * ones,  # reversed among small caps
        "fades": np.where(holdout, -1.5, 1.5) * MODERATE * ones,  # reverses in the holdout
        "sparse": MODERATE * ones,  # priced, but valued for half the universe only
    }
    shocks = sum(loading[name] * values[name] for name in loading)
    shocks = shocks + np.sqrt(1.0 - sum(loading[name] ** 2 for name in loading)) * rng.standard_normal(ones.shape)
    market = Market(np.expm1(0.08 * shocks + rng.normal(0.005, 0.04, (periods, 1))), MONTHS)
    variants = {name: ("rank_normal",) for name in MAIN_FEATURES}
    variants["sparse"] = ("rank_normal", "industry_neutral")  # a neutral variant (reported, never gating)
    features = {name: market.feature(name, values[name], SIGNS.get(name, 1), variants[name],
                                     index[index % 2 == 0] if name == "sparse" else None) for name in MAIN_FEATURES}
    catalog = tuple(ev.CatalogFeature(name, SIGNS.get(name, 1), "value", variants[name])
                    for name in sorted(MAIN_FEATURES))
    spec = ev.validate_spec(ev.EvaluationSpec(
        run_id="r4_main", split=POLICY.split, min_names=100, min_bucket_names=30, fm_min_obs=100, min_formations=24,
        nyse_min_names=20, bootstrap_resamples=0, policy_sha256=sorted(POLICY.file_sha256s)[0],
        label_diagnostics=False))
    tables = ev.evaluate_bases([market.basis(features, variants), ev.empty_basis("strict")], spec, catalog=catalog)
    return tables, spec, catalog


def _persist(con: duckdb.DuckDBPyConnection, tables: ev.EvaluationTables, spec: ev.EvaluationSpec) -> None:
    """Write the result tables and a sealed run row, as ``run_evaluation`` would."""
    ev.ensure_evaluation_schema(con)
    for key, (table, columns, _) in ev.RESULT_TABLES.items():
        frame = getattr(tables, key).reset_index(drop=True)
        if not len(frame):
            continue
        typed = {}
        for name, kind in columns:
            series = frame[name] if name in frame else pd.Series([None] * len(frame), dtype=object)
            if kind == "DOUBLE":
                typed[name] = pd.to_numeric(series, errors="coerce").astype("float64")
            elif kind in ("INTEGER", "BIGINT"):
                typed[name] = pd.to_numeric(series, errors="coerce").astype("Int64")
            elif kind == "BOOLEAN":
                typed[name] = series.astype("boolean")
            elif kind == "DATE":
                typed[name] = pd.to_datetime(series)
            else:
                typed[name] = pd.Series([None if pd.isna(v) else str(v) for v in series], dtype=object)
        con.register("_rows", pd.DataFrame(typed))
        names = ", ".join(name for name, _ in columns)
        select = ", ".join(f"CAST({name} AS {kind})" for name, kind in columns)
        con.execute(f"INSERT INTO {table} (run_id, {names}) SELECT ?, {select} FROM _rows", [spec.run_id])
        con.unregister("_rows")
    assert ev.results_digest(con, spec.run_id)[0] == tables.results_sha256  # the store copy is the sealed run
    now = dt.datetime(2026, 9, 25, 12, 0)
    con.execute("""
        INSERT INTO research_eval_runs (run_id, status, evaluation_version, spec_json, spec_sha256, code_sha256,
                                        results_sha256, family_json, blockers_json, created_at, finished_at,
                                        family_complete)
        VALUES (?, 'complete', ?, ?, 'spec', 'code', ?, ?, ?, ?, ?, ?)
    """, [spec.run_id, ev.EVALUATION_VERSION, json.dumps(ev.spec_payload(spec), sort_keys=True),
          tables.results_sha256, json.dumps(tables.family, sort_keys=True),
          json.dumps(["research_only_not_release_eligible", "strict_basis_untestable"]), now, now,
          bool(tables.family["family_complete"])])


def test_ledger_end_to_end_statuses_refusals_and_reproducible_bytes(tmp_path: Path):
    tables, spec, catalog = _evaluated()
    annotations = [SimpleNamespace(feature_id=item.feature_id, expected_sign=item.expected_sign,
                                   anomaly_class=item.anomaly_class, hypothesis_family=f"{item.feature_id}_family",
                                   prior_evidence="published_anomaly", admission="eligible", caveat_codes=(),
                                   is_research_eligible=True) for item in catalog]
    con = duckdb.connect(str(tmp_path / "research.duckdb"), config={"memory_limit": "256MB", "threads": 1})
    try:
        _persist(con, tables, spec)
        ledger = rq.qualify_run(con, "r4_main", POLICY, catalog=annotations)
        rows = {(row["feature_id"], row["basis"]): row for row in ledger.bases}
        status = {row["feature_id"]: row["status"] for row in ledger.features}
        assert sorted(status) == sorted(MAIN_FEATURES)  # exactly one status per tested feature
        assert set(status.values()) <= set(rq.STATUSES)

        # Planted: qualified on reconstructed evidence; the strict cohort is empty, so nothing is strict.
        assert status["planted"] == rq.QUALIFIED_RECONSTRUCTED
        planted = rows[("planted", "reconstructed")]
        assert planted["hlz"] and planted["ic_n"] >= POLICY.min_observations and planted["coverage_share"] == 1.0
        assert planted["holdout_ic_z"] >= POLICY.holdout_min_signed_z and planted["subperiod_same_sign"] == 3
        assert planted["small_ic_mean"] > 0 and planted["large_ic_mean"] > 0
        cell = tables.cells.set_index(["basis", "feature_id", "variant", "horizon_months"]).loc[
            ("reconstructed", "planted", "rank_normal", POLICY.primary_horizon)]
        assert planted["dsr_n_trials"] == tables.family["n_trials"] and planted["sharpe"] > planted["dsr_benchmark"]
        assert math.isclose(planted["dsr"], cell.dsr, rel_tol=0, abs_tol=1e-12)  # R3b's DSR, whole family
        assert all(row["status"] == rq.UNTESTABLE_STRICT for (_, basis), row in rows.items() if basis == "strict")
        assert rq.QUALIFIED_STRICT not in status.values()
        # Two-sided hypothesis: qualifies on |z|; its realized negative sign is fixed and reported.
        two = next(row for row in ledger.features if row["feature_id"] == "two_sided")
        assert two["status"] == rq.QUALIFIED_RECONSTRUCTED and two["raw_direction"] == -1
        assert two["holdout_ic_z"] < -POLICY.holdout_min_signed_z
        # Sign-flipped: significant against the pre-registered sign.
        assert status["flipped"] == rq.SIGN_REVERSED and rows[("flipped", "reconstructed")]["ic_z"] < -3.0
        # Noise never qualifies. BH at q=0.05 controls the false-discovery rate: here 19/20 noise
        # features are not_significant and one (q=0.047, |z|<3) is a BH-branch false discovery,
        # graded sign_reversed because its sign is against the hypothesis.
        noise = [rows[(f"noise_{i:02d}", "reconstructed")] for i in range(N_NOISE)]
        assert sum(row["status"] == rq.NOT_SIGNIFICANT for row in noise) >= N_NOISE - 1
        assert all(row["status"] in (rq.NOT_SIGNIFICANT, rq.SIGN_REVERSED) and not row["hlz"] for row in noise)
        # Microcap guard: the pooled IC is significant, but small caps carry the opposite sign.
        size_split = rows[("size_split", "reconstructed")]
        assert size_split["ic_z"] > 3.0 and size_split["small_ic_mean"] < 0 < size_split["large_ic_mean"]
        assert size_split["status"] == rq.UNSTABLE and "size_small_sign" in size_split["status_reasons"]
        assert status["fades"] == rq.UNSTABLE and "holdout_sign_flip" in rows[("fades", "reconstructed")][
            "status_reasons"]
        assert status["sparse"] == rq.INSUFFICIENT_COVERAGE  # significant, but half the universe

        # Pure function of (results, policy): identical bytes, and a sealed ledger is immutable.
        again = rq.qualify(rq.load_evaluation_results(con, "r4_main", POLICY), POLICY, annotations)
        assert again.to_json_bytes() == ledger.to_json_bytes() and again.to_csv_bytes() == ledger.to_csv_bytes()
        assert rq.persist_ledger(con, again) == "exists"
        with pytest.raises(rq.QualificationError, match="sealed"):
            rq.persist_ledger(con, dataclasses.replace(again, sha256="0" * 64))

        # Unproduced cells are a blocker, never a smaller family.
        results = rq.load_evaluation_results(con, "r4_main", POLICY)
        cells = results.cells.copy()
        cells.loc[(cells.feature_id == "noise_00") & (cells.basis == "reconstructed"), "status"] = "not_produced"
        with pytest.raises(rq.QualificationRefused, match="cells_not_produced:4"):
            rq.qualify(dataclasses.replace(results, cells=cells, family_complete=False), POLICY)

        # RX7: a frozen policy edited in place (even re-hashed) is refused; a change needs a new version.
        edited = json.loads(json.dumps(dict(POLICY.content)))
        edited["coverage"]["min_observations"] = 60
        edited["policy_sha256"] = rq.policy_sha256(edited)
        with pytest.raises(rq.PolicyError, match="frozen with sha256"):
            rq.load_policy(edited)
        doc = (REPO / "docs" / "research" / "QUALIFIED_SIGNALS.md").read_text(encoding="utf-8")
        assert POLICY.sha256 in doc and POLICY.split.sha256 in doc  # the committed doc is current
    finally:
        con.close()
