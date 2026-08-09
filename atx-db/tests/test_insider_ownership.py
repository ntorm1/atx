from __future__ import annotations

import datetime as dt


OWNERSHIP_XML = """<?xml version="1.0" encoding="UTF-8"?>
<ownershipDocument>
  <schemaVersion>X0508</schemaVersion>
  <documentType>4</documentType>
  <periodOfReport>2024-05-01</periodOfReport>
  <issuer>
    <issuerCik>0000320193</issuerCik>
    <issuerName>Apple Inc.</issuerName>
    <issuerTradingSymbol>AAPL</issuerTradingSymbol>
  </issuer>
  <reportingOwner>
    <reportingOwnerId>
      <rptOwnerCik>0001214156</rptOwnerCik>
      <rptOwnerName>Jane Q. Insider</rptOwnerName>
    </reportingOwnerId>
    <reportingOwnerRelationship>
      <isDirector>1</isDirector>
      <isOfficer>1</isOfficer>
      <isTenPercentOwner>0</isTenPercentOwner>
      <isOther>0</isOther>
      <officerTitle>Chief Financial Officer</officerTitle>
    </reportingOwnerRelationship>
  </reportingOwner>
  <nonDerivativeTable>
    <nonDerivativeTransaction>
      <securityTitle><value>Common Stock</value></securityTitle>
      <transactionDate><value>2024-05-01</value></transactionDate>
      <transactionCoding>
        <transactionFormType>4</transactionFormType>
        <transactionCode>P</transactionCode>
        <equitySwapInvolved>0</equitySwapInvolved>
      </transactionCoding>
      <transactionAmounts>
        <transactionShares><value>100</value></transactionShares>
        <transactionPricePerShare><value>10.50</value></transactionPricePerShare>
        <transactionAcquiredDisposedCode><value>A</value></transactionAcquiredDisposedCode>
      </transactionAmounts>
      <postTransactionAmounts>
        <sharesOwnedFollowingTransaction><value>1000</value></sharesOwnedFollowingTransaction>
      </postTransactionAmounts>
      <ownershipNature>
        <directOrIndirectOwnership><value>D</value></directOrIndirectOwnership>
      </ownershipNature>
    </nonDerivativeTransaction>
    <nonDerivativeTransaction>
      <securityTitle><value>Common Stock</value></securityTitle>
      <transactionDate><value>2024-05-01</value></transactionDate>
      <transactionCoding>
        <transactionFormType>4</transactionFormType>
        <transactionCode>S</transactionCode>
        <equitySwapInvolved>0</equitySwapInvolved>
        <rule10b5-1Indicator>1</rule10b5-1Indicator>
        <plan10b5-1AdoptionDate><value>2024-01-01</value></plan10b5-1AdoptionDate>
      </transactionCoding>
      <transactionAmounts>
        <transactionShares><value>50</value></transactionShares>
        <transactionPricePerShare><value>12.25</value></transactionPricePerShare>
        <transactionAcquiredDisposedCode><value>D</value></transactionAcquiredDisposedCode>
      </transactionAmounts>
      <postTransactionAmounts>
        <sharesOwnedFollowingTransaction><value>950</value></sharesOwnedFollowingTransaction>
      </postTransactionAmounts>
      <ownershipNature>
        <directOrIndirectOwnership><value>D</value></directOrIndirectOwnership>
      </ownershipNature>
    </nonDerivativeTransaction>
    <nonDerivativeHolding>
      <securityTitle><value>Common Stock</value></securityTitle>
      <postTransactionAmounts>
        <sharesOwnedFollowingTransaction><value>950</value></sharesOwnedFollowingTransaction>
      </postTransactionAmounts>
      <ownershipNature>
        <directOrIndirectOwnership><value>D</value></directOrIndirectOwnership>
      </ownershipNature>
    </nonDerivativeHolding>
  </nonDerivativeTable>
  <footnotes>
    <footnote id="F1">Open market transaction.</footnote>
  </footnotes>
</ownershipDocument>
"""


BLOCKHOLDER_XML = """<?xml version="1.0" encoding="UTF-8"?>
<schedule13DDocument>
  <documentType>SC 13D</documentType>
  <issuer>
    <issuerCik>0000320193</issuerCik>
    <issuerName>Apple Inc.</issuerName>
    <cusip>037833100</cusip>
  </issuer>
  <dateOfEvent>2025-01-15</dateOfEvent>
  <filingDate>2025-01-21</filingDate>
  <isGroupFiling>false</isGroupFiling>
  <purposeOfTransaction>Board engagement and capital allocation discussions.</purposeOfTransaction>
  <reportingPersons>
    <reportingPerson>
      <reportingPersonName>Example Activist LP</reportingPersonName>
      <typeOfReportingPerson>IA</typeOfReportingPerson>
      <citizenshipOrPlaceOfOrganization>DE</citizenshipOrPlaceOfOrganization>
      <soleVotingPower>1000</soleVotingPower>
      <sharedVotingPower>200</sharedVotingPower>
      <soleDispositivePower>1000</soleDispositivePower>
      <sharedDispositivePower>200</sharedDispositivePower>
      <aggregateBeneficiallyOwned>1200</aggregateBeneficiallyOwned>
      <percentOfClass>5.2</percentOfClass>
    </reportingPerson>
  </reportingPersons>
</schedule13DDocument>
"""


def _write_xml(tmp_path):
    path = tmp_path / "0001214156-24-000001.xml"
    path.write_text(OWNERSHIP_XML, encoding="utf-8")
    return path


def _write_blockholder_xml(tmp_path):
    path = tmp_path / "0001214156-25-000013.xml"
    path.write_text(BLOCKHOLDER_XML, encoding="utf-8")
    return path


def _load_sample(tmp_store, tmp_path):
    from atx_db.insider_ownership import InsiderOwnershipDataset, InsiderOwnershipOptions

    xml_path = _write_xml(tmp_path)
    return InsiderOwnershipDataset().run(
        tmp_store,
        InsiderOwnershipOptions(
            source_files=(xml_path,),
            metadata_by_source={
                str(xml_path): {
                    "accession_number": "0001214156-24-000001",
                    "form": "4",
                    "filing_date": "2024-05-03",
                    "acceptance_datetime": "2024-05-03T16:10:00Z",
                }
            },
        ),
    )


def _load_blockholder_sample(tmp_store, tmp_path):
    from atx_db.insider_ownership import BlockholderOwnershipDataset, BlockholderOwnershipOptions

    xml_path = _write_blockholder_xml(tmp_path)
    return BlockholderOwnershipDataset().run(
        tmp_store,
        BlockholderOwnershipOptions(
            source_files=(xml_path,),
            metadata_by_source={
                str(xml_path): {
                    "accession_number": "0001214156-25-000013",
                    "form": "SC 13D",
                    "filing_date": "2025-01-21",
                    "acceptance_datetime": "2025-01-21T17:31:00Z",
                }
            },
        ),
    )


def test_migration_version_6_present(tmp_store):
    applied = {
        int(row[0])
        for row in tmp_store.con.execute(
            "SELECT CAST(version AS INTEGER) FROM schema_migrations WHERE version ~ '^[0-9]+$'"
        ).fetchall()
    }
    assert 6 in applied


def test_insider_ownership_loader_populates_core_tables(tmp_store, tmp_path):
    result = _load_sample(tmp_store, tmp_path)
    assert result.dataset_id == "sec_insider_ownership"
    assert result.rows_loaded == 3

    counts = dict(
        tmp_store.con.execute(
            """
            SELECT 'insider', count(*) FROM insider
            UNION ALL SELECT 'filing_form4', count(*) FROM filing_form4
            UNION ALL SELECT 'insider_relationship', count(*) FROM insider_relationship
            UNION ALL SELECT 'insider_transaction', count(*) FROM insider_transaction
            UNION ALL SELECT 'insider_holding', count(*) FROM insider_holding
            UNION ALL SELECT 'tradingplan_10b5_1', count(*) FROM tradingplan_10b5_1
            """
        ).fetchall()
    )
    assert counts == {
        "insider": 1,
        "filing_form4": 1,
        "insider_relationship": 1,
        "insider_transaction": 2,
        "insider_holding": 1,
        "tradingplan_10b5_1": 1,
    }

    row = tmp_store.con.execute(
        """
        SELECT
            i.reporting_owner_cik,
            r.officer_title_norm,
            t.transaction_code,
            t.rule_10b5_1_indicator,
            p.cooling_off_compliant
        FROM insider_transaction t
        JOIN insider i ON i.insider_id = t.insider_id
        JOIN insider_relationship r ON r.insider_id = t.insider_id
        LEFT JOIN tradingplan_10b5_1 p ON p.plan_id = t.plan_10b5_1_id
        WHERE t.transaction_code = 'S'
        """
    ).fetchone()
    assert row == ("0001214156", "CFO", "S", True, True)


def test_insider_loader_is_idempotent_and_records_source(tmp_store, tmp_path):
    _load_sample(tmp_store, tmp_path)
    _load_sample(tmp_store, tmp_path)

    assert tmp_store.con.execute("SELECT count(*) FROM filing_form4").fetchone()[0] == 1
    assert tmp_store.con.execute("SELECT count(*) FROM insider_transaction").fetchone()[0] == 2
    assert (
        tmp_store.con.execute(
            "SELECT count(*) FROM raw_source_files WHERE dataset_id = 'sec_insider_ownership'"
        ).fetchone()[0]
        == 1
    )


def test_insider_asof_filters_by_availability_and_code(tmp_store, tmp_path):
    _load_sample(tmp_store, tmp_path)
    from atx_db.asof import insider_relationships_asof, insider_transactions_asof

    before = insider_transactions_asof(
        tmp_store,
        as_of_date=dt.date(2024, 5, 1),
        as_of_ts=dt.datetime(2024, 5, 3, 15, 0),
    )
    assert before.empty

    purchases = insider_transactions_asof(
        tmp_store,
        as_of_date=dt.date(2024, 5, 1),
        as_of_ts=dt.datetime(2024, 5, 3, 17, 0),
        transaction_codes=("P",),
    )
    assert purchases["transaction_code"].tolist() == ["P"]

    relationships = insider_relationships_asof(
        tmp_store,
        as_of_date=dt.date(2024, 5, 1),
        as_of_ts=dt.datetime(2024, 5, 3, 17, 0),
    )
    assert relationships["officer_title_norm"].tolist() == ["CFO"]


def test_insider_quality_checks_pass_clean_sample(tmp_store, tmp_path):
    _load_sample(tmp_store, tmp_path)
    _load_blockholder_sample(tmp_store, tmp_path)
    from atx_db.quality import run_warehouse_quality_checks

    results = run_warehouse_quality_checks(
        tmp_store,
        dataset_ids=("sec_insider_ownership", "sec_blockholder_ownership"),
    )
    s3_results = [
        result
        for result in results
        if result.dataset_id in {"sec_insider_ownership", "sec_blockholder_ownership"}
    ]
    assert s3_results
    assert {result.status for result in s3_results} == {"passed"}


def test_blockholder_loader_populates_13d_tables(tmp_store, tmp_path):
    result = _load_blockholder_sample(tmp_store, tmp_path)
    assert result.dataset_id == "sec_blockholder_ownership"
    assert result.rows_loaded == 2

    filing = tmp_store.con.execute(
        """
        SELECT schedule_type, issuer_cik, cusip, event_date, filing_date, purpose_text
        FROM blockholder_filing
        """
    ).fetchone()
    assert filing == (
        "13D",
        "0000320193",
        "037833100",
        dt.date(2025, 1, 15),
        dt.date(2025, 1, 21),
        "Board engagement and capital allocation discussions.",
    )

    person = tmp_store.con.execute(
        """
        SELECT reporting_person_name, type_of_reporting_person, aggregate_beneficially_owned, percent_of_class
        FROM blockholder_reporting_person
        """
    ).fetchone()
    assert person == ("Example Activist LP", "IA", 1200.0, 5.2)

    from atx_db.asof import blockholder_asof

    before = blockholder_asof(
        tmp_store,
        as_of_date=dt.date(2025, 1, 15),
        as_of_ts=dt.datetime(2025, 1, 21, 16, 0),
    )
    assert before.empty

    after = blockholder_asof(
        tmp_store,
        as_of_date=dt.date(2025, 1, 15),
        as_of_ts=dt.datetime(2025, 1, 21, 18, 0),
        schedule_types=("13D",),
    )
    assert after["reporting_person_name"].tolist() == ["Example Activist LP"]
