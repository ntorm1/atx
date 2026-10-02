"""S2.2-S2.4 pure rules: CUSIP / ISIN check digits, ISIN fragments, listing-event classes, OpenFIGI parsing,
GLEIF name / address normalisation and LEI checks."""

import datetime as dt
import json

import pytest

from atx_db.alpha_panel import cusip_history as CH
from atx_db.alpha_panel import figi_lei as FL
from atx_db.alpha_panel import listing_events as LE


@pytest.mark.parametrize("cusip", ["037833100", "594918104", "36467W109", "084670108", "02079K305", "G1151C101"])
def test_cusip_check_digit_known(cusip: str) -> None:
    assert CH.cusip_valid(cusip)
    assert CH.cusip_check_digit(cusip[:8]) == cusip[8]


def test_cusip_invalid() -> None:
    assert not CH.cusip_valid("037833101")
    assert not CH.cusip_valid("004432874")      # Chubb's ISIN fragment in a 13F CUSIP field
    assert not CH.cusip_valid(None) and not CH.cusip_valid("12345")


@pytest.mark.parametrize("cusip,country,isin", [
    ("037833100", "US", "US0378331005"),        # Apple
    ("594918104", "US", "US5949181045"),        # Microsoft
    ("36467W109", "US", "US36467W1099"),        # GameStop
    ("084670108", "US", "US0846701086"),        # Berkshire Hathaway A
    ("02079K305", "US", "US02079K3059"),        # Alphabet A
    ("82509L107", "CA", "CA82509L1076"),        # Shopify (Canadian issuer)
])
def test_isin_from_cusip_known(cusip: str, country: str, isin: str) -> None:
    assert CH.isin_from_cusip(cusip, country) == isin
    assert CH.isin_valid(isin)


@pytest.mark.parametrize("isin", ["IE00B4BNMY34", "GB0002634946", "CH0044328745", "NL0009538784", "US38259P5089"])
def test_isin_valid_known_foreign(isin: str) -> None:
    assert CH.isin_valid(isin)


def test_isin_invalid_and_cins() -> None:
    assert not CH.isin_valid("US0378331006")
    assert not CH.isin_valid("us0378331005")
    assert CH.isin_from_cusip("G1151C101") is None        # CINS: ISIN not derivable
    assert CH.isin_from_cusip("03783310") is None


def test_isin_fragment_candidates() -> None:
    assert "CH0044328745" in CH.isin_fragment_candidates("004432874", ("IE", "CH"))
    assert CH.isin_fragment_candidates("00B4BNMY3", ("IE",)) == ["IE00B4BNMY34"]
    assert CH.isin_fragment_candidates("000953878", ("NL",)) == ["NL0009538784"]
    assert CH.isin_fragment_candidates("BAD", ("IE",)) == []


def test_issue_kind() -> None:
    assert CH.issue_kind("037833100") == "equity"
    assert CH.issue_kind("09857LAN8") == "debt"           # Booking convertible note typed SH
    assert CH.issue_kind("004432874") == "unverified"
    assert CH.issue_kind("0378") == "invalid"


def test_listing_event_classes() -> None:
    assert LE.event_class("8-A12B") == "registration" and LE.event_class("8-A12G/A") == "registration"
    assert LE.event_class("CERTNYS") == "certification" and LE.event_class("CERTNAS") == "certification"
    assert LE.event_class("424B4") == "offering" and LE.event_class("S-1/A") == "offering"
    assert LE.event_class("8-K12B") == "successor" and LE.event_class("F-6EF") == "adr"
    assert LE.event_class("25-NSE") == "delisting" and LE.event_class("15-12G") == "deregistration"
    assert LE.event_class("10-K") is None and LE.event_class(None) is None


def test_listing_type() -> None:
    assert LE.listing_type({"8-A12B", "424B4"}) == "ipo"
    assert LE.listing_type({"10-12B", "8-A12B"}) == "spin_off"
    assert LE.listing_type({"8-K12B"}) == "successor"
    assert LE.listing_type({"F-6", "20-FR12B"}) == "adr"
    assert LE.listing_type({"CERTNAS", "8-A12B"}) == "exchange_registration"


def test_former_name_ranges_and_et_days() -> None:
    assert LE._day("2007-01-10T05:00:00.000Z") == dt.date(2007, 1, 10)   # midnight EST
    assert LE._day("2019-08-05T04:00:00.000Z") == dt.date(2019, 8, 5)    # midnight EDT
    got = LE.name_ranges("Apple Inc.", [
        {"name": "APPLE INC", "from": "2007-01-10T05:00:00.000Z", "to": "2019-08-05T04:00:00.000Z"},
        {"name": "APPLE COMPUTER INC", "from": "1994-01-26T05:00:00.000Z", "to": "2007-01-04T05:00:00.000Z"}])
    assert got == [("APPLE COMPUTER INC", dt.date(1994, 1, 26), dt.date(2007, 1, 4)),
                   ("APPLE INC", dt.date(2007, 1, 10), dt.date(2019, 8, 5)),
                   ("Apple Inc.", dt.date(2019, 8, 6), None)]


def test_openfigi_parse_and_share_class() -> None:
    jobs = [{"idType": "ID_CUSIP", "idValue": "037833100", "exchCode": "US"},
            {"idType": "ID_CUSIP", "idValue": "90184L102", "exchCode": "US"}]
    body = json.dumps([
        {"data": [{"figi": "BBG000B9XRY4", "name": "APPLE INC", "ticker": "AAPL", "exchCode": "US",
                   "compositeFIGI": "BBG000B9XRY4", "securityType": "Common Stock", "marketSector": "Equity",
                   "shareClassFIGI": "BBG001S5N8V8", "securityType2": "Common Stock", "securityDescription": "AAPL"}]},
        {"warning": "No identifier found."}]).encode()
    rows = FL.parse_response(jobs, body)
    assert len(rows) == 2
    assert rows[0]["composite_figi"] == "BBG000B9XRY4" and rows[0]["result_rank"] == 0
    assert rows[1]["result_rank"] is None and rows[1]["warning"] == FL.FIGI_WARN_NONE
    assert FL.pick_share_class([{"share_class_figi": "A"}, {"share_class_figi": "A"}]) == "A"
    assert FL.pick_share_class([{"share_class_figi": "A"}, {"share_class_figi": "B"}]) is None
    with pytest.raises(ValueError):
        FL.parse_response(jobs, json.dumps([{"warning": "x"}]).encode())


def test_openfigi_jobs() -> None:
    jobs = [{"idType": "ID_CUSIP", "idValue": str(i)} for i in range(23)]
    assert [len(c) for c in FL.chunk(jobs)] == [10, 10, 3]
    assert FL.job_key({"idType": "TICKER", "idValue": "BRK/B", "exchCode": "US"}) == "TICKER|BRK/B|US"
    assert FL.bloomberg_ticker("BRK.B") == "BRK/B" and FL.bloomberg_ticker("aapl") == "AAPL"


@pytest.mark.parametrize("sec,gleif", [
    ("APPLE INC", "Apple Inc."),
    ("MOODYS CORP /DE/", "Moody's Corporation"),
    ("AT&T INC.", "AT&T Inc."),
    ("COCA COLA CO", "The Coca-Cola Company"),
    ("TAIWAN SEMICONDUCTOR MANUFACTURING CO LTD", "Taiwan Semiconductor Manufacturing Company Limited"),
    ("JPMORGAN CHASE & CO", "JPMorgan Chase & Co."),
    ("INTERNATIONAL BUSINESS MACHINES CORP", "International Business Machines Corporation"),
    ("NESTLE S A", "Nestlé S.A."),
])
def test_entity_names_normalise_equal(sec: str, gleif: str) -> None:
    assert FL.norm_entity_name(sec) == FL.norm_entity_name(gleif)


def test_entity_names_distinct() -> None:
    assert FL.norm_entity_name("APPLE HOSPITALITY REIT INC") != FL.norm_entity_name("Apple Inc.")
    assert FL.norm_entity_name(None) is None


def test_postal_region_and_lei_check() -> None:
    assert FL.postal5("95014-2083") == "95014" and FL.postal5("95014") == "95014"
    assert FL.postal5("sw1a 1aa") == "SW1A1AA"
    assert FL.sec_region("DE") == "US-DE" and FL.sec_region("A6") == "CA-ON" and FL.sec_region("K3") is None
    assert FL.lei_valid("HWUPKR0MPOU8FGXBT394")          # Apple Inc.
    assert FL.lei_valid("8I5DZWZKVSZI1NUHU748")          # JPMorgan Chase & Co.
    assert not FL.lei_valid("HWUPKR0MPOU8FGXBT395")
    assert not FL.lei_valid("short")


def test_edgar_country_codes() -> None:
    assert FL.edgar_country("CA") == "US" and FL.edgar_country("A6") == "CA"
    assert FL.edgar_country("X0") == "GB" and FL.edgar_country("F4") == "CN" and FL.edgar_country("E9") == "KY"
    assert FL.edgar_country("9Z") is None and FL.edgar_country(None) is None


def test_csv_zip_to_parquet_streams_quoted_newlines(tmp_path) -> None:
    import zipfile

    import pyarrow.parquet as pq

    body = ('\ufeffLEI,Entity.LegalName,Extra\n'
            'HWUPKR0MPOU8FGXBT394,"Apple Inc.",x\n'
            '8I5DZWZKVSZI1NUHU748,"JPMorgan Chase & Co.\nline two",\n'
            '5493001KJTIIGC8Y1R12,,y\n')
    z = tmp_path / "golden.csv.zip"
    with zipfile.ZipFile(z, "w") as f:
        f.writestr("golden.csv", body.encode("utf-8"))
    dest = tmp_path / "out.parquet"
    n = FL.csv_zip_to_parquet(z, {"LEI": "lei", "Entity.LegalName": "legal_name"}, dest, batch=2)
    t = pq.read_table(dest).to_pylist()
    assert n == 3 and [r["lei"] for r in t][0] == "HWUPKR0MPOU8FGXBT394"
    assert t[1]["legal_name"] == "JPMorgan Chase & Co.\nline two" and t[2]["legal_name"] is None
    assert list(t[0]) == ["lei", "legal_name"]
