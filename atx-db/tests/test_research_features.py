"""R2b standardized feature store: oracle parity, PIT domain operands, size/universe gates, versions.

The fixture is a sealed, validator-clean R2a-shaped panel (reconstructed basis, four
month-ends, 300 issuers plus secondary class lines and an unlinked tail of losers, five
of which delist after February) in a research store attached to a small warehouse
holding the derived states, standardized items and FF12 classifications that the
feature store reads for domain operands, item legs and industry groups. At the thin
last formation half of the issuers lose their owner link (``missing_owner_link``).
"""

from __future__ import annotations

import csv
import datetime as dt
import json
import math
from pathlib import Path
from statistics import NormalDist
from typing import Any

import duckdb
import numpy as np
import pandas as pd
import pytest

from atx_db import signal_eval
from atx_db.factors import cross_section
from atx_db.research import catalog as research_catalog
from atx_db.research import features as rf
from atx_db.research import panel as rp
from atx_db.research.store import ResearchStore

FORMATIONS = [rp.expected_month_end_session(2024, month) for month in (1, 2, 3, 4)]
THIN_FORMATION = FORMATIONS[3]
N_ISSUERS = 300
THIN_VALID = 150          # valid issuers at the thin formation (< 200)
SECONDARY = 10            # issuers 0..9 also list a class-B line
TAIL = 15                 # eligible lines without an owner link (losers: momentum below every issuer's)
TAIL_DELISTED = 5         # tail lines 0..4 delist after FORMATIONS[1]
VERIFIED = rp.SIZE_VERIFIED
UNVERIFIED = rp.UNVERIFIED_VENDOR_SHARES
Q3 = dt.date(2023, 9, 30)
Q4 = dt.date(2023, 12, 31)
Q3_AT = dt.datetime(2023, 11, 9, 22)
Q4_AT = dt.datetime(2024, 2, 20, 22)
LOAD_DAY = dt.date(2026, 9, 1)

PANEL_FEATURES = (  # (feature_id, window, scope, size feature)
    ("book_to_market", "daily", "owner", True),
    ("debt_to_equity", "q", "owner", False),
    ("dividend_yield", "daily", "owner", True),
    ("earnings_yield", "daily", "owner", True),
    ("enterprise_value", "daily", "owner", True),
    ("gross_profit_to_ev", "daily", "owner", True),
    ("market_cap", "daily", "owner", True),
    ("momentum_12_1", "daily", "price_line", False),
    ("revenue_ttm", "ttm", "owner", False),
    ("roe", "ttm", "owner", False),
)


# ---------------------------------------------------------------------------
# Fixture helpers
# ---------------------------------------------------------------------------

def _cik(i: int) -> str:
    return f"{1000 + i:010d}"


def _owner(i: int) -> str:
    return f"SEC-CIK-{_cik(i)}"


def _bucket(day: dt.date) -> int:
    return (day.year * 12 + day.month - 1 + (1 if day.day >= 15 else 0)) // 3


def _quarter(formation: dt.date) -> tuple[dt.date, dt.datetime]:
    return (Q3, Q3_AT) if formation < dt.date(2024, 2, 21) else (Q4, Q4_AT)


def _bulk(con: Any, table: str, frame: pd.DataFrame) -> None:
    con.register("_fixture_frame", frame)
    try:
        con.execute(f"INSERT INTO {table} ({', '.join(frame.columns)}) SELECT * FROM _fixture_frame")
    finally:
        con.unregister("_fixture_frame")


def _issuer_values(rng: np.random.Generator) -> dict[str, np.ndarray]:
    n = N_ISSUERS
    base = np.exp(20 + 1.5 * rng.standard_normal(n))
    return {
        "market_cap": base,
        "book_to_market": np.where(np.arange(n) % 23 == 1, -0.3, 0.2 + 0.8 * rng.random(n)),
        "earnings_yield": 0.06 + 0.05 * rng.standard_normal(n),
        "dividend_yield": np.where(np.arange(n) % 6 == 0, 0.0, 0.01 + 0.03 * rng.random(n)),
        "ev_multiple": np.where(np.arange(n) % 37 == 5, -1.5, 1.2 + 0.3 * rng.random(n)),
        "gross_profit": base * (0.1 + 0.2 * rng.random(n)),
        "roe": 0.12 + 0.1 * rng.standard_normal(n),
        "equity": np.where(np.arange(n) % 17 == 3, -50.0, 100.0 + 50 * rng.random(n)),
        "debt": 10.0 + 100 * rng.random(n),
        "stockholders_equity": np.where(np.arange(n) % 13 == 6, -20.0, 80.0 + 40 * rng.random(n)),
        "revenue": base * (0.5 + rng.random(n)),
        "total_assets": base * (0.8 + rng.random(n)),
    }


def _warehouse(path: Path, values: dict[str, np.ndarray]) -> None:
    con = duckdb.connect(str(path), config={"threads": "1", "memory_limit": "256MB"})
    con.execute("""
        CREATE TABLE derived_metric_values (derived_value_id VARCHAR, source VARCHAR, security_id VARCHAR,
            metric_code VARCHAR, metric_window VARCHAR, target_bucket BIGINT, period_end DATE, value DOUBLE,
            available_at TIMESTAMP);
        CREATE TABLE fundamental_standardized (standardized_id VARCHAR, source VARCHAR, security_id VARCHAR,
            cik VARCHAR, canonical_code VARCHAR, basis VARCHAR, period_end DATE, value DOUBLE,
            available_at TIMESTAMP, rule_id VARCHAR);
        CREATE TABLE taxonomy (taxonomy_id VARCHAR, code VARCHAR, name VARCHAR);
        CREATE TABLE entity_classification (classification_id VARCHAR, security_id VARCHAR, taxonomy_id VARCHAR,
            node_id VARCHAR, node_code VARCHAR, is_primary BOOLEAN, valid_from DATE, valid_to DATE,
            as_of_date DATE, available_at TIMESTAMP, source_loaded_at TIMESTAMP, run_id VARCHAR, source VARCHAR);
    """)
    source = rp.DERIVED_SOURCE_NAME
    derived, items, classes = [], [], []
    for i in range(N_ISSUERS):
        for period, clock in ((Q3, Q3_AT), (Q4, Q4_AT)):
            bucket = _bucket(period)
            for code, window, value in (("roe", "ttm", values["roe"][i]),
                                        ("debt_to_equity", "q", values["debt"][i] / values["stockholders_equity"][i])):
                derived.append((f"{code}-{i}-{period}", source, _owner(i), code, window, bucket, period, value, clock))
            if i % 29 != 2:  # no equity state at all: missing_book_operand
                # i % 47 == 11: an older valid revision, then an explicit NULL (invalid) state at the
                # roe state's own clock; the latest state is the NULL one (engine rule), never the older value.
                derived.append((f"eq-{i}-{period}", source, _owner(i), "common_equity_avg2", "avg2", bucket, period,
                                values["equity"][i], clock - dt.timedelta(days=1) if i % 47 == 11 else clock))
                if i % 47 == 11:
                    derived.append((f"eq-void-{i}-{period}", source, _owner(i), "common_equity_avg2", "avg2",
                                    bucket, period, 0.0, clock))
            if i % 19 == 4:  # a later revision of the same bucket, after the roe state's clock: ignored
                derived.append((f"eq-late-{i}-{period}", source, _owner(i), "common_equity_avg2", "avg2", bucket,
                                period, -values["equity"][i] if values["equity"][i] > 0 else 99.0,
                                clock + dt.timedelta(days=10)))
            items.append((f"se-{i}-{period}", "sec", _owner(i), str(1000 + i), "stockholders_equity", "instant",
                          period, values["stockholders_equity"][i], clock, "r1"))
            if i % 31 != 9:
                items.append((f"ta-{i}-{period}", "sec", _owner(i), str(1000 + i), "total_assets", "instant",
                              period, values["total_assets"][i] * (1.0 if period == Q4 else 0.9), clock, "r1"))
            if i % 53 == 20 and period == Q3:  # a later explicit NULL restatement of the Q3 balance (R2e)
                items.append((f"ta-void-{i}", "sec", _owner(i), str(1000 + i), "total_assets", "instant", Q3, 0.0,
                              Q3_AT + dt.timedelta(days=1), "r1"))
        if i % 31 == 9:  # only a stale balance: numerator:stale_item
            items.append((f"ta-old-{i}", "sec", _owner(i), str(1000 + i), "total_assets", "instant",
                          dt.date(2022, 12, 31), values["total_assets"][i], dt.datetime(2023, 2, 20, 22), "r1"))
        if i % 11 == 1:  # an amendment filed after every cutoff: never used
            items.append((f"ta-amend-{i}", "sec", _owner(i), str(1000 + i), "total_assets", "instant", Q4,
                          values["total_assets"][i] * 5.0, dt.datetime(2024, 5, 15, 22), "r2"))
        if i % 50 != 7:  # unclassified issuers
            classes.append((f"c-{i}", _owner(i), "ff12", "n", f"FF{i % 12 + 1:02d}", False, LOAD_DAY, None,
                            LOAD_DAY, dt.datetime(2026, 9, 1, 12), dt.datetime(2026, 9, 1, 12), "load", "entity"))
    _bulk(con, "derived_metric_values", pd.DataFrame(derived, columns=[
        "derived_value_id", "source", "security_id", "metric_code", "metric_window", "target_bucket", "period_end",
        "value", "available_at"]))
    con.execute("UPDATE derived_metric_values SET value = NULL WHERE derived_value_id LIKE 'eq-void-%'")
    _bulk(con, "fundamental_standardized", pd.DataFrame(items, columns=[
        "standardized_id", "source", "security_id", "cik", "canonical_code", "basis", "period_end", "value",
        "available_at", "rule_id"]))
    con.execute("UPDATE fundamental_standardized SET value = NULL WHERE standardized_id LIKE 'ta-void-%'")
    con.execute("INSERT INTO taxonomy VALUES ('ff12', 'FAMA_FRENCH_12', 'FF12')")
    _bulk(con, "entity_classification", pd.DataFrame(classes, columns=[
        "classification_id", "security_id", "taxonomy_id", "node_id", "node_code", "is_primary", "valid_from",
        "valid_to", "as_of_date", "available_at", "source_loaded_at", "run_id", "source"]))
    con.close()


def _members(day: dt.date) -> list[dict[str, Any]]:
    rows = []
    for i in range(N_ISSUERS):
        valid = not (day == THIN_FORMATION and i >= THIN_VALID)
        rows.append({"security_id": _owner(i), "owner_cik": _cik(i) if valid else None, "issuer": i,
                     "cohort_reason": "valid" if valid else "missing_owner_link",
                     "primary_line": True if valid else None, "issuer_lines": (2 if i < SECONDARY else 1)
                     if valid else None})
    for i in range(SECONDARY):
        rows.append({"security_id": f"TBL-B-{i:03d}", "owner_cik": _cik(i), "issuer": i, "cohort_reason": "valid",
                     "primary_line": False, "issuer_lines": 2})
    for j in range(TAIL):
        if j < TAIL_DELISTED and day not in FORMATIONS[:2]:
            continue                 # delisted: no longer an eligible member
        rows.append({"security_id": f"TBL-TAIL-{j:03d}", "owner_cik": None, "issuer": None,
                     "cohort_reason": "missing_owner_link", "primary_line": None, "issuer_lines": None})
    return rows


def _linked_lines(day: dt.date) -> set[str]:
    return {_owner(i) for i in range(N_ISSUERS) if not (day == THIN_FORMATION and i >= THIN_VALID)}


def _unlinked_lines(day: dt.date) -> set[str]:
    """Eligible lines without an owner link at ``day``: the live tail plus, at the thin formation, the
    issuers that lost their link."""
    tail = {f"TBL-TAIL-{j:03d}" for j in range(TAIL) if not (j < TAIL_DELISTED and day not in FORMATIONS[:2])}
    return tail | ({_owner(i) for i in range(THIN_VALID, N_ISSUERS)} if day == THIN_FORMATION else set())


def _panel_values(day: dt.date, members: list[dict[str, Any]], values: dict[str, np.ndarray],
                  rng: np.random.Generator) -> list[dict[str, Any]]:
    cutoff = dt.datetime.combine(day, dt.time(22))
    period, clock = _quarter(day)
    drift = 1.0 + 0.02 * FORMATIONS.index(day)
    rows = []
    for member in members:
        i = member["issuer"]
        primary = member["cohort_reason"] == "valid" and member["primary_line"]
        for feature, window, scope, sized in PANEL_FEATURES:
            row: dict[str, Any] = {
                "formation_date": day, "security_id": member["security_id"], "feature_id": feature,
                "metric_code": feature, "metric_window": window, "raw_value": None, "reason": None,
                "available_at": None, "latest_input_clock": None, "period_end": None, "fiscal_period_end": None,
                "value_origin": None, "age_days": None, "max_age_days": None, "owner_cik": member["owner_cik"],
                "derived_value_id": None, "derived_owner_security_id": None,
                "identity_basis": "price_line" if scope == "price_line" else "current_ticker_unverified",
                "universe_basis": "us_listed_reconstructed_v1",
                "availability_basis": rp.MARKET_AVAILABILITY_BASIS if window == "daily"
                else rp.FUNDAMENTAL_AVAILABILITY_BASIS, "feature_scope": scope, "shares_source": None,
                "size_status": None}
            if scope == "price_line":
                draw = float(rng.standard_normal())
                loser = member["security_id"].startswith("TBL-TAIL-")   # later-delisting losers
                row.update(reason="valid", raw_value=-1.5 + 0.1 * draw if loser else 0.05 + 0.3 * draw,
                           available_at=cutoff, latest_input_clock=cutoff, value_origin="market_daily", age_days=0)
                rows.append(row)
                continue
            if not primary:
                row["reason"] = rp.SECONDARY_LINE_REASON if member["cohort_reason"] == "valid" \
                    else member["cohort_reason"]
                rows.append(row)
                continue
            if window == "daily":
                mcap = values["market_cap"][i] * drift
                ev = mcap * values["ev_multiple"][i]
                value = {"market_cap": mcap, "enterprise_value": ev,
                         "book_to_market": values["book_to_market"][i], "earnings_yield": values["earnings_yield"][i],
                         "dividend_yield": values["dividend_yield"][i],
                         "gross_profit_to_ev": values["gross_profit"][i] / ev}[feature]
                if feature == "market_cap" and i % 41 == 8:
                    row["reason"] = "missing_market_row"
                    rows.append(row)
                    continue
                row.update(reason="valid", raw_value=float(value), available_at=cutoff, latest_input_clock=cutoff,
                           value_origin="market_daily", age_days=0)
                if sized:
                    verified = i % 10 != 3
                    row.update(shares_source="dei" if verified else "archive",
                               size_status=VERIFIED if verified else UNVERIFIED)
            else:
                value = {"roe": values["roe"][i],
                         "debt_to_equity": values["debt"][i] / values["stockholders_equity"][i],
                         "revenue_ttm": values["revenue"][i]}[feature]
                reason = "stale_current_anchor" if feature == "roe" and i % 43 == 5 else "valid"
                row.update(reason=reason, raw_value=float(value), available_at=clock, latest_input_clock=clock,
                           period_end=period, fiscal_period_end=period, value_origin="quarterly",
                           age_days=(day - period).days, max_age_days=200,
                           derived_value_id=f"{feature}-{i}-{period}", derived_owner_security_id=_owner(i))
            rows.append(row)
    return rows


def _spec(basis: str) -> dict[str, Any]:
    return {"basis": basis, "features": [[f, f, w, s] for f, w, s, _ in PANEL_FEATURES], "max_age_days": 200,
            "size_policy": {"size_features": sorted(f for f, _, _, sized in PANEL_FEATURES if sized),
                            "verified_shares_sources": ["dei"], "verified_status": VERIFIED,
                            "unverified_status": UNVERIFIED,
                            "rule": "use a size feature only where size_status = verified_status"}}


def _seal_panel(con: Any, run_id: str, basis: str, status: str, members_by_day: dict[dt.date, list],
                values_rows: list[dict[str, Any]]) -> None:
    spec_json = rp._canonical(_spec(basis))
    spec_sha, definitions_sha, code_sha = rp._sha(spec_json), rp._sha("{}"), "c" * 64
    calendar = []
    for day in FORMATIONS:
        entry = day + dt.timedelta(days=3 if day.weekday() == 4 else 1)
        calendar.append((dt.date(day.year, day.month, 1), day, day, day, dt.datetime.combine(day, dt.time(22)),
                         entry, rp.CALENDAR_FORMED))
    calendar_sha = rp._sha(rp._canonical([[rp._iso(v) for v in row] for row in calendar]))
    con.execute("""
        INSERT INTO research_panel_runs (run_id, status, basis, universe_id, identity_basis, universe_basis,
            fundamental_availability_basis, market_availability_basis, query_version, spec_json, spec_sha256,
            definitions_json, definitions_sha256, code_sha256, source_ids_json, calendar_sha256, start_month,
            end_month, as_of_date, run_at, warehouse_path, blockers_json, created_at)
        VALUES (?, 'building', ?, 'u', ?, 'u', ?, ?, ?, ?, ?, '{}', ?, ?, '{}', ?, ?, ?, ?, ?, 'w', ?, ?)
    """, [run_id, basis, "current_ticker_unverified" if basis == "reconstructed" else "dated_identifier_history",
          rp.FUNDAMENTAL_AVAILABILITY_BASIS, rp.MARKET_AVAILABILITY_BASIS, rp.QUERY_VERSION, spec_json, spec_sha,
          definitions_sha, code_sha, calendar_sha, dt.date(2024, 1, 1), dt.date(2024, 4, 1), dt.date(2024, 5, 31),
          dt.datetime(2024, 6, 1), rp._canonical(["research_only_not_release_eligible"]), dt.datetime(2024, 6, 1)])
    for month_start, expected, last, formation, cutoff, entry, cal_status in calendar:
        con.execute("""
            INSERT INTO research_panel_calendar (run_id, month_start, expected_session, last_observed_session,
                formation_date, cutoff, entry_date, status) VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        """, [run_id, month_start, expected, last, formation, cutoff, entry, cal_status])
    cohort = [{"run_id": run_id, "formation_date": day, "security_id": m["security_id"], "security_type": "common",
               "exchange_code": "XNYS" if (m["issuer"] or 0) % 2 else "XNAS", "owner_cik": m["owner_cik"],
               "identity_basis": "current_ticker_unverified", "cohort_reason": m["cohort_reason"], "eligible": True,
               "issuer_lines": m["issuer_lines"], "primary_line": m["primary_line"]}
              for day, members in members_by_day.items() for m in members]
    if cohort:
        _bulk(con, "research_panel_cohort", pd.DataFrame(cohort))
    if values_rows:
        frame = pd.DataFrame(values_rows)
        frame.insert(0, "run_id", run_id)
        _bulk(con, "research_panel_values", frame)
    for month_start, _, _, day, _, _, _ in calendar:
        members = members_by_day.get(day, [])
        eligible = len(members)
        valid = sum(1 for m in members if m["cohort_reason"] == "valid")
        digest = con.execute(rp._COHORT_DIGEST_SQL.format(
            relation="research_panel_cohort", where="WHERE run_id=? AND formation_date=?"), [run_id, day]).fetchone()[0]
        con.execute("""
            UPDATE research_panel_calendar SET visible_members=?, eligible_members=?, valid_members=?,
                owner_unlinked_members=?, owner_link_attrition=?, multi_line_issuers=?, cohort_reasons_json='{}',
                cohort_sha256=? WHERE run_id=? AND month_start=?
        """, [eligible, eligible, valid, eligible - valid, None, 0, digest, run_id, month_start])
        digests = dict(con.execute(rp._VALUE_DIGEST_SQL.format(
            relation="research_panel_values", where="WHERE run_id=? AND formation_date=?"), [run_id, day]).fetchall())
        for feature, window, scope, _ in PANEL_FEATURES:
            reasons = dict(con.execute("SELECT reason, count(*) FROM research_panel_values WHERE run_id=? "
                                       "AND formation_date=? AND feature_id=? GROUP BY 1", [run_id, day, feature])
                           .fetchall())
            con.execute(f"INSERT INTO research_panel_coverage ({','.join(rp._COVERAGE_COLUMNS)}) VALUES "
                        f"({','.join('?' * len(rp._COVERAGE_COLUMNS))})",
                        [run_id, day, feature, feature, window, 0,
                         "formed" if eligible else "empty_common_cohort", eligible, valid, 0,
                         sum(reasons.values()), reasons.get("valid", 0), 0, rp._canonical(reasons),
                         digests.get(feature, rp._EMPTY_SHA), scope])
    manifest = [rp.QUERY_VERSION, spec_sha, definitions_sha, code_sha, calendar_sha, "{}", None]
    panel_sha = rp._panel_digest(con, run_id, manifest)
    con.execute("UPDATE research_panel_runs SET status=?, panel_sha256=? WHERE run_id=?", [status, panel_sha, run_id])


def _catalog_row(feature_id: str, anomaly_class: str, sign: str, transform: str, domain: str, *,
                 source_kind: str = "seed_metric", window: str | None = "daily", numerator: str = "",
                 denominator: str = "", caveat: str = "", admission: str = "eligible", family: str | None = None,
                 scale: str = "ratio") -> dict[str, str]:
    return {"feature_id": feature_id, "source_kind": source_kind,
            "metric_code": feature_id if source_kind == "seed_metric" else "",
            "metric_window": (window or "") if source_kind == "seed_metric" else "", "numerator": numerator,
            "denominator": denominator, "anomaly_class": anomaly_class, "economic_definition": f"{feature_id} fixture",
            "expected_sign": sign, "sign_rationale": "fixture", "reference": "fixture 2026",
            "prior_evidence": "economic_conjecture", "hypothesis_family": family or feature_id, "scale_type": scale,
            "preferred_transform": transform, "domain": domain, "availability_clock": "conservative_filing_46h",
            "min_history_quarters": "1", "min_history_sessions": "0", "admission": admission,
            "caveat_code": caveat, "admission_note": "" if admission == "eligible" else "fixture caveat",
            "supersedes": ""}


def _catalog_entries(path: Path, *, drop: tuple[str, ...] = (),
                     extra: tuple[dict[str, str], ...] = ()) -> tuple[research_catalog.AnomalyCatalogEntry, ...]:
    rows = [
        _catalog_row("market_cap", "size", "-1", "log_winsor_z", "positive_value_required", scale="dollar_level"),
        _catalog_row("book_to_market", "value", "+1", "rank_normal", "negative_book_excluded", caveat="sign_flip",
                     admission="eligible_with_caveat"),
        _catalog_row("earnings_yield", "value", "+1", "winsor_z", "loss_firms_separate", caveat="non_monotone",
                     admission="eligible_with_caveat"),
        _catalog_row("dividend_yield", "payout_issuance", "+1", "winsor_z", "zero_payer_separate",
                     caveat="non_monotone", admission="eligible_with_caveat"),
        _catalog_row("gross_profit_to_ev", "value", "+1", "rank_normal",
                     "positive_denominator_required:metric:enterprise_value", caveat="sign_flip",
                     admission="eligible_with_caveat"),
        _catalog_row("momentum_12_1", "momentum", "+1", "winsor_z", "unrestricted", scale="return"),
        _catalog_row("roe", "profitability", "+1", "rank_normal", "negative_book_excluded:metric:common_equity_avg2",
                     window="ttm", caveat="sign_flip", admission="eligible_with_caveat"),
        _catalog_row("debt_to_equity", "leverage", "-1", "rank_normal",
                     "negative_book_excluded:item:stockholders_equity", window="q", caveat="sign_flip",
                     admission="eligible_with_caveat"),
        _catalog_row("sales_to_price", "value", "+1", "winsor_z", "unrestricted", source_kind="composition",
                     numerator="metric:revenue_ttm", denominator="metric:market_cap"),
        _catalog_row("assets_to_market", "leverage", "two_sided", "rank_normal", "unrestricted",
                     source_kind="composition", numerator="item:total_assets", denominator="metric:market_cap",
                     caveat="mixed_evidence", admission="eligible_with_caveat", family="market_leverage"),
        _catalog_row("earnings_surprise_to_market", "growth", "+1", "rank_normal", "unrestricted",
                     source_kind="composition", numerator="metric:ni_q_change_yoy",
                     denominator="metric:market_cap"),
        _catalog_row("revenue_growth_qoq", "growth", "+1", "rank_normal", "unrestricted", window="q",
                     caveat="sequential_quarter", admission="blocked_incomparable_origin"),
    ]
    rows = [row for row in rows if row["feature_id"] not in drop] + list(extra)
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(research_catalog.ANOMALY_CATALOG_COLUMNS))
        writer.writeheader()
        writer.writerows(rows)
    return research_catalog.read_anomaly_catalog(path)


def _research_store(root: Path, warehouse: Path, values: dict[str, np.ndarray]) -> ResearchStore:
    store = ResearchStore(root / "research.duckdb", warehouse_path=warehouse, memory_limit="256MB")
    store.open()
    con = store.con
    members_by_day = {day: _members(day) for day in FORMATIONS}
    rng = np.random.default_rng(11)
    rows = [row for day in FORMATIONS for row in _panel_values(day, members_by_day[day], values, rng)]
    _seal_panel(con, "panel_recon", "reconstructed", "complete", members_by_day, rows)
    _seal_panel(con, "panel_strict", "strict", "untestable_strict", {}, [])
    return store


def _options(entries: Any, **overrides: Any) -> rf.FeatureStoreOptions:
    base: dict[str, Any] = {"panel_run_id": "panel_recon", "catalog_entries": entries, "formation_chunk": 2}
    base.update(overrides)
    return rf.FeatureStoreOptions(**base)


@pytest.fixture(scope="module")
def built(tmp_path_factory: pytest.TempPathFactory) -> Any:
    root = tmp_path_factory.mktemp("r2b")
    values = _issuer_values(np.random.default_rng(5))
    warehouse = root / "warehouse.duckdb"
    _warehouse(warehouse, values)
    entries = _catalog_entries(root / "catalog.csv")
    store = _research_store(root, warehouse, values)
    result = rf.build_feature_version(store, _options(entries))
    yield {"store": store, "result": result, "entries": entries, "values": values, "root": root,
           "warehouse": warehouse}
    store.close()


def _matrix(store: ResearchStore, version: str, feature: str) -> pd.DataFrame:
    frame = store.con.execute("""
        SELECT m.*, x.log_size, x.industry_group FROM research_feature_matrix m
        LEFT JOIN research_feature_context x ON x.feature_version = m.feature_version
         AND x.formation_date = m.formation_date AND x.security_id = m.security_id
        WHERE m.feature_version=? AND m.feature_id=?
        ORDER BY m.formation_date, m.security_id
    """, [version, feature]).df()
    frame["formation_date"] = pd.to_datetime(frame["formation_date"]).dt.date
    return frame


def _dates(store: ResearchStore, version: str, feature: str) -> pd.DataFrame:
    frame = store.con.execute("""
        SELECT * FROM research_feature_dates WHERE feature_version=? AND feature_id=?
        ORDER BY formation_date, variant
    """, [version, feature]).df()
    frame["formation_date"] = pd.to_datetime(frame["formation_date"]).dt.date
    return frame


def _universe_raw(store: ResearchStore, feature: str, *, verified: bool, lines: bool = False) -> pd.DataFrame:
    """Independent read of the ranked universe's valid panel values (the oracle's input).

    ``lines``: the price-line universe, i.e. also every eligible line without an owner link.
    """
    unlinked = f"OR k.cohort_reason IN ({', '.join(repr(r) for r in rp.OWNER_LINK_FAILURES)})" if lines else ""
    frame = store.con.execute(f"""
        SELECT v.formation_date, v.security_id, v.raw_value FROM research_panel_values v
        JOIN research_panel_cohort k ON k.run_id=v.run_id AND k.formation_date=v.formation_date
                                    AND k.security_id=v.security_id
        WHERE v.run_id='panel_recon' AND v.feature_id=? AND v.reason='valid' AND k.eligible
          AND ((k.cohort_reason='valid' AND k.primary_line) {unlinked})
          {"AND v.size_status = '" + VERIFIED + "'" if verified else ""}
        ORDER BY v.formation_date, v.security_id
    """, [feature]).df()
    frame["formation_date"] = pd.to_datetime(frame["formation_date"]).dt.date
    return frame


def _blom(values: pd.Series) -> np.ndarray:
    ranks = values.rank(method="average").to_numpy()
    n = values.notna().sum()
    return np.array([NormalDist().inv_cdf((r - 0.375) / (n + 0.25)) for r in ranks])


# ---------------------------------------------------------------------------
# Acceptance
# ---------------------------------------------------------------------------

def test_version_is_sealed_valid_and_lists_every_catalog_row(built: Any) -> None:
    store, result = built["store"], built["result"]
    assert result.status == rf.STATUS_SEALED and not result.reused
    validation = rf.validate_feature_version(store, result.feature_version)
    assert validation.values_sha256 == result.values_sha256
    catalog = store.con.execute("""
        SELECT feature_id, status, status_reason, expected_sign, orientation_sign, hypothesis_family, variants_json
        FROM research_feature_catalog WHERE feature_version=? ORDER BY feature_id
    """, [result.feature_version]).fetchall()
    by_id = {row[0]: row for row in catalog}
    assert len(by_id) == 12
    assert by_id["revenue_growth_qoq"][1:3] == (rf.FEATURE_BLOCKED, "blocked_incomparable_origin")
    assert by_id["earnings_surprise_to_market"][1:3] == (rf.FEATURE_INPUT_MISSING, "numerator:metric:ni_q_change_yoy")
    built_ids = {row[0] for row in catalog if row[1] == rf.FEATURE_BUILT}
    assert built_ids == {"assets_to_market", "book_to_market", "debt_to_equity", "dividend_yield", "earnings_yield",
                         "gross_profit_to_ev", "market_cap", "momentum_12_1", "roe", "sales_to_price"}
    # Two-sided hypothesis: catalog sign 0 carried, raw direction kept; the family travels with it.
    assert by_id["assets_to_market"][3:6] == (0, 1, "market_leverage")
    assert by_id["debt_to_equity"][3:5] == (-1, -1)
    # Size-class features have no size-neutral variant.
    assert "size_neutral" not in json.loads(by_id["market_cap"][6])
    assert json.loads(by_id["momentum_12_1"][6]) == list(rf.VARIANTS)
    blockers = set(result.blockers)
    assert {"catalog_features_not_built:1", "catalog_injected_not_the_committed_seed",
            "reconstructed_identity_universe_and_availability_not_certifiable"} <= blockers
    # Controls at rank_normal for every formation (the evaluation reads them there).
    for control in rf.DEFAULT_CONTROLS:
        dates = _dates(store, result.feature_version, control)
        assert len(dates[dates["variant"] == "rank_normal"]) == len(FORMATIONS)


def test_winsor_zscore_and_rank_normal_equal_the_cross_section_oracle(built: Any) -> None:
    store, version = built["store"], built["result"].feature_version
    # momentum ranks every line (unlinked ones too), so even the last formation is formed for it.
    for feature, sign, log, lines in (("momentum_12_1", 1, False, True), ("market_cap", -1, True, False)):
        raw = _universe_raw(store, feature, verified=feature == "market_cap", lines=lines)
        if not lines:
            raw = raw[raw["formation_date"] != THIN_FORMATION]
        matrix = _matrix(store, version, feature).set_index(["formation_date", "security_id"])
        base = np.log(raw["raw_value"]) if log else raw["raw_value"]
        frame = pd.DataFrame({"as_of_date": raw["formation_date"], "value": sign * base,
                              "security_id": raw["security_id"]})
        winsor = cross_section.winsorize(frame, partition_columns=("as_of_date",), limits=(0.01, 0.01))
        zscore = cross_section.zscore(winsor, partition_columns=("as_of_date",))
        blom = frame.groupby("as_of_date", group_keys=False)["value"].apply(
            lambda s: pd.Series(_blom(s), index=s.index))
        keys = list(zip(raw["formation_date"], raw["security_id"], strict=True))
        stored = matrix.loc[keys]
        assert len(stored) == len(raw) > 2 * 200
        np.testing.assert_allclose(stored["signed_raw"].to_numpy(), sign * raw["raw_value"].to_numpy(), atol=0)
        assert np.max(np.abs(stored["winsor"].to_numpy() - winsor["value"].to_numpy(float))) < 1e-12
        assert np.max(np.abs(stored["zscore"].to_numpy() - pd.to_numeric(zscore["value"]).to_numpy(float))) < 1e-12
        assert np.max(np.abs(stored["rank_normal"].to_numpy() - blom.sort_index().to_numpy(float))) < 1e-12
        # The winsorization really caps: 1% tails pinned at the cross-sectional quantiles.
        for _, part in stored.groupby(level="formation_date"):
            signed = part["signed_raw"] if not log else sign * np.log(sign * part["signed_raw"])
            assert part["winsor"].max() <= signed.quantile(0.99) + 1e-12
            assert part["winsor"].min() >= signed.quantile(0.01) - 1e-12
            assert abs(part["zscore"].mean()) < 1e-12 and abs(part["zscore"].std(ddof=1) - 1) < 1e-12


def test_industry_neutral_is_the_unit_variance_signal_eval_neutralization(built: Any) -> None:
    store, version = built["store"], built["result"].feature_version
    # Oracle input: momentum's whole ranked universe (unlinked lines have no industry: the oracle
    # drops them as unclassified). The last formation's coverage is below 80% (see the thin test).
    raw = _universe_raw(store, "momentum_12_1", verified=False, lines=True)
    raw = raw[raw["formation_date"] != THIN_FORMATION]
    groups = {_owner(i): f"FF{i % 12 + 1:02d}" for i in range(N_ISSUERS) if i % 50 != 7}
    panel = pd.DataFrame({"security_id": raw["security_id"], "as_of_date": pd.to_datetime(raw["formation_date"]),
                          "factor_id": "momentum_12_1", "value": raw["raw_value"]})
    classes = panel.loc[panel["security_id"].isin(groups), ["security_id", "as_of_date"]].assign(
        classification_group=lambda f: f["security_id"].map(groups))
    expected = signal_eval.neutralize_panel_by_industry(panel, classes, strict=False).panel
    assert expected.groupby("as_of_date")["value"].std(ddof=1).max() < 0.35     # the raw centered rank (~0.29)
    # Re-standardized per formation to unit variance (FM slopes comparable across variants).
    expected["value"] = expected.groupby("as_of_date")["value"].transform(lambda s: (s - s.mean()) / s.std(ddof=1))
    matrix = _matrix(store, version, "momentum_12_1")
    stored = matrix.dropna(subset=["industry_neutral"])
    merged = expected.assign(formation_date=expected["as_of_date"].dt.date).merge(
        stored[["formation_date", "security_id", "industry_neutral", "industry_group"]],
        on=["formation_date", "security_id"], how="outer", indicator=True)
    assert (merged["_merge"] == "both").all() and len(merged) > 2 * 200
    assert np.max(np.abs(merged["value"] - merged["industry_neutral"])) < 1e-12
    assert (merged["classification_group"] == merged["industry_group"]).all()
    for feature in ("momentum_12_1", "book_to_market", "roe"):
        for _, part in _matrix(store, version, feature).dropna(subset=["industry_neutral"]).groupby("formation_date"):
            assert abs(part["industry_neutral"].mean()) < 1e-12
            assert abs(part["industry_neutral"].std(ddof=1) - 1.0) < 1e-12
    # Unclassified names (and unlinked lines, which have no owner) carry no industry-neutral value.
    unclassified = matrix[matrix["security_id"].isin({_owner(i) for i in range(N_ISSUERS) if i % 50 == 7})]
    assert unclassified["industry_neutral"].isna().all() and unclassified["rank_normal"].notna().any()
    tail = matrix[matrix["security_id"].str.startswith("TBL-TAIL-")]
    assert len(tail) and tail["industry_neutral"].isna().all() and tail["rank_normal"].notna().all()
    dates = _dates(store, version, "momentum_12_1")
    industry = dates[(dates["variant"] == "industry_neutral") & (dates["formation_date"] != THIN_FORMATION)]
    assert (industry["date_status"] == rf.DATE_FORMED).all() and (industry["covariate_coverage"] >= 0.8).all()


def test_size_neutral_is_orthogonal_to_verified_log_size(built: Any) -> None:
    store, version = built["store"], built["result"].feature_version
    checked = 0
    for feature in ("momentum_12_1", "book_to_market", "roe", "sales_to_price"):
        matrix = _matrix(store, version, feature)
        for day, part in matrix.dropna(subset=["size_neutral"]).groupby("formation_date"):
            corr = np.corrcoef(part["size_neutral"], part["log_size"])[0, 1]
            assert abs(corr) < 1e-10, (feature, day, corr)
            assert abs(part["size_neutral"].std(ddof=1) - 1.0) < 1e-12
            checked += 1
        # The regressor is verified size only: unverified issuers get no size-neutral value.
        unverified = matrix[matrix["security_id"].isin({_owner(i) for i in range(N_ISSUERS) if i % 10 == 3})]
        assert unverified["size_neutral"].isna().all() and unverified["log_size"].isna().all()
    assert checked >= 8


def test_thin_formation_is_flagged_and_never_standardized(built: Any) -> None:
    store, version = built["store"], built["result"].feature_version
    for feature in ("roe", "sales_to_price", "market_cap"):     # owner features: 150 linked issuers left
        dates = _dates(store, version, feature)
        thin = dates[dates["formation_date"] == THIN_FORMATION]
        assert set(thin["date_status"]) == {rf.DATE_THIN}
        assert (thin["in_domain_names"] < 200).all()
        matrix = _matrix(store, version, feature)
        part = matrix[matrix["formation_date"] == THIN_FORMATION]
        assert len(part) and part[list(rf.STANDARDIZED_VARIANTS)].isna().all().all()
        assert part.loc[part["domain_status"] == rf.IN_DOMAIN, "signed_raw"].notna().all()
        formed = dates[(dates["formation_date"] != THIN_FORMATION) & (dates["variant"] == "rank_normal")]
        assert (formed["date_status"] == rf.DATE_FORMED).all()


def test_price_line_features_rank_unlinked_delisted_lines_and_fundamentals_do_not(built: Any) -> None:
    """Controller ruling on R2b I1: identity-free price-line features rank every eligible member
    (a linked issuer through its primary line, an unlinked line as its own name); fundamental and
    size/valuation features stay on linked primary lines. Reasons still sum to eligible members."""
    store, result = built["store"], built["result"]
    version = result.feature_version
    delisted = "TBL-TAIL-000"            # unlinked, delists after FORMATIONS[1]
    momentum = _matrix(store, version, "momentum_12_1")
    rows = momentum[momentum["security_id"] == delisted]
    assert list(rows["formation_date"]) == FORMATIONS[:2]
    assert (rows["owner_basis"] == rf.OWNER_BASIS_UNLINKED).all()
    assert rows["rank_normal"].notna().all() and rows["zscore"].notna().all()
    assert (rows["rank_normal"] < -1.5).all()                       # a loser: at the bottom of the ranking
    assert not momentum["security_id"].str.startswith("TBL-B-").any()   # a secondary class: its issuer ranks once
    for day in FORMATIONS:
        part = momentum[momentum["formation_date"] == day]
        assert set(part.loc[part["owner_basis"] == rf.OWNER_BASIS_LINKED, "security_id"]) == _linked_lines(day)
        assert set(part.loc[part["owner_basis"] == rf.OWNER_BASIS_UNLINKED, "security_id"]) == _unlinked_lines(day)
    for feature in ("roe", "market_cap", "sales_to_price", "book_to_market"):
        matrix = _matrix(store, version, feature)
        assert (matrix["owner_basis"] == rf.OWNER_BASIS_LINKED).all(), feature
        for day, part in matrix.groupby("formation_date"):
            assert not set(part["security_id"]) & _unlinked_lines(day), (feature, day)
        assert delisted not in set(matrix["security_id"])
    for feature in ("momentum_12_1", "market_cap", "roe", "sales_to_price"):
        line = feature == "momentum_12_1"
        for _, row in _dates(store, version, feature).iterrows():
            day = row["formation_date"]
            reasons = json.loads(row["reasons_json"])
            assert sum(reasons.values()) == row["eligible_members"]
            assert reasons.get(rp.SECONDARY_LINE_REASON) == SECONDARY
            unlinked = len(_unlinked_lines(day))
            assert reasons.get("cohort:missing_owner_link", 0) == (0 if line else unlinked)
            # The coverage denominator is the feature's ranked universe (M4).
            assert row["universe_names"] == len(_linked_lines(day)) + (unlinked if line else 0)
            assert row["coverage_fraction"] == pytest.approx(row["valid_names"] / row["universe_names"], abs=1e-15)
    reasons = json.loads(_dates(store, version, "roe").iloc[0]["reasons_json"])
    assert reasons["panel:stale_current_anchor"] == sum(1 for i in range(N_ISSUERS) if i % 43 == 5)
    scopes = dict(store.con.execute("SELECT feature_id, universe_scope FROM research_feature_catalog "
                                    "WHERE feature_version=?", [version]).fetchall())
    assert scopes["momentum_12_1"] == rf.UNIVERSE_SCOPE_ALL_LINES
    assert {scopes[f] for f in ("roe", "market_cap", "sales_to_price", "book_to_market", "assets_to_market",
                                "gross_profit_to_ev")} == {rf.UNIVERSE_SCOPE_LINKED}
    assert scopes["revenue_growth_qoq"] is None                       # blocked: never planned
    # Context: unlinked lines carry no owner covariates; the long view labels every value.
    context = store.con.execute("SELECT owner_basis, owner_cik, log_size, industry_group FROM research_feature_context "
                                "WHERE feature_version=? AND security_id=?", [version, delisted]).fetchall()
    assert context == [(rf.OWNER_BASIS_UNLINKED, None, None, None)] * 2
    view = store.con.execute("SELECT DISTINCT owner_basis FROM research_feature_values WHERE feature_version=? "
                             "AND feature_id='momentum_12_1' AND security_id=? AND value IS NOT NULL",
                             [version, delisted]).fetchall()
    assert view == [(rf.OWNER_BASIS_UNLINKED,)]
    assert rf.UNLINKED_LINES_BLOCKER in result.blockers and result.status == rf.STATUS_SEALED
    # N1 (R2e): a formed neutral variant ranks linked names only; the date row labels it.
    labels = _dates(store, version, "momentum_12_1").set_index(["formation_date", "variant"])
    assert labels.loc[(FORMATIONS[0], "industry_neutral"), "sample_conditioning"] == rf.CONDITIONING_LINKED_ONLY
    assert labels.loc[(FORMATIONS[0], "industry_neutral"), "unlinked_excluded"] == TAIL
    assert labels.loc[(FORMATIONS[0], "rank_normal"), "unlinked_excluded"] == 0
    assert any(b.startswith(f"{rf.NEUTRAL_CONDITIONING_BLOCKER}:") for b in result.blockers)
    # Linked-vs-full diagnostic (price-line features only): the unlinked losers sit at the bottom, so a
    # linked-only ranking would hide that the linked names' mean score is above the full cross-section's 0.
    stats = store.con.execute("SELECT feature_id, owner_basis, names, ranked_names, rank_normal_mean "
                              "FROM research_feature_owner_basis WHERE feature_version=? AND formation_date=?",
                              [version, FORMATIONS[0]]).df().set_index("owner_basis")
    assert set(stats["feature_id"]) == {"momentum_12_1"}
    first = momentum[momentum["formation_date"] == FORMATIONS[0]]
    for owner_basis, n in ((rf.OWNER_BASIS_LINKED, N_ISSUERS), (rf.OWNER_BASIS_UNLINKED, TAIL)):
        part = first[first["owner_basis"] == owner_basis]
        assert stats.loc[owner_basis, "names"] == stats.loc[owner_basis, "ranked_names"] == len(part) == n
        assert stats.loc[owner_basis, "rank_normal_mean"] == pytest.approx(part["rank_normal"].mean(), abs=1e-12)
    assert stats.loc[rf.OWNER_BASIS_UNLINKED, "rank_normal_mean"] < -1.5
    assert stats.loc[rf.OWNER_BASIS_LINKED, "rank_normal_mean"] > 0.05


def test_domain_rules_exclude_out_of_domain_values_before_any_transform(built: Any) -> None:
    store, version = built["store"], built["result"].feature_version
    values = built["values"]
    day = FORMATIONS[0]
    checks = {
        "book_to_market": ("negative_book", {i for i in range(N_ISSUERS) if i % 23 == 1 and i % 10 != 3}),
        "earnings_yield": ("loss_firm", {i for i in range(N_ISSUERS)
                                         if values["earnings_yield"][i] <= 0 and i % 10 != 3}),
        "dividend_yield": ("zero_payer", {i for i in range(N_ISSUERS) if i % 6 == 0 and i % 10 != 3}),
        "gross_profit_to_ev": ("nonpositive_denominator", {i for i in range(N_ISSUERS)
                                                           if i % 37 == 5 and i % 10 != 3}),
        "debt_to_equity": ("negative_book", {i for i in range(N_ISSUERS) if i % 13 == 6}),
    }
    for feature, (status, issuers) in checks.items():
        matrix = _matrix(store, version, feature)
        part = matrix[matrix["formation_date"] == day]
        outside = part[part["domain_status"] == status]
        assert set(outside["security_id"]) == {_owner(i) for i in issuers}, feature
        assert outside[list(rf.VARIANTS)].isna().all().all()                     # never ranked
        assert outside["raw_value"].notna().all()                                # the separate indicator
        inside = part[part["domain_status"] == rf.IN_DOMAIN]
        assert inside["rank_normal"].notna().all() and (part["domain_status"].isin([rf.IN_DOMAIN, status])).all()
        reasons = json.loads(_dates(store, version, feature).iloc[0]["reasons_json"])
        assert reasons[status] == len(issuers)
        # rank_normal spans exactly the in-domain names: its Blom extremes match n.
        n = len(inside)
        assert inside["rank_normal"].max() == pytest.approx(NormalDist().inv_cdf((n - 0.375) / (n + 0.25)), abs=1e-12)
    # Loss firms are the separate indicator: E/P ranks only positive earnings.
    ep = _matrix(store, version, "earnings_yield")
    assert (ep.loc[ep["domain_status"] == rf.IN_DOMAIN, "raw_value"] > 0).all()


def test_same_state_operand_ignores_later_revisions_and_missing_operands_are_excluded(built: Any) -> None:
    store, version = built["store"], built["result"].feature_version
    values = built["values"]
    matrix = _matrix(store, version, "roe")
    part = matrix[matrix["formation_date"] == FORMATIONS[2]]    # Q4 state (clock 2024-02-20) seen in March
    status = dict(zip(part["security_id"], part["domain_status"], strict=True))
    for i in range(N_ISSUERS):
        if i % 43 == 5:
            assert _owner(i) not in status          # stale anchor: no value row at all
        elif i % 29 == 2:
            assert status[_owner(i)] == "missing_book_operand"
        elif i % 47 == 11:
            # The latest equity state at the roe state's clock is an explicit NULL (invalid); the older
            # valid revision is not used (plain arg_max would skip the NULL and pick it).
            assert status[_owner(i)] == "missing_book_operand", i
        elif values["equity"][i] <= 0:
            assert status[_owner(i)] == "negative_book"
        else:
            # i % 19 == 4 has a sign-flipped revision published 10 days after the roe
            # state's clock (still before the March cutoff): the same-state rule ignores it.
            assert status[_owner(i)] == rf.IN_DOMAIN, i


def test_size_gate_and_compositions_use_verified_size_and_the_latest_input_clock(built: Any) -> None:
    store, version = built["store"], built["result"].feature_version
    values = built["values"]
    unverified = {_owner(i) for i in range(N_ISSUERS) if i % 10 == 3}
    market_cap = _matrix(store, version, "market_cap")
    assert not set(market_cap["security_id"]) & unverified
    reasons = json.loads(_dates(store, version, "market_cap").iloc[0]["reasons_json"])
    # The first failing gate is the reason: a missing market row precedes the size gate.
    assert reasons["unverified_size"] == len([i for i in range(N_ISSUERS) if i % 10 == 3 and i % 41 != 8])
    assert reasons["panel:missing_market_row"] == len([i for i in range(N_ISSUERS) if i % 41 == 8])
    sales = _matrix(store, version, "sales_to_price")
    assert not set(sales["security_id"]) & unverified
    day = FORMATIONS[2]
    row = sales[(sales["formation_date"] == day) & (sales["security_id"] == _owner(0))].iloc[0]
    assert row["raw_value"] == pytest.approx(values["revenue"][0] / (values["market_cap"][0] * 1.04), rel=1e-12)
    assert row["available_at"] == pd.Timestamp(dt.datetime.combine(day, dt.time(22)))   # max(Q4 clock, bar clock)
    assets = _matrix(store, version, "assets_to_market")
    assert (assets["expected_sign"] == 0).all()
    a_row = assets[(assets["formation_date"] == day) & (assets["security_id"] == _owner(1))].iloc[0]
    # Issuer 1 has a Q4 amendment filed 2024-05-15: after every cutoff, never used.
    assert a_row["raw_value"] == pytest.approx(values["total_assets"][1] / (values["market_cap"][1] * 1.04),
                                               rel=1e-12)
    assert a_row["signed_raw"] == a_row["raw_value"]      # two-sided: unoriented
    january = assets[(assets["formation_date"] == FORMATIONS[0]) & (assets["security_id"] == _owner(1))].iloc[0]
    assert january["raw_value"] == pytest.approx(0.9 * values["total_assets"][1] / values["market_cap"][1],
                                                 rel=1e-12)   # Q4 not yet filed in January: Q3 balance
    stale = json.loads(_dates(store, version, "assets_to_market").iloc[0]["reasons_json"])
    assert stale["numerator:stale_item"] == len([i for i in range(N_ISSUERS) if i % 31 == 9])


def test_newest_null_item_state_is_invalid_and_never_revives_an_older_value(built: Any) -> None:
    """R2e: the latest item state visible at the cutoff is an explicit NULL restatement of Q3; the
    item leg is invalid there, never the older finite Q3 balance (plain arg_max skips the NULL)."""
    store, version, values = built["store"], built["result"].feature_version, built["values"]
    voided = [i for i in range(N_ISSUERS) if i % 53 == 20]
    assets = _matrix(store, version, "assets_to_market")
    january = assets[assets["formation_date"] == FORMATIONS[0]]
    assert len(january) and not set(january["security_id"]) & {_owner(i) for i in voided}
    reasons = json.loads(_dates(store, version, "assets_to_market").iloc[0]["reasons_json"])
    assert reasons["numerator:invalid_item"] == len(voided)
    # The void covers Q3 only: once Q4 is filed, the later period's state is selected again.
    march = assets[(assets["formation_date"] == FORMATIONS[2]) & (assets["security_id"] == _owner(20))].iloc[0]
    assert march["raw_value"] == pytest.approx(values["total_assets"][20] / (values["market_cap"][20] * 1.04),
                                               rel=1e-12)


def test_versions_are_content_addressed_immutable_and_reproducible(built: Any, tmp_path: Path) -> None:
    store, result, entries = built["store"], built["result"], built["entries"]
    again = rf.build_feature_version(store, _options(entries))
    assert again.reused and again.feature_version == result.feature_version
    assert again.values_sha256 == result.values_sha256
    rows = store.con.execute("SELECT count(*) FROM research_feature_matrix WHERE feature_version=?",
                             [result.feature_version]).fetchone()[0]
    assert rows == result.value_rows
    # A fresh store over the same inputs reproduces the version id and the bytes.
    fresh = _research_store(tmp_path, built["warehouse"], built["values"])
    try:
        rebuilt = rf.build_feature_version(fresh, _options(entries, formation_chunk=4))
        assert (rebuilt.feature_version, rebuilt.values_sha256) == (result.feature_version, result.values_sha256)
    finally:
        fresh.close()
    # A changed spec is a new version; the old one is untouched and still validates.
    changed = rf.build_feature_version(store, _options(entries, winsor_limits=(0.02, 0.02)))
    assert changed.feature_version != result.feature_version and not changed.reused
    assert changed.values_sha256 != result.values_sha256
    assert rf.validate_feature_version(store, result.feature_version).values_sha256 == result.values_sha256


def _reseal(con: Any, version: str, feature: str) -> None:
    """Re-seal a tampered feature so that only the semantic contract checks can catch it."""
    digest = rf._feature_digest(con, version, feature, FORMATIONS, 24)
    con.execute("UPDATE research_feature_catalog SET values_sha256=? WHERE feature_version=? AND feature_id=?",
                [digest, version, feature])
    combined, _, _ = rf._combined_digest(con, version, rf._context_digest(con, version, FORMATIONS, 24))
    con.execute("UPDATE research_feature_versions SET values_sha256=? WHERE feature_version=?", [combined, version])


def test_validator_detects_tampering_and_contract_violations(built: Any, tmp_path: Path) -> None:
    store = _research_store(tmp_path, built["warehouse"], built["values"])
    try:
        version = rf.build_feature_version(store, _options(built["entries"])).feature_version
        con = store.con
        con.execute("UPDATE research_feature_matrix SET zscore = zscore + 1e-9 WHERE feature_version=? "
                    "AND feature_id='momentum_12_1' AND security_id=? AND formation_date=?",
                    [version, _owner(0), FORMATIONS[0]])
        with pytest.raises(rf.FeatureStoreError, match="sealed digest"):
            rf.validate_feature_version(store, version, verify_panel=False)
        _reseal(con, version, "momentum_12_1")
        rf.validate_feature_version(store, version, verify_panel=False)
        # A ranked value on an out-of-domain row (negative book), consistently re-sealed.
        con.execute("UPDATE research_feature_matrix SET rank_normal = 0.5 WHERE feature_version=? "
                    "AND feature_id='book_to_market' AND domain_status='negative_book' AND formation_date=?",
                    [version, FORMATIONS[0]])
        _reseal(con, version, "book_to_market")
        with pytest.raises(rf.FeatureStoreError, match="valued_out_of_domain"):
            rf.validate_feature_version(store, version, verify_panel=False)
        con.execute("UPDATE research_feature_matrix SET rank_normal = NULL WHERE feature_version=? "
                    "AND feature_id='book_to_market' AND domain_status='negative_book'", [version])
        _reseal(con, version, "book_to_market")
        rf.validate_feature_version(store, version, verify_panel=False)
        # A valued secondary class line (a second line of one issuer), consistently re-sealed.
        con.execute("""
            INSERT INTO research_feature_matrix (feature_version, formation_date, security_id, feature_id,
                owner_basis, expected_sign, available_at, raw_value, domain_status, signed_raw)
            VALUES (?, ?, 'TBL-B-000', 'momentum_12_1', 'linked_primary', 1, ?, 0.1, 'in_domain', 0.1)
        """, [version, FORMATIONS[0], dt.datetime.combine(FORMATIONS[0], dt.time(22))])
        con.execute("UPDATE research_feature_catalog SET value_rows = value_rows + 1 WHERE feature_version=? "
                    "AND feature_id='momentum_12_1'", [version])
        _reseal(con, version, "momentum_12_1")
        with pytest.raises(rf.FeatureStoreError, match="outside_universe=1"):
            rf.validate_feature_version(store, version, verify_panel=False)
    finally:
        store.close()


def test_resume_after_failure_reproduces_the_clean_version(built: Any, tmp_path: Path,
                                                           monkeypatch: pytest.MonkeyPatch) -> None:
    store = _research_store(tmp_path, built["warehouse"], built["values"])
    try:
        original = rf._build_feature
        calls = {"n": 0}

        def flaky(*args: Any, **kwargs: Any) -> Any:
            calls["n"] += 1
            if calls["n"] == 3:
                raise RuntimeError("injected failure")
            return original(*args, **kwargs)

        monkeypatch.setattr(rf, "_build_feature", flaky)
        with pytest.raises(RuntimeError, match="injected"):
            rf.build_feature_version(store, _options(built["entries"]))
        status = store.con.execute("SELECT status FROM research_feature_versions").fetchone()[0]
        assert status == rf.STATUS_FAILED
        monkeypatch.setattr(rf, "_build_feature", original)
        resumed = rf.build_feature_version(store, _options(built["entries"]))
        assert (resumed.feature_version, resumed.values_sha256) == (built["result"].feature_version,
                                                                   built["result"].values_sha256)
    finally:
        store.close()


def test_strict_basis_is_untestable_with_every_date_status_explained(built: Any) -> None:
    store, entries = built["store"], built["entries"]
    result = rf.build_feature_version(store, _options(entries, panel_run_id="panel_strict"))
    assert result.status == rf.STATUS_UNTESTABLE and result.value_rows == 0
    dates = store.con.execute("SELECT DISTINCT date_status FROM research_feature_dates WHERE feature_version=?",
                              [result.feature_version]).fetchall()
    assert dates == [(rf.DATE_EMPTY,)]
    n = store.con.execute("SELECT count(*) FROM research_feature_dates WHERE feature_version=?",
                          [result.feature_version]).fetchone()[0]
    assert n == len(FORMATIONS) * (10 * len(rf.VARIANTS) - 1)   # 10 buildable features, market_cap w/o size_neutral
    assert rf.validate_feature_version(store, result.feature_version).status == rf.STATUS_UNTESTABLE


def test_the_evaluation_adapter_accepts_the_version(built: Any) -> None:
    from atx_db.research import evaluation

    store, version = built["store"], built["result"].feature_version
    table = evaluation.load_feature_table(store, version)
    assert (table.status, table.basis, table.panel_run_id) == ("sealed", "reconstructed", "panel_recon")
    con = store.con
    con.execute("CREATE OR REPLACE TEMP TABLE _ev_months AS SELECT formation_date, "
                "row_number() OVER (ORDER BY formation_date) - 1 AS month_index FROM research_panel_calendar "
                "WHERE run_id='panel_recon'")
    con.execute("CREATE OR REPLACE TEMP TABLE _ev_securities AS SELECT security_id, "
                "row_number() OVER (ORDER BY security_id) - 1 AS code FROM "
                "(SELECT DISTINCT security_id FROM research_panel_cohort WHERE run_id='panel_recon')")
    for feature in ("momentum_12_1", "assets_to_market", "book_to_market"):
        arrays = table.load_values(feature, list(rf.VARIANTS))
        assert (arrays["month_index"] >= 0).all() and (arrays["security"] >= 0).all()
        assert len(set(arrays["expected_sign"].tolist())) == 1
        assert np.isfinite(arrays["value"]).any()


def test_controls_must_be_buildable(built: Any, tmp_path: Path) -> None:
    entries = _catalog_entries(tmp_path / "catalog.csv", drop=("book_to_market",))
    with pytest.raises(rf.FeatureStoreError, match="controls"):
        rf.build_feature_version(built["store"], _options(entries))


def test_standardize_frame_is_formation_independent() -> None:
    rng = np.random.default_rng(3)
    rows = []
    for day in FORMATIONS[:2]:
        for i in range(260):
            rows.append({"formation_date": pd.Timestamp(day), "security_id": f"S{i:03d}",
                         "raw_value": float(rng.standard_normal()), "domain_status": rf.IN_DOMAIN,
                         "industry_group": f"G{i % 8}", "log_size": float(rng.standard_normal())})
    frame = pd.DataFrame(rows)
    policy = rf.StandardizationPolicy()
    together, _ = rf.standardize_frame(frame, feature_id="x", orientation=-1, log_base=False,
                                       variants=rf.VARIANTS, policy=policy)
    alone, _ = rf.standardize_frame(frame[frame["formation_date"] == pd.Timestamp(FORMATIONS[1])], feature_id="x",
                                    orientation=-1, log_base=False, variants=rf.VARIANTS, policy=policy)
    part = together[together["formation_date"] == pd.Timestamp(FORMATIONS[1])].reset_index(drop=True)
    for variant in rf.VARIANTS:
        np.testing.assert_array_equal(part[variant].to_numpy(), alone[variant].to_numpy())
    assert math.isclose(float(np.corrcoef(part["signed_raw"], part["rank_normal"])[0, 1]), 1.0, abs_tol=0.05)


def _seal_research_sources(con: Any, panel_sha: str, parent_version: str) -> None:
    """E1 fixture: a sealed P3 event version ``ev1`` (``ear_m1p1``), a complete P4 factor run ``fr1``
    (``beta_mkt_252d``) and a sealed P9 ownership version ``ow1`` (``io_ratio_13f``) of ``panel_recon``.

    Events: no row when i % 7 == 0; i % 7 == 1 is valid in January and ``event_value_pending`` (NULL)
    afterwards. Ownership: no row when i % 11 == 4; i % 5 == 0 ``no_mapped_13f_holding``; i % 5 == 1 valid in
    January and ``unverified_shares`` afterwards; issuer 2's IO is 1.3, flagged ``io_above_one``. Factor: every
    ranked line (unlinked ones too) but issuer 3's; January is the beta burn-in (``insufficient_obs``); tail
    line 7's March beta is clocked after the March cutoff.
    """
    from atx_db import ownership_identity
    from atx_db.research import events as rev
    from atx_db.research import factor_returns as rfr
    from atx_db.research import ownership_features as rof

    for ensure in (rev.ensure_event_schema, rfr.ensure_factor_schema, rof.ensure_ownership_schema):
        ensure(con)
    created = dt.datetime(2026, 9, 25)
    events, owners, exposures = [], [], []
    for n, day in enumerate(FORMATIONS):
        cutoff = dt.datetime.combine(day, dt.time(22))
        period = Q3 if n < 2 else Q4
        deadline = dt.datetime.combine(period, dt.time()) + dt.timedelta(days=45, hours=46)
        linked = _linked_lines(day)
        for i in (i for i in range(N_ISSUERS) if _owner(i) in linked):
            if i % 7:
                pending = i % 7 == 1 and n > 0
                events.append(["ev1", day, _owner(i), _cik(i), "ear_m1p1", None if pending else 0.01 * (i % 13 - 6),
                               "event_value_pending" if pending else "valid",
                               None if pending else cutoff - dt.timedelta(days=5), period,
                               day - dt.timedelta(days=12), "8k_202"])
            if i % 11 != 4:
                reason = ("no_mapped_13f_holding" if i % 5 == 0
                          else "unverified_shares" if i % 5 == 1 and n > 0 else "valid")
                valid = reason == "valid"
                owners.append(["ow1", day, _owner(i), _cik(i), "io_ratio_13f",
                               (1.3 if i == 2 else 0.2 + 0.03 * (i % 17)) if valid else None, reason,
                               deadline if valid else None, period, deadline,
                               "io_above_one" if valid and i == 2 else None, "cusip_exact"])
        for line in sorted(linked | _unlinked_lines(day)):
            if line == _owner(3):
                continue
            estimated = n > 0
            exposures.append({"run_id": "fr1", "formation_date": day, "security_id": line,
                              "owner_basis": rf.OWNER_BASIS_LINKED if line in linked else rf.OWNER_BASIS_UNLINKED,
                              "beta_mkt_252d": 0.5 + (sum(map(ord, line)) % 50) / 50 if estimated else None,
                              "n_daily_obs": 240 if estimated else 120,
                              "status_252d": "estimated" if estimated else "insufficient_obs",
                              "status_36m": "no_factor_history",
                              "available_at": None if not estimated else cutoff + dt.timedelta(days=1)
                              if (n, line) == (2, "TBL-TAIL-007") else cutoff - dt.timedelta(days=1),
                              "basis": "reconstructed", "venue_basis": "not_applicable", "rf_basis": "none"})
    con.executemany("""
        INSERT INTO research_event_features (event_version, formation_date, security_id, owner_cik, feature_id,
            raw_value, reason, available_at, fiscal_period_end, event_session, announcement_basis)
        VALUES (?,?,?,?,?,?,?,?,?,?,?)""", events)
    con.executemany("""
        INSERT INTO research_ownership_features (ownership_version, formation_date, security_id, owner_cik,
            feature_id, raw_value, reason, available_at, source_period, source_clock, value_flag, identity_basis)
        VALUES (?,?,?,?,?,?,?,?,?,?,?,?)""", owners)
    con.executemany("""
        INSERT INTO research_ownership_identity (ownership_version, source, source_key, source_period, name_key,
            owner_cik, security_id, identity_basis, confidence, reason) VALUES (?,?,?,?,?,?,?,?,?,?)
    """, [["ow1", "13f", f"cusip{i}", period, None, _cik(i), _owner(i), "cusip_exact", "high",
           ownership_identity.MAPPED] for i in range(N_ISSUERS) for period in (Q3, Q4)])
    con.execute("""
        INSERT INTO research_event_versions (event_version, status, basis, panel_run_id, panel_sha256, query_version,
            spec_json, spec_sha256, code_sha256, inputs_json, blockers_json, events_sha256, features_sha256,
            created_at)
        VALUES ('ev1', 'sealed', 'reconstructed', 'panel_recon', ?, ?, '{}', 's', 'c', '{}', '["event_fixture"]',
                ?, ?, ?)
    """, [panel_sha, rev.QUERY_VERSION, rev._events_digest(con, "ev1"), rev._features_digest(con, "ev1"), created])
    con.execute("""
        INSERT INTO research_ownership_versions (ownership_version, status, basis, panel_run_id, panel_sha256,
            query_version, spec_json, spec_sha256, code_sha256, inputs_json, blockers_json, identity_sha256,
            features_sha256, created_at)
        VALUES ('ow1', 'sealed', 'reconstructed', 'panel_recon', ?, ?, '{}', 's', 'c', '{}',
                '["ownership_fixture"]', ?, ?, ?)
    """, [panel_sha, rof.QUERY_VERSION, rof._identity_digest(con, "ow1"), rof._features_digest(con, "ow1"),
          created])
    rfr._insert(con, "runs", [{
        "run_id": "fr1", "status": "complete", "basis": "reconstructed", "feature_version": parent_version,
        "panel_run_id": "panel_recon", "panel_sha256": panel_sha, "factor_version": rfr.FACTOR_VERSION,
        "rf_basis": "none", "label_source": "fixture", "label_cutoff": created, "spec_json": "{}",
        "spec_sha256": "s", "code_sha256": "c", "blockers_json": '["factor_fixture"]', "created_at": created}])
    rfr._insert(con, "exposures", exposures)
    con.execute("UPDATE research_factor_runs SET results_sha256=? WHERE run_id='fr1'",
                [rfr.results_digest(con, "fr1")[0]])


def test_research_store_sources_enter_point_in_time_and_count_every_member(built: Any) -> None:
    """E1: P3 event, P4 factor-exposure and P9 ownership features enter from pinned sealed versions. Per kind a
    value enters only from the same formation's source row, when its reason is valid and its own clock is
    visible at the cutoff; an invalid newest state is NULL with its reason (never January's value), missing
    rows are counted, IO above one is kept and flagged, and a source failing its own validator is refused."""
    from atx_db.research import events as rev

    store = built["store"]
    con = store.con
    panel_sha = con.execute("SELECT panel_sha256 FROM research_panel_runs WHERE run_id='panel_recon'").fetchone()[0]
    _seal_research_sources(con, panel_sha, built["result"].feature_version)
    # P3's validator binds its session calendar (there are no event rows to check against it here).
    con.execute("CREATE OR REPLACE TEMP TABLE trading_calendar "
                "(calendar_id VARCHAR, source VARCHAR, trade_date DATE, is_open BOOLEAN)")

    def source_row(feature_id: str, kind: str, code: str, window: str, anomaly_class: str, sign: str) -> dict:
        return {**_catalog_row(feature_id, anomaly_class, sign, "rank_normal", "unrestricted"),
                "source_kind": kind, "metric_code": code, "metric_window": window}

    entries = _catalog_entries(built["root"] / "catalog_e1.csv", extra=(
        source_row("ear_m1p1", "event", "ear_m1p1", "event", "growth", "+1"),
        source_row("beta_252d", "factor_exposure", "beta_mkt_252d", "252d", "volatility", "-1"),
        source_row("io_ratio_13f", "ownership", "io_ratio_13f", "13f_quarter", "quality", "+1")))
    unpinned = rf._plan_features(entries, rf._panel_context(store, "panel_recon", False),
                                 rf._validate_options(_options(entries)))
    assert [(p.status, p.reason) for p in unpinned if p.feature_id == "ear_m1p1"] == [
        (rf.FEATURE_SOURCE_NOT_PINNED, "event_version:none")]
    options = _options(entries, features=("beta_252d", "ear_m1p1", "io_ratio_13f"), event_version="ev1",
                       factor_run_id="fr1", ownership_version="ow1")
    result = rf.build_feature_version(store, options)
    assert result.status == rf.STATUS_SEALED
    rf.validate_feature_version(store, result.feature_version)   # status counts sum to the members, flags aside
    assert {"event:event_fixture", "factor:factor_fixture", "ownership:ownership_fixture"} <= set(result.blockers)
    inputs = json.loads(con.execute("SELECT inputs_json FROM research_feature_versions WHERE feature_version=?",
                                    [result.feature_version]).fetchone()[0])
    assert {kind: source["version"] for kind, source in inputs["external_sources"].items()} == {
        "event": "ev1", "factor_exposure": "fr1", "ownership": "ow1"}

    def value(feature: str, day: dt.date, line: str) -> tuple | None:
        return con.execute("""
            SELECT signed_raw, available_at, age_days, owner_basis FROM research_feature_matrix
            WHERE feature_version=? AND feature_id=? AND formation_date=? AND security_id=?
        """, [result.feature_version, feature, day, line]).fetchone()

    def reasons(feature: str, day: dt.date) -> dict[str, int]:
        return json.loads(con.execute("""
            SELECT reasons_json FROM research_feature_dates
            WHERE feature_version=? AND feature_id=? AND formation_date=? AND variant='signed_raw'
        """, [result.feature_version, feature, day]).fetchone()[0])

    jan, feb, mar = FORMATIONS[:3]
    issuers = range(N_ISSUERS)
    # Events: the value's own clock and age; after January issuer 1's newest state is pending (NULL).
    assert value("ear_m1p1", jan, _owner(1)) == (0.01 * (1 % 13 - 6), dt.datetime.combine(jan, dt.time(22))
                                                 - dt.timedelta(days=5), 12, rf.OWNER_BASIS_LINKED)
    assert value("ear_m1p1", feb, _owner(1)) is None
    counts = reasons("ear_m1p1", feb)
    assert counts["event:event_value_pending"] == sum(1 for i in issuers if i % 7 == 1)
    assert counts["event:missing_source_row"] == sum(1 for i in issuers if i % 7 == 0)
    assert counts["cohort:missing_owner_link"] == TAIL
    # Factor exposures rank unlinked lines; burn-in, missing and late rows are NULL and counted.
    counts = reasons("beta_252d", jan)
    assert counts["factor_exposure:insufficient_obs"] == len(_linked_lines(jan) | _unlinked_lines(jan)) - 1
    assert counts["factor_exposure:missing_source_row"] == 1
    assert value("beta_252d", mar, "TBL-TAIL-007") is None
    assert reasons("beta_252d", mar)["factor_exposure:not_visible_at_cutoff"] == 1
    tail = value("beta_252d", feb, "TBL-TAIL-008")
    assert tail[0] == -(0.5 + (sum(map(ord, "TBL-TAIL-008")) % 50) / 50) and tail[3] == rf.OWNER_BASIS_UNLINKED
    # Ownership: IO above one is kept and flagged; issuer 1's newest state is unverified (NULL) after January.
    assert value("io_ratio_13f", jan, _owner(2))[:3] == (1.3, dt.datetime.combine(Q3, dt.time())
                                                         + dt.timedelta(days=45, hours=46), (jan - Q3).days)
    assert [reasons("io_ratio_13f", day).get("flag:io_above_one") for day in FORMATIONS] == [1] * len(FORMATIONS)
    assert value("io_ratio_13f", jan, _owner(1)) is not None and value("io_ratio_13f", feb, _owner(1)) is None
    counts = reasons("io_ratio_13f", feb)
    assert counts["ownership:unverified_shares"] == sum(1 for i in issuers if i % 5 == 1 and i % 11 != 4)
    assert counts["ownership:missing_source_row"] == sum(1 for i in issuers if i % 11 == 4)
    # A source row visible after its cutoff fails the source's own validator: the build is refused.
    con.execute("UPDATE research_event_features SET available_at = available_at + INTERVAL 10 DAY "
                "WHERE event_version='ev1' AND formation_date=? AND security_id=?", [jan, _owner(2)])
    with pytest.raises(rf.FeatureStoreError, match="fails its validator"):
        rf.build_feature_version(store, options)
    con.execute("UPDATE research_event_features SET available_at = available_at - INTERVAL 10 DAY "
                "WHERE event_version='ev1' AND formation_date=? AND security_id=?", [jan, _owner(2)])
    assert rev.validate_event_version(store, "ev1")
