"""Focused PIT boundaries for the shared Parquet accounting engine."""
import datetime as dt
import pytest

from atx_db.research.item_vintages import build_owner, corroborate, _book_equity
from atx_db.research.items_map import FSDS_ENDPOINT_AUTHORITY


def fact(identifier, *, end, start=None, qtrs=1, quarter="Q1", clock="2020-05-01", value="50", source="cf"):
    end = dt.date.fromisoformat(end)
    instant = dt.datetime.fromisoformat(clock)
    return dict(candidate_id=identifier, cik="0000000001", accession=identifier,
                taxonomy="us-gaap", taxonomy_version="us-gaap/2020", concept="Revenues", unit="USD",
                period_start=dt.date.fromisoformat(start) if start else None, period_end=end, qtrs=qtrs,
                reported_fp=quarter, reported_fy=2020, filing_period=end,
                source_status="candidate", source_kind=source, clock_basis="cf_fc1_reconstructed",
                available_at=instant, filed_date=instant.date(), value_exact=value,
                item_id=1001, unit_type="monetary", period_kind="duration", multiplier=1,
                alias_valid_from="1900-01-01", alias_valid_to=None, rule_valid_from="1900-01-01",
                rule_valid_to=None, sign_rule="statement_normalized", scale_rule="identity",
                priority=10, rule_id="sale", source_occurrences=1)


MAPPING = {"rules": [], "chains": {"SALE": {"item_ids": [1001], "magnitude": False, "additive": True}}}


def public(rows, freq, end):
    vintages, dispositions = build_owner(rows, MAPPING)
    assert len(dispositions) == len({r["candidate_id"] for r in rows})
    return [r for r in vintages if r["is_public"] and r["item"] == "SALE" and r["freq"] == freq
            and r["period_end"] == dt.date.fromisoformat(end)]


def test_ytd_quarter_recomputes_on_restated_and_null_operand():
    rows = [fact("q1", end="2020-03-31", start="2020-01-01"),
            fact("h1", end="2020-06-30", start="2020-01-01", qtrs=2, quarter="Q2", clock="2020-08-01", value="110"),
            fact("q1r", end="2020-03-31", start="2020-01-01", clock="2020-09-01", value="60"),
            fact("q1null", end="2020-03-31", start="2020-01-01", clock="2020-10-01", value=None)]
    result = public(rows, "q", "2020-06-30")
    assert [(r["available_at"].month, r["value"]) for r in result] == [(8, 60), (9, 50), (10, None)]
    assert result[-1]["status"] == "invalid_operand"
    assert all(a["valid_until"] == b["available_at"] for a, b in zip(result, result[1:]))


def test_equal_precedence_conflict_is_null_before_provenance_tiebreak():
    first = fact("a", end="2020-03-31", start="2020-01-01")
    other = {**first, "candidate_id": "z", "value_exact": "51"}
    result = public([first, other], "q", "2020-03-31")
    assert len(result) == 1
    assert result[0]["value"] is None
    assert result[0]["status"] == "semantic_conflict"


def test_exact_fy_stands_alone_and_weighted_shares_are_not_annual_divided():
    rows = [fact("fy", end="2020-12-31", start="2020-01-01", qtrs=4, quarter="FY", clock="2021-02-01", value="240")]
    result = public(rows, "ttm", "2020-12-31")
    assert len(result) == 1 and result[0]["value"] == 240
    mapping = {"rules": [], "chains": {"WAB": {"item_ids": [1001], "magnitude": False, "additive": False}}}
    vintages, _ = build_owner(rows, mapping)
    assert not any(r["is_public"] and r["freq"] in {"q", "ttm"} for r in vintages)


def test_cf_acceptance_borrowing_requires_exact_fact_and_unique_duration_start():
    fsds = fact("fsds", end="2020-03-31", source="fsds", clock="2020-04-30T21:00:00")
    cf = fact("cf", end="2020-03-31", start="2020-01-01", clock="2020-05-02T22:00:00")
    cf["accession"] = fsds["accession"]
    mismatch = {**cf, "candidate_id": "different", "value_exact": "51"}
    corroborate([fsds, cf, mismatch])
    assert cf["available_at"] == fsds["available_at"]
    assert cf["clock_basis"] == "fsds_same_fact_corroborated"
    assert mismatch["available_at"] == dt.datetime(2020, 5, 2, 22)
    fsds2 = {**fsds, "candidate_id": "f2", "period_start": None}
    cf2 = {**cf, "candidate_id": "c2", "available_at": dt.datetime(2020, 5, 2, 22)}
    ambiguous = {**cf2, "candidate_id": "c3", "period_start": dt.date(2020, 1, 2)}
    corroborate([fsds2, cf2, ambiguous])
    assert cf2["available_at"] == dt.datetime(2020, 5, 2, 22)


def test_book_equity_rejected_stock_period_is_not_ordinary_absence():
    stock = fact("stock", end="2020-03-31", qtrs=0, value="100")
    stock.update(item_id=1221, concept="StockholdersEquity", period_kind="instant")
    tax = fact("tax", end="2020-03-31", qtrs=0, clock="2020-06-01", value="5")
    tax.update(item_id=1211, concept="DeferredIncomeTaxLiabilitiesNet", period_kind="instant")
    rejected = {**tax, "candidate_id": "taxbad", "accession": "taxbad", "qtrs": 1,
                "available_at": dt.datetime(2020, 7, 1), "filed_date": dt.date(2020, 7, 1)}
    mapping = {"rules": [{"item_id": -100004, "basis": "instant", "rule_id": "BE",
                          "source_item_ids": [1221, 1220, 1214, 1101, 1201, 1211],
                          "input_kinds": ["item"]*6, "combination_rule": "book_equity_jkp"}],
               "chains": {"BE": {"item_ids": [-100004], "magnitude": False, "additive": True}}}
    vintages, _ = build_owner([stock, tax, rejected], mapping)
    actual = [r for r in vintages if r["is_public"] and r["item"] == "BE"]
    assert [(r["available_at"].month, r["value"]) for r in actual] == [(5, 100), (6, 105), (7, None)]
    assert "TXDITC_ordinary_absence_zero" in actual[0]["missing_value_basis"]
    assert actual[-1]["status"] == "invalid_operand"


def test_book_equity_explicit_common_null_blocks_assets_liabilities_fallback():
    value = lambda number, status="valid": {"value_exact": number, "status": status}
    common = value(None, "invalid_value")
    result = _book_equity([None, common, None, value("150"), value("50"), None])
    assert result == (None, "invalid_operand")


@pytest.mark.parametrize("source_clock", ["2020-07-15", "2020-11-15"])
def test_fiscal_trigger_lineage_excludes_later_tied_contradiction(source_clock):
    # Same economic quarter/tie clock, but only the inferred-start fact depends
    # on a predecessor whose contradiction becomes visible before the end-point's.
    q1 = fact("q1_context", end="2020-03-31", qtrs=0, source="fsds", clock="2020-05-01")
    q2 = fact("q2_context", end="2020-06-30", qtrs=0, quarter="Q2", source="fsds", clock="2020-07-01")
    q1_bad = {**q1, "candidate_id": "q1_conflict", "accession": "q1_conflict", "reported_fy": 2021,
              "available_at": dt.datetime(2020, 11, 1), "filed_date": dt.date(2020, 11, 1)}
    q2_bad = {**q2, "candidate_id": "q2_conflict", "accession": "q2_conflict", "reported_fy": 2021,
              "available_at": dt.datetime(2020, 12, 1), "filed_date": dt.date(2020, 12, 1)}
    contexts = [q1, q2, q1_bad, q2_bad]
    for row in contexts:
        row["item_id"] = None
    direct = fact("direct", end="2020-06-30", start="2020-04-01", quarter="Q2", clock=source_clock)
    inferred = fact("inferred", end="2020-06-30", quarter="Q2", source="fsds", clock=source_clock, value=None)
    direct["accession"] = inferred["accession"] = "same_filing"
    rows = contexts+[direct, inferred]
    clocks = {r["candidate_id"]: r["available_at"] for r in rows}
    vintages, _ = build_owner(rows, MAPPING)
    clocks.update({v["vintage_id"]: v["available_at"] for v in vintages})
    source_events = [v for v in vintages if v["event_stage"] == "source" and v["freq"] == "q"]
    expected_clock = max(dt.datetime.fromisoformat(source_clock), dt.datetime(2020, 11, 1))
    conflicted = [v for v in source_events if v["available_at"] == expected_clock]
    assert len(conflicted) == 1 and conflicted[0]["status"] == "fiscal_grid_conflict"
    assert "q1_conflict" in conflicted[0]["inputs"]
    assert all("q2_conflict" not in v["inputs"] for v in vintages if v["available_at"] < dt.datetime(2020, 12, 1))
    assert all(clocks[i] <= v["available_at"] for v in vintages for i in v["inputs"])


def encoded_pair(fsds_clock="2020-05-05", cf_clock="2020-05-06"):
    fsds = fact("rounded", end="2020-04-30", source="fsds", clock=fsds_clock)
    cf = fact("precise", end="2020-05-03", start="2020-02-03", clock=cf_clock)
    fsds["source_slice"] = "2020q2"
    fsds["archive_sha256"] = FSDS_ENDPOINT_AUTHORITY["manifest_sha256"]
    fsds["accession"] = cf["accession"] = "joint"
    return fsds, cf


@pytest.mark.parametrize("fsds_clock,cf_clock", [("2020-05-05", "2020-05-06"), ("2020-05-07", "2020-05-06")])
def test_encoded_endpoint_uses_joint_original_clock_and_both_inputs(fsds_clock, cf_clock):
    rows = list(encoded_pair(fsds_clock, cf_clock))
    originals = {r["candidate_id"]: (r["period_end"], r["available_at"], r["reported_fy"], r["reported_fp"]) for r in rows}
    vintages, dispositions = build_owner(rows, {**MAPPING, "fsds_endpoint_authority": FSDS_ENDPOINT_AUTHORITY})
    clock = max(dt.datetime.fromisoformat(fsds_clock), dt.datetime.fromisoformat(cf_clock))
    actual = [v for v in vintages if v["is_public"] and v["item"] == "SALE" and v["freq"] == "q"]
    assert len(actual) == 1 and actual[0]["period_end"] == dt.date(2020, 5, 3)
    assert actual[0]["value"] == 50 and actual[0]["available_at"] == clock
    source = [v for v in vintages if v["event_stage"] == "source"]
    assert source and all(set(v["inputs"]) == {"rounded", "precise"} for v in source)
    assert all(d["original_effective_at"] == originals[d["candidate_id"]][1] and
               d["raw_period_end"] == originals[d["candidate_id"]][0] and d["effective_at"] == clock for d in dispositions)
    assert all((r["reported_fy"], r["reported_fp"]) == originals[r["candidate_id"]][2:] for r in rows)


@pytest.mark.parametrize("case", ["two_starts", "two_ends", "midpoint", "missing", "null", "unit", "unknown_quarter"])
def test_encoded_endpoint_ambiguities_and_absences_refuse_recovery(case):
    fsds, cf = encoded_pair()
    rows = [fsds, cf]
    expected = "ambiguous_joint_context"
    if case == "two_starts":
        rows.append({**cf, "candidate_id": "other_start", "period_start": dt.date(2020, 2, 2)})
    elif case == "two_ends":
        rows.append({**cf, "candidate_id": "other_end", "period_end": dt.date(2020, 5, 2)})
    elif case == "midpoint":
        fsds["period_end"] = fsds["filing_period"] = dt.date(2020, 3, 31)
        cf.update(period_end=dt.date(2020, 4, 15), period_start=dt.date(2020, 1, 15))
        expected = "encoding_midpoint_tie"
    elif case == "missing":
        rows.remove(cf)
        expected = "no_joint_encoded_counterpart"
    elif case == "null":
        fsds["value_exact"] = None
        expected = "encoding_invalid_numeric"
    elif case == "unit":
        fsds["unit"] = "EUR"
        expected = "encoding_ineligible_context"
    elif case == "unknown_quarter":
        fsds["source_slice"] = "2027q1"
        expected = "unverified_encoding_authority"
    original = fsds["period_end"], fsds["available_at"]
    corroborate(rows, endpoint_authority=FSDS_ENDPOINT_AUTHORITY)
    assert (fsds["period_end"], fsds["available_at"]) == original
    assert fsds["endpoint_encoding"] == expected


def test_encoded_policy_keeps_calendar_exact_borrowing_and_raw_fiscal_conflicts():
    fsds, cf = encoded_pair()
    fsds["period_end"] = fsds["filing_period"] = cf["period_end"] = cf["filing_period"] = dt.date(2020, 3, 31)
    cf["period_start"] = dt.date(2020, 1, 1)
    corroborate([fsds, cf], endpoint_authority=FSDS_ENDPOINT_AUTHORITY)
    assert cf["available_at"] == dt.datetime(2020, 5, 5)
    assert cf["endpoint_encoding"] == "exact_same_fact"
    fsds, cf = encoded_pair()
    cf["reported_fy"] = 2021  # Precision does not authorize changing either label.
    vintages, _ = build_owner([fsds, cf], {**MAPPING, "fsds_endpoint_authority": FSDS_ENDPOINT_AUTHORITY})
    assert fsds["reported_fy"] == 2020 and cf["reported_fy"] == 2021
    assert all(v["value"] is None for v in vintages if v["is_public"])
    assert any(v["status"] == "fiscal_grid_conflict" for v in vintages)


@pytest.mark.parametrize("rejected", [False, True])
def test_encoded_filing_context_preserves_latest_null_and_rejected_stock(rejected):
    fsds, cf = encoded_pair("2020-05-07", "2020-05-08")
    for row in (fsds, cf):
        row.update(qtrs=0, period_start=None, period_kind="instant", concept="StockholdersEquity", item_id=1221, value_exact="100")
    prior = fact("prior_tax", end="2020-05-03", qtrs=0, value="5", clock="2020-05-04")
    prior.update(period_kind="instant", item_id=1211, concept="DeferredIncomeTaxLiabilitiesNet")
    prior_equity = {**prior, "candidate_id": "prior_equity", "item_id": 1221,
                    "concept": "StockholdersEquity", "value_exact": "100"}
    latest = {**fsds, "candidate_id": "latest_tax", "item_id": 1211, "concept": prior["concept"],
              "value_exact": "6" if rejected else None, "available_at": dt.datetime(2020, 5, 7),
              "filed_date": dt.date(2020, 5, 7), "source_status": "unsupported_duration" if rejected else "candidate"}
    mapping = {"fsds_endpoint_authority": FSDS_ENDPOINT_AUTHORITY,
        "rules": [{"item_id": -100004, "basis": "instant", "rule_id": "BE", "source_item_ids": [1221, 1220, 1214, 1101, 1201, 1211],
                   "input_kinds": ["item"]*6, "combination_rule": "book_equity_jkp"}],
        "chains": {"BE": {"item_ids": [-100004], "magnitude": False, "additive": True}}}
    vintages, dispositions = build_owner([fsds, cf, prior, prior_equity, latest], mapping)
    actual = sorted([v for v in vintages if v["is_public"] and v["item"] == "BE"], key=lambda v:v["available_at"])
    assert actual[-2]["value"] == 105 and actual[-1]["value"] is None
    assert actual[-1]["available_at"] == dt.datetime(2020, 5, 8)
    assert actual[-1]["status"] == "invalid_operand"
    disposition = next(d for d in dispositions if d["candidate_id"] == "latest_tax")
    assert disposition["endpoint_encoding"] == "joint_filing_context"
    assert disposition["raw_period_end"] == dt.date(2020, 4, 30)
    assert disposition["economic_period_end"] == dt.date(2020, 5, 3)
