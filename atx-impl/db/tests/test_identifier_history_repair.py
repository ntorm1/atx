from __future__ import annotations

import datetime as dt

import pandas as pd


def _identifier_row(*, security_id, id_type, id_value, valid_from, available_at, valid_to=None, source="SEC ownership XML issuer seed"):
    return {
        "security_id": security_id,
        "id_type": id_type,
        "id_value": id_value,
        "valid_from": valid_from,
        "valid_to": valid_to,
        "as_of_date": valid_from,
        "available_at": available_at,
        "source": source,
        "run_id": None,
    }


def test_dedupe_open_identifier_intervals_keeps_earliest():
    from db.security_master import dedupe_open_identifier_intervals

    frame = pd.DataFrame(
        [
            # Same identifier re-observed across three filings (open-ended each).
            _identifier_row(security_id="SEC-CIK-1", id_type="CIK", id_value="1", valid_from=dt.date(2026, 4, 1), available_at=dt.datetime(2026, 4, 3)),
            _identifier_row(security_id="SEC-CIK-1", id_type="CIK", id_value="1", valid_from=dt.date(2026, 2, 1), available_at=dt.datetime(2026, 2, 3)),
            _identifier_row(security_id="SEC-CIK-1", id_type="CIK", id_value="1", valid_from=dt.date(2026, 2, 1), available_at=dt.datetime(2026, 2, 4)),
            # A different identifier survives independently.
            _identifier_row(security_id="SEC-CIK-1", id_type="TICKER", id_value="AAA", valid_from=dt.date(2026, 3, 1), available_at=dt.datetime(2026, 3, 2)),
        ]
    )
    out = dedupe_open_identifier_intervals(frame)
    cik = out[(out["id_type"] == "CIK")]
    assert len(cik) == 1
    assert cik.iloc[0]["valid_from"] == dt.date(2026, 2, 1)
    # Tie on valid_from -> keep the earliest availability (first disclosure).
    assert cik.iloc[0]["available_at"] == dt.datetime(2026, 2, 3)
    assert len(out[out["id_type"] == "TICKER"]) == 1


def test_dedupe_preserves_closed_intervals():
    from db.security_master import dedupe_open_identifier_intervals

    frame = pd.DataFrame(
        [
            _identifier_row(security_id="SEC-CIK-2", id_type="TICKER", id_value="OLD", valid_from=dt.date(2020, 1, 1), valid_to=dt.date(2022, 1, 1), available_at=dt.datetime(2020, 1, 2)),
            _identifier_row(security_id="SEC-CIK-2", id_type="TICKER", id_value="NEW", valid_from=dt.date(2022, 1, 1), available_at=dt.datetime(2022, 1, 2)),
        ]
    )
    out = dedupe_open_identifier_intervals(frame)
    # A genuine ticker change (closed old + open new) must be left intact.
    assert len(out) == 2


def test_collapse_identifier_history_open_duplicates_clears_overlaps(tmp_store):
    from db.quality import run_warehouse_quality_checks
    from db.security_master import collapse_identifier_history_open_duplicates

    rows = [
        _identifier_row(security_id="SEC-CIK-0000320193", id_type="CIK", id_value="0000320193", valid_from=dt.date(2026, 2, 1), available_at=dt.datetime(2026, 2, 3)),
        _identifier_row(security_id="SEC-CIK-0000320193", id_type="CIK", id_value="0000320193", valid_from=dt.date(2026, 2, 24), available_at=dt.datetime(2026, 2, 26)),
        _identifier_row(security_id="SEC-CIK-0000320193", id_type="CIK", id_value="0000320193", valid_from=dt.date(2026, 3, 1), available_at=dt.datetime(2026, 3, 6)),
        _identifier_row(security_id="SEC-CIK-0000320193", id_type="TICKER", id_value="AAPL", valid_from=dt.date(2026, 2, 1), available_at=dt.datetime(2026, 2, 3)),
        _identifier_row(security_id="SEC-CIK-0000320193", id_type="TICKER", id_value="AAPL", valid_from=dt.date(2026, 4, 1), available_at=dt.datetime(2026, 4, 3)),
    ]
    frame = pd.DataFrame(rows)
    tmp_store.con.register("seed_ids", frame)
    tmp_store.con.execute(
        """
        INSERT INTO security_identifier_history
            (security_id, id_type, id_value, valid_from, valid_to, as_of_date, available_at, source, run_id)
        SELECT security_id, id_type, id_value, valid_from, valid_to, as_of_date, available_at, source, run_id
        FROM seed_ids
        """
    )
    tmp_store.con.unregister("seed_ids")

    # Sanity: the seeded data reproduces the standing-failure shape.
    overlaps_before = tmp_store.con.execute(
        """
        SELECT count(*) FROM security_identifier_history a JOIN security_identifier_history b
          ON a.security_id=b.security_id AND a.id_type=b.id_type AND a.id_value=b.id_value AND a.source=b.source
         AND a.valid_from < b.valid_from
         AND a.valid_from < coalesce(b.valid_to, DATE '9999-12-31')
         AND b.valid_from < coalesce(a.valid_to, DATE '9999-12-31')
        """
    ).fetchone()[0]
    assert overlaps_before > 0

    removed = collapse_identifier_history_open_duplicates(tmp_store.con)
    assert removed == 3  # two redundant CIK + one redundant TICKER

    kept = tmp_store.con.execute(
        "SELECT id_type, valid_from FROM security_identifier_history ORDER BY id_type"
    ).fetchall()
    assert kept == [("CIK", dt.date(2026, 2, 1)), ("TICKER", dt.date(2026, 2, 1))]

    results = run_warehouse_quality_checks(
        tmp_store,
        record=False,
        check_names=("identifier_same_source_self_overlaps", "duplicate_identifier_history_keys"),
    )
    by_name = {getattr(r, "check_name", None): r for r in results}
    for name in ("identifier_same_source_self_overlaps", "duplicate_identifier_history_keys"):
        r = by_name[name]
        status = getattr(r, "status", None)
        observed = getattr(r, "observed_value", None)
        assert status == "passed", f"{name} observed={observed}"


def _form4_issuer_xml(*, owner_cik: str, transaction_date: str) -> str:
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<ownershipDocument>
  <schemaVersion>X0508</schemaVersion>
  <documentType>4</documentType>
  <periodOfReport>{transaction_date}</periodOfReport>
  <issuer>
    <issuerCik>0000320193</issuerCik>
    <issuerName>Apple Inc.</issuerName>
    <issuerTradingSymbol>AAPL</issuerTradingSymbol>
  </issuer>
  <reportingOwner>
    <reportingOwnerId>
      <rptOwnerCik>{owner_cik}</rptOwnerCik>
      <rptOwnerName>Jane Q. Insider</rptOwnerName>
    </reportingOwnerId>
    <reportingOwnerRelationship>
      <isDirector>1</isDirector>
      <isOfficer>1</isOfficer>
      <isTenPercentOwner>0</isTenPercentOwner>
      <officerTitle>CFO</officerTitle>
    </reportingOwnerRelationship>
  </reportingOwner>
  <nonDerivativeTable>
    <nonDerivativeTransaction>
      <securityTitle><value>Common Stock</value></securityTitle>
      <transactionDate><value>{transaction_date}</value></transactionDate>
      <transactionCoding>
        <transactionFormType>4</transactionFormType>
        <transactionCode>P</transactionCode>
        <equitySwapInvolved>0</equitySwapInvolved>
      </transactionCoding>
      <transactionAmounts>
        <transactionShares><value>100</value></transactionShares>
        <transactionPricePerShare><value>100</value></transactionPricePerShare>
        <transactionAcquiredDisposedCode><value>A</value></transactionAcquiredDisposedCode>
      </transactionAmounts>
      <postTransactionAmounts>
        <sharesOwnedFollowingTransaction><value>1000</value></sharesOwnedFollowingTransaction>
      </postTransactionAmounts>
      <ownershipNature>
        <directOrIndirectOwnership><value>D</value></directOrIndirectOwnership>
      </ownershipNature>
    </nonDerivativeTransaction>
  </nonDerivativeTable>
</ownershipDocument>
"""


def _load_form4(store, tmp_path, name, xml, *, accession, accepted_at):
    from db.insider_ownership import InsiderOwnershipDataset, InsiderOwnershipOptions

    path = tmp_path / name
    path.write_text(xml, encoding="utf-8")
    return InsiderOwnershipDataset().run(
        store,
        InsiderOwnershipOptions(
            source="fixture-insider",
            source_files=(path,),
            metadata_by_source={
                str(path): {
                    "accession_number": accession,
                    "form": "4",
                    "filing_date": accepted_at[:10],
                    "acceptance_datetime": accepted_at,
                }
            },
        ),
    )


def test_issuer_seed_identifier_history_idempotent(tmp_store, tmp_path):
    # Two Form 4 filings for the same issuer on different dates must NOT create
    # two overlapping open-ended CIK/TICKER intervals.
    _load_form4(
        tmp_store,
        tmp_path,
        "f1.xml",
        _form4_issuer_xml(owner_cik="0001214156", transaction_date="2026-02-01"),
        accession="0001214156-26-000001",
        accepted_at="2026-02-03T16:10:00Z",
    )
    _load_form4(
        tmp_store,
        tmp_path,
        "f2.xml",
        _form4_issuer_xml(owner_cik="0001214156", transaction_date="2026-04-01"),
        accession="0001214156-26-000002",
        accepted_at="2026-04-03T16:10:00Z",
    )

    counts = dict(
        tmp_store.con.execute(
            """
            SELECT id_type, count(*)
            FROM security_identifier_history
            WHERE security_id = 'SEC-CIK-0000320193'
              AND source = 'SEC ownership XML issuer seed'
            GROUP BY 1
            """
        ).fetchall()
    )
    assert counts.get("CIK") == 1
    assert counts.get("TICKER") == 1

    earliest = tmp_store.con.execute(
        """
        SELECT min(valid_from) FROM security_identifier_history
        WHERE security_id = 'SEC-CIK-0000320193' AND id_type = 'CIK'
          AND source = 'SEC ownership XML issuer seed'
        """
    ).fetchone()[0]
    assert earliest == dt.date(2026, 2, 1)
