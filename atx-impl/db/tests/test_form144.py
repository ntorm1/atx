from __future__ import annotations

import datetime as dt

import pandas as pd
import pytest


FORM144_XML = """<?xml version="1.0" encoding="UTF-8"?>
<form144Document>
  <documentType>144</documentType>
  <noticeDate>2024-05-08</noticeDate>
  <sellerInfo>
    <sellerName>Jane Q. Insider</sellerName>
    <sellerCik>0001214156</sellerCik>
  </sellerInfo>
  <issuerInfo>
    <issuerCik>0000320193</issuerCik>
    <issuerName>Apple Inc.</issuerName>
    <issuerTradingSymbol>AAPL</issuerTradingSymbol>
  </issuerInfo>
  <securitiesInformation>
    <titleOfClass>Common Stock</titleOfClass>
    <aggregateNbrOfShares>100</aggregateNbrOfShares>
    <aggregateMarketValue>13000</aggregateMarketValue>
    <approxDateOfSale>2024-05-10</approxDateOfSale>
  </securitiesInformation>
  <brokerDetails>
    <brokerName>Example Broker</brokerName>
  </brokerDetails>
  <acquisitionInformation>
    <acquisitionDate>2020-01-01</acquisitionDate>
    <natureOfAcquisition>RSU vesting</natureOfAcquisition>
  </acquisitionInformation>
</form144Document>
"""


FORM4_SALE_XML = """<?xml version="1.0" encoding="UTF-8"?>
<ownershipDocument>
  <schemaVersion>X0508</schemaVersion>
  <documentType>4</documentType>
  <periodOfReport>2024-05-10</periodOfReport>
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
      <transactionDate><value>2024-05-10</value></transactionDate>
      <transactionCoding>
        <transactionFormType>4</transactionFormType>
        <transactionCode>S</transactionCode>
        <equitySwapInvolved>0</equitySwapInvolved>
        <rule10b5-1Indicator>0</rule10b5-1Indicator>
      </transactionCoding>
      <transactionAmounts>
        <transactionShares><value>100</value></transactionShares>
        <transactionPricePerShare><value>130</value></transactionPricePerShare>
        <transactionAcquiredDisposedCode><value>D</value></transactionAcquiredDisposedCode>
      </transactionAmounts>
      <postTransactionAmounts>
        <sharesOwnedFollowingTransaction><value>900</value></sharesOwnedFollowingTransaction>
      </postTransactionAmounts>
      <ownershipNature>
        <directOrIndirectOwnership><value>D</value></directOrIndirectOwnership>
      </ownershipNature>
    </nonDerivativeTransaction>
  </nonDerivativeTable>
</ownershipDocument>
"""


def _write(tmp_path, name: str, content: str):
    path = tmp_path / name
    path.write_text(content, encoding="utf-8")
    return path


def _load_form144(tmp_store, tmp_path):
    from db.form144 import Form144IntentDataset, Form144Options

    path = _write(tmp_path, "0001214156-24-000144.xml", FORM144_XML)
    return Form144IntentDataset().run(
        tmp_store,
        Form144Options(
            source="fixture-form144",
            source_files=(path,),
            metadata_by_source={
                str(path): {
                    "accession_number": "0001214156-24-000144",
                    "form": "144",
                    "filing_date": "2024-05-08",
                    "acceptance_datetime": "2024-05-08T14:00:00Z",
                }
            },
        ),
    )


def _load_form4_sale(tmp_store, tmp_path):
    from db.insider_ownership import InsiderOwnershipDataset, InsiderOwnershipOptions

    path = _write(tmp_path, "0001214156-24-000004.xml", FORM4_SALE_XML)
    return InsiderOwnershipDataset().run(
        tmp_store,
        InsiderOwnershipOptions(
            source="fixture-insider",
            source_files=(path,),
            metadata_by_source={
                str(path): {
                    "accession_number": "0001214156-24-000004",
                    "form": "4",
                    "filing_date": "2024-05-11",
                    "acceptance_datetime": "2024-05-11T16:10:00Z",
                }
            },
        ),
    )


def test_form144_loader_links_visible_form4_sales_pit(tmp_store, tmp_path):
    from db.asof import form144_intents_asof, form144_reconciliation_asof
    from db.form144 import Form144Options, Form144ReconciliationDataset

    result = _load_form144(tmp_store, tmp_path)
    assert result.dataset_id == "form144_intent"
    assert result.rows_loaded == 1
    assert tmp_store.con.execute("SELECT count(*) FROM form144_to_form4_link").fetchone()[0] == 0

    intent_before_execution = form144_intents_asof(
        tmp_store,
        as_of_date=dt.date(2024, 5, 10),
        as_of_ts=dt.datetime(2024, 5, 10, 23, 59),
        symbols=("AAPL",),
    )
    assert len(intent_before_execution) == 1
    assert intent_before_execution.iloc[0]["matched_transaction_count"] == 0
    assert intent_before_execution.iloc[0]["approx_price_per_share"] == pytest.approx(130.0)

    _load_form4_sale(tmp_store, tmp_path)
    refresh = Form144ReconciliationDataset().run(
        tmp_store,
        Form144Options(source="fixture-form144", match_window_days=92),
    )
    assert refresh.rows_loaded == 1

    link = tmp_store.con.execute(
        """
        SELECT match_method, match_status, shares_matched, value_matched,
               share_match_ratio, match_confidence, available_at
        FROM form144_to_form4_link
        """
    ).fetchone()
    assert link[0] == "insider_id_security_window"
    assert link[1] == "FULL"
    assert link[2] == pytest.approx(100.0)
    assert link[3] == pytest.approx(13000.0)
    assert link[4] == pytest.approx(1.0)
    assert link[5] == pytest.approx(0.95)
    assert link[6] == dt.datetime(2024, 5, 11, 16, 10)

    before_form4_public = form144_intents_asof(
        tmp_store,
        as_of_date=dt.date(2024, 5, 10),
        as_of_ts=dt.datetime(2024, 5, 11, 15, 0),
        symbols=("AAPL",),
    )
    assert before_form4_public.iloc[0]["matched_transaction_count"] == 0

    after_form4_public = form144_intents_asof(
        tmp_store,
        as_of_date=dt.date(2024, 5, 10),
        as_of_ts=dt.datetime(2024, 5, 12, 9, 0),
        symbols=("AAPL",),
    )
    assert after_form4_public.iloc[0]["matched_transaction_count"] == 1
    assert after_form4_public.iloc[0]["completion_ratio"] == pytest.approx(1.0)

    reconciliation = form144_reconciliation_asof(
        tmp_store,
        as_of_date=dt.date(2024, 5, 10),
        as_of_ts=dt.datetime(2024, 5, 12, 9, 0),
        symbols=("AAPL",),
    )
    assert len(reconciliation) == 1
    assert reconciliation.iloc[0]["match_status"] == "FULL"
    assert reconciliation.iloc[0]["form4_accession_number"] == "0001214156-24-000004"


def test_form144_csv_loader_idempotent_and_normalizes_price(tmp_store, tmp_path):
    from db.form144 import Form144IntentDataset, Form144Options

    path = tmp_path / "form144.csv"
    pd.DataFrame(
        [
            {
                "accession_number": "0001214156-24-000144",
                "seller_name": "Jane Q. Insider",
                "seller_cik": "1214156",
                "issuer_cik": "320193",
                "issuer_name": "Apple Inc.",
                "issuer_trading_symbol": "AAPL",
                "filing_date": "2024-05-08",
                "approx_sale_date": "2024-05-10",
                "shares_proposed": "100",
                "aggregate_market_value": "13000",
                "available_at": "2024-05-08T14:00:00Z",
            }
        ]
    ).to_csv(path, index=False)
    options = Form144Options(source="fixture-form144", source_files=(path,), reconcile=False)

    assert Form144IntentDataset().run(tmp_store, options).rows_loaded == 1
    assert Form144IntentDataset().run(tmp_store, options).rows_loaded == 1
    row = tmp_store.con.execute(
        """
        SELECT count(*), min(approx_price_per_share), min(seller_cik),
               min(source_file_sha256) IS NOT NULL
        FROM form144_intent
        """
    ).fetchone()
    assert row == (1, 130.0, "0001214156", True)


def test_form144_quality_checks_pass_clean_sample(tmp_store, tmp_path):
    from db.form144 import Form144Options, Form144ReconciliationDataset
    from db.quality import run_warehouse_quality_checks
    from db.watermarks import refresh_warehouse_watermarks

    _load_form144(tmp_store, tmp_path)
    _load_form4_sale(tmp_store, tmp_path)
    Form144ReconciliationDataset().run(tmp_store, Form144Options(source="fixture-form144"))

    refresh_warehouse_watermarks(tmp_store)
    watermarks = tmp_store.con.execute(
        """
        SELECT dataset_id, watermark_name
        FROM dataset_watermarks
        WHERE dataset_id IN ('form144_intent', 'form144_to_form4_link')
        ORDER BY dataset_id, watermark_name
        """
    ).fetchall()
    assert watermarks == [
        ("form144_intent", "max_approx_sale_date"),
        ("form144_intent", "max_as_of_date"),
        ("form144_intent", "max_available_at"),
        ("form144_intent", "max_filing_date"),
        ("form144_to_form4_link", "max_as_of_date"),
        ("form144_to_form4_link", "max_available_at"),
        ("form144_to_form4_link", "max_sale_date"),
    ]

    results = run_warehouse_quality_checks(
        tmp_store,
        dataset_ids=("form144_intent", "form144_to_form4_link"),
    )
    form144_results = [
        result
        for result in results
        if result.dataset_id in {"form144_intent", "form144_to_form4_link"}
    ]
    assert form144_results
    assert {result.status for result in form144_results} == {"passed"}
