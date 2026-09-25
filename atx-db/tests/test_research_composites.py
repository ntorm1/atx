"""P5 build forward, end to end: planted signals evaluated by the real R3b engine and graded
by R4 in a research store, then annotated (clusters, spanning), composited on the fit
sample only, evaluated once through the R3b engine, and written as R2b-shaped composite
feature versions that pass R2b's own validator."""

from __future__ import annotations

import dataclasses
import datetime as dt
import json
import math
from types import SimpleNamespace

import duckdb
import numpy as np
import pandas as pd
import pytest

from atx_db.research import composites as cmp
from atx_db.research import evaluation as ev
from atx_db.research import features as rf
from atx_db.research import qualification as rq
from atx_db.research.store import ResearchStore
from tests.test_research_qualification import MONTHS, NAMES, Market, _ar1, _fixture_policy, _persist

STRENGTH = 0.22
PLANTED = ("alpha", "alpha_twin", "carry")
NOISE = ("noise_a", "noise_b", "noise_c")
CLASSES = {"alpha": "value", "alpha_twin": "value", "carry": "profitability", "noise_a": "value",
           "noise_b": "momentum", "noise_c": "profitability"}


def _market(seed: int = 20260926) -> tuple[Market, dict[str, np.ndarray]]:
    """alpha and carry are priced and independent; alpha_twin is alpha plus a little noise (rho ~ 0.9)."""
    rng = np.random.default_rng(seed)
    periods = MONTHS + 12
    raw = {name: _ar1(rng, periods, NAMES) for name in ("alpha", "carry", "twin_noise", *NOISE)}
    values = {"alpha": raw["alpha"], "alpha_twin": 0.9 * raw["alpha"] + math.sqrt(0.19) * raw["twin_noise"],
              "carry": raw["carry"], **{name: raw[name] for name in NOISE}}
    shocks = STRENGTH * (values["alpha"] + values["carry"]) + math.sqrt(1.0 - 2 * STRENGTH ** 2) * \
        rng.standard_normal((periods, NAMES))
    return Market(np.expm1(0.08 * shocks + rng.normal(0.005, 0.04, (periods, 1))), MONTHS), values


def _basis(market: Market, values: dict[str, np.ndarray]) -> ev.BasisInputs:
    features = {name: market.feature(name, values[name], 1, ("rank_normal",)) for name in values}
    return market.basis(features, {name: ("rank_normal",) for name in values})


def _panel_store(con: duckdb.DuckDBPyConnection, market: Market) -> None:
    """R2a-shaped panel runs and R2b constituent versions (schema v3) for the composite writer."""
    rf.ensure_feature_schema(con)
    spec = json.dumps({"size_policy": {"verified_status": "verified_dei_shares", "size_features": ["market_cap"]}})
    for run_id, basis, status in (("panel_recon", "reconstructed", "complete"),
                                  ("panel_strict", "strict", "untestable_strict")):
        con.execute("""
            INSERT INTO research_panel_runs (run_id, status, basis, universe_id, identity_basis, universe_basis,
                fundamental_availability_basis, market_availability_basis, query_version, spec_json, spec_sha256,
                definitions_json, definitions_sha256, code_sha256, source_ids_json, calendar_sha256, start_month,
                end_month, as_of_date, run_at, warehouse_path, blockers_json, panel_sha256, created_at)
            VALUES (?, ?, ?, 'u', 'x', 'u', 'f', 'm', 'q', ?, 'x', '[]', 'x', 'x', '{}', 'x', '2012-01-01',
                    '2026-02-01', '2026-03-31', '2026-04-01', 'w', '[]', ?, '2026-04-01')
        """, [run_id, status, basis, spec, "p" * 64])
    calendar = market.calendar
    con.register("_cal", calendar.assign(month_start=calendar["month_start"], expected=calendar["formation_date"]))
    con.execute("""
        INSERT INTO research_panel_calendar (run_id, month_start, expected_session, last_observed_session,
            formation_date, cutoff, entry_date, status, eligible_members)
        SELECT 'panel_recon', month_start, expected, expected, formation_date, cutoff, entry_date, status,
               eligible_members FROM _cal
    """)
    con.unregister("_cal")
    days = np.repeat(calendar["formation_date"].to_numpy(), NAMES)
    codes = np.tile(np.arange(NAMES), len(calendar))
    ids = np.array([f"S{i:03d}" for i in range(NAMES)], dtype=object)[codes]
    cohort = pd.DataFrame({"formation_date": days, "security_id": ids,
                           "owner_cik": [f"{i:010d}" for i in codes], "log_size": np.log(market.caps)[codes],
                           "industry_group": [f"g{i % 6}" for i in codes]})
    con.register("_cohort", cohort)
    con.execute("""
        INSERT INTO research_panel_cohort (run_id, formation_date, security_id, identity_basis, owner_cik,
                                           cohort_reason, eligible, primary_line)
        SELECT 'panel_recon', CAST(formation_date AS DATE), security_id, 'x', owner_cik, 'valid', true, true
        FROM _cohort
    """)
    con.execute("""
        INSERT INTO research_feature_context (feature_version, formation_date, security_id, owner_basis, owner_cik,
                                              log_size, industry_group)
        SELECT 'fv_recon', CAST(formation_date AS DATE), security_id, 'linked_primary', owner_cik, log_size,
               industry_group FROM _cohort
    """)
    con.unregister("_cohort")
    version_spec = json.dumps({"query_version": rf.QUERY_VERSION, "store_schema": rf.FEATURE_SCHEMA_VERSION,
                               "winsor_limits": [0.01, 0.01], "min_names": 100, "industry_min_group_size": 5,
                               "neutral_min_coverage": 0.8, "taxonomy_code": "FAMA_FRENCH_12"})
    for version, basis, status, panel in (("fv_recon", "reconstructed", "sealed", "panel_recon"),
                                          ("fv_strict", "strict", "untestable_strict", "panel_strict")):
        con.execute("""
            INSERT INTO research_feature_versions (feature_version, status, basis, panel_run_id, panel_sha256,
                classification_basis, values_sha256, query_version, universe_rule, spec_json, spec_sha256,
                code_sha256, catalog_sha256, inputs_json, inputs_sha256, blockers_json, created_at)
            VALUES (?, ?, ?, ?, ?, 'current_sic_backcast:FAMA_FRENCH_12', 'v', ?, ?, ?, 'x', 'x', ?, '{}', 'x',
                    '["research_only_not_release_eligible"]', '2026-04-01')
        """, [version, status, basis, panel, "p" * 64, rf.QUERY_VERSION, rf.UNIVERSE_RULE, version_spec, "c" * 64])


def test_build_forward_annotates_composites_once_and_writes_a_valid_feature_version(tmp_path):
    r4 = _fixture_policy(tmp_path / "policy.json")
    policy = cmp.load_composite_policy()
    market, values = _market()
    catalog = tuple(ev.CatalogFeature(name, 1, CLASSES[name], ("rank_normal",)) for name in sorted(values))
    spec = ev.validate_spec(ev.EvaluationSpec(run_id="p5_source", fm_min_obs=100, bootstrap_resamples=0,
                                              label_diagnostics=False, **rq.evaluation_spec_kwargs(r4)))
    tables = ev.evaluate_bases([_basis(market, values), ev.empty_basis("strict")], spec, catalog=catalog)
    warehouse = tmp_path / "warehouse.duckdb"
    duckdb.connect(str(warehouse)).close()
    store = ResearchStore(tmp_path / "research.duckdb", warehouse_path=warehouse, memory_limit="128MB")
    with store:
        con = store.con
        assert rq.register_policy(con, r4) == "registered"
        _persist(con, tables, spec)
        entries = [SimpleNamespace(feature_id=c.feature_id, expected_sign=c.expected_sign, anomaly_class=c.anomaly_class,
                                   hypothesis_family=f"{c.feature_id}_family", prior_evidence="published_anomaly",
                                   admission="eligible", caveat_codes=(), is_research_eligible=True) for c in catalog]
        graded = rq.qualify_run(con, "p5_source", r4, catalog=entries)
        status = {row["feature_id"]: row["status"] for row in graded.features}
        assert all(status[name] == rq.QUALIFIED_RECONSTRUCTED for name in PLANTED)
        assert not any(status[name] in rq.QUALIFIED_STATUSES for name in NOISE)
        ledger = cmp.load_ledger(con, graded.ledger_id)

        # Known factors: "hml" is carry's own decile long-short (as HML is for book-to-market), the rest noise.
        series = cmp.load_selection_series(con, "p5_source", policy, PLANTED)
        frame = tables.series
        carry = frame[(frame.feature_id == "carry") & (frame.horizon_months == 1) & (frame.variant == "rank_normal")
                      & (frame.basis == "reconstructed")]
        carry = pd.Series(carry["ls_ew10"].to_numpy(dtype=float), index=pd.to_datetime(carry["formation_date"]))
        rng = np.random.default_rng(7)
        dates = pd.DatetimeIndex(pd.to_datetime(market.calendar["formation_date"]))
        factors = pd.DataFrame({name: rng.normal(0.0, 0.02, len(dates)) for name in policy.span_factors}, index=dates)
        factors["hml"] = carry.reindex(dates).to_numpy(dtype=float) + rng.normal(0.0, 0.001, len(dates))
        split = spec.split
        result = cmp.build_forward(ledger, _basis(market, values), split, series, factors, policy)

        # Redundancy: clusters {alpha, alpha_twin} and {carry}; carry is spanned; R4 statuses unchanged.
        rows = {row["feature_id"]: row for row in result.redundancy}
        clusters = [sorted(c) for c in result.manifest["clusters"] if set(c) & set(PLANTED)]
        assert sorted(len(c) for c in clusters) == [1, 2] and ["alpha", "alpha_twin"] in clusters
        assert rows["alpha"]["redundancy_status"] == cmp.NOVEL and rows["alpha"]["span_alpha_z"] > policy.alpha_min_z
        assert rows["alpha_twin"]["redundancy_status"] == "duplicate_of:alpha"
        assert rows["alpha_twin"]["rho_to_representative"] >= policy.rho_threshold
        assert rows["carry"]["redundancy_status"] == cmp.SPANNED and rows["carry"]["span_status"] == "tested"
        assert all(rows[name]["r4_status"] == status[name] for name in PLANTED)
        assert rq.stored_ledger_sha256(con, graded.ledger_id) == graded.sha256

        # Composites: value class (alpha + its twin) and across classes, equal and IC-shrunk.
        emitted = {d.composite_id: d for d in result.emitted}
        assert set(emitted) == {"cmp_value_eq", "cmp_value_icw", "cmp_classes_eq", "cmp_classes_icw"}
        weights = dict((m, w) for m, w, _, _ in emitted["cmp_value_icw"].members)
        assert weights["alpha"] > weights["alpha_twin"] > 0.25 and math.isclose(sum(weights.values()), 1.0)
        # Pure function of its inputs: byte-identical rerun.
        again = cmp.build_forward(ledger, _basis(market, values), split, series, factors, policy)
        assert again.to_json_bytes() == result.to_json_bytes() and again.sha256 == result.sha256
        # The fit never sees the holdout: a holdout row, or a label window reaching into it, is refused.
        holdout = split.holdout_start
        for day, end in ((holdout, holdout + dt.timedelta(days=90)), (holdout - dt.timedelta(days=31), holdout)):
            leak = pd.DataFrame({"member_id": ["alpha"], "formation_date": [day], "label_end": [end], "ic": [0.1]})
            with pytest.raises(cmp.CompositeLeakError, match="holdout"):
                cmp.fit_weights(leak, ["alpha"], split, weighting=cmp.WEIGHT_IC, shrinkage=0.5)
        assert result.manifest["fit_sample"]["last"] < holdout.isoformat()

        # One evaluation through the R3b engine: the composite beats the best planted signal, in and out of sample.
        codes = np.tile(np.arange(NAMES), MONTHS)
        covariates = pd.DataFrame({"month_index": np.repeat(np.arange(MONTHS), NAMES), "security": codes,
                                   "log_size": np.log(market.caps)[codes], "industry_group": [f"g{i % 6}" for i in codes]})
        evaluated = cmp.evaluate_composites(result, _basis(market, values),
                                            dataclasses.replace(spec, run_id="p5_composites"),
                                            standardization=rf.StandardizationPolicy(min_names=100),
                                            covariates=covariates)
        key = {"basis": "reconstructed", "variant": "rank_normal", "horizon_months": r4.primary_horizon}

        def ic(frame: pd.DataFrame, feature: str, **extra: str) -> float:
            mask = frame.feature_id == feature
            for column, value in {**key, **extra}.items():
                mask &= frame[column] == value
            return float(frame.loc[mask, "ic_mean"].iloc[0])

        best = max(ic(tables.cells, name) for name in PLANTED)
        best_holdout = max(ic(tables.slices, name, slice_kind="split", slice_name="holdout") for name in PLANTED)
        assert ic(evaluated.cells, "cmp_classes_icw") >= best
        assert ic(evaluated.slices, "cmp_classes_icw", slice_kind="split", slice_name="holdout") >= best_holdout

        # One build per R4 ledger: identical rerun is a no-op, anything else is refused (the holdout is spent).
        assert cmp.persist_build(con, result) == "stored" and cmp.persist_build(con, again) == "exists"
        with pytest.raises(cmp.CompositeError, match="spent"):
            cmp.persist_build(con, dataclasses.replace(result, sha256="0" * 64))

        # Storable: R2b-shaped composite versions (schema 3) that R2b's validator and R3b's adapter accept.
        _panel_store(con, market)
        formation_dates = {int(m): d for m, d in zip(market.calendar["month_index"], market.calendar["formation_date"],
                                                     strict=True)}
        security_ids = [f"S{i:03d}" for i in range(NAMES)]
        recon = cmp.write_composite_version(store, result, "fv_recon", values=result.composites,
                                            security_ids=security_ids, formation_dates=formation_dates)
        strict = cmp.write_composite_version(store, result, "fv_strict", values=None, security_ids=[],
                                             formation_dates={})
        checked = rf.validate_feature_version(store, recon, verify_panel=False)
        assert checked.status == "sealed" and checked.features_built == 4
        assert rf.validate_feature_version(store, strict, verify_panel=False).status == "untestable_strict"
        table = ev.load_feature_table(store, recon)
        assert json.loads(con.execute("SELECT spec_json FROM research_feature_versions WHERE feature_version = ?",
                                      [recon]).fetchone()[0])["store_schema"] == ev.MIN_FEATURE_STORE_SCHEMA
        family = ev.feature_version_catalog(store, recon)[1]
        assert {f.feature_id for f in family} == set(emitted) and {f.anomaly_class for f in family} == {"composite"}
        members = json.loads(con.execute("SELECT inputs_json FROM research_feature_catalog WHERE feature_version = ? "
                                         "AND feature_id = 'cmp_value_icw'", [recon]).fetchone()[0])["members"]
        assert [m for m, _ in members] == ["alpha", "alpha_twin"] and table.status == "sealed"
        assert cmp.write_composite_version(store, result, "fv_recon", values=result.composites,
                                           security_ids=security_ids, formation_dates=formation_dates) == recon
        cmp.record_holdout_evaluation(con, result.ledger_id, [recon, strict], "p5_composites")
        with pytest.raises(cmp.CompositeError, match="evaluated once"):
            cmp.record_holdout_evaluation(con, result.ledger_id, [recon, strict], "p5_again")
