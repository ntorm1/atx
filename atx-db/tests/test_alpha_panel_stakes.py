"""Schedule 13D / 13G parsing: EDGAR XML (2024-12+) and pre-XML text 13D, activism terms, CUSIP mapping."""

from __future__ import annotations

import datetime as dt

from atx_db.alpha_panel import stakes as K

XML_13D = b"""<?xml version="1.0" encoding="UTF-8"?><edgarSubmission xmlns="http://www.sec.gov/edgar/schedule13D"
 xmlns:com="http://www.sec.gov/edgar/common"><headerData><submissionType>SCHEDULE 13D/A</submissionType><filerInfo><filer>
 <filerCredentials><cik>0001386989</cik></filerCredentials></filer></filerInfo></headerData><formData><coverPageHeader>
 <securitiesClassTitle>Ordinary Shares</securitiesClassTitle><dateOfEvent>05/08/2025</dateOfEvent><amendmentNo>3</amendmentNo>
 <issuerInfo><issuerCIK>0001772253</issuerCIK><issuerCUSIP>G35947202</issuerCUSIP><issuerName>Flex LNG Ltd.</issuerName>
 </issuerInfo></coverPageHeader><reportingPersons><reportingPersonInfo><reportingPersonNoCIK>Y</reportingPersonNoCIK>
 <reportingPersonName>Geveran Trading Co. Limited</reportingPersonName><memberOfGroup>a</memberOfGroup><fundType>WC</fundType>
 <soleVotingPower>0.00</soleVotingPower><sharedVotingPower>23118636.00</sharedVotingPower><soleDispositivePower>0.00
 </soleDispositivePower><sharedDispositivePower>23118636.00</sharedDispositivePower><aggregateAmountOwned>23118636.00
 </aggregateAmountOwned><percentOfClass>42.74</percentOfClass><typeOfReportingPerson>CO</typeOfReportingPerson>
 </reportingPersonInfo><reportingPersonInfo><reportingPersonCIK>0001731638</reportingPersonCIK><reportingPersonName>C.K.
 Limited</reportingPersonName><aggregateAmountOwned>1000.00</aggregateAmountOwned><percentOfClass>0.1</percentOfClass>
 </reportingPersonInfo></reportingPersons><items1To7><item4><transactionPurpose>The Reporting Persons intend to nominate
 two director candidates at the annual meeting.</transactionPurpose></item4></items1To7></formData></edgarSubmission>"""

XML_13G = b"""<?xml version="1.0"?><edgarSubmission xmlns="http://www.sec.gov/edgar/schedule13g"><headerData>
 <submissionType>SCHEDULE 13G</submissionType><filerInfo><filer><filerCredentials><cik>0001721695</cik></filerCredentials>
 </filer></filerInfo></headerData><formData><coverPageHeader><securitiesClassTitle>Common</securitiesClassTitle>
 <eventDateRequiresFilingThisStatement>04/17/2026</eventDateRequiresFilingThisStatement><issuerInfo><issuerCik>0001720424
 </issuerCik><issuerName>HIVE Digital</issuerName><issuerCusips><issuerCusipNumber>433921103</issuerCusipNumber></issuerCusips>
 </issuerInfo><designateRulesPursuantThisScheduleFiled><designateRulePursuantThisScheduleFiled>Rule 13d-1(c)
 </designateRulePursuantThisScheduleFiled></designateRulesPursuantThisScheduleFiled></coverPageHeader>
 <coverPageHeaderReportingPersonDetails><reportingPersonName>Citadel Securities GP LLC</reportingPersonName>
 <reportingPersonBeneficiallyOwnedNumberOfShares><soleVotingPower>0</soleVotingPower><sharedVotingPower>11511599
 </sharedVotingPower></reportingPersonBeneficiallyOwnedNumberOfShares>
 <reportingPersonBeneficiallyOwnedAggregateNumberOfShares>11511599.00</reportingPersonBeneficiallyOwnedAggregateNumberOfShares>
 <classPercent>4.5</classPercent><typeOfReportingPerson>HC</typeOfReportingPerson><typeOfReportingPerson>OO
 </typeOfReportingPerson></coverPageHeaderReportingPersonDetails><items><item4><amountBeneficiallyOwned>x</amountBeneficiallyOwned>
 </item4></items></formData></edgarSubmission>"""

HTML_13D = """<html><body><p>SCHEDULE 13D</p><p>(Amendment No.)*</p><p>Glatfelter</p><p>Corporation</p>
<p>(Name of Issuer)</p><p>Common Stock, $0.01 par value</p><p>(Title of Class of Securities)</p><p>377320106</p>
<p>(CUSIP Number)</p><p>Carlson Capital, L.P.</p><p>October 13, 2022</p><p>(Date of Event Which Requires Filing of This
Statement)</p><table><tr><td>1</td><td>NAMES OF REPORTING PERSON</td></tr><tr><td>Carlson Capital, L.P.</td></tr>
<tr><td>11</td><td>AGGREGATE AMOUNT BENEFICIALLY OWNED BY EACH REPORTING PERSON</td></tr><tr><td>5,415,000*</td></tr>
<tr><td>12</td><td>CHECK IF THE AGGREGATE AMOUNT IN ROW (11) EXCLUDES CERTAIN SHARES</td></tr>
<tr><td>13</td><td>PERCENT OF CLASS REPRESENTED BY AMOUNT IN ROW (11)</td></tr><tr><td>12.1%*</td></tr>
<tr><td>14</td><td>TYPE OF REPORTING PERSON</td></tr><tr><td>PN, IA</td></tr>
<tr><td>1</td><td>NAMES OF REPORTING PERSON</td></tr><tr><td>Clint D. Carlson</td></tr>
<tr><td>11</td><td>AGGREGATE AMOUNT BENEFICIALLY OWNED BY EACH REPORTING PERSON</td></tr><tr><td>-0-</td></tr>
<tr><td>13</td><td>PERCENT OF CLASS REPRESENTED BY AMOUNT IN ROW (11)</td></tr><tr><td>0%</td></tr></table>
<p>Item 4. Purpose of Transaction</p><p>The Reporting Persons acquired the shares for investment purposes. The Reporting
Persons may propose or consider any one or more of the actions described in subsections (a) through (j) of Item 4 of
Schedule 13D.</p><p>Item 5. Interest in Securities of the Issuer</p><p>See rows 11 and 13.</p></body></html>"""


def test_parse_xml_13d() -> None:
    f, persons = K.parse_xml(XML_13D)
    assert (f["schedule"], f["issuer_cik"], f["cusip"], f["event_date"], f["amendment_no"], f["filer_cik"]) == \
        ("13D", 1772253, "G35947202", dt.date(2025, 5, 8), 3, 1386989)
    assert [p["shares"] for p in persons] == [23118636.0, 1000.0]
    assert persons[0]["percent"] == 42.74 and persons[0]["member_of_group"] == "a" and persons[1]["person_cik"] == 1731638
    assert "nominate" in f["purpose_text"] and K.activism_terms(f["purpose_text"]) == ["director candidates", "nominat"]


def test_parse_xml_13g() -> None:
    f, persons = K.parse_xml(XML_13G)
    assert (f["schedule"], f["issuer_cik"], f["cusip"], f["rule_13g"]) == ("13G", 1720424, "433921103", "Rule 13d-1(c)")
    assert f["purpose_text"] is None                       # 13G Item 4 is ownership, not purpose
    assert persons[0]["shares"] == 11511599.0 and persons[0]["percent"] == 4.5 and persons[0]["person_type"] == "HC,OO"


def test_parse_text_13d() -> None:
    f, persons = K.parse_text_13d(HTML_13D.encode())
    assert f["issuer_name"] == "Glatfelter Corporation" and f["cusip"] == "377320106"
    assert f["event_date"] == dt.date(2022, 10, 13)
    assert [(p["person_name"], p["shares"], p["percent"]) for p in persons] == \
        [("Carlson Capital, L.P.", 5415000.0, 12.1), ("Clint D. Carlson", 0.0, 0.0)]
    assert persons[0]["person_type"] == "PN,IA"
    assert f["purpose_text"].startswith("The Reporting Persons acquired")
    assert K.activism_terms(f["purpose_text"]) == []       # the (a)-(j) reservation of rights is boilerplate


def test_activism_terms_and_amounts() -> None:
    assert K.activism_terms("We delivered a letter to the Board urging a sale of the Company.") == \
        ["delivered a letter", "letter to the board", "sale of the company"]
    assert K.activism_terms(None) == []
    assert K._first_amount("\n12\nCHECK IF THE AGGREGATE") is None
    assert K._first_amount(" 1,234,567 (see Item 5)") == 1234567.0
    assert K._first_amount(" -0- ") == 0.0


def test_map_cusip_asof_and_next_quarter() -> None:
    cmap = {"037833100": [(dt.date(2024, 12, 31), 7), (dt.date(2025, 3, 31), 8)]}
    assert K.map_cusip(cmap, "037833100", dt.date(2025, 2, 1)) == (7, "13f_map_asof")
    assert K.map_cusip(cmap, "037833100", dt.date(2024, 11, 1)) == (7, "13f_map_next_quarter")
    assert K.map_cusip(cmap, "999999999", dt.date(2025, 1, 1)) == (None, "unmapped")
    assert K.map_cusip(cmap, None, dt.date(2025, 1, 1)) == (None, None)


def test_issuer_name_skips_form_boilerplate_and_split_dates() -> None:
    text = ("SCHEDULE 13D\nUnder the Securities Exchange Act of 1934\n(Amendment )*\nLyra Therapeutics, Inc.\n"
            "(Name of Issuer)\nCommon Stock\n")
    assert K._issuer_name(text) == "Lyra Therapeutics, Inc."
    text2 = ("INFORMATION TO BE INCLUDED IN STATEMENTS\nFILED PURSUANT TO RULE 13d-1(a) AND\n AMENDMENTS THERETO\n"
             "FILED PURSUANT TO RULE 13d-2(a)\n Triterras, Inc.\n(Name of Issuer)\n")
    assert K._issuer_name(text2) == "Triterras, Inc."
    assert K._issuer_name("(Amendment No. ) 1 Quest Resource Holding Corporation\n(Name of Issuer)") == \
        "Quest Resource Holding Corporation"
    assert K._parse_long_date("(212) 728-8000\n September 16,\n2022") == dt.date(2022, 9, 16)
    assert K._parse_long_date("October\n6, 2020") == dt.date(2020, 10, 6)
    assert K._parse_long_date("no date here") is None


def test_parse_xml_13g_issuer_cusip_tag() -> None:
    body = XML_13G.replace(b"<issuerCusips><issuerCusipNumber>433921103</issuerCusipNumber></issuerCusips>",
                           b"<issuerCusip>78413P101</issuerCusip>")
    f, _persons = K.parse_xml(body)
    assert f["cusip"] == "78413P101"                    # the tag live 13G filings use
