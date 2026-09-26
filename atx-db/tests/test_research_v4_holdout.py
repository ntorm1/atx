"""Policy v4 holdout end to end through the node 1.9 label matrix (ruling C-52; node 1.10 fix round 2).

One synthetic market (700 lines, formations 2012-01 .. 2024-06, so policy v4's 2024-01-01 holdout has six
formations) is written to a Parquet label matrix as a provisional and a final label set, both sealed from
2024-01-01, and a two-feature wave is registered in a scratch trial registry. Checked:

* ``open_holdout`` records the label-matrix spec sha after checking that set: a provisional, unknown,
  incomplete or otherwise sealed set is refused; the production registry always checks, a scratch registry
  only when given ``labels_root``;
* ``LabelMatrix.r3b_inputs`` lets the holdout through for that sha, and ``evaluation.label_read_meta``
  records the read (with the opening) in the basis ``meta``;
* the engine refuses a basis read from another set, or not recorded, before it prepares anything (n1);
* ``grade_wave_v4`` grades the holdout of the opened set and refuses a spec, a basis read or an opening
  that differ; a sealed spec needs the engine seal in its run bases (n2); an aware ``created_at`` is a
  refusal, not a ``TypeError`` (n3).
"""

from __future__ import annotations

import copy
import datetime as dt
from pathlib import Path
from types import SimpleNamespace
from typing import Any

import numpy as np
import pyarrow as pa
import pytest

from atx_db.research import evaluation as ev
from atx_db.research import label_matrix as lm
from atx_db.research import qualification as rq
from atx_db.research import trial_registry as tr
from tests.test_research_evaluation import Market, _ar1

NAMES, MONTHS = 700, 150
HORIZONS = (1, 3, 6, 12)
WAVE = "w_hold"
DIGEST = "f" * 64
LINES = {f"L{code:04d}": code for code in range(NAMES)}
LABEL_CUTOFF = dt.datetime(2026, 1, 1)          # every window (the last ends 2025-06-30) has matured
AFTER_LAST_FORMATION = dt.date(2025, 1, 1)


def _market() -> tuple[Market, dict[str, tuple[np.ndarray, int]]]:
    rng = np.random.default_rng(11)
    returns = rng.standard_normal((MONTHS + 13, NAMES)) * 0.08
    strong = _ar1(rng, MONTHS + 13, NAMES, 0.9)
    null = _ar1(rng, MONTHS + 13, NAMES, 0.9)
    returns[1:] += 0.012 * strong[:-1]
    return Market(returns, MONTHS), {"strong_rep": (strong, 1), "null_rep": (null, 1)}


def _row(feature_id: str, sign: int, wave: str = WAVE) -> SimpleNamespace:
    return SimpleNamespace(feature_id=feature_id, expected_sign=sign, evidence_class="replication", population="all",
                           anomaly_class="value", hypothesis_family=feature_id, jkp_theme="value", wave=wave,
                           is_research_eligible=True, source_kind="panel_native", metric_code=None,
                           metric_window="252d", numerator=None, denominator=None, domain="any",
                           availability_clock="daily", preferred_transform="rank_normal", min_history_quarters=0,
                           min_history_sessions=252, admission="admitted")


def _label_spec(**keys: Any) -> dict[str, Any]:
    return {"source": "synthetic_market_v4_holdout_test", "horizons": list(HORIZONS),
            "code_digest": "test-v4-holdout", "input_digests": {"synthetic_market": "seed-11"}, **keys}


def _write_label_set(root: Path, market: Market, *, provisional: bool) -> str:
    """The market's labels as a complete label-matrix set sealed from 2024-01-01."""
    spec = _label_spec(provisional=provisional, holdout_start="2024-01-01")
    sha = lm.compute_label_sha(spec)
    matrix = lm.LabelMatrix(root)
    matrix.create(sha, spec)
    months = np.arange(market.n_months)
    eom = np.array(list(market.calendar["formation_date"]), dtype="datetime64[D]")   # the month ends
    entry = np.array(list(market.calendar["entry_date"]), dtype="datetime64[D]")
    maturity = market.maturity()
    end = {(int(m), int(h)): day for m, h, day in zip(maturity["month_index"], maturity["horizon_months"],
                                                      maturity["expected_end"], strict=True)}
    exits = {h: np.array([end[(int(m), h)] for m in months], dtype="datetime64[D]") for h in HORIZONS}
    matrix.write_windows(sha, pa.table({
        "eom": pa.array(np.concatenate([eom] * len(HORIZONS)), pa.date32()),
        "h": pa.array(np.repeat(np.array(HORIZONS, np.int16), len(months)), pa.int16()),
        "formation_date": pa.array(np.concatenate([eom] * len(HORIZONS)), pa.date32()),
        "entry_date": pa.array(np.concatenate([entry] * len(HORIZONS)), pa.date32()),
        "exit_date": pa.array(np.concatenate([exits[h] for h in HORIZONS]), pa.date32())}))
    labels = market.labels()
    names = np.array(list(LINES), dtype=object)
    years = sorted(set((eom.astype("datetime64[Y]").astype(np.int64) + 1970).tolist()))
    for h in HORIZONS:
        part = labels[labels["horizon_months"] == h].sort_values(["month_index", "security"], kind="stable")
        month = part["month_index"].to_numpy(np.int64)
        year = eom[month].astype("datetime64[Y]").astype(np.int64) + 1970
        for y in years:
            chosen = year == y
            n = int(chosen.sum())
            matrix.write(sha, h, y, [pa.table({
                "eom": pa.array(eom[month][chosen], pa.date32()),
                "line_id": pa.array(names[part["security"].to_numpy(np.int64)][chosen].tolist(), pa.string()),
                "owner_id": pa.nulls(n, pa.string()),
                "entry_date": pa.array(entry[month][chosen], pa.date32()),
                "exit_date": pa.array(exits[h][month][chosen], pa.date32()),
                "ret": pa.array(part["forward_return"].to_numpy(float)[chosen], pa.float64()),
                "ret_exc": pa.nulls(n, pa.float64()),
                "basis": pa.array([ev.LABEL_BASIS] * n, pa.string()),
                "reason": pa.array(np.zeros(n, np.int8), pa.int8())})])
    matrix.complete(sha, HORIZONS, years)
    return sha


def _write_tiny_set(root: Path, **keys: Any) -> str:
    """A complete label set of one empty (h, year) file."""
    spec = _label_spec(**keys)
    sha = lm.compute_label_sha(spec)
    matrix = lm.LabelMatrix(root)
    matrix.create(sha, spec)
    matrix.write(sha, 1, 2012, [])
    matrix.write_windows(sha, lm.WINDOW_SCHEMA.empty_table())
    matrix.complete(sha, [1], [2012])
    return sha


def _basis(market: Market, features: dict[str, tuple[np.ndarray, int]], labels: Any, maturity: Any,
           label_read: dict[str, Any] | None) -> ev.BasisInputs:
    loaded = {fid: market.feature(fid, values * sign, variant="rank_normal", sign=sign)
              for fid, (values, sign) in features.items()}
    basis = market.basis(loaded, variants=("rank_normal",), labels=labels)
    basis.maturity = maturity
    context = market.context()
    context["price"] = np.tile(np.linspace(2.0, 80.0, NAMES), len(market.formed))
    basis.context = context
    calendar = market.calendar.copy()
    calendar["nyse_me_p20"] = float(np.percentile(market.caps, 25))
    calendar["nyse_me_p50"] = float(np.percentile(market.caps, 60))
    basis.calendar = calendar
    if label_read is not None:
        basis.meta = {**basis.meta, ev.LABEL_READ_META: label_read}
    return basis


def test_holdout_opens_and_grades_on_the_label_matrix_sha(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    policy = rq.load_policy_v4()
    market, features = _market()
    signs = {fid: sign for fid, (_, sign) in features.items()}
    catalog = [_row(fid, sign) for fid, sign in signs.items()]
    expected = [ev.CatalogFeature(c.feature_id, c.expected_sign, "value", ("rank_normal",), None, "all")
                for c in catalog]
    labels_root, registry = tmp_path / "research", tr.TrialRegistry(tmp_path / "registry")
    provisional = _write_label_set(labels_root, market, provisional=True)
    final = _write_label_set(labels_root, market, provisional=False)
    cells = [(fid, "rank_normal", h) for fid in features for h in HORIZONS]
    registry.register_wave(WAVE, DIGEST, policy.sha256, list(features), cells, rows=catalog, preregistration=signs)
    registration = registry.registration(WAVE)
    read = {"calendar": market.calendar, "securities": LINES, "label_cutoff": LABEL_CUTOFF}

    def evaluate(run_id: str, kwargs: dict[str, Any], labels: Any, maturity: Any,
                 label_read: dict[str, Any] | None) -> tuple[ev.EvaluationSpec, ev.EvaluationTables]:
        spec = ev.EvaluationSpec(run_id=run_id, verify_panels=False, variants=("rank_normal",),
                                 bootstrap_resamples=99, **kwargs)
        return spec, ev.evaluate_bases([_basis(market, features, labels, maturity, label_read)], spec,
                                       catalog=expected)

    def grade(tables: ev.EvaluationTables, spec: Any, basis: str, bases: Any = None) -> rq.V4Ledger:
        return rq.grade_wave_v4(tables.cells, tables.slices, tables.series, policy, wave=WAVE, spec=spec,
                                catalog=catalog, catalog_digest=DIGEST, grade_basis=basis,
                                run_bases=tables.bases if bases is None else bases, registry=registry)

    def refused(tables: ev.EvaluationTables, spec: Any, basis: str, bases: Any = None) -> tuple[str, ...]:
        with pytest.raises(rq.QualificationRefused) as error:
            grade(tables, spec, basis, bases)
        return error.value.reasons

    # Provisional labels, sealed run: read up to the holdout start, no opening involved.
    with lm.LabelMatrix(labels_root) as matrix:
        prov_labels, prov_maturity, prov_info = matrix.r3b_inputs(provisional, HORIZONS, **read)
    prov_read = ev.label_read_meta(prov_info)
    assert prov_read == {"label_sha": provisional, "provisional": True, "holdout_start": "2024-01-01",
                         "eom_before": "2024-01-01", "holdout_opening": None}
    spec_prov, tables_prov = evaluate("w_hold_prov", rq.evaluation_spec_kwargs_v4(policy, registration=registration),
                                      prov_labels, prov_maturity, prov_read)
    ledger = grade(tables_prov, spec_prov, rq.GRADE_PROVISIONAL)
    assert ledger.manifest["audit"]["label_reads"] == {"reconstructed": prov_read}
    # n2: a sealed spec whose run bases do not show the engine seal is refused.
    unsealed = copy.deepcopy(tables_prov.bases)
    del unsealed["reconstructed"]["prepared_digests"]["labels_3m_sealed_formations"]
    assert "sealed_spec_but_run_bases_not_sealed:reconstructed:3m" in \
        refused(tables_prov, spec_prov, rq.GRADE_PROVISIONAL, unsealed)
    # n3: a timezone-aware created_at in a raw payload is a refusal reason, not a TypeError.
    payload = ev.spec_payload(ev.validate_spec(spec_prov))
    assert "evaluation_spec_created_at_not_naive_utc" in \
        refused(tables_prov, {**payload, "created_at": payload["created_at"] + "+00:00"}, rq.GRADE_PROVISIONAL)

    # open_holdout checks the label-matrix set it records (ruling C-52).
    otherwise_sealed = _write_tiny_set(labels_root, code_digest="test-v4-holdout-tiny", provisional=False,
                                       holdout_start="2025-01-01")
    incomplete_spec = _label_spec(code_digest="test-v4-holdout-incomplete", provisional=False,
                                  holdout_start="2024-01-01")
    incomplete = lm.compute_label_sha(incomplete_spec)
    lm.LabelMatrix(labels_root).create(incomplete, incomplete_spec)
    for label_sha, match in ((provisional, "is provisional"), ("a" * 64, "was not created"),
                             (incomplete, "is not complete"), (otherwise_sealed, "seals from '2025-01-01'")):
        with pytest.raises(tr.RegistryError, match=match):
            registry.open_holdout(WAVE, final_labels=True, label_sha=label_sha, labels_root=labels_root)
    assert registry.holdout_opening(WAVE) is None
    registry.open_holdout(WAVE, final_labels=True, label_sha=final, labels_root=labels_root)
    opening = registry.holdout_opening(WAVE)
    assert opening is not None and opening["label_sha"] == final
    assert opening["label_set"] == {"verified": True, "labels_root": labels_root.as_posix(), "provisional": False,
                                    "holdout_start": "2024-01-01", "horizons": list(HORIZONS),
                                    "years": list(range(2012, 2025))}
    with pytest.raises(tr.RegistryError, match="already opened"):
        registry.open_holdout(WAVE, final_labels=True, label_sha=final, labels_root=labels_root)

    # The label matrix lets the holdout through for the recorded sha; the read records the opening.
    with lm.LabelMatrix(labels_root) as matrix:
        final_labels, final_maturity, final_info = matrix.r3b_inputs(
            final, HORIZONS, eom_before=AFTER_LAST_FORMATION, allow_holdout=True, holdout_wave=WAVE,
            registry_root=registry.root, **read)
    final_read = ev.label_read_meta(final_info)
    assert final_read["label_sha"] == final and final_read["provisional"] is False
    assert final_read["holdout_opening"] == {"wave": WAVE, "sequence": opening["sequence"],
                                             "record_sha": opening["record_sha"]}
    assert len(final_labels) > len(prov_labels)

    # n1: the engine refuses a basis read from another set (or not recorded) before it prepares anything.
    kwargs_final = rq.evaluation_spec_kwargs_v4(policy, registration=registration, label_sha=final)
    prepared: list[str] = []
    real_prepare = ev._prepare
    with monkeypatch.context() as patch:
        patch.setattr(ev, "_prepare", lambda inputs, spec: prepared.append(inputs.basis) or real_prepare(inputs, spec))
        for label_read in (prov_read, None):
            with pytest.raises(ev.EvaluationInputError, match="refused before any statistic"):
                evaluate("w_hold_other", kwargs_final, final_labels, final_maturity, label_read)
    assert prepared == []

    # The final run of the opened set grades the holdout.
    spec_final, tables_final = evaluate("w_hold_final", kwargs_final, final_labels, final_maturity, final_read)
    assert rq.holdout_statistics(tables_final.series, tables_final.slices, policy)
    final_ledger = grade(tables_final, spec_final, rq.GRADE_FINAL)
    manifest = final_ledger.manifest
    assert manifest["holdout_graded"] is True
    assert "holdout_label_set_not_verified" not in manifest["blockers"]
    assert manifest["audit"]["holdout_opening"]["label_sha"] == final == manifest["audit"]["spec_label_sha256"]
    assert manifest["audit"]["label_reads"] == {"reconstructed": final_read}
    status = {row["feature_id"]: row["status"] for row in final_ledger.features}
    assert status["strong_rep"] == rq.QUALIFIED_RECONSTRUCTED

    # The grade refuses a spec, a basis read or a read path that differ from the opening.
    payload_final = ev.spec_payload(ev.validate_spec(spec_final))
    assert refused(tables_final, {**payload_final, "label_sha256": provisional}, rq.GRADE_FINAL) == \
        ("evaluation_spec_label_sha_differs_from_the_opening",)
    for label_read, reason in ((prov_read, "holdout_label_set_differs_from_the_opening"),
                               ({**final_read, "holdout_opening": None}, "run_labels_not_read_through_the_opening"),
                               (None, "run_labels_not_read_from_the_label_matrix")):
        bases = copy.deepcopy(tables_final.bases)
        if label_read is None:
            del bases["reconstructed"]["meta"][ev.LABEL_READ_META]
        else:
            bases["reconstructed"]["meta"][ev.LABEL_READ_META] = label_read
        assert refused(tables_final, spec_final, rq.GRADE_FINAL, bases) == (f"{reason}:reconstructed",)

    # A scratch registry without labels_root records an unchecked opening (the label matrix still refuses a
    # holdout read of a provisional set); the production registry always checks, at the default label root.
    registry.register_wave("w_other", DIGEST, policy.sha256, list(features), cells,
                           rows=[_row(fid, sign, "w_other") for fid, sign in signs.items()], preregistration=signs)
    registry.open_holdout("w_other", final_labels=True, label_sha=provisional)
    assert registry.holdout_opening("w_other")["label_set"] is None
    monkeypatch.setenv(tr.REGISTRY_DIR_ENV, str(tmp_path / "default_registry"))
    monkeypatch.setenv(tr.ANCHOR_ENV, str(tmp_path / "default_anchor.jsonl"))
    monkeypatch.setattr(lm, "DEFAULT_ROOT", labels_root)
    production = tr.TrialRegistry()
    assert production.is_default
    production.register_wave(WAVE, DIGEST, policy.sha256, list(features), cells, rows=catalog, preregistration=signs)
    with pytest.raises(tr.RegistryError, match="is provisional"):
        production.open_holdout(WAVE, final_labels=True, label_sha=provisional)
    production.open_holdout(WAVE, final_labels=True, label_sha=final)
    assert production.holdout_opening(WAVE)["label_set"]["labels_root"] == labels_root.as_posix()
