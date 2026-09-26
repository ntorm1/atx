"""Research store (node 1.9) end to end: the R2b/R3b DuckDB path versus the Parquet research store.

One fixture: the R3b store fixture's warehouse (60 lines, 2023-2024 bars, real R3a monthly labels, an
observed delisting, a point-in-time NYSE venue from 2024) and an R2a-shaped panel of five catalog
features (a price-line feature ranking five unlinked lines, a verified-size log feature with
unverified caps, a two-sided feature, a fundamental on an older clock with a thin formation and a
non-finite value).

* Path A (oracle): R2b ``build_feature_version`` -> R3b ``open_basis_inputs`` -> ``evaluate_bases``.
* Path B (store): the panel tables exported to a lake snapshot (L1) -> the five features built from
  the lake with R2b's own transforms into the Parquet feature store (L2) -> the R3a labels
  round-tripped through the label matrix (L3) -> ``load_feature_table_from_store`` ->
  ``evaluate_bases`` -> the evaluation cache (L4).

Feature values agree within 1e-12, the date statuses and coverage are equal, and every R3b result
table (and its digest) is identical.
"""

from __future__ import annotations

import csv
import datetime as dt
import json
import math
from pathlib import Path
from typing import Any

import numpy as np
import pandas as pd
import pyarrow as pa
import pytest

from atx_db.research import catalog as research_catalog
from atx_db.research import eval_cache as ec
from atx_db.research import evaluation as ev
from atx_db.research import feature_store as fs
from atx_db.research import features as rf
from atx_db.research import label_matrix as lm
from atx_db.research import panel as rp
from atx_db.research import research_lake as lake
from atx_db.research.store import ResearchStore
from tests.test_research_evaluation import DELIST_LAST_TRADE, N_SECURITIES, SESSIONS, _warehouse
from tests.test_research_features import _catalog_row

VARIANTS = ("rank_normal", "signed_raw", "zscore")
MIN_NAMES = 30
UNVERIFIED_SIZE = {f"S{i:03d}" for i in range(20, 25)}
UNLINKED = {f"S{i:03d}" for i in range(50, 55)}
THIN_MONTH = 5                    # roe: only 25 valid issuers at this formation (< MIN_NAMES)
FEATURES = (  # feature_id, metric window, R2a scope, size feature, catalog row
    ("book_to_market", "daily", "owner", False,
     _catalog_row("book_to_market", "value", "+1", "rank_normal", "unrestricted")),
    ("earnings_yield", "daily", "owner", False,
     _catalog_row("earnings_yield", "value", "two_sided", "winsor_z", "unrestricted", caveat="mixed_evidence",
                  admission="eligible_with_caveat", family="yield_two_sided")),
    ("market_cap", "daily", "owner", True,
     _catalog_row("market_cap", "size", "-1", "log_winsor_z", "positive_value_required", scale="dollar_level")),
    ("momentum_12_1", "daily", "price_line", False,
     _catalog_row("momentum_12_1", "momentum", "+1", "winsor_z", "unrestricted", scale="return")),
    ("roe", "ttm", "owner", False,
     _catalog_row("roe", "profitability", "-1", "rank_normal", "unrestricted", window="ttm")),
)
VERIFIED, UNVERIFIED = rp.SIZE_VERIFIED, rp.UNVERIFIED_VENDOR_SHARES


def _bulk(con: Any, table: str, frame: pd.DataFrame) -> None:
    con.register("_parity_frame", frame)
    try:
        con.execute(f"INSERT INTO {table} ({', '.join(frame.columns)}) SELECT * FROM _parity_frame")
    finally:
        con.unregister("_parity_frame")


def _calendar(month_keys: list[tuple[int, int]]) -> list[tuple[Any, ...]]:
    rows = []
    for year, month in month_keys:
        end = max(day for day in SESSIONS if (day.year, day.month) == (year, month))
        later = [day for day in SESSIONS if day > end]
        rows.append((dt.date(year, month, 1), end, end if later else None,
                     dt.datetime.combine(end, dt.time(22)) if later else None, later[0] if later else None,
                     rp.CALENDAR_FORMED if later else "missing_next_session"))
    return rows


def _panel(research: ResearchStore, signal: np.ndarray, month_keys: list[tuple[int, int]]) -> None:
    """A sealed-status R2a panel of the five features (verify_panel is off on both paths)."""
    con = research.con
    rng = np.random.default_rng(97)
    spec = {"basis": "reconstructed", "features": [[f, f, w, s] for f, w, s, _, _ in FEATURES], "max_age_days": 200,
            "size_policy": {"size_features": ["market_cap"], "verified_status": VERIFIED,
                            "unverified_status": UNVERIFIED, "verified_shares_sources": ["dei"]}}
    con.execute("""
        INSERT INTO research_panel_runs (run_id, status, basis, universe_id, identity_basis, universe_basis,
            fundamental_availability_basis, market_availability_basis, query_version, spec_json, spec_sha256,
            definitions_json, definitions_sha256, code_sha256, source_ids_json, calendar_sha256, start_month,
            end_month, as_of_date, run_at, warehouse_path, blockers_json, panel_sha256, created_at)
        VALUES ('panel_recon', 'complete', 'reconstructed', 'u', 'reconstructed_identity', 'u', ?, ?, ?, ?, 'x',
                '[]', 'x', 'x', '{}', 'x', '2023-01-01', '2024-12-01', '2024-12-31', '2025-01-02', 'w',
                '["research_only_not_release_eligible"]', ?, '2025-01-02')
    """, [rp.FUNDAMENTAL_AVAILABILITY_BASIS, rp.MARKET_AVAILABILITY_BASIS, rp.QUERY_VERSION, json.dumps(spec),
          "a" * 64])
    calendar = _calendar(month_keys)
    con.executemany("""
        INSERT INTO research_panel_calendar (run_id, month_start, expected_session, last_observed_session,
            formation_date, cutoff, entry_date, status, eligible_members)
        VALUES ('panel_recon', ?, ?, ?, ?, ?, ?, ?, ?)
    """, [(s, e, e, f, c, n, st, N_SECURITIES) for s, e, f, c, n, st in calendar])
    cohort, values = [], []
    for position, (_, _, formation, cutoff, _, status) in enumerate(calendar):
        if status != rp.CALENDAR_FORMED:
            continue
        clock = dt.datetime.combine(formation - dt.timedelta(days=40), dt.time(22))  # a quarterly filing clock
        for i in range(N_SECURITIES):
            security = f"S{i:03d}"
            if security == "S007" and formation >= DELIST_LAST_TRADE:
                continue
            unlinked = security in UNLINKED
            reason = "missing_owner_link" if unlinked else "not_common" if security == "S012" else "valid"
            owner = None if unlinked else f"{10 if i == 11 else i:010d}"
            primary = None if reason != "valid" else security != "S011"
            cohort.append({"run_id": "panel_recon", "formation_date": formation, "security_id": security,
                           "security_type": "common", "exchange_code": "XNYS" if i % 2 else "XNAS",
                           "owner_cik": owner, "identity_basis": "current_ticker_unverified", "cohort_reason": reason,
                           "eligible": True, "primary_line": primary})
            ranked = reason == "valid" and primary
            for feature, window, scope, _, _ in FEATURES:
                row = {"run_id": "panel_recon", "formation_date": formation, "security_id": security,
                       "feature_id": feature, "metric_code": feature, "metric_window": window, "raw_value": None,
                       "reason": None, "available_at": None, "latest_input_clock": None, "value_origin": None,
                       "age_days": None, "owner_cik": owner, "identity_basis": "current_ticker_unverified",
                       "universe_basis": "u", "feature_scope": scope, "size_status": None,
                       "availability_basis": rp.FUNDAMENTAL_AVAILABILITY_BASIS if window == "ttm"
                       else rp.MARKET_AVAILABILITY_BASIS}
                if scope == "price_line":
                    raw = float(signal[position, i] + 0.3 * rng.standard_normal())
                    row.update(reason="valid", raw_value=raw, available_at=cutoff, latest_input_clock=cutoff,
                               value_origin="market_daily", age_days=0)
                elif not ranked:
                    row["reason"] = rp.SECONDARY_LINE_REASON if reason == "valid" else reason
                elif feature == "roe":
                    if position == THIN_MONTH and i >= 25:
                        row["reason"] = "missing_state"
                    else:
                        value = None if (i == 40 and position == 7) else float(0.1 * rng.standard_normal())
                        row.update(reason="valid", raw_value=value, available_at=clock, latest_input_clock=clock,
                                   value_origin="quarterly", age_days=40)
                else:
                    if feature == "market_cap" and security == "S030" and position % 2 == 0:
                        row["reason"] = "missing_market_row"
                    else:
                        raw = {"market_cap": 1e8 * (i + 1) * (1.0 + 0.01 * position) * math.exp(
                                   0.05 * rng.standard_normal()),
                               "book_to_market": math.sin(i + position) + 0.5 * float(signal[position, i]),
                               "earnings_yield": math.cos(3 * i + position) + 0.1 * float(rng.standard_normal())
                               }[feature]
                        row.update(reason="valid", raw_value=float(raw), available_at=cutoff,
                                   latest_input_clock=cutoff, value_origin="market_daily", age_days=0)
                        if feature == "market_cap":
                            row["size_status"] = UNVERIFIED if security in UNVERIFIED_SIZE else VERIFIED
                values.append(row)
    _bulk(con, "research_panel_cohort", pd.DataFrame(cohort))
    _bulk(con, "research_panel_values", pd.DataFrame(values))


def _catalog(path: Path) -> tuple[research_catalog.AnomalyCatalogEntry, ...]:
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(research_catalog.ANOMALY_CATALOG_COLUMNS))
        writer.writeheader()
        writer.writerows([row for *_, row in FEATURES])
    return research_catalog.read_anomaly_catalog(path)


def _spec(version: str) -> ev.EvaluationSpec:
    split = ev.freeze_split("parity_split", [("train", "2023-01-01", "2023-09-30"),
                                             ("validation", "2023-10-01", "2024-03-31"),
                                             ("holdout", "2024-04-01", "2024-12-31")])
    return ev.EvaluationSpec(run_id="parity", feature_versions=(version,),
                             label_cutoff=dt.datetime(2025, 1, 10, tzinfo=dt.UTC), split=split, min_names=MIN_NAMES,
                             min_bucket_names=5, fm_min_obs=MIN_NAMES, min_formations=3, nyse_min_names=5,
                             bootstrap_resamples=99, verify_panels=False, variants=VARIANTS)


def _month_end(day: Any) -> dt.date:
    return (pd.Timestamp(day) + pd.offsets.MonthEnd(0)).date()


def _status(frame: pd.DataFrame, *, size: bool, positive: bool) -> pd.Series:
    """R2b's seed-row gate order over the dense universe rows (panel, non-finite, size, domain)."""
    status = pd.Series("in_domain", index=frame.index, dtype=object)
    raw = pd.to_numeric(frame["raw"], errors="coerce")
    panel_ok = frame["panel_reason"].eq("valid")
    status[~panel_ok] = "missing_input"
    nonfinite = panel_ok & ~np.isfinite(raw.fillna(np.nan))
    status[nonfinite] = "nonfinite_value"
    if size:
        status[panel_ok & ~nonfinite & frame["size_status"].ne(VERIFIED)] = "unverified_size"
    if positive:
        status[status.eq("in_domain") & (raw <= 0)] = "nonpositive_value"
    return status


def _build_store_features(root: Path, snapshot: str, entries: tuple[Any, ...],
                          policy: rf.StandardizationPolicy) -> list[dict[str, str]]:
    """Path B, L1 -> L2: the five features from the lake snapshot, standardized per formation year."""
    store = fs.FeatureStore(root)
    con = lake.connect_bounded(None, root=root)
    files = {name: "[" + ", ".join(lake.sql_text(p) for p in lake.lake_files(snapshot, name, root=root)) + "]"
             for name in ("panel_values", "panel_cohort", "panel_calendar")}
    digests = {name: lake.lake_dataset_digest(snapshot, name, root) for name in files}
    by_id = {entry.feature_id: entry for entry in entries}
    unlinked = ", ".join(lake.sql_text(r) for r in rp.OWNER_LINK_FAILURES)
    manifest = []
    for feature, _, scope, size, _ in FEATURES:
        entry = by_id[feature]
        frame = con.execute(f"""
            WITH cal AS (SELECT formation_date FROM read_parquet({files['panel_calendar']}) WHERE status = 'formed'),
            universe AS (
                SELECT k.formation_date, k.security_id, k.owner_cik,
                       CASE WHEN k.cohort_reason = 'valid' AND coalesce(k.primary_line, false) THEN 'linked'
                            WHEN k.cohort_reason IN ({unlinked}) THEN 'unlinked' END AS basis
                FROM read_parquet({files['panel_cohort']}) k JOIN cal USING (formation_date)
                WHERE coalesce(k.eligible, false))
            SELECT last_day(u.formation_date) AS eom, u.security_id AS line_id,
                   CASE WHEN u.basis = 'linked' THEN u.owner_cik END AS owner_id,
                   CAST(v.raw_value AS DOUBLE) AS raw, v.reason AS panel_reason, v.size_status,
                   greatest(v.available_at, v.latest_input_clock) AS available_at
            FROM universe u
            LEFT JOIN read_parquet({files['panel_values']}) v
              ON v.formation_date = u.formation_date AND v.security_id = u.security_id AND v.feature_id = ?
            WHERE u.basis = 'linked' OR (u.basis = 'unlinked' AND ?)
            ORDER BY eom, line_id
        """, [feature, scope == "price_line"]).df()
        frame["status"] = _status(frame, size=size, positive=entry.domain_rule == "positive_value_required")
        sha = fs.compute_feature_sha(feature, fs.catalog_row_payload(entry, policy),
                                     fs.store_code_digest(Path(__file__)), digests)
        years = [part for _, part in frame.groupby(pd.to_datetime(frame["eom"]).dt.year)]
        fs.build_feature(store, "reconstructed", feature, sha, years, expected_sign=int(entry.expected_sign),
                         log_base=entry.preferred_transform == "log_winsor_z", policy=policy,
                         meta={"lake_snapshot": snapshot, "catalog_feature": feature})
        manifest.append({"basis": "reconstructed", "feature_id": feature, "feature_sha": sha})
    con.close()
    store.close()
    return manifest


def _write_labels(root: Path, labels: pd.DataFrame, maturity: pd.DataFrame, calendar: pd.DataFrame,
                  lines: dict[int, str]) -> str:
    """Path B, L3: the R3a labels path A read, written to the label matrix (a container round trip)."""
    spec = {"provisional": True, "holdout_start": None, "source": "r3a_monthly_fixture_round_trip",
            "horizons": [1, 3, 6, 12]}
    label_sha = lm.compute_label_sha(spec)
    matrix = lm.LabelMatrix(root)
    matrix.create(label_sha, spec)
    months = calendar.set_index("month_index")
    eom = {int(k): _month_end(v) for k, v in months["month_start"].items()}
    windows = maturity.assign(eom=maturity["month_index"].map(eom))
    matrix.write_windows(label_sha, pa.table({
        "eom": pa.array(windows["eom"].tolist(), pa.date32()),
        "h": pa.array(windows["horizon_months"].astype("int16").to_numpy(), pa.int16()),
        "formation_date": pa.array(pd.to_datetime(windows["month_index"].map(months["formation_date"])).dt.date
                                   .tolist(), pa.date32()),
        "entry_date": pa.array(pd.to_datetime(windows["month_index"].map(months["entry_date"])).dt.date.tolist(),
                               pa.date32()),
        "exit_date": pa.array([None if pd.isna(x) else pd.Timestamp(x).date() for x in windows["expected_end"]],
                              pa.date32())}))
    ends = {(int(m), int(h)): x for m, h, x in zip(maturity["month_index"], maturity["horizon_months"],
                                                    maturity["expected_end"], strict=True)}
    codes = {(0, 0): 0, (0, 1): 1, (0, 2): 2}
    frame = labels.assign(eom=labels["month_index"].map(eom), line_id=labels["security"].map(lines))
    frame["year"] = [day.year for day in frame["eom"]]
    for (h, year), part in frame.groupby(["horizon_months", "year"]):
        part = part.sort_values(["eom", "line_id"])
        reasons = [codes.get((int(s), int(t)), 3 if int(s) == 1 else 4)
                   for s, t in zip(part["status"], part["terminal"], strict=True)]
        matrix.write(label_sha, int(h), int(year), [pa.table({
            "eom": pa.array(part["eom"].tolist(), pa.date32()),
            "line_id": pa.array(part["line_id"].tolist(), pa.string()),
            "owner_id": pa.array([None] * len(part), pa.string()),
            "entry_date": pa.array(pd.to_datetime(part["anchor_date"]).dt.date.tolist(), pa.date32()),
            "exit_date": pa.array([pd.Timestamp(ends[(int(m), int(h))]).date() for m in part["month_index"]],
                                  pa.date32()),
            "ret": pa.array(part["forward_return"].to_numpy(dtype=float), pa.float64()),
            "ret_exc": pa.array([None] * len(part), pa.float64()),
            "basis": pa.array([ev.LABEL_BASIS] * len(part), pa.string()),
            "reason": pa.array(reasons, pa.int8())})])
    matrix.close()
    return label_sha


def _sorted(frame: pd.DataFrame, keys: list[str]) -> pd.DataFrame:
    return frame.sort_values(keys, kind="stable").reset_index(drop=True)


def test_parquet_research_store_reproduces_the_r2b_r3b_path(tmp_path: Path) -> None:
    warehouse = tmp_path / "warehouse.duckdb"
    signal, month_keys = _warehouse(warehouse)
    research_path = tmp_path / "research.duckdb"
    research = ResearchStore(research_path, warehouse_path=warehouse, memory_limit="256MB")
    research.open()
    root = tmp_path / "research_root"
    policy = rf.StandardizationPolicy(min_names=MIN_NAMES)
    entries = _catalog(tmp_path / "catalog.csv")
    catalog = tuple(ev.CatalogFeature(e.feature_id, int(e.expected_sign), e.anomaly_class, VARIANTS,
                                      e.hypothesis_family) for e in entries)
    catalog_map = {item.feature_id: item for item in catalog}
    try:
        # ---- path A: R2b build -> R3b ------------------------------------------------------------------
        _panel(research, signal, month_keys)
        built = rf.build_feature_version(research, rf.FeatureStoreOptions(
            panel_run_id="panel_recon", catalog_entries=entries, min_names=MIN_NAMES, formation_chunk=6,
            verify_panel=False))
        assert built.status == rf.STATUS_SEALED and built.features_built == len(FEATURES)
        spec = _spec(built.feature_version)
        a_inputs = ev.open_basis_inputs(research, built.feature_version, ev.validate_spec(spec), catalog_map)
        frames = {name: getattr(a_inputs, name).copy() for name in ("calendar", "labels", "maturity", "context",
                                                                    "controls")}
        a_features = {f: a_inputs.load_feature(f) for f, *_ in FEATURES}
        codes = dict(research.con.execute("SELECT security_id, code FROM _ev_securities").fetchall())
        matrix = research.con.execute("""
            SELECT feature_id, formation_date, security_id, domain_status, signed_raw, zscore, rank_normal
            FROM research_feature_matrix WHERE feature_version = ?
        """, [built.feature_version]).df()
        tables_a = ev.evaluate_bases([a_inputs], spec, catalog=catalog)
    finally:
        research.close()

    # ---- path B: lake (L1) -> Parquet features (L2) -> labels (L3) -> R3b -> cache (L4) -----------------
    snapshot = "parity-panel"
    lake.export_lake_snapshot(research_path, snapshot, {
        "panel_values": "SELECT *, year(formation_date) AS year FROM research_panel_values",
        "panel_cohort": "SELECT *, year(formation_date) AS year FROM research_panel_cohort",
        "panel_calendar": "SELECT *, year(month_start) AS year FROM research_panel_calendar"}, root=root)
    assert lake.verify_lake_snapshot(snapshot, root)["ok"]
    with pytest.raises(lake.LakeError, match="immutable"):
        lake.export_lake_snapshot(research_path, snapshot, {"x": "SELECT 1 AS year"}, root=root)
    with pytest.raises(lake.LakeError, match="prod warehouse"):
        lake.export_lake_snapshot(warehouse, "never", {"x": "SELECT 1 AS year"}, root=root)
    manifest = _build_store_features(root, snapshot, entries, policy)
    manifest_sha = fs.write_manifest(root, "parity", manifest)
    assert fs.read_manifest(root, "parity", manifest_sha) == sorted(manifest, key=lambda r: r["feature_id"])
    # A rebuild with identical inputs is a no-op (same sha, file untouched).
    stamp = {r["feature_id"]: fs.FeatureStore(root).path(**r).stat().st_mtime_ns for r in manifest}
    assert _build_store_features(root, snapshot, entries, policy) == manifest
    assert stamp == {r["feature_id"]: fs.FeatureStore(root).path(**r).stat().st_mtime_ns for r in manifest}

    # Feature values: every R2b matrix row is a value row of the store, within 1e-12 (exact in practice).
    store = fs.FeatureStore(root)
    stored = store.scan("reconstructed", [f for f, *_ in FEATURES],
                        shas={r["feature_id"]: r["feature_sha"] for r in manifest}).df()
    stored = stored[stored["reason"] < fs.VALUE_SLOT_LIMIT]
    matrix["eom"] = [_month_end(day) for day in matrix["formation_date"]]
    stored["eom"] = pd.to_datetime(stored["eom"]).dt.date
    joined = matrix.merge(stored, left_on=["feature_id", "eom", "security_id"],
                          right_on=["feature_id", "eom", "line_id"], how="outer", indicator=True)
    assert (joined["_merge"] == "both").all() and len(joined) == len(matrix) == len(stored)
    normal = np.full(len(joined), np.nan)
    ranked = np.isfinite(joined["rank_u"].to_numpy(dtype=float))
    normal[ranked] = rf._inverse_normal(joined["rank_u"].to_numpy(dtype=float)[ranked])
    measured: dict[str, Any] = {"value_rows_compared": len(joined), "max_abs_diff": {}}
    for name, ours, theirs in (("signed_vs_signed_raw", joined["signed"], joined["signed_raw"]),
                               ("z_vs_zscore", joined["z"], joined["zscore"]),
                               ("inv_rank_u_vs_rank_normal", pd.Series(normal), joined["rank_normal"])):
        a, b = ours.to_numpy(dtype=float), theirs.to_numpy(dtype=float)
        assert np.array_equal(np.isnan(a), np.isnan(b))
        measured["max_abs_diff"][name] = float(np.nanmax(np.abs(a - b), initial=0.0))
        assert measured["max_abs_diff"][name] <= 1e-12
    reasons = joined.groupby(["domain_status", "reason"]).size().to_dict()
    assert set(reasons) <= {("in_domain", 0), ("in_domain", 1), ("nonfinite_value", 18)}
    assert ("in_domain", 1) in reasons and ("nonfinite_value", 18) in reasons   # thin roe formation, NULL roe

    # Labels through the label matrix are the labels path A read.
    lines = {code: security for security, code in codes.items()}
    label_sha = _write_labels(root, frames["labels"], frames["maturity"], frames["calendar"], lines)
    with lm.LabelMatrix(root) as matrix_store:
        labels_b, maturity_b, label_info = matrix_store.r3b_inputs(
            label_sha, spec.horizons_months, calendar=frames["calendar"], securities=codes,
            label_cutoff=spec.label_cutoff.replace(tzinfo=None))
    keys = ["horizon_months", "month_index", "security"]
    left, right = _sorted(frames["labels"], keys), _sorted(labels_b, keys)
    for column in ("month_index", "security", "horizon_months", "status", "terminal"):
        assert np.array_equal(left[column].to_numpy(np.int64), right[column].to_numpy(np.int64))
    assert np.array_equal(left["forward_return"].to_numpy(float), right["forward_return"].to_numpy(float))
    assert (pd.to_datetime(left["anchor_date"]).dt.date == pd.to_datetime(right["anchor_date"]).dt.date).all()
    mat_a, mat_b = _sorted(frames["maturity"], ["horizon_months", "month_index"]), \
        _sorted(maturity_b, ["horizon_months", "month_index"])
    assert np.array_equal(mat_a["matured"].to_numpy(bool), mat_b["matured"].to_numpy(bool))
    ends_a, ends_b = (pd.to_datetime(m["expected_end"]).dt.date for m in (mat_a, mat_b))
    assert (ends_a.isna() == ends_b.isna()).all() and (ends_a[ends_a.notna()] == ends_b[ends_b.notna()]).all()
    assert label_info["label_rows"] == len(frames["labels"])

    # R3b over the store adapter: identical date statuses/coverage and identical result tables.
    table = fs.load_feature_table_from_store(store, manifest, calendar=frames["calendar"], securities=codes,
                                             variants=VARIANTS, catalog=catalog_map)
    for feature, *_ in FEATURES:
        ours = _sorted(table.load_feature(feature).dates, ["variant", "month_index"])
        dates_a = a_features[feature].dates
        theirs = _sorted(dates_a[dates_a["variant"].isin(VARIANTS)], ["variant", "month_index"])
        assert list(ours["date_status"]) == list(theirs["date_status"]), feature
        assert np.allclose(ours["coverage_fraction"].to_numpy(float), theirs["coverage_fraction"].to_numpy(float),
                           rtol=0, atol=0, equal_nan=True), feature
    b_inputs = ev.BasisInputs(
        "reconstructed", ev.BASIS_AVAILABLE, a_inputs.security_count, frames["calendar"], labels_b, maturity_b,
        frames["context"], table.load_controls(spec.control_features, spec.control_variant),
        table.variants_by_feature, table.load_feature, dict(a_inputs.meta), dict(a_inputs.digests))
    controls_a = _sorted(frames["controls"], ["control", "month_index", "security"])
    controls_b = _sorted(b_inputs.controls, ["control", "month_index", "security"])
    assert np.array_equal(controls_a["value"].to_numpy(float), controls_b["value"].to_numpy(float), equal_nan=True)
    tables_b = ev.evaluate_bases([b_inputs], spec, catalog=catalog)
    for key in ev.RESULT_TABLES:
        pd.testing.assert_frame_equal(tables_a.frames()[key], tables_b.frames()[key], check_exact=True, obj=key)
    assert tables_b.results_sha256 == tables_a.results_sha256
    cells = tables_b.cells
    # 24 fixture months: every 1- and 3-month cell is tested (6/12 months lack selection formations).
    short = cells[cells["horizon_months"].isin([1, 3])]
    assert len(short) == len(FEATURES) * len(VARIANTS) * 2 and (short["status"] == "tested").all()
    store.close()

    # The evaluation cache keeps each feature's cells under (feature_sha, label_sha, eval_spec_sha).
    cache = ec.EvalCache(root)
    spec_sha = ec.compute_eval_spec_sha(ev.spec_payload(ev.validate_spec(spec)))
    keys_cached = []
    for row in manifest:
        part = cells[cells["feature_id"] == row["feature_id"]].reset_index(drop=True)
        series = tables_b.series[tables_b.series["feature_id"] == row["feature_id"]].reset_index(drop=True)
        cache.write(row["feature_sha"], label_sha, spec_sha, part, {"feature_id": row["feature_id"]},
                    extra={"series": series})
        keys_cached.append((row["feature_sha"], label_sha, spec_sha))
        back = cache.read(row["feature_sha"], label_sha, spec_sha).to_pandas()
        pd.testing.assert_frame_equal(back, part, check_dtype=False, check_exact=True)
    assert cache.missing(keys_cached) == []
    assert cache.scan(keys_cached).count("*").fetchone()[0] == len(cells)
    cache.close()
    measured.update({"reason_pairs": {f"{k[0]}:{k[1]}": int(v) for k, v in reasons.items()},
                     "label_rows": len(labels_b), "maturity_rows": len(maturity_b),
                     "result_rows": {key: len(tables_b.frames()[key]) for key in ev.RESULT_TABLES},
                     "cells_tested": int((cells["status"] == "tested").sum()), "cells": len(cells),
                     "results_sha256_a": tables_a.results_sha256, "results_sha256_b": tables_b.results_sha256,
                     "manifest_sha": manifest_sha, "label_sha": label_sha, "eval_spec_sha": spec_sha})
    print("PARITY " + json.dumps(measured, sort_keys=True))
