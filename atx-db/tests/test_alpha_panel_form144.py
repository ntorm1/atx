"""Form 144 XML parsing."""

from __future__ import annotations

import datetime as dt

from atx_db.alpha_panel import form144 as F

XML_144 = b"""<?xml version="1.0" encoding="UTF-8"?><edgarSubmission xmlns="http://www.sec.gov/edgar/ownership"
 xmlns:com="http://www.sec.gov/edgar/common"><headerData><submissionType>144</submissionType></headerData><formData>
 <issuerInfo><issuerCik>0000065984</issuerCik><issuerName>ENTERGY CORP /DE/</issuerName>
 <nameOfPersonForWhoseAccountTheSecuritiesAreToBeSold>CHAPMAN JASON</nameOfPersonForWhoseAccountTheSecuritiesAreToBeSold>
 <relationshipsToIssuer><relationshipToIssuer>Officer</relationshipToIssuer><relationshipToIssuer>Director
 </relationshipToIssuer></relationshipsToIssuer></issuerInfo>
 <securitiesInformation><securitiesClassTitle>Common</securitiesClassTitle><brokerOrMarketmakerDetails>
 <name>Fidelity Brokerage Services LLC</name></brokerOrMarketmakerDetails><noOfUnitsSold>8239</noOfUnitsSold>
 <aggregateMarketValue>843945.79</aggregateMarketValue><noOfUnitsOutstanding>446596904</noOfUnitsOutstanding>
 <approxSaleDate>02/13/2026</approxSaleDate><securitiesExchangeName>NYSE</securitiesExchangeName></securitiesInformation>
 <securitiesInformation><securitiesClassTitle>Common</securitiesClassTitle><noOfUnitsSold>1000</noOfUnitsSold>
 <aggregateMarketValue>100000</aggregateMarketValue></securitiesInformation>
 <securitiesToBeSold><natureOfAcquisitionTransaction>Restricted Stock Vesting</natureOfAcquisitionTransaction>
 </securitiesToBeSold><securitiesToBeSold><natureOfAcquisitionTransaction>Exercise of Options</natureOfAcquisitionTransaction>
 </securitiesToBeSold><nothingToReportFlagOnSecuritiesSoldInPast3Months>Y</nothingToReportFlagOnSecuritiesSoldInPast3Months>
 <noticeSignature><noticeDate>02/13/2026</noticeDate><planAdoptionDates><planAdoptionDate>11/15/2025</planAdoptionDate>
 </planAdoptionDates></noticeSignature></formData></edgarSubmission>"""


def test_parse_144() -> None:
    p = F.parse_144(XML_144)
    assert (p["issuer_cik"], p["seller_name"], p["relationship"]) == (65984, "CHAPMAN JASON", "Director,Officer")
    assert p["units_to_sell"] == 9239.0 and p["aggregate_market_value"] == 943945.79     # two blocks summed
    assert p["units_outstanding"] == 446596904.0 and p["approx_sale_date"] == dt.date(2026, 2, 13)
    assert p["broker"] == "Fidelity Brokerage Services LLC" and p["exchange"] == "NYSE" and p["n_blocks"] == 2
    assert p["acquisition_nature"] == "Exercise of Options|Restricted Stock Vesting" and p["n_lots"] == 2
    assert p["nothing_sold_past_3m"] is True and p["notice_date"] == dt.date(2026, 2, 13)
    assert p["plan_10b5_1_adoption"] == dt.date(2025, 11, 15)
