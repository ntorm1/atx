"""Stage ``reference`` (S1.5): FRED rates, H.10 FX and VIX, and the Ken French library -> ``reference/``.

Sources (raw landings as served under ``data/raw/{fred,french}/``, receipt ledger ``receipts.jsonl`` per source):

* FRED graph CSV, no key: ``https://fred.stlouisfed.org/graph/fredgraph.csv?id=<SERIES>&cosd=2008-01-01``
  (2010+ is the deliverable; 2008-2009 is lookback for FY2010 period-average FX). One file per fetch
  (``<SERIES>__<UTC stamp>.csv``), the latest receipt per series is parsed.
  - H.15 Selected Interest Rates: Treasury constant maturities ``DGS1MO .. DGS30``, the 3-month bill (secondary
    market, discount basis) ``DTB3`` and the effective federal funds rate ``DFF`` (percent, annualised).
  - H.10 Foreign Exchange Rates: every daily bilateral series FRED lists on the H.10 release (release id 17,
    ``RELEASE_PAGE_URL``; 23 series, ``H10``). Noon buying rates in New York.
  - ``VIXCLS``: Cboe VIX close.
* Federal Reserve Board H.10 release dates (``H10_DATES_URL``, JSON, 1996+), landed under ``data/raw/fred/``.
* Ken French data library (``FRENCH_BASE``): FF5 (2x3) and FF3 daily and monthly, momentum daily and monthly, and
  the industry definition files ``Siccodes{5,10,12,17,30,38,48,49}``. Zips are parsed and deleted; the CSV/TXT
  members are kept with the zip's SHA-256 in the receipt.

Clock rules (``available_at``, naive TIMESTAMP in UTC; ``PUBLISH_ET`` = 16:30 America/New_York, 15 minutes after
the stated 16:15 posting time, so never earlier than publication):

* ``h10-weekly-monday-v1``: the Board releases H.10 "on Mondays at 4:15 p.m. ... daily bilateral exchange rates
  ... for the previous business week. If Monday falls on a Federal Holiday, the data will be released on the
  following business day" (federalreserve.gov/releases/h10). An observation dated d (week W) is available at the
  first release date on or after the Monday of week W+1, taken from the Board's release-date list, at 16:30 ET.
  Mid-week releases (revisions) are skipped, which can only make the clock later. The weekly release was suspended
  from May 2006 to January 2009: 2008 observations take the first release after the suspension (2009-01-05).
  After the list: the Monday of W+1, moved past federal holidays.
* ``h15-next-business-day-v1``: H.15 "is posted daily Monday through Friday at 4:15pm ... not posted on holidays"
  and carries the previous business day (verified: the 2026-09-29 posting ends at 2026-09-28; FRED Last-Modified
  20:16 UTC). An observation dated d is available on the first federal business day strictly after B(d) at
  16:30 ET, where B(d) is d or, for weekend and holiday values (``DFF`` is 7-day), the next business day.
  Business days are weekdays that are not US federal holidays, observed dates included (``holidays.US``), which
  closes the Board on more days than it closes, so the clock can only be later.
* ``vix-cboe-close-v1``: the VIX close is disseminated by Cboe at the 16:15 ET close of day d; available at d
  16:30 ET.
* ``french-fetch-v1``: the library re-posts whole files with every CRSP update and keeps no history, so each row is
  available at the fetch time of the file (``vintage_risk``), with the file's ``CRSP database`` stamp as
  ``vintage``.

Outputs (stage ``reference``; ``fx_daily.parquet`` is a contract of lane FUND, ruling D4):

* ``fx_daily.parquet``: exactly ``obs_date DATE, currency VARCHAR (ISO-4217), usd_per_ccy DOUBLE, series_id
  VARCHAR, available_at TIMESTAMP``. Series quoted in currency per USD are inverted. Blank (holiday) days have no
  row. DEXVZUS carries VEF, VES from 2018-08-20 and VED from 2021-10-04 (``CURRENCY_BREAKS``).
* ``rates_daily.parquet``: ``obs_date, series_id, description, tenor_months, value_pct, release, available_at``.
* ``vix_daily.parquet``: ``obs_date, vix_close, series_id, available_at``.
* ``series_catalog.parquet``: one row per FRED series with first/last observation and the completeness count
  since 2010 (expected federal business days, missing ones).
* ``french/{ff5,ff3,mom}_{daily,monthly}.parquet``: ``date`` (daily) or ``month`` (first day of the month), the
  factors in decimal units, ``vintage``, ``available_at``, ``vintage_risk``.
* ``french/siccodes.parquet``: ``scheme, industry_no, industry_short, industry_name, sic_lo, sic_hi, range_desc``
  (a scheme's residual industry without ranges has one row with NULL bounds).
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import hashlib
import io
import json
import re
import sys
import zipfile
from pathlib import Path
from typing import Any
from zoneinfo import ZoneInfo

from . import common as C
from . import shortflow_common as S

STAGE = "reference"
SCHEMA = "atx.alpha-panel.reference/v1"
MODULES = ("reference", "common", "shortflow_common")
FRED_URL = "https://fred.stlouisfed.org/graph/fredgraph.csv"
RELEASE_PAGE_URL = "https://fred.stlouisfed.org/release/tables?rid=17&eid=23340"
H10_DATES_URL = "https://www.federalreserve.gov/releases/h10/releaseDates.json"
FRENCH_BASE = "https://mba.tuck.dartmouth.edu/pages/faculty/ken.french/ftp/"
FRED_START = "2008-01-01"
DELIVERY_START = dt.date(2010, 1, 1)
RAW_FRED = S.RAW_ROOT / "fred"
RAW_FRENCH = S.RAW_ROOT / "french"
HOST_INTERVAL_S = 1.1  # <= 1 request/s per host (lane rule for non-SEC hosts)
ET = ZoneInfo("America/New_York")
PUBLISH_ET = dt.time(16, 30)
FX_COLUMNS = ("obs_date", "currency", "usd_per_ccy", "series_id", "available_at")

# H.10 daily series on FRED -> (ISO-4217 currency, quote). ``usd_per_ccy`` series are used as is; ``ccy_per_usd``
# series are inverted. DEXVZUS (bolivar fuerte, VEF) was discontinued by the Board.
H10: dict[str, tuple[str, str]] = {
    "DEXBZUS": ("BRL", "ccy_per_usd"), "DEXCAUS": ("CAD", "ccy_per_usd"), "DEXCHUS": ("CNY", "ccy_per_usd"),
    "DEXDNUS": ("DKK", "ccy_per_usd"), "DEXHKUS": ("HKD", "ccy_per_usd"), "DEXINUS": ("INR", "ccy_per_usd"),
    "DEXJPUS": ("JPY", "ccy_per_usd"), "DEXKOUS": ("KRW", "ccy_per_usd"), "DEXMAUS": ("MYR", "ccy_per_usd"),
    "DEXMXUS": ("MXN", "ccy_per_usd"), "DEXNOUS": ("NOK", "ccy_per_usd"), "DEXSDUS": ("SEK", "ccy_per_usd"),
    "DEXSFUS": ("ZAR", "ccy_per_usd"), "DEXSIUS": ("SGD", "ccy_per_usd"), "DEXSLUS": ("LKR", "ccy_per_usd"),
    "DEXSZUS": ("CHF", "ccy_per_usd"), "DEXTAUS": ("TWD", "ccy_per_usd"), "DEXTHUS": ("THB", "ccy_per_usd"),
    "DEXVZUS": ("VEF", "ccy_per_usd"), "DEXUSAL": ("AUD", "usd_per_ccy"), "DEXUSEU": ("EUR", "usd_per_ccy"),
    "DEXUSNZ": ("NZD", "usd_per_ccy"), "DEXUSUK": ("GBP", "usd_per_ccy"),
}
# FRED keeps one code across redenominations: DEXVZUS is VEF, then VES (1:100,000) from 2018-08-20 and VED
# (1:1,000,000) from 2021-10-04 (the first observations on the new basis in the FRED series).
CURRENCY_BREAKS: dict[str, list[tuple[dt.date, str]]] = {
    "DEXVZUS": [(dt.date(2018, 8, 20), "VES"), (dt.date(2021, 10, 4), "VED")],
}
# H.15 series -> (description, tenor in months)
H15: dict[str, tuple[str, float | None]] = {
    "DGS1MO": ("Treasury constant maturity 1-month", 1.0), "DGS3MO": ("Treasury constant maturity 3-month", 3.0),
    "DGS6MO": ("Treasury constant maturity 6-month", 6.0), "DGS1": ("Treasury constant maturity 1-year", 12.0),
    "DGS2": ("Treasury constant maturity 2-year", 24.0), "DGS3": ("Treasury constant maturity 3-year", 36.0),
    "DGS5": ("Treasury constant maturity 5-year", 60.0), "DGS7": ("Treasury constant maturity 7-year", 84.0),
    "DGS10": ("Treasury constant maturity 10-year", 120.0), "DGS20": ("Treasury constant maturity 20-year", 240.0),
    "DGS30": ("Treasury constant maturity 30-year", 360.0),
    "DTB3": ("3-month Treasury bill, secondary market, discount basis", 3.0),
    "DFF": ("Effective federal funds rate (7-day series)", None),
}
VIX = {"VIXCLS": "Cboe VIX close"}
LAKE_STAGES = [
    {"name": "reference", "lane": "MKT", "schema": "atx.alpha-panel.reference/v1", "module": "atx_db.alpha_panel.reference",
     "args": ["build"], "fetch": ["fetch"], "inputs": [],
     "outputs": [{"glob": "reference/fx_daily.parquet", "view": "reference_fx_daily"},
                 {"glob": "reference/rates_daily.parquet", "view": "reference_rates_daily"},
                 {"glob": "reference/vix_daily.parquet", "view": "reference_vix_daily"},
                 {"glob": "reference/series_catalog.parquet", "view": "reference_series_catalog", "clock": None},
                 {"glob": "reference/french/ff5_daily.parquet", "view": "french_ff5_daily"},
                 {"glob": "reference/french/ff5_monthly.parquet", "view": "french_ff5_monthly"},
                 {"glob": "reference/french/ff3_daily.parquet", "view": "french_ff3_daily"},
                 {"glob": "reference/french/ff3_monthly.parquet", "view": "french_ff3_monthly"},
                 {"glob": "reference/french/mom_daily.parquet", "view": "french_mom_daily"},
                 {"glob": "reference/french/mom_monthly.parquet", "view": "french_mom_monthly"},
                 {"glob": "reference/french/siccodes.parquet", "view": "french_siccodes"}],
     "staleness": "daily series; H.10 weekly (stale after 10 days), H.15 / VIX after 5 business days",
     "vintage": "event", "guard_gb": 0.3},
]
FRENCH_FACTOR_FILES: dict[str, str] = {
    "ff5_daily": "F-F_Research_Data_5_Factors_2x3_daily_CSV.zip",
    "ff5_monthly": "F-F_Research_Data_5_Factors_2x3_CSV.zip",
    "ff3_daily": "F-F_Research_Data_Factors_daily_CSV.zip",
    "ff3_monthly": "F-F_Research_Data_Factors_CSV.zip",
    "mom_daily": "F-F_Momentum_Factor_daily_CSV.zip",
    "mom_monthly": "F-F_Momentum_Factor_CSV.zip",
}
SICCODE_SCHEMES = (5, 10, 12, 17, 30, 38, 48, 49)


def all_fred_series() -> list[str]:
    return list(H15) + list(H10) + list(VIX)


# ---------------------------------------------------------------- calendars and clocks
_HOLIDAYS: dict[int, set[dt.date]] = {}


def is_business_day(d: dt.date) -> bool:
    """Weekday and not a US federal holiday (observed dates included)."""
    if d.weekday() >= 5:
        return False
    if d.year not in _HOLIDAYS:
        import holidays

        _HOLIDAYS[d.year] = set(holidays.US(years=d.year, observed=True).keys())
    return d not in _HOLIDAYS[d.year]


def next_business_day(d: dt.date, strict: bool = True) -> dt.date:
    x = d + dt.timedelta(days=1) if strict else d
    while not is_business_day(x):
        x += dt.timedelta(days=1)
    return x


def et_to_utc(d: dt.date, t: dt.time = PUBLISH_ET) -> dt.datetime:
    """Wall time ``t`` on ``d`` in America/New_York as a naive UTC datetime."""
    return dt.datetime.combine(d, t, tzinfo=ET).astimezone(dt.UTC).replace(tzinfo=None)


def h15_available_at(obs: dt.date) -> dt.datetime:
    return et_to_utc(next_business_day(next_business_day(obs, strict=False), strict=True))


def vix_available_at(obs: dt.date) -> dt.datetime:
    return et_to_utc(obs)


def next_week_monday(obs: dt.date) -> dt.date:
    return obs + dt.timedelta(days=7 - obs.weekday())


def h10_release_date(obs: dt.date, release_dates: list[dt.date]) -> tuple[dt.date, str]:
    """First Board release on or after the Monday after the observation's week; rule fallback outside the list."""
    import bisect

    monday = next_week_monday(obs)
    if release_dates and release_dates[0] <= monday <= release_dates[-1]:
        i = bisect.bisect_left(release_dates, monday)
        if (release_dates[i] - monday).days <= 7:
            return release_dates[i], "board_release_list"
        # the weekly release was suspended (May 2006 - January 2009; data went to the DDP on an undocumented
        # schedule): the next weekly release is the earliest date the observation is known to have been public
        return release_dates[i], "board_release_after_suspension"
    return next_business_day(monday, strict=False), "rule_monday_or_next_business_day"


def parse_release_dates(blob: bytes) -> list[dt.date]:
    out = set()
    for year in json.loads(blob.decode("utf-8-sig")):
        for month in year.get("Months", []):
            for s in month.get("Dates", []):
                out.add(dt.date(int(s[:4]), int(s[4:6]), int(s[6:8])))
    return sorted(out)


# ---------------------------------------------------------------- FRED parsing
def parse_fred_csv(text: str, series_id: str) -> list[tuple[dt.date, float | None]]:
    """``observation_date,<SERIES>`` rows; blank or ``.`` values are None (holidays)."""
    rows: list[tuple[dt.date, float | None]] = []
    reader = csv.reader(io.StringIO(text))
    header = next(reader, None)
    if not header or len(header) < 2 or header[1].strip() != series_id:
        raise ValueError(f"{series_id}: unexpected header {header}")
    for rec in reader:
        if len(rec) < 2 or not rec[0].strip():
            continue
        d = dt.date.fromisoformat(rec[0].strip())
        v = rec[1].strip()
        rows.append((d, float(v) if v not in ("", ".") else None))
    return rows


def fx_rows(series_id: str, obs: list[tuple[dt.date, float | None]],
            release_dates: list[dt.date]) -> list[tuple[dt.date, str, float, str, dt.datetime]]:
    """``fx_daily`` rows of one H.10 series: inverted when quoted per USD, blank and non-positive values dropped."""
    _, quote = H10[series_id]
    out = []
    for d, v in obs:
        if v is None or not v > 0:
            continue
        rel, _ = h10_release_date(d, release_dates)
        out.append((d, currency_of(series_id, d), v if quote == "usd_per_ccy" else 1.0 / v, series_id, et_to_utc(rel)))
    return out


def currency_of(series_id: str, d: dt.date) -> str:
    """ISO-4217 code of an H.10 series on date d (redenominations in ``CURRENCY_BREAKS``)."""
    code = H10[series_id][0]
    for start, new in CURRENCY_BREAKS.get(series_id, []):
        if d >= start:
            code = new
    return code


def completeness(obs: list[tuple[dt.date, float | None]], start: dt.date = DELIVERY_START) -> dict[str, Any]:
    """Expected federal business days in [max(start, first obs), last obs] against the days holding a value."""
    have = {d for d, v in obs if v is not None}
    if not have:
        return {"first_obs": None, "last_obs": None, "expected": 0, "observed": 0, "missing": 0, "missing_days": []}
    lo, hi = max(start, min(have)), max(have)
    expected, missing = 0, []
    d = lo
    while d <= hi:
        if is_business_day(d):
            expected += 1
            if d not in have:
                missing.append(d.isoformat())
        d += dt.timedelta(days=1)
    return {"first_obs": min(have).isoformat(), "last_obs": hi.isoformat(), "expected": expected,
            "observed": expected - len(missing), "missing": len(missing), "missing_days": missing}


# ---------------------------------------------------------------- French parsing
def parse_french_table(text: str) -> tuple[list[str], list[tuple[dt.date, list[float | None]]], str | None]:
    """The first table of a French factor CSV: ``(columns, [(date, values in decimal)], vintage)``.

    Daily files key rows by ``YYYYMMDD``; monthly files by ``YYYYMM`` (-> first day of the month) and continue with
    an annual table that is not read. Values are percent; -99.99 / -999 are missing."""
    m = re.search(r"using the (\d{6}) CRSP database", text)
    vintage = f"CRSP {m.group(1)}" if m else None
    cols: list[str] = []
    rows: list[tuple[dt.date, list[float | None]]] = []
    width = None
    for line in text.splitlines():
        s = line.strip()
        if not cols:
            if s.startswith(",") and len(s) > 1:
                cols = [c.strip() for c in s.split(",")[1:]]
            continue
        parts = [p.strip() for p in s.split(",")]
        key = parts[0] if parts else ""
        if not key.isdigit() or len(key) not in (6, 8) or (width is not None and len(key) != width):
            if rows:
                break
            continue
        width = len(key)
        d = dt.date(int(key[:4]), int(key[4:6]), int(key[6:8]) if width == 8 else 1)
        vals: list[float | None] = []
        for p in parts[1:len(cols) + 1]:
            try:
                x = float(p)
            except ValueError:
                x = None
            vals.append(None if x is None or x <= -99.99 else x / 100.0)
        rows.append((d, vals))
    return cols, rows, vintage


def factor_column(name: str) -> str:
    return {"Mkt-RF": "mkt_rf", "Mom": "mom", "MOM": "mom", "WML": "mom"}.get(name, name.lower().replace("-", "_"))


_IND = re.compile(r"^\s*(\d{1,2})\s+(\S+)\s+(.*?)\s*$")
_RANGE = re.compile(r"^\s+(\d{4})-(\d{4})\s*(.*?)\s*$")


def parse_siccodes(text: str, scheme: int) -> list[tuple]:
    """``(scheme, industry_no, short, name, sic_lo, sic_hi, range_desc)`` rows of a French ``SiccodesNN.txt``."""
    out: list[tuple] = []
    cur: tuple[int, str, str] | None = None
    ranges = 0
    for line in text.splitlines():
        if not line.strip():
            continue
        r = _RANGE.match(line)
        if r and cur is not None:
            out.append((scheme, cur[0], cur[1], cur[2], int(r.group(1)), int(r.group(2)), r.group(3) or None))
            ranges += 1
            continue
        m = _IND.match(line)
        if m and not line.startswith(" " * 6):
            if cur is not None and ranges == 0:
                out.append((scheme, cur[0], cur[1], cur[2], None, None, None))
            cur = (int(m.group(1)), m.group(2), m.group(3))
            ranges = 0
    if cur is not None and ranges == 0:
        out.append((scheme, cur[0], cur[1], cur[2], None, None, None))
    return out


# ---------------------------------------------------------------- download
def _ledger(raw: Path) -> S.Ledger:
    return S.Ledger(raw / "receipts.jsonl")


def _stamp() -> str:
    return dt.datetime.now(dt.UTC).strftime("%Y%m%dT%H%M%SZ")


def _fresh(rec: dict[str, Any] | None, raw: Path, max_age_h: float) -> bool:
    if not rec or rec.get("http_status") != 200 or not rec.get("file") or not (raw / rec["file"]).exists():
        return False
    age = dt.datetime.now(dt.UTC) - dt.datetime.fromisoformat(rec["fetched_at"])
    return age.total_seconds() < max_age_h * 3600


def download(refetch: bool = False, max_age_h: float = 20.0) -> dict[str, Any]:
    """Land every FRED series, the H.10 release dates and the French files; resumable via the receipt ledgers."""
    host = S.PoliteHost(min_interval=HOST_INTERVAL_S)
    stats: dict[str, Any] = {"fred": 0, "french": 0, "skipped": 0, "bytes": 0}
    led = _ledger(RAW_FRED)
    have = led.latest()
    # H.10 series listed on the FRED release page (checked against the curated currency table)
    if refetch or not _fresh(have.get("h10_release_page"), RAW_FRED, max_age_h):
        st, body, hdr = host.request(RELEASE_PAGE_URL)
        listed = sorted(set(re.findall(r"/series/(DEX[A-Z]{4})", body.decode("utf-8", "replace"))))
        name = f"h10_release_page__{_stamp()}.html"
        (RAW_FRED / name).write_bytes(body)
        led.append({"key": "h10_release_page", "kind": "page", "url": RELEASE_PAGE_URL, "http_status": st,
                    "bytes": len(body), "sha256": hashlib.sha256(body).hexdigest(), "fetched_at": S.utc_now(),
                    "file": name, "listed_series": listed})
        stats["bytes"] += len(body)
    listed = led.latest()["h10_release_page"].get("listed_series", [])
    unknown = sorted(set(listed) - set(H10))
    if unknown:
        raise RuntimeError(f"H.10 series without a currency mapping: {unknown}")
    stats["h10_listed"] = listed
    targets = [("h10_release_dates", H10_DATES_URL, None, "json")] + [
        (sid, FRED_URL, {"id": sid, "cosd": FRED_START}, "csv") for sid in all_fred_series()]
    for key, url, params, ext in targets:
        if not refetch and _fresh(have.get(key), RAW_FRED, max_age_h):
            stats["skipped"] += 1
            continue
        st, body, hdr = host.request(url, params=params)
        rec = {"key": key, "kind": "fred_series" if params else "frb_release_dates", "url": S.full_url(url, params),
               "http_status": st, "fetched_at": S.utc_now(), "last_modified": hdr.get("last-modified")}
        if st == 200 and body:
            name = f"{key}__{_stamp()}.{ext}"
            tmp = RAW_FRED / (name + ".partial")
            tmp.write_bytes(body)
            tmp.replace(RAW_FRED / name)
            rec.update(file=name, bytes=len(body), sha256=hashlib.sha256(body).hexdigest())
            stats["fred"] += 1
            stats["bytes"] += len(body)
        led.append(rec)
    fled = _ledger(RAW_FRENCH)
    fhave = fled.latest()
    french = list(FRENCH_FACTOR_FILES.values()) + [f"Siccodes{n}.zip" for n in SICCODE_SCHEMES]
    for fname in french:
        if not refetch and _fresh(fhave.get(fname), RAW_FRENCH, max_age_h * 7):
            stats["skipped"] += 1
            continue
        url = FRENCH_BASE + fname
        st, body, hdr = host.request(url)
        rec = {"key": fname, "kind": "french_zip", "url": url, "http_status": st, "fetched_at": S.utc_now(),
               "last_modified": hdr.get("last-modified")}
        if st == 200 and body:
            rec.update(bytes=len(body), sha256=hashlib.sha256(body).hexdigest())
            dest = RAW_FRENCH / Path(fname).stem / _stamp()
            dest.mkdir(parents=True, exist_ok=True)
            members = {}
            with zipfile.ZipFile(io.BytesIO(body)) as z:
                for n in z.namelist():
                    if n.endswith("/"):
                        continue
                    blob = z.read(n)
                    (dest / Path(n).name).write_bytes(blob)
                    members[Path(n).name] = {"bytes": len(blob), "sha256": hashlib.sha256(blob).hexdigest()}
            rec.update(file=dest.relative_to(RAW_FRENCH).as_posix(), members=members, zip_deleted=True)
            stats["french"] += 1
            stats["bytes"] += len(body)
        fled.append(rec)
    return stats


def _latest(raw: Path, key: str) -> dict[str, Any]:
    rec = _ledger(raw).latest().get(key)
    if not rec or rec.get("http_status") != 200 or not rec.get("file"):
        raise RuntimeError(f"{key}: no landed file (run --step download)")
    return rec


def _read_verified(raw: Path, rec: dict[str, Any]) -> bytes:
    blob = (raw / rec["file"]).read_bytes()
    if hashlib.sha256(blob).hexdigest() != rec["sha256"]:
        raise RuntimeError(f"{rec['file']}: sha256 differs from its receipt")
    return blob


def _french_member(rec: dict[str, Any]) -> tuple[str, dt.datetime]:
    d = RAW_FRENCH / rec["file"]
    (name, meta), = rec["members"].items()
    blob = (d / name).read_bytes()
    if hashlib.sha256(blob).hexdigest() != meta["sha256"]:
        raise RuntimeError(f"{d / name}: sha256 differs from its receipt")
    fetched = dt.datetime.fromisoformat(rec["fetched_at"]).astimezone(dt.UTC).replace(tzinfo=None)
    return blob.decode("latin-1"), fetched


# ---------------------------------------------------------------- build
def build() -> dict[str, Any]:
    import pyarrow as pa
    import pyarrow.parquet as pq

    out = C.stage_dir(STAGE)
    receipt: dict[str, Any] = {"clock_rules": ["h10-weekly-monday-v1", "h15-next-business-day-v1",
                                               "vix-cboe-close-v1", "french-fetch-v1"]}
    rel_rec = _latest(RAW_FRED, "h10_release_dates")
    release_dates = parse_release_dates(_read_verified(RAW_FRED, rel_rec))
    receipt["h10_release_dates"] = {"n": len(release_dates), "first": release_dates[0].isoformat(),
                                    "last": release_dates[-1].isoformat(), "sha256": rel_rec["sha256"]}
    catalog: list[dict[str, Any]] = []
    series: dict[str, list[tuple[dt.date, float | None]]] = {}
    sources: dict[str, Any] = {}
    for sid in all_fred_series():
        rec = _latest(RAW_FRED, sid)
        obs = [(d, v) for d, v in parse_fred_csv(_read_verified(RAW_FRED, rec).decode("utf-8-sig"), sid)
               if d >= dt.date.fromisoformat(FRED_START)]
        series[sid] = obs
        sources[sid] = {k: rec.get(k) for k in ("file", "sha256", "bytes", "fetched_at", "last_modified", "url")}
        comp = completeness(obs)
        family = "H10" if sid in H10 else ("H15" if sid in H15 else "VIX")
        catalog.append({"series_id": sid, "family": family,
                        "currency": H10[sid][0] if sid in H10 else None,
                        "quote": H10[sid][1] if sid in H10 else ("percent" if sid in H15 else "index"),
                        "description": H15[sid][0] if sid in H15 else VIX.get(sid, f"H.10 {sid}"),
                        "first_obs": comp["first_obs"], "last_obs": comp["last_obs"],
                        "n_obs": sum(1 for _, v in obs if v is not None),
                        "business_days_expected_2010": comp["expected"], "business_days_missing_2010": comp["missing"],
                        "missing_days_2010": ",".join(comp["missing_days"]),
                        "last_modified": rec.get("last_modified"), "fetched_at": rec.get("fetched_at")})
    # fx_daily (the FUND contract: exactly FX_COLUMNS)
    fx = [r for sid in H10 for r in fx_rows(sid, series[sid], release_dates)]
    fx.sort(key=lambda r: (r[1], r[0]))
    fx_tbl = pa.table({"obs_date": pa.array([r[0] for r in fx], pa.date32()),
                       "currency": pa.array([r[1] for r in fx], pa.string()),
                       "usd_per_ccy": pa.array([r[2] for r in fx], pa.float64()),
                       "series_id": pa.array([r[3] for r in fx], pa.string()),
                       "available_at": pa.array([r[4] for r in fx], pa.timestamp("us"))})
    assert tuple(fx_tbl.column_names) == FX_COLUMNS
    _write(pq, fx_tbl, out / "fx_daily.parquet")
    basis = {}
    for sid in H10:
        for d, v in series[sid]:
            if v is not None:
                b = h10_release_date(d, release_dates)[1]
                basis[b] = basis.get(b, 0) + 1
    receipt["fx_daily"] = {"rows": fx_tbl.num_rows, "currencies": sorted({r[1] for r in fx}), "clock_basis": basis}
    rates = [(d, sid, H15[sid][0], H15[sid][1], v, "H.15", h15_available_at(d))
             for sid in H15 for d, v in series[sid] if v is not None]
    rates.sort(key=lambda r: (r[1], r[0]))
    _write(pq, pa.table({"obs_date": pa.array([r[0] for r in rates], pa.date32()),
                         "series_id": [r[1] for r in rates], "description": [r[2] for r in rates],
                         "tenor_months": pa.array([r[3] for r in rates], pa.float64()),
                         "value_pct": pa.array([r[4] for r in rates], pa.float64()),
                         "release": [r[5] for r in rates],
                         "available_at": pa.array([r[6] for r in rates], pa.timestamp("us"))}),
           out / "rates_daily.parquet")
    vix = [(d, v, "VIXCLS", vix_available_at(d)) for d, v in series["VIXCLS"] if v is not None]
    _write(pq, pa.table({"obs_date": pa.array([r[0] for r in vix], pa.date32()),
                         "vix_close": pa.array([r[1] for r in vix], pa.float64()),
                         "series_id": [r[2] for r in vix],
                         "available_at": pa.array([r[3] for r in vix], pa.timestamp("us"))}),
           out / "vix_daily.parquet")
    receipt["rates_daily"] = {"rows": len(rates)}
    receipt["vix_daily"] = {"rows": len(vix)}
    _write(pq, pa.Table.from_pylist(catalog), out / "series_catalog.parquet")
    receipt["completeness_2010"] = {c["series_id"]: {k: c[k] for k in ("first_obs", "last_obs",
                                                                         "business_days_expected_2010",
                                                                         "business_days_missing_2010")}
                                    for c in catalog}
    receipt["french"] = build_french(pa, pq, out / "french", sources)
    C.write_stage_manifest(STAGE, SCHEMA, MODULES, {
        "rule_text": __doc__, "staleness": "daily series; consumers as-of join on available_at",
        "vintage_policy": "FRED CSV is the current vintage (H.10 / H.15 revisions are rare); French files re-posted "
                          "with every CRSP update (vintage_risk)",
        "sources": {"fred": sources, "h10_release_dates": {k: rel_rec.get(k) for k in ("file", "sha256", "url",
                                                                                         "fetched_at")},
                    "receipts_sha256": {p.parent.name: C.sha256_file(p) for p in
                                        (RAW_FRED / "receipts.jsonl", RAW_FRENCH / "receipts.jsonl")}},
        "receipt": receipt})
    return receipt


def build_french(pa, pq, out: Path, sources: dict[str, Any]) -> dict[str, Any]:
    out.mkdir(parents=True, exist_ok=True)
    res: dict[str, Any] = {}
    for key, fname in FRENCH_FACTOR_FILES.items():
        rec = _latest(RAW_FRENCH, fname)
        text, fetched = _french_member(rec)
        cols, rows, vintage = parse_french_table(text)
        names = [factor_column(c) for c in cols]
        datecol = "date" if key.endswith("daily") else "month"
        data: dict[str, Any] = {datecol: pa.array([r[0] for r in rows], pa.date32())}
        for i, n in enumerate(names):
            data[n] = pa.array([r[1][i] if i < len(r[1]) else None for r in rows], pa.float64())
        data["vintage"] = [vintage] * len(rows)
        data["available_at"] = pa.array([fetched] * len(rows), pa.timestamp("us"))
        data["vintage_risk"] = [True] * len(rows)
        _write(pq, pa.table(data), out / f"{key}.parquet")
        res[key] = {"rows": len(rows), "columns": names, "first": rows[0][0].isoformat(),
                    "last": rows[-1][0].isoformat(), "vintage": vintage}
        sources[fname] = {k: rec.get(k) for k in ("file", "sha256", "fetched_at", "last_modified", "url", "members")}
    sic_rows: list[tuple] = []
    fetched_at: dict[int, dt.datetime] = {}
    for n in SICCODE_SCHEMES:
        rec = _latest(RAW_FRENCH, f"Siccodes{n}.zip")
        text, fetched_at[n] = _french_member(rec)
        rows = parse_siccodes(text, n)
        sic_rows.extend(rows)
        res[f"siccodes{n}"] = {"industries": len({r[1] for r in rows}), "ranges": sum(1 for r in rows if r[4] is not None)}
        sources[f"Siccodes{n}.zip"] = {k: rec.get(k) for k in ("file", "sha256", "fetched_at", "url", "members")}
    cols = ("scheme", "industry_no", "industry_short", "industry_name", "sic_lo", "sic_hi", "range_desc")
    tbl = {c: [r[i] for r in sic_rows] for i, c in enumerate(cols)}
    tbl["available_at"] = pa.array([fetched_at[r[0]] for r in sic_rows], pa.timestamp("us"))
    _write(pq, pa.table(tbl), out / "siccodes.parquet")
    return res


def _write(pq, table, dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_name(dest.name + ".partial")
    pq.write_table(table, tmp, compression="zstd")
    tmp.replace(dest)


def signal_fx_ready() -> str:
    path = C.build_root() / STAGE / "fx_daily.parquet"
    line = f"FX READY {path.as_posix()} {C.sha256_file(path)}"
    sig = C.PACKAGE_ROOT.parent / ".superpowers" / "sdd" / "tier1-v3" / "signals.md"
    with sig.open("a", encoding="utf-8") as fh:
        fh.write(line + "\n")
    return line


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("step", nargs="?", choices=("fetch", "build", "all"), default="all")
    ap.add_argument("--refetch", action="store_true")
    ap.add_argument("--signal", action="store_true", help="append FX READY to the sprint signals file")
    args = ap.parse_args(argv)
    if args.step in ("fetch", "all"):
        print(json.dumps(download(refetch=args.refetch), default=str), flush=True)
    if args.step in ("build", "all"):
        rec = build()
        print(json.dumps({k: v for k, v in rec.items() if k != "completeness_2010"}, default=str)[:4000], flush=True)
        if args.signal:
            print(signal_fx_ready(), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
