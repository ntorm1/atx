from __future__ import annotations

import datetime as dt

import pytest


def _ownership_xml(
    *,
    owner_cik: str,
    owner_name: str,
    title: str,
    transaction_date: str,
    code: str,
    acquired_disposed: str,
    shares: str,
    price: str,
    plan: bool = False,
    plan_adoption_date: str = "2024-01-01",
) -> str:
    plan_xml = ""
    if plan:
        plan_xml = f"""
        <rule10b5-1Indicator>1</rule10b5-1Indicator>
        <plan10b5-1AdoptionDate><value>{plan_adoption_date}</value></plan10b5-1AdoptionDate>
        """
    elif code.upper() == "S":
        plan_xml = "<rule10b5-1Indicator>0</rule10b5-1Indicator>"
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
      <rptOwnerName>{owner_name}</rptOwnerName>
    </reportingOwnerId>
    <reportingOwnerRelationship>
      <isDirector>1</isDirector>
      <isOfficer>1</isOfficer>
      <isTenPercentOwner>0</isTenPercentOwner>
      <isOther>0</isOther>
      <officerTitle>{title}</officerTitle>
    </reportingOwnerRelationship>
  </reportingOwner>
  <nonDerivativeTable>
    <nonDerivativeTransaction>
      <securityTitle><value>Common Stock</value></securityTitle>
      <transactionDate><value>{transaction_date}</value></transactionDate>
      <transactionCoding>
        <transactionFormType>4</transactionFormType>
        <transactionCode>{code}</transactionCode>
        <equitySwapInvolved>0</equitySwapInvolved>
        {plan_xml}
      </transactionCoding>
      <transactionAmounts>
        <transactionShares><value>{shares}</value></transactionShares>
        <transactionPricePerShare><value>{price}</value></transactionPricePerShare>
        <transactionAcquiredDisposedCode><value>{acquired_disposed}</value></transactionAcquiredDisposedCode>
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


def _write_xml(tmp_path, name: str, xml: str):
    path = tmp_path / name
    path.write_text(xml, encoding="utf-8")
    return path


def _load_ownership(tmp_store, path, *, accession: str, accepted_at: str):
    from atx_db.insider_ownership import InsiderOwnershipDataset, InsiderOwnershipOptions

    return InsiderOwnershipDataset().run(
        tmp_store,
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


def test_insider_transaction_metrics_accept_string_10b5_1_flags():
    import pandas as pd

    from atx_db.insider_metrics import InsiderTransactionMetricsOptions, compute_insider_transaction_metrics

    metrics = compute_insider_transaction_metrics(
        pd.DataFrame(
            [
                {
                    "transaction_id": "plan-sale",
                    "input_source": "fixture-insider",
                    "security_id": "SEC-CIK-0000320193",
                    "issuer_cik": "0000320193",
                    "issuer_name": "Apple Inc.",
                    "issuer_trading_symbol": "AAPL",
                    "insider_id": "SEC-INSIDER-CIK-0001214156",
                    "signal_date": dt.date(2024, 5, 10),
                    "transaction_code": "S",
                    "acquired_disposed": "D",
                    "transaction_shares": 50,
                    "transaction_price": 120,
                    "rule_10b5_1_indicator": "1",
                    "available_at": dt.datetime(2024, 5, 11, 16, 10),
                    "is_director": 1,
                    "is_officer": 1,
                    "is_ten_percent_owner": 0,
                },
                {
                    "transaction_id": "discretionary-sale",
                    "input_source": "fixture-insider",
                    "security_id": "SEC-CIK-0000320193",
                    "issuer_cik": "0000320193",
                    "issuer_name": "Apple Inc.",
                    "issuer_trading_symbol": "AAPL",
                    "insider_id": "SEC-INSIDER-CIK-0001414141",
                    "signal_date": dt.date(2024, 5, 10),
                    "transaction_code": "S",
                    "acquired_disposed": "D",
                    "transaction_shares": 100,
                    "transaction_price": 130,
                    "rule_10b5_1_indicator": "0",
                    "available_at": dt.datetime(2024, 5, 11, 16, 20),
                    "is_director": 0,
                    "is_officer": 1,
                    "is_ten_percent_owner": 0,
                },
            ]
        ),
        options=InsiderTransactionMetricsOptions(source="metric-fixture", input_source="fixture-insider"),
    )

    assert len(metrics) == 1
    assert metrics.iloc[0]["plan_sale_count"] == 1
    assert metrics.iloc[0]["discretionary_sale_count"] == 1
    assert metrics.iloc[0]["plan_sale_value"] == pytest.approx(6000.0)
    assert metrics.iloc[0]["discretionary_sale_value"] == pytest.approx(13000.0)


def test_insider_transaction_metrics_preserve_late_filing_revisions(tmp_store, tmp_path):
    from atx_db.asof import insider_transaction_metrics_asof
    from atx_db.insider_metrics import InsiderTransactionMetricsOptions, refresh_insider_transaction_metrics

    first = _write_xml(
        tmp_path,
        "first.xml",
        _ownership_xml(
            owner_cik="0001214156",
            owner_name="Jane Q. Insider",
            title="Chief Financial Officer",
            transaction_date="2024-05-01",
            code="P",
            acquired_disposed="A",
            shares="100",
            price="100",
        ),
    )
    _load_ownership(
        tmp_store,
        first,
        accession="0001214156-24-000001",
        accepted_at="2024-05-03T16:10:00Z",
    )
    options = InsiderTransactionMetricsOptions(
        source="metric-fixture",
        input_source="fixture-insider",
        cluster_min_buyers=2,
        cluster_min_purchase_value=25_000,
    )
    assert refresh_insider_transaction_metrics(tmp_store, options) == 1

    early = insider_transaction_metrics_asof(
        tmp_store,
        as_of_date=dt.date(2024, 5, 4),
        symbols=("AAPL",),
    )
    assert len(early) == 1
    assert early.iloc[0]["cluster_buyer_count"] == 1
    assert bool(early.iloc[0]["is_cluster_buy"]) is False

    late = _write_xml(
        tmp_path,
        "late.xml",
        _ownership_xml(
            owner_cik="0001313131",
            owner_name="John Cluster Buyer",
            title="Chief Executive Officer",
            transaction_date="2024-05-01",
            code="P",
            acquired_disposed="A",
            shares="200",
            price="100",
        ),
    )
    _load_ownership(
        tmp_store,
        late,
        accession="0001313131-24-000002",
        accepted_at="2024-05-05T16:10:00Z",
    )
    assert refresh_insider_transaction_metrics(tmp_store, options) == 1

    revisions = tmp_store.con.execute(
        """
        SELECT cluster_buyer_count, cluster_purchase_value, is_cluster_buy,
               restatement_seq, is_latest_revision
        FROM insider_transaction_metrics
        ORDER BY available_at
        """
    ).fetchall()
    assert revisions == [
        (1, 10000.0, False, 0, False),
        (2, 30000.0, True, 1, True),
    ]

    before_late_filing = insider_transaction_metrics_asof(
        tmp_store,
        as_of_date=dt.date(2024, 5, 4),
        symbols=("AAPL",),
    )
    assert before_late_filing.iloc[0]["cluster_buyer_count"] == 1
    after_late_filing = insider_transaction_metrics_asof(
        tmp_store,
        as_of_date=dt.date(2024, 5, 6),
        symbols=("AAPL",),
    )
    assert after_late_filing.iloc[0]["cluster_buyer_count"] == 2
    assert after_late_filing.iloc[0]["cluster_purchase_value"] == pytest.approx(30000.0)
    assert bool(after_late_filing.iloc[0]["is_cluster_buy"]) is True


def test_insider_transaction_metrics_split_plan_and_discretionary_sales(tmp_store, tmp_path):
    from atx_db.insider_metrics import InsiderTransactionMetricsOptions, refresh_insider_transaction_metrics

    plan_sale = _write_xml(
        tmp_path,
        "plan-sale.xml",
        _ownership_xml(
            owner_cik="0001214156",
            owner_name="Jane Q. Insider",
            title="Chief Financial Officer",
            transaction_date="2024-05-10",
            code="S",
            acquired_disposed="D",
            shares="50",
            price="120",
            plan=True,
            plan_adoption_date="2024-01-01",
        ),
    )
    discretionary_sale = _write_xml(
        tmp_path,
        "discretionary-sale.xml",
        _ownership_xml(
            owner_cik="0001414141",
            owner_name="Sam Discretionary",
            title="Chief Operating Officer",
            transaction_date="2024-05-10",
            code="S",
            acquired_disposed="D",
            shares="100",
            price="130",
            plan=False,
        ),
    )
    _load_ownership(
        tmp_store,
        plan_sale,
        accession="0001214156-24-000003",
        accepted_at="2024-05-11T16:10:00Z",
    )
    _load_ownership(
        tmp_store,
        discretionary_sale,
        accession="0001414141-24-000004",
        accepted_at="2024-05-11T16:20:00Z",
    )
    rows = refresh_insider_transaction_metrics(
        tmp_store,
        InsiderTransactionMetricsOptions(
            source="metric-fixture",
            input_source="fixture-insider",
            cluster_min_buyers=2,
            cluster_min_purchase_value=25_000,
        ),
    )
    assert rows == 1

    metric = tmp_store.con.execute(
        """
        SELECT open_market_sale_count, plan_sale_count, discretionary_sale_count,
               gross_sale_value, plan_sale_value, discretionary_sale_value,
               plan_sale_value_ratio, is_10b5_1_heavy_sale
        FROM insider_transaction_metrics
        """
    ).fetchone()
    assert metric[:3] == (2, 1, 1)
    assert metric[3] == pytest.approx(19000.0)
    assert metric[4] == pytest.approx(6000.0)
    assert metric[5] == pytest.approx(13000.0)
    assert metric[6] == pytest.approx(6000.0 / 19000.0)
    assert metric[7] is False
