"""Shape-class cohorts (tools/shape_cohort.py): predicates, label isolation, carry source."""
from __future__ import annotations

import datetime as dt
import importlib.util
import json
import math
from pathlib import Path

import polars as pl
import pyarrow as pa
import pyarrow.parquet as pq
import pytest


TOOLS = Path(__file__).resolve().parents[2] / "tools"


def _load(name: str):
    spec = importlib.util.spec_from_file_location(name, TOOLS / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


qb = _load("quote_benchmark")
sc = _load("shape_cohort")
RULES = {name: rule for name, _label, _reps, rule in sc.SYMBOL_CLASSES + sc.EXPIRY_CLASSES}
SNAP = dt.datetime(2026, 10, 14, 14, 30, tzinfo=dt.timezone.utc)  # 10:30 New York


def mega(**change) -> dict:
    names = {"index_etf_dense": False, "spot": 300.0, "expiries": 12,
             "median_rows_per_expiry": 100, "core_rel_spread": 0.15, "straddle_vol": 0.59}
    names.update(change)
    return names


def test_symbol_rules_pin_the_published_class_boundaries():
    assert sc.evaluate(RULES["index_etf_dense"], {"expiries_within_7d": 4, "median_rows_per_expiry": 200})
    assert not sc.evaluate(RULES["index_etf_dense"], {"expiries_within_7d": 3, "median_rows_per_expiry": 999})
    assert not sc.evaluate(RULES["index_etf_dense"], {"expiries_within_7d": 9, "median_rows_per_expiry": 199})
    assert sc.evaluate(RULES["mega_cap_single"], mega())
    for change in ({"index_etf_dense": True}, {"spot": 99.99}, {"expiries": 11},
                   {"median_rows_per_expiry": 99}, {"core_rel_spread": 0.151},
                   {"core_rel_spread": None}, {"straddle_vol": 0.60}, {"straddle_vol": None}):
        assert not sc.evaluate(RULES["mega_cap_single"], mega(**change)), change
    high = {"straddle_vol": 0.60, "spot": 20.0, "expiries": 8, "median_rows_per_expiry": 40}
    assert sc.evaluate(RULES["high_vol"], high)
    for change in ({"straddle_vol": 0.599}, {"straddle_vol": None}, {"spot": 19.99},
                   {"expiries": 7}, {"median_rows_per_expiry": 39}):
        assert not sc.evaluate(RULES["high_vol"], {**high, **change}), change
    assert sc.evaluate(RULES["sparse"], {"median_rows_per_expiry": 24})
    assert not sc.evaluate(RULES["sparse"], {"median_rows_per_expiry": 24.5})
    assert sc.evaluate(RULES["low_price_wide"], {"spot": 19.99, "core_rel_spread": None})
    assert sc.evaluate(RULES["low_price_wide"], {"spot": 19.99, "core_rel_spread": 0.05})
    assert not sc.evaluate(RULES["low_price_wide"], {"spot": 19.99, "core_rel_spread": 0.049})
    assert not sc.evaluate(RULES["low_price_wide"], {"spot": 20.0, "core_rel_spread": 0.5})
    assert sc.evaluate(RULES["dividend_payer"], {"has_cash_dividend": True})
    assert not sc.evaluate(RULES["dividend_payer"], {"has_cash_dividend": False})
    carry = RULES["carry_abnormal"]
    assert sc.evaluate(carry, {"carry": -0.0201, "rate": 0.045})
    assert not sc.evaluate(carry, {"carry": -0.0199, "rate": 0.045})
    assert sc.evaluate(carry, {"carry": 0.0951, "rate": 0.045})
    assert not sc.evaluate(carry, {"carry": 0.0949, "rate": 0.045})
    assert not sc.evaluate(carry, {"carry": None, "rate": 0.045})


def test_expiry_rules_and_the_bool_contract():
    assert sc.evaluate(RULES["short_dated"], {"dte": 3})
    assert not sc.evaluate(RULES["short_dated"], {"dte": 4})
    assert sc.evaluate(RULES["long_dated"], {"dte": 366})
    assert not sc.evaluate(RULES["long_dated"], {"dte": 365})
    with pytest.raises(TypeError):
        sc.evaluate("dte + 1", {"dte": 3})
    with pytest.raises(NameError):  # no builtins reach a rule
        sc.evaluate("len([1]) == 1", {})


def quote_rows(underlier: str, spot: float, expiries: list[dt.date], strikes: list[float], *,
               mids=None, spread: float = 0.1, ddiv_pv: float = 0.0, sdiv: float = 0.01) -> list[dict]:
    rows = []
    for expiry in expiries:
        days = (expiry - SNAP.date()).days
        for strike in strikes:
            for side in ("Call", "Put"):
                mid = (mids or {}).get((expiry, strike, side), 1.0)
                rows.append({
                    "okey_tk": underlier, "okey_dt": expiry, "okey_xx": strike, "okey_cp": side,
                    "date": SNAP, "timestamp": SNAP, "undSecKey_tk": underlier,
                    "uBid": spot - 0.01, "uAsk": spot + 0.01,
                    "bidPrc": mid - spread / 2, "askPrc": mid + spread / 2, "bidSz": 10, "askSz": 10,
                    "years": days / 365.0, "yearsC": days / 365.0, "rate": 0.04, "sdiv": sdiv,
                    "ddiv": ddiv_pv, "ddivPv": ddiv_pv, "exType": "American",
                    "srPrc": 1.0, "srVol": 0.3})
    return rows


def make_cohort(tmp_path: Path, rows: list[dict], name: str = "cohort") -> Path:
    source = tmp_path / f"{name}.parquet"
    pq.write_table(pa.Table.from_pylist(rows), source)
    output = tmp_path / name
    args = qb.parser().parse_args(["prepare", "--source", str(source), "--output", str(output),
                                  "--split", "all", "--csv"])
    qb.prepare(args)
    return output


def build(tmp_path: Path, out: str, *specs: str, reports: tuple[str, ...] = ()) -> dict:
    argv = ["build", "--output", str(tmp_path / out)]
    for spec in specs:
        argv += ["--cohort", spec]
    for spec in reports:
        argv += ["--implied-forward", spec]
    return sc.build(sc.parser().parse_args(argv))


NEAR, MONTH, LEAP = dt.date(2026, 10, 16), dt.date(2026, 11, 13), dt.date(2027, 11, 19)


def small_cohort(tmp_path: Path) -> Path:
    rows = (quote_rows("DIV", 100.0, [NEAR, MONTH, LEAP], [90., 100., 110.], ddiv_pv=1.0)
            + quote_rows("LOW", 5.0, [NEAR, MONTH], [4., 5., 6.], spread=0.2)
            + quote_rows("PLAIN", 50.0, [MONTH], [45., 50., 55.], spread=0.02))
    return make_cohort(tmp_path, rows)


def members(output: Path, cls: str) -> set[tuple[str, str]]:
    frame = pl.read_csv(output / f"{cls}.csv")
    return set(zip(frame["underlier"].to_list(), frame["expiry"].to_list()))


def test_build_classifies_quote_features_into_member_files(tmp_path):
    cohort = small_cohort(tmp_path)
    manifest = build(tmp_path, "classes", f"t={cohort}")
    out = tmp_path / "classes"
    assert members(out, "dividend_payer") == {("DIV", d.isoformat()) for d in (NEAR, MONTH, LEAP)}
    assert members(out, "low_price_wide") == {("LOW", NEAR.isoformat()), ("LOW", MONTH.isoformat())}
    assert members(out, "short_dated") == {("DIV", NEAR.isoformat()), ("LOW", NEAR.isoformat())}
    assert members(out, "long_dated") == {("DIV", LEAP.isoformat())}
    assert {u for u, _ in members(out, "sparse")} == {"DIV", "LOW", "PLAIN"}
    for cls in ("index_etf_dense", "mega_cap_single", "high_vol", "carry_abnormal"):
        assert members(out, cls) == set(), cls
    entry = {c["name"]: c for c in manifest["classes"]}
    for name, rule in RULES.items():
        assert entry[name]["rule"] == rule
        assert entry[name]["sha256"] == qb.sha256(out / entry[name]["file"])
    assert entry["dividend_payer"]["counts"]["t"] == {"underliers": 1, "expiries": 3, "rows": 18,
                                                      "eligible_rows": 18}
    assert entry["sparse"]["plan_representatives"] == {"CRWV": "absent from every cohort"}
    overlap = manifest["overlap_rows"]["t"]
    assert overlap["dividend_payer"]["dividend_payer"] == 18
    assert overlap["dividend_payer"]["short_dated"] == 6 == overlap["short_dated"]["dividend_payer"]
    assert manifest["cohorts"]["t"]["quotes_parquet_sha256"] == qb.sha256(cohort / "quotes.parquet")
    assert manifest["cohorts"]["t"]["labels_read"] is False
    written = json.loads((out / "manifest.json").read_text(encoding="utf-8"))
    assert written["features"]["sha256"] == qb.sha256(out / "features.csv")


def test_labels_never_reach_the_classifier(tmp_path):
    cohort = small_cohort(tmp_path)
    build(tmp_path, "with-labels", f"t={cohort}")
    (cohort / "labels.parquet").rename(tmp_path / "labels-moved.parquet")
    build(tmp_path, "without-labels", f"t={cohort}")
    for name in [f"{n}.csv" for n in RULES] + ["features.csv"]:
        assert (qb.sha256(tmp_path / "with-labels" / name)
                == qb.sha256(tmp_path / "without-labels" / name)), name


def test_label_bearing_or_mismatched_quotes_and_existing_output_are_refused(tmp_path):
    cohort = small_cohort(tmp_path)
    build(tmp_path, "first", f"t={cohort}")
    with pytest.raises(FileExistsError):
        build(tmp_path, "first", f"t={cohort}")
    quotes = pq.read_table(cohort / "quotes.parquet")
    pq.write_table(quotes.append_column("srVol", pa.array([0.3] * quotes.num_rows)),
                   cohort / "quotes.parquet")
    with pytest.raises(ValueError, match="does not match its cohort manifest"):
        build(tmp_path, "second", f"t={cohort}")
    manifest = json.loads((cohort / "manifest.json").read_text(encoding="utf-8"))
    manifest["files"]["quotes.parquet"]["sha256"] = qb.sha256(cohort / "quotes.parquet")
    (cohort / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
    with pytest.raises(ValueError, match="label columns"):
        build(tmp_path, "third", f"t={cohort}")


def report(path: Path, cohort: Path, *, underlier: str, implied_yield: float, years: float,
           foreign: bool = False) -> Path:
    quotes = pl.read_parquet(cohort / "quotes.parquet")
    chains = quotes.filter(pl.col("underlier") == underlier)["chain_id"].unique().to_list()
    records = [{"chain_id": c, "underlier": underlier, "applied": True,
                "implied_yield": implied_yield, "calendar_years": years} for c in chains]
    if foreign:
        records.append({"chain_id": 12345, "underlier": "X", "applied": False})
    path.write_text(json.dumps({"carry_resolutions": records}), encoding="utf-8")
    return path


def test_implied_forward_report_sets_the_carry_and_must_belong_to_the_cohort(tmp_path):
    cohort = small_cohort(tmp_path)
    hot = report(tmp_path / "hot.json", cohort, underlier="PLAIN", implied_yield=0.10, years=0.5)
    manifest = build(tmp_path, "implied", f"t={cohort}", reports=(f"t={hot}",))
    features = pl.read_csv(tmp_path / "implied" / "features.csv").filter(pl.col("underlier") == "PLAIN")
    assert features["carry_source"].item() == "implied_forward_report"
    assert features["carry"].item() == pytest.approx(0.04 - 0.10)
    assert features["carry_supplied"].item() == pytest.approx(0.04 - 0.01)
    assert members(tmp_path / "implied", "carry_abnormal") == {("PLAIN", MONTH.isoformat())}
    assert manifest["carry_cross_check"]["t"]["carry_abnormal_differs_if_supplied_carry_were_used"] == ["PLAIN"]
    assert manifest["cohorts"]["t"]["implied_forward_report"]["sha256"] == qb.sha256(hot)
    short = report(tmp_path / "short.json", cohort, underlier="PLAIN", implied_yield=0.10, years=0.05)
    build(tmp_path, "short", f"t={cohort}", reports=(f"t={short}",))
    features = pl.read_csv(tmp_path / "short" / "features.csv").filter(pl.col("underlier") == "PLAIN")
    assert features["carry_source"].item() == "supplied_input"
    assert members(tmp_path / "short", "carry_abnormal") == set()
    foreign = report(tmp_path / "foreign.json", cohort, underlier="PLAIN", implied_yield=0.0,
                     years=0.5, foreign=True)
    with pytest.raises(ValueError, match="not a chain of this cohort"):
        build(tmp_path, "foreign", f"t={cohort}", reports=(f"t={foreign}",))


def test_straddle_proxy_recovers_the_atm_volatility_from_quote_mids(tmp_path):
    sigma, spot = 0.8, 40.0
    years = (MONTH - SNAP.date()).days / 365.0
    atm = 0.4 * spot * sigma * math.sqrt(years)  # Brenner-Subrahmanyam ATM price
    mids = {(MONTH, 40.0, "Call"): atm, (MONTH, 40.0, "Put"): atm}
    rows = quote_rows("VOL", spot, [MONTH], [36., 40., 44.], mids=mids, spread=0.02)
    cohort = make_cohort(tmp_path, rows)
    build(tmp_path, "vol", f"t={cohort}")
    features = pl.read_csv(tmp_path / "vol" / "features.csv")
    assert features["straddle_dte"].item() == 30
    assert features["straddle_vol"].item() == pytest.approx(sigma * 0.4 * math.sqrt(2 * math.pi), rel=1e-9)
