"""Lane F universe fit benchmark harness (tools/universe_fit_bench.py, tools/universe_archetypes.py).

Pure Python over synthetic adapter-shaped datasets: the panel draw (deterministic, label-free,
development-only), shard planning, chain scorecards against quote_benchmark's own aggregate and
shape-break helper, the failure taxonomy, label governance, the schema-v1 report and worst-N
ranking. atxvol-free (runs in the driver's Arrow partition with --noconftest).
"""
from __future__ import annotations

import datetime as dt
import importlib.util
import json
import math
from pathlib import Path
import random
import sys

import polars as pl
import pyarrow as pa
import pyarrow.csv as pacsv
import pyarrow.parquet as pq
import pytest

TOOLS = Path(__file__).resolve().parents[2] / "tools"
sys.path.insert(0, str(TOOLS))


def _load(name: str):
    spec = importlib.util.spec_from_file_location(name, TOOLS / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


qb = _load("quote_benchmark")
ufb = _load("universe_fit_bench")
SNAP = dt.datetime(2026, 10, 14, 14, 30, tzinfo=dt.timezone.utc)  # 10:30 New York
SNAP_NS = int(SNAP.timestamp()) * 10**9
DAY0 = (SNAP.date() - dt.date(1970, 1, 1)).days


# ── synthetic datasets ───────────────────────────────────────────────────────

def _names(n: int, split: str) -> list[str]:
    """n synthetic underliers of the given split (deterministic; no real tickers)."""
    out, i = [], 0
    while len(out) < n:
        name = f"Z{i:05d}"
        if qb.split_for(name) == split:
            out.append(name)
        i += 1
    return out


def _quote_rows(names: list[str], roots: dict | None = None) -> list[dict]:
    rows, rid = [], 1
    for u_i, name in enumerate(names):
        spot = 50.0 + 10.0 * u_i
        for root in (roots or {}).get(name, [name]):
            for dte in (3, 30, 400):
                exp = DAY0 + dte
                chain = qb.stable_id([name, root, SNAP_NS, exp])
                years = dte / 365.0
                for k in (0.8, 0.9, 1.0, 1.1, 1.2, 3.5):
                    strike = round(spot * k, 2)
                    for call in (1, 0):
                        intrinsic = max(0.0, (spot - strike) if call else (strike - spot))
                        mid = intrinsic + 0.4 * spot * math.sqrt(years) * math.exp(-abs(math.log(k)) * 3)
                        rows.append({
                            "row_id": rid, "chain_id": chain, "snapshot_ns": SNAP_NS,
                            "observation_ns": SNAP_NS + 7 * 10**9, "underlier": name, "root": root,
                            "expiry_days": exp, "strike": strike, "is_call": call,
                            "exercise": "American", "spot_bid": spot - 0.01, "spot_ask": spot + 0.01,
                            "spot": spot, "bid": max(0.0, mid - 0.05), "ask": mid + 0.05,
                            "bid_size": 10, "ask_size": 10, "years": years,
                            "years_calendar": years * 1.01, "rate": 0.045, "sdiv": 0.01,
                            "ddiv": 0.0, "ddiv_pv": 0.0, "split": qb.split_for(name),
                            "input_status": "ok"})
                        rid += 1
    return rows


def make_dataset(tmp: Path, names: list[str], *, split_label: str | None = None,
                 labels: bool = True, roots: dict | None = None, csv: bool = False) -> Path:
    ds = tmp / f"ds-{len(list(tmp.iterdir()))}"
    ds.mkdir()
    rows = _quote_rows(names, roots)
    table = pa.Table.from_pylist(rows, schema=qb.QUOTE_SCHEMA)
    pq.write_table(table, ds / "quotes.parquet", compression="zstd")
    files = {"quotes.parquet": {"sha256": qb.sha256(ds / "quotes.parquet"), "bytes": 1}}
    if csv:
        w = pacsv.CSVWriter(ds / "quotes.csv", qb.QUOTE_SCHEMA)
        w.write_table(table)
        w.close()
        files["quotes.csv"] = {"sha256": qb.sha256(ds / "quotes.csv"), "bytes": 1}
    if labels:
        lab = pa.table({"row_id": pa.array([r["row_id"] for r in rows], pa.uint64()),
                        "reference_price": pa.array([(r["bid"] + r["ask"]) / 2 for r in rows]),
                        "reference_vol": pa.array([0.30 + 0.001 * (r["row_id"] % 7) for r in rows])})
        pq.write_table(lab, ds / "labels.parquet")
        files["labels.parquet"] = {"sha256": qb.sha256(ds / "labels.parquet"), "bytes": 1}
    split = split_label or rows[0]["split"]
    manifest = {"schema_version": 1, "quote_columns": qb.QUOTE_SCHEMA.names, "files": files,
                "rows_selected": len(rows), "rows_by_split": {split: len(rows)},
                "selection": {"buckets_new_york": ["1030"]},
                "source": {"sha256": "0" * 64}}
    qb.write_json(ds / "manifest.json", manifest)
    return ds


def make_predictions(ds: Path, path: Path, *, seed: int = 7) -> Path:
    """Served at mid with a few refusals, price-only rows, an off-market row, a kIvMin vol and
    a non-monotone call price (a shape break)."""
    quotes = pl.read_parquet(ds / "quotes.parquet")
    rng = random.Random(seed)
    out = []
    for r in quotes.iter_rows(named=True):
        mid = (r["bid"] + r["ask"]) / 2
        status, price, vol = "ok", mid, 0.3 + 0.0005 * (r["row_id"] % 11)
        draw = rng.random()
        if r["expiry_days"] == DAY0 + 400 and r["underlier"].endswith("1"):
            status, price, vol = "insufficient_quotes", None, None
        elif draw < 0.05:
            status, vol = "ok_price_only", None
        elif draw < 0.08:
            price = r["ask"] + 0.5  # outside the box
        elif draw < 0.10:
            vol = 0.005
        if status == "ok" and r["is_call"] == 1 and abs(r["strike"] / r["spot"] - 0.9) < 1e-9:
            price = r["spot"]  # deep call above its lower-strike neighbour: a monotone break
        out.append({"row_id": r["row_id"], "predicted_price": price, "predicted_vol": vol,
                    "status": status, "chain_id": r["chain_id"], "admitted": 1})
    pl.DataFrame(out, schema_overrides={"row_id": pl.UInt64, "chain_id": pl.UInt64}).write_csv(path)
    return path


def make_run(ds: Path, tmp: Path, *, probe: dict | None = None) -> Path:
    run = tmp / f"run-{len(list(tmp.iterdir()))}"
    run.mkdir()
    pred = make_predictions(ds, run / "predictions.csv")
    manifest = json.loads((ds / "manifest.json").read_text())
    receipt = {"predictions": str(pred), "prediction_sha256": qb.sha256(pred),
               "quote_parquet_sha256": manifest["files"]["quotes.parquet"]["sha256"],
               "native_report": {}}
    if "quotes.csv" in manifest["files"]:
        receipt["quote_csv_sha256"] = manifest["files"]["quotes.csv"]["sha256"]
    (run / "receipt.json").write_text(json.dumps(receipt))
    shard = {"index": 0, "rows": manifest["rows_selected"], "receipt": str(run / "receipt.json"),
             "probe": probe, "load_before": {}, "load_after": {}}
    run_doc = {"schema": ufb.RUN_SCHEMA, "schema_version": 1, "shards": [shard], "policy": "auto",
               "policy_sha256": None, "dividends": "funding-pv", "binary_sha256": "b" * 64,
               "probe_sha256": "p" * 64, "fit_workers": 4,
               "dataset_manifest_sha256": qb.sha256(ds / "manifest.json")}
    (run / "run.json").write_text(json.dumps(run_doc))
    return run


def _features(names: list[str]) -> pl.DataFrame:
    """Each name's strata are a function of the name (its rank in sorted order), never of the
    input order, so a reordered frame describes the same pool."""
    strata = ufb.STRATA
    rank = {n: i for i, n in enumerate(sorted(names))}
    return pl.DataFrame([{"underlier": n, "classified": True, "eligible_rows": 10,
                          "strata": ";".join(sorted({strata[rank[n] % len(strata)],
                                                     strata[(rank[n] * 7) % len(strata)]}))}
                         for n in names])


# ── tests ───────────────────────────────────────────────────────────────────

def test_panel_is_deterministic_and_label_free(tmp_path, monkeypatch):
    names = _names(500, "development") + ["SPY", "AAPL"]
    a = ufb.draw_panel(_features(names))
    b = ufb.draw_panel(_features(list(reversed(names))))
    assert a["roster"] == b["roster"]
    assert len(a["roster"]) == ufb.PANEL_SIZE
    assert {"SPY", "AAPL"} & set(a["roster"]) == {n for n in ("SPY", "AAPL") if ufb.is_development(n)}
    assert sum(1 for v in a["per_stratum"].values() if v["drawn"]) == len(ufb.STRATA)
    # The feature loader reads quotes.parquet only, even with a labels file beside it.
    ds = make_dataset(tmp_path, _names(3, "development"))
    opened = []
    real = pl.read_parquet
    monkeypatch.setattr(pl, "read_parquet", lambda p, *a, **k: (opened.append(str(p)), real(p, *a, **k))[1])
    quotes, info = ufb.load_feature_quotes(ds)
    features = ufb.build_features(quotes)
    assert opened and not any(p.endswith("labels.parquet") for p in opened)
    assert info["labels_read"] is False
    assert not set(features.columns) & set(ufb.LABEL_COLUMNS)
    assert features.height == 3


def test_panel_contains_no_validation_underlier():
    dev, val = _names(400, "development"), _names(150, "validation")
    mixed = [n for pair in zip(dev, val) for n in pair] + dev[150:]
    draw = ufb.draw_panel(_features(mixed))
    leaked = sum(1 for n in draw["roster"] if qb.split_for(n, 0.2, "atx-vol-v1") != "development")
    assert leaked == 0, f"{leaked} non-development underliers drawn"  # counts only, no names
    assert draw["pool_underliers"] == len(dev)


def test_shards_respect_row_budget():
    rows = {f"U{i:04d}": (i * 37) % 500 + 1 for i in range(600)}
    rows["GIANT"] = 5000
    plan = ufb.plan_shards(rows, 2000)
    sizes = {}
    for name, k in plan["shard_of"].items():
        sizes.setdefault(k, []).append(name)
    assert sorted(plan["shard_of"]) == sorted(rows)
    for k, members in sizes.items():
        total = sum(rows[n] for n in members)
        assert total <= 2000 or members == ["GIANT"], (k, total)
    assert plan["oversize_underliers"] == ["GIANT"]
    shuffled = dict(sorted(rows.items(), key=lambda kv: kv[1]))
    assert ufb.plan_shards(shuffled, 2000)["shard_of"] == plan["shard_of"]
    assert sum(plan["rows"]) == sum(rows.values())


def test_chain_scorecards_sum_to_quote_benchmark_aggregate(tmp_path):
    ds = make_dataset(tmp_path, _names(4, "development"))
    run = make_run(ds, tmp_path)
    _manifest, _receipt, joined = qb._load_evaluation(ds, run / "receipt.json", "all", 0.01)
    agg = qb._aggregate(joined)
    rows, cards, info = ufb.score_bucket(ds, run, "labelled-panel", {})
    assert info["labels_opened"]
    assert sum(c["rows"] for c in cards) == agg["rows"]
    assert sum(c["eligible"] for c in cards) == agg["eligible_rows"]
    assert sum(c["served_price_rows"] for c in cards) == agg["price_prediction_rows"]
    assert sum(c["served_vol_rows"] for c in cards) == agg["vol_prediction_rows"]
    assert sum(c["price_only_rows"] for c in cards) == agg["price_only_predicted_rows"]
    inside = sum((c["containment"] or 0) * c["served_price_rows"] for c in cards)
    assert inside / agg["price_prediction_rows"] == pytest.approx(agg["price_quote_containment"])
    totals = ufb.metrics(rows, cards, True)
    assert totals["vol_bp_rmse"] == pytest.approx(agg["vol_bp_rmse"])
    assert totals["vol_bp_mae"] == pytest.approx(agg["vol_bp_mae"])
    assert sum(sum(c["status_counts"].values()) for c in cards) == agg["rows"]


def test_shape_breaks_identical_to_quote_benchmark(tmp_path):
    names = _names(3, "development")
    ds = make_dataset(tmp_path, names, roots={names[0]: [names[0], names[0] + "1"]})
    run = make_run(ds, tmp_path)
    rows, cards, _ = ufb.score_bucket(ds, run, "labelled-panel", {})
    reference = qb._shape_breaks(rows.lazy())
    mine = ufb.chain_shape_breaks(rows)
    assert reference.height > 0
    assert mine.height == reference.height
    split = mine.with_columns(pl.col("underlier").str.split("\x1f").list.first().alias("underlier"))
    key = ["kind", "underlier", "expiry_days"]
    assert sorted(split.select(key).rows()) == sorted(reference.select(key).rows())
    assert mine["chain_id"].null_count() == 0
    by_kind = {k: sum(c["shape"][k] for c in cards) for k in ("monotone", "convex", "calendar", "duplicate")}
    assert by_kind == {k: int((reference["kind"] == k).sum()) for k in by_kind}


def _card(**change) -> dict:
    card = {"rows": 10, "unsupported_rows": 0, "eligible": 10, "far_strike_rows": 0,
            "served_price_rows": 10, "served_vol_rows": 10, "price_only_rows": 0,
            "containment": 1.0, "kivmin_rows": 0, "status_counts": {"ok": 10},
            "fit": {"refusal": "", "wall_us": 1000.0}, "labels": None, "greeks": None,
            "archetypes": [], "uid": "A", "root": "A", "bucket_et": "1030", "expiry": "2026-11-13",
            "chain_id": 1}
    for k, v in change.items():
        card[k] = v
    return card


def test_failure_mode_rules_total_and_ordered():
    assert ufb.FAILURE_ORDER == (
        "unsupported_exercise", "invalid_input", "strike_ladder", "insufficient_quotes",
        "calendar_arbitrage", "arbitrage_failure", "fit_failure", "outside_market",
        "price_only_dominant", "kIvMin_floor", "vendor_disagreement", "greek_refusal",
        "reprice_mismatch", "slow_fit", "ok")
    greeks = {"evaluated": 10, "availability": {"ok": 10}, "reprice_mismatch": 0}
    cases = [
        (_card(unsupported_rows=10, eligible=0), "unsupported_exercise"),
        (_card(eligible=0, served_price_rows=0), "invalid_input"),
        (_card(far_strike_rows=3, fit={"refusal": "insufficient_quotes", "wall_us": None}),
         "strike_ladder"),
        (_card(served_price_rows=0, status_counts={"insufficient_quotes": 10}, containment=None),
         "insufficient_quotes"),
        (_card(fit={"refusal": "calendar_arbitrage", "wall_us": None}, containment=0.5),
         "calendar_arbitrage"),
        (_card(served_price_rows=0, status_counts={"market_distance_failure": 10},
               containment=None), "arbitrage_failure"),
        (_card(served_price_rows=0, status_counts={"weird": 10}, containment=None), "fit_failure"),
        (_card(containment=0.9, kivmin_rows=2), "outside_market"),
        (_card(price_only_rows=6), "price_only_dominant"),
        (_card(kivmin_rows=1, greeks={**greeks, "reprice_mismatch": 3}), "kIvMin_floor"),
        (_card(labels={"vol_rmse_bp": 400.0}), "vendor_disagreement"),
        (_card(greeks={**greeks, "availability": {"ok": 7, "refused": 3}}), "greek_refusal"),
        (_card(greeks={**greeks, "reprice_mismatch": 1}), "reprice_mismatch"),
        (_card(), "ok"),
    ]
    cards = [c for c, _ in cases]
    ufb.classify_failures(cards)
    for card, (_, expected) in zip(cards, cases):
        assert card["failure_mode"] == expected
    # slow_fit: above its stratum's p99 and above 1 s.
    batch = [_card(chain_id=i, fit={"refusal": "", "wall_us": 1000.0 + i}) for i in range(200)]
    batch.append(_card(chain_id=999, fit={"refusal": "", "wall_us": 5e6}))
    ufb.classify_failures(batch)
    assert [c["failure_mode"] for c in batch].count("slow_fit") == 1
    assert all(c["failure_mode"] in ufb.FAILURE_ORDER for c in batch)


def test_label_free_tier_never_opens_labels(tmp_path, monkeypatch):
    names = _names(3, "development")
    # A universe-split dataset that (adversarially) still has a labels file beside it.
    ds = make_dataset(tmp_path, names, split_label="universe", labels=True)
    run = make_run(ds, tmp_path)

    def forbidden(*_a, **_k):
        raise AssertionError("label reader called on the label-free tier")

    monkeypatch.setattr(ufb, "_read_labels", forbidden)
    rows, cards, info = ufb.score_bucket(ds, run, "label-free-board", {})
    assert info["labels_opened"] is False
    assert all(c["labels"] is None for c in cards)
    assert rows["vol_err_bp"].null_count() == rows.height
    monkeypatch.undo()
    with pytest.raises(PermissionError):
        ufb._read_labels(ds)
    with pytest.raises(PermissionError):
        ufb.require_development({"rows_by_split": {"development": 5, "validation": 1}})
    assert ufb.main(["score", "--pair", f"{ds}={run}", "--tier", "labelled-panel",
                     "--archetypes", str(_arch_csv(tmp_path, names)),
                     "--output", str(tmp_path / "refused.json")]) == 2
    assert not (tmp_path / "refused.json").exists()


def _arch_csv(tmp: Path, names: list[str]) -> Path:
    path = tmp / "arch.csv"
    pl.DataFrame({"underlier": names,
                  "strata": ["high_vol;adr", "", "strike_ladder"][:len(names)]}).write_csv(path)
    return path


def test_report_schema_v1_round_trip(tmp_path):
    names = _names(3, "development")
    ds = make_dataset(tmp_path, names)
    run = make_run(ds, tmp_path)
    out = tmp_path / "report.json"
    assert ufb.main(["score", "--pair", f"{ds}={run}", "--tier", "labelled-panel",
                     "--archetypes", str(_arch_csv(tmp_path, names)), "--output", str(out)]) == 0
    text = out.read_text(encoding="utf-8")
    report = json.loads(text)
    ufb.validate_report(report)
    assert tuple(report) == ufb.REPORT_KEYS + ("run_cost",) or set(ufb.REPORT_KEYS) <= set(report)
    assert report["schema"] == "atx-vol-universe-fit-bench" and report["schema_version"] == 1
    assert report["provenance"]["timing_qualified"] is False
    assert report["scope"]["tier"] == "labelled-panel" and report["scope"]["labels_opened"]
    assert json.loads(json.dumps(report)) == report
    chain = report["chains"][0]
    assert set(ufb.CHAIN_KEYS) <= set(chain)
    assert {m["mode"] for m in report["failure_modes"]} == set(ufb.FAILURE_ORDER)
    with pytest.raises(ValueError):
        ufb.validate_report({**report, "schema_version": 2})
    summary = tmp_path / "summary.md"
    assert ufb.main(["report", "--input", f"panel={out}", "--output", str(summary)]) == 0
    md = summary.read_text(encoding="utf-8")
    assert "Failure mode x archetype" in md and "Cross-bucket stability" in md


def test_worst_ranking_stable_under_ties():
    cards = [_card(chain_id=i, uid=f"U{i % 5}", root=f"U{i % 5}", expiry=f"2026-11-{10 + i % 9:02d}",
                   containment=[0.5, 0.7, 0.5, 0.9][i % 4]) for i in range(40)]
    first = ufb.worst(cards, lambda c: c["containment"], descending=False, n=15)
    for seed in range(5):
        shuffled = cards[:]
        random.Random(seed).shuffle(shuffled)
        assert ufb.worst(shuffled, lambda c: c["containment"], descending=False, n=15) == first
    values = [w["value"] for w in first]
    assert values == sorted(values)
    ties = [w for w in first if w["value"] == 0.5]
    keys = [(w["uid"], w["root"], w["bucket_et"], w["expiry"], w["chain_id"]) for w in ties]
    assert keys == sorted(keys)
