"""Focused PIT boundaries for the shared Parquet accounting engine."""
import datetime as dt

from atx_db.research.item_vintages import build_owner, corroborate


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
