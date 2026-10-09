#!/usr/bin/env python3
r"""Build a historical earnings-announcement calendar for the atx-vol cross-section.

Emits a TSV of ``ticker / earn_date / session_hint / announce_ts_et / source / fetched_utc``
for every name in the universe file(s) -- the days-to-earnings feature the single-name
vega book needs, which `vrp_panel.hpp` currently does without.

PRIMARY SOURCE -- SEC EDGAR, form 8-K Item 2.02
-----------------------------------------------
Item 2.02 ("Results of Operations and Financial Condition") is the filing a US issuer
makes to furnish its earnings release. We enumerate it from the per-company submissions
API (``data.sec.gov/submissions/CIK##########.json``), which carries an ``items`` field
per filing and, crucially, ``acceptanceDateTime``.

``acceptanceDateTime`` is genuine UTC, not naive ET -- verified: AAPL's Q3 print is
20:30Z in summer and its Q1 print 21:30Z in winter, both 16:30 America/New_York. Parsing
it as ET would put Apple's release at 8:30pm. We therefore parse as UTC and convert to
America/New_York, which reproduces known wire times to the minute (JPM 06:30, XOM 06:31,
WMT 06:58, MSFT 16:04, AAPL 16:30).

Session hint is derived from that ET timestamp against regular market hours:

    t <  09:30 ET  -> bmo       (event lands in the D-1 -> D return)
    t >= 16:00 ET  -> amc       (event lands in the D   -> D+1 return)
    otherwise      -> intraday  (its own category -- NOT forced into bmo/amc)

The raw ET timestamp is kept in its own column so downstream work can re-derive the
rule rather than trusting this one. Getting the session wrong shifts the event window a
full day and can invert the sign of a pre-announcement signal, so it is not a cosmetic
field and unknown-time filings are never guessed into a bucket.

Announcement DATE is the ET date of acceptance, never ``filingDate``: EDGAR rolls
filingDate to the next business day for anything accepted after 17:30 ET, which would
silently push late-afternoon AMC prints onto the wrong calendar day.

SECONDARY SOURCE -- yfinance
----------------------------
Used for (a) an independent cross-check of every EDGAR date/session, and (b) filling
tickers EDGAR structurally misses -- foreign private issuers file 6-K, which has no
Item 2.02, so ADRs have no 8-K earnings trail. yfinance's clock is a coarse Yahoo
estimate (a 16:00/06:00 bucket), not a wire time; rows sourced from it say so in the
``source`` column and their timestamps should be trusted only at bmo/amc granularity.

NOT POINT-IN-TIME. Both sources record when an announcement *happened*, not when the
market knew it was scheduled. See the README beside the output file.

Usage
-----
    python fetch_earnings_calendar.py                     # edgar + yfinance, then build
    python fetch_earnings_calendar.py --sources edgar     # edgar only
    python fetch_earnings_calendar.py --build-only        # rebuild TSV from cache
    python fetch_earnings_calendar.py --refresh AAPL,MSFT # re-fetch named tickers
    python fetch_earnings_calendar.py --end 2026-12-31    # include forward dates
    python fetch_earnings_calendar.py --report            # QA + cross-check stats
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import gzip
import json
import os
import random
import statistics
import sys
import time
import urllib.error
import urllib.request
from collections import defaultdict
from pathlib import Path
from zoneinfo import ZoneInfo

# --------------------------------------------------------------------------------------
# config
# --------------------------------------------------------------------------------------

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_UNIVERSE = [
    REPO_ROOT / "atx-vol" / "data" / "universe" / "xsec_2026-08.csv",
    REPO_ROOT / "atx-vol" / "data" / "universe" / "sp100_2026-07.csv",
]
DEFAULT_OUT = Path(r"C:\atx-data\earnings\earnings_dates_v1.tsv")

DEFAULT_START = "2024-06-01"
DEFAULT_END = "2026-08-17"

SRC_EDGAR = "sec_edgar_8k_item202"
SRC_YF = "yfinance"

ET = ZoneInfo("America/New_York")
MARKET_OPEN = dt.time(9, 30)
MARKET_CLOSE = dt.time(16, 0)

# SEC asks for a descriptive UA with a real contact and caps traffic at 10 req/s.
SEC_UA = os.environ.get(
    "SEC_USER_AGENT", "atx-vol research (nathan.tormaschy2@gmail.com)"
)
SEC_SLEEP = 0.13  # ~7/s, comfortably inside the limit

TICKER_MAP_URL = "https://www.sec.gov/files/company_tickers.json"
SUBMISSIONS_URL = "https://data.sec.gov/submissions/CIK{cik:010d}.json"

YF_FETCH_LIMIT = 40

# Several issuers furnish MORE than one Item 2.02 8-K per quarter: Tesla's quarterly
# production & deliveries release (~09:05, three weeks ahead of the print), Occidental's
# preliminary-results 8-K, EOG's quarterly commodity-position update, AbbVie's early-month
# filing. Only the last of such a cluster is the actual earnings report -- pre-announcements
# by construction precede it, and true 8-K/A amendments are already excluded because we
# match form == "8-K" exactly.
#
# So: cluster filings whose dates fall within DEDUP_WINDOW_DAYS of the cluster's FIRST
# member (never of the running last, which could chain across a real quarter) and keep the
# LAST. Validated against yfinance over the full 617-name universe -- exact-date agreement
# 90.8% -> 95.4%, event-window agreement 92.5% -> 97.1%, spurious EDGAR-only rows 491 -> 242.
# A 45d span cannot merge two real prints, which are ~91 days apart.
DEDUP_WINDOW_DAYS = 45


def to_yahoo(ticker: str) -> str:
    """Universe files carry BRK.B / BF.B; Yahoo and SEC both want BRK-B / BF-B."""
    return ticker.replace(".", "-")


# --------------------------------------------------------------------------------------
# universe
# --------------------------------------------------------------------------------------

def read_universe(paths: list[Path]) -> list[str]:
    """Union of symbols across universe files, first-seen order preserved.

    Handles both layouts in data/universe: headerless ``symbol<TAB>weight`` and a
    headered TSV with a ``symbol`` column.
    """
    seen: dict[str, None] = {}
    for path in paths:
        if not path.exists():
            print(f"[warn] universe file missing, skipping: {path}", file=sys.stderr)
            continue
        with open(path, newline="", encoding="utf-8") as fh:
            rows = list(csv.reader(fh, delimiter="\t"))
        if not rows:
            continue
        header = [c.strip().lower() for c in rows[0]]
        if "symbol" in header:
            col, body = header.index("symbol"), rows[1:]
        else:
            col, body = 0, rows
        for row in body:
            if row and len(row) > col and row[col].strip():
                seen.setdefault(row[col].strip().upper(), None)
    return list(seen)


# --------------------------------------------------------------------------------------
# session classification
# --------------------------------------------------------------------------------------

def classify_session(ts_et: dt.datetime) -> str:
    """bmo / amc / intraday from an ET announcement timestamp."""
    clock = ts_et.time()
    if clock < MARKET_OPEN:
        return "bmo"
    if clock >= MARKET_CLOSE:
        return "amc"
    return "intraday"


def event_anchor(row: dict) -> dt.date:
    """The close-to-close return the announcement lands in, keyed by its opening date.

    An `amc` print on D and a `bmo` print on D+1 move the *same* return (D -> D+1), so
    this is the quantity a days-to-earnings feature actually cares about -- and the right
    unit for comparing two sources that disagree on the raw calendar date.
    """
    d = dt.date.fromisoformat(row["date"])
    return d - dt.timedelta(days=1) if row["session"] == "bmo" else d


# --------------------------------------------------------------------------------------
# checkpointing
# --------------------------------------------------------------------------------------

def load_checkpoint(path: Path) -> dict[str, dict]:
    done: dict[str, dict] = {}
    if not path.exists():
        return done
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                continue  # tolerate a torn final line from a hard kill
            done[rec["ticker"]] = rec  # later record wins -> --refresh works by append
    return done


def append_checkpoint(fh, rec: dict) -> None:
    fh.write(json.dumps(rec) + "\n")
    fh.flush()
    os.fsync(fh.fileno())  # a kill must not lose the line we just wrote


# --------------------------------------------------------------------------------------
# EDGAR
# --------------------------------------------------------------------------------------

def sec_get_json(url: str, retries: int = 4) -> dict:
    last: Exception | None = None
    for attempt in range(retries):
        try:
            req = urllib.request.Request(
                url, headers={"User-Agent": SEC_UA, "Accept-Encoding": "gzip"}
            )
            with urllib.request.urlopen(req, timeout=45) as resp:
                raw = resp.read()
                if resp.headers.get("Content-Encoding") == "gzip":
                    raw = gzip.decompress(raw)
                return json.loads(raw)
        except Exception as exc:  # noqa: BLE001
            last = exc
            code = getattr(exc, "code", None)
            if code == 404:
                raise
            # back off hard on throttling; SEC returns 403 when it thinks you are a bot
            time.sleep(min(30.0, 2.0 * (2**attempt)) + random.random())
    raise last  # type: ignore[misc]


def load_cik_map(cache: Path) -> dict[str, int]:
    """ticker -> CIK, cached on disk (the map is ~10k entries and rarely changes)."""
    if cache.exists():
        try:
            return {k: int(v) for k, v in json.loads(cache.read_text()).items()}
        except Exception:  # noqa: BLE001 - refetch on any corruption
            pass
    data = sec_get_json(TICKER_MAP_URL)
    out = {
        str(row["ticker"]).upper(): int(row["cik_str"])
        for row in data.values()
        if row.get("ticker")
    }
    cache.parent.mkdir(parents=True, exist_ok=True)
    cache.write_text(json.dumps(out))
    return out


def _iter_filing_blocks(cik: int, need_back_to: dt.date):
    """Yield EDGAR filing blocks, following pagination only if `recent` is too shallow.

    `filings.recent` holds the last ~1000 filings. For heavy filers that may not reach
    our window start, in which case the older shards in `filings.files` are pulled too.
    """
    doc = sec_get_json(SUBMISSIONS_URL.format(cik=cik))
    recent = doc["filings"]["recent"]
    yield recent

    dates = recent.get("filingDate") or []
    if not dates:
        return
    oldest = dt.date.fromisoformat(dates[-1])
    if oldest <= need_back_to:
        return  # `recent` already spans the window
    for shard in doc["filings"].get("files", []):
        time.sleep(SEC_SLEEP)
        yield sec_get_json(f"https://data.sec.gov/submissions/{shard['name']}")
        try:
            if dt.date.fromisoformat(shard.get("filingFrom", "9999-12-31")) <= need_back_to:
                break
        except ValueError:
            continue


def fetch_edgar_one(ticker: str, cik: int | None, need_back_to: dt.date) -> dict:
    """Fetch one ticker's Item 2.02 8-K trail. Returns a checkpoint record; never raises."""
    now = dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat()
    rec = {"ticker": ticker, "cik": cik, "fetched_utc": now, "rows": [],
           "status": "ok", "error": ""}
    if cik is None:
        rec["status"] = "no_cik"
        return rec
    try:
        blocks = list(_iter_filing_blocks(cik, need_back_to))
    except Exception as exc:  # noqa: BLE001 - one bad ticker must not kill the run
        rec["status"] = "error"
        rec["error"] = f"{type(exc).__name__}: {exc}"
        return rec

    for blk in blocks:
        forms = blk.get("form") or []
        for i, form in enumerate(forms):
            if form != "8-K":
                continue
            items = {s.strip() for s in (blk["items"][i] or "").split(",")}
            if "2.02" not in items:
                continue
            acc = blk["acceptanceDateTime"][i]
            if not acc:
                continue
            try:
                # genuine UTC (see module docstring) -> convert to exchange-local ET
                ts_et = dt.datetime.fromisoformat(
                    acc.replace("Z", "+00:00")
                ).astimezone(ET)
            except ValueError:
                continue
            rec["rows"].append({
                "date": ts_et.strftime("%Y-%m-%d"),
                "ts_et": ts_et.strftime("%Y-%m-%dT%H:%M:%S%z"),
                "session": classify_session(ts_et),
                "filing_date": blk["filingDate"][i],
                "accession": blk["accessionNumber"][i],
            })
    if not rec["rows"]:
        rec["status"] = "empty"
    return rec


def run_edgar(tickers: list[str], ckpt: Path, cikcache: Path, start: str,
              refresh: set[str]) -> None:
    done = load_checkpoint(ckpt)
    todo = [t for t in tickers if t not in done or t in refresh]
    print(f"[edgar] {len(tickers)} universe / {len(done)} cached / {len(todo)} to fetch",
          file=sys.stderr)
    if not todo:
        return

    cikmap = load_cik_map(cikcache)
    need_back_to = dt.date.fromisoformat(start)
    ckpt.parent.mkdir(parents=True, exist_ok=True)
    t0 = time.time()
    tally: dict[str, int] = defaultdict(int)
    with open(ckpt, "a", encoding="utf-8") as fh:
        for i, ticker in enumerate(todo, 1):
            cik = cikmap.get(to_yahoo(ticker)) or cikmap.get(ticker)
            rec = fetch_edgar_one(ticker, cik, need_back_to)
            append_checkpoint(fh, rec)
            tally[rec["status"]] += 1
            if i % 50 == 0 or i == len(todo):
                rate = i / max(1e-9, time.time() - t0)
                print(f"[edgar] {i}/{len(todo)} {dict(tally)} {rate:.1f}/s "
                      f"eta={(len(todo)-i)/max(1e-9,rate)/60:.1f}m", file=sys.stderr)
            time.sleep(SEC_SLEEP)


# --------------------------------------------------------------------------------------
# yfinance
# --------------------------------------------------------------------------------------

def fetch_yf_one(ticker: str) -> dict:
    import yfinance as yf

    now = dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat()
    rec = {"ticker": ticker, "fetched_utc": now, "rows": [], "status": "ok", "error": ""}
    try:
        df = yf.Ticker(to_yahoo(ticker)).get_earnings_dates(limit=YF_FETCH_LIMIT)
    except Exception as exc:  # noqa: BLE001
        rec["status"] = "error"
        rec["error"] = f"{type(exc).__name__}: {exc}"
        return rec
    if df is None or df.empty:
        rec["status"] = "empty"
        return rec

    idx = df.index
    try:
        idx = idx.tz_localize(ET) if idx.tz is None else idx.tz_convert(ET)
    except Exception:  # noqa: BLE001 - fall back to whatever Yahoo gave us
        pass
    for ts in idx:
        rec["rows"].append({
            "date": ts.strftime("%Y-%m-%d"),
            "ts_et": ts.strftime("%Y-%m-%dT%H:%M:%S%z"),
            "session": classify_session(ts),
        })
    return rec


def run_yfinance(tickers: list[str], ckpt: Path, sleep: float, refresh: set[str]) -> None:
    done = load_checkpoint(ckpt)
    todo = [t for t in tickers if t not in done or t in refresh]
    print(f"[yfin ] {len(tickers)} universe / {len(done)} cached / {len(todo)} to fetch",
          file=sys.stderr)
    if not todo:
        return

    ckpt.parent.mkdir(parents=True, exist_ok=True)
    t0 = time.time()
    tally: dict[str, int] = defaultdict(int)
    with open(ckpt, "a", encoding="utf-8") as fh:
        for i, ticker in enumerate(todo, 1):
            rec = fetch_yf_one(ticker)
            if rec["status"] == "error":  # one retry covers transient throttling
                time.sleep(min(30.0, 5.0 + random.random() * 5.0))
                rec = fetch_yf_one(ticker)
            append_checkpoint(fh, rec)
            tally[rec["status"]] += 1
            if i % 50 == 0 or i == len(todo):
                rate = i / max(1e-9, time.time() - t0)
                print(f"[yfin ] {i}/{len(todo)} {dict(tally)} {rate:.2f}/s "
                      f"eta={(len(todo)-i)/max(1e-9,rate)/60:.1f}m", file=sys.stderr)
            time.sleep(sleep + random.random() * sleep)


# --------------------------------------------------------------------------------------
# build
# --------------------------------------------------------------------------------------

def _ts_et(r: dict) -> dt.datetime | None:
    """The row's ET announcement instant, tolerating the older cache layout.

    Revision 1 of this script stored a bare ``time`` ("HH:MM") beside ``date`` instead of
    a full ``ts_et``; rebuilding it here means an existing yfinance cache stays usable
    instead of forcing a 25-minute refetch.
    """
    ts = r.get("ts_et")
    if ts:
        try:
            return dt.datetime.fromisoformat(ts)
        except ValueError:
            return None
    clock = r.get("time")
    if not clock:
        return None
    try:
        naive = dt.datetime.fromisoformat(f"{r['date']}T{clock}")
    except ValueError:
        return None
    return naive.replace(tzinfo=ET)


def _session_of(r: dict) -> str:
    """Re-derive the session hint from the stored timestamp.

    Deliberately NOT trusting any label cached alongside it: the cache holds the raw
    instant, the build owns the rule. That keeps an on-disk cache written by an older
    revision from freezing an older classification into the artifact.
    """
    ts = _ts_et(r)
    return classify_session(ts) if ts else "unknown"


def _window_rows(rec: dict | None, lo: dt.date, hi: dt.date,
                 keep: str = "last") -> list[dict]:
    """In-window rows for one ticker, deduped to one entry per announcement event.

    `keep` selects which member of a same-quarter cluster survives -- "last" for EDGAR
    (see DEDUP_WINDOW_DAYS), "first" for yfinance, which carries one row per print and
    so needs only same-day de-duplication.
    """
    if not rec:
        return []
    rows = sorted(
        (r for r in rec["rows"] if lo <= dt.date.fromisoformat(r["date"]) <= hi),
        key=lambda r: (r["date"], r.get("ts_et", "")),
    )
    kept: list[dict] = []
    anchor: dt.date | None = None
    for r in rows:
        d = dt.date.fromisoformat(r["date"])
        # cluster against the FIRST member so a run of near-misses can never chain
        # across a genuine ~91-day quarter boundary
        if anchor is not None and (d - anchor).days <= DEDUP_WINDOW_DAYS:
            if keep == "last":
                kept[-1] = r
            continue
        kept.append(r)
        anchor = d
    out = []
    for r in kept:
        ts = _ts_et(r)
        out.append(dict(
            r,
            session=_session_of(r),
            ts_et=ts.strftime("%Y-%m-%dT%H:%M:%S%z") if ts else "",
        ))
    return out


def build_tsv(tickers: list[str], edgar_ckpt: Path, yf_ckpt: Path, out: Path,
              start: str, end: str) -> tuple[list[dict], dict]:
    edgar = load_checkpoint(edgar_ckpt)
    yfin = load_checkpoint(yf_ckpt)
    lo, hi = dt.date.fromisoformat(start), dt.date.fromisoformat(end)

    rows: list[dict] = []
    filled: list[str] = []
    for ticker in tickers:
        e_rows = _window_rows(edgar.get(ticker), lo, hi, keep="last")
        if e_rows:
            rec = edgar[ticker]
            src, fetched = SRC_EDGAR, rec["fetched_utc"]
            use = e_rows
        else:
            y_rows = _window_rows(yfin.get(ticker), lo, hi, keep="first")
            if not y_rows:
                continue
            rec = yfin[ticker]
            src, fetched = SRC_YF, rec["fetched_utc"]
            use = y_rows
            filled.append(ticker)
        for r in use:
            rows.append({
                "ticker": ticker, "earn_date": r["date"], "session_hint": r["session"],
                "announce_ts_et": r.get("ts_et", ""), "source": src,
                "fetched_utc": fetched,
            })

    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix(out.suffix + ".tmp")
    cols = ["ticker", "earn_date", "session_hint", "announce_ts_et", "source",
            "fetched_utc"]
    with open(tmp, "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh, delimiter="\t", lineterminator="\n")
        w.writerow(cols)
        for r in rows:
            w.writerow([r[c] for c in cols])
    os.replace(tmp, out)  # atomic: a reader never sees a half-written calendar
    print(f"[build] {len(rows)} rows -> {out} "
          f"(yfinance-filled tickers: {len(filled)})", file=sys.stderr)
    return rows, {"filled": filled}


# --------------------------------------------------------------------------------------
# QA + cross-check
# --------------------------------------------------------------------------------------

def report(tickers: list[str], rows: list[dict], edgar_ckpt: Path, yf_ckpt: Path,
           start: str, end: str, meta: dict) -> None:
    lo, hi = dt.date.fromisoformat(start), dt.date.fromisoformat(end)
    years = (hi - lo).days / 365.25

    per: dict[str, list[dt.date]] = defaultdict(list)
    hints: dict[str, int] = defaultdict(int)
    srcs: dict[str, int] = defaultdict(int)
    for r in rows:
        per[r["ticker"]].append(dt.date.fromisoformat(r["earn_date"]))
        hints[r["session_hint"]] += 1
        srcs[r["source"]] += 1

    zero = [t for t in tickers if t not in per]
    counts = sorted(len(v) for v in per.values())
    all_dates = [d for v in per.values() for d in v]

    print("=" * 78)
    print(f"window             {start} .. {end}  ({years:.2f}y, ~{4*years:.1f} quarters)")
    print(f"universe           {len(tickers)}")
    print(f"tickers >=1 date   {len(per)}")
    print(f"tickers 0 dates    {len(zero)}")
    print(f"total rows         {len(rows)}")
    if counts:
        print(f"dates/ticker       median={statistics.median(counts):.1f} "
              f"mean={statistics.mean(counts):.2f} min={counts[0]} max={counts[-1]}")
    if all_dates:
        print(f"date range         {min(all_dates)} .. {max(all_dates)}")
    if per:
        theo = len(per) * 4.0 * years
        print(f"quarter coverage   {len(rows)}/{theo:.0f} = {100*len(rows)/theo:.1f}% "
              f"of ~4/yr over covered tickers")
        theo_all = len(tickers) * 4.0 * years
        print(f"                   {len(rows)}/{theo_all:.0f} = "
              f"{100*len(rows)/theo_all:.1f}% over full universe")
    print("source             " + "  ".join(f"{k}={v}" for k, v in sorted(srcs.items())))
    print("session_hint       " + "  ".join(
        f"{k}={v} ({100*v/max(1,len(rows)):.1f}%)" for k, v in sorted(hints.items())))

    # ---- spacing anomalies -------------------------------------------------------
    anomalies = []
    for t, v in per.items():
        ds = sorted(v)
        for a, b in zip(ds, ds[1:]):
            gap = (b - a).days
            if gap < 45 or gap > 200:
                anomalies.append((t, a.isoformat(), b.isoformat(), gap))
    anomalies.sort(key=lambda x: x[3])
    n_gaps = sum(max(0, len(v) - 1) for v in per.values())
    print(f"spacing anomalies  {len(anomalies)} of {n_gaps} gaps "
          f"({100*len(anomalies)/max(1,n_gaps):.2f}%)  [<45d or >200d]")
    for row in anomalies[:30]:
        print(f"    {row[0]:<8} {row[1]} -> {row[2]}  {row[3]}d")
    if len(anomalies) > 30:
        print(f"    ... {len(anomalies)-30} more")

    # ---- EDGAR vs yfinance cross-check -------------------------------------------
    edgar = load_checkpoint(edgar_ckpt)
    yfin = load_checkpoint(yf_ckpt)
    both = agree_d = off1 = only_e = only_y = anchor_ok = 0
    sess_agree = sess_dis = 0
    sess_examples: list[str] = []
    for t in tickers:
        e = {r["date"]: r for r in _window_rows(edgar.get(t), lo, hi, keep="last")}
        y = {r["date"]: r for r in _window_rows(yfin.get(t), lo, hi, keep="first")}
        if not e or not y:
            continue
        both += 1
        ydates = sorted(dt.date.fromisoformat(d) for d in y)
        yanchors = {event_anchor(r) for r in y.values()}
        for ds, er in e.items():
            d = dt.date.fromisoformat(ds)
            if event_anchor(er) in yanchors:
                anchor_ok += 1
            if ds in y:
                agree_d += 1
                ysess = y[ds]["session"]
                if ysess == er["session"] or "intraday" in (ysess, er["session"]):
                    sess_agree += 1
                else:
                    sess_dis += 1
                    sess_examples.append(
                        f"{t} {ds} edgar={er['session']}@{er['ts_et'][11:16]} yf={ysess}")
                continue
            near = min((abs((d - yd).days) for yd in ydates), default=99)
            if near <= 1:
                off1 += 1
            else:
                only_e += 1
        only_y += sum(1 for ds in y if ds not in e)

    tot = agree_d + off1 + only_e
    print(f"cross-check        {both} tickers present in BOTH edgar and yfinance")
    if tot:
        print(f"  date exact match {agree_d}/{tot} = {100*agree_d/tot:.2f}%")
        print(f"  off-by-1-day     {off1} ({100*off1/tot:.2f}%)")
        print(f"  edgar-only       {only_e} ({100*only_e/tot:.2f}%)   yfinance-only {only_y}")
        # The number that actually matters: an amc print on D and a bmo print on D+1 are
        # the SAME event window, so most off-by-1 date differences are not disagreements.
        print(f"  SAME event window {anchor_ok}/{tot} = {100*anchor_ok/tot:.2f}%  "
              f"(bmo D -> D-1->D return; amc D -> D->D+1 return)")
    if sess_agree + sess_dis:
        tot_s = sess_agree + sess_dis
        print(f"  session agree    {sess_agree}/{tot_s} = {100*sess_agree/tot_s:.2f}% "
              f"(on exact-date matches; intraday excluded)")
        print(f"  session DISAGREE {sess_dis} -- listed in full (late-filed 8-K risk):")
    for s in sess_examples:
        print(f"    ! {s}")

    if meta.get("filled"):
        print(f"yfinance-filled ({len(meta['filled'])}): {' '.join(meta['filled'])}")
    if zero:
        print(f"zero-coverage ({len(zero)}): {' '.join(zero)}")
    print("=" * 78)


# --------------------------------------------------------------------------------------

def main() -> int:
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--universe", action="append", type=Path, default=None,
                   help="universe file(s); repeatable. default: xsec + sp100")
    p.add_argument("--out", type=Path, default=DEFAULT_OUT)
    p.add_argument("--cache", type=Path, default=None,
                   help="checkpoint dir. default: <out-dir>/.cache")
    p.add_argument("--start", default=DEFAULT_START)
    p.add_argument("--end", default=DEFAULT_END,
                   help="inclusive; push past today for unconfirmed forward dates")
    p.add_argument("--sources", default="edgar,yfinance",
                   help="comma list: edgar,yfinance (default both)")
    p.add_argument("--sleep", type=float, default=0.25,
                   help="yfinance base inter-request sleep, jittered up to 2x")
    p.add_argument("--build-only", action="store_true", help="rebuild TSV from cache")
    p.add_argument("--refresh", default="", help="comma-separated tickers to re-fetch")
    p.add_argument("--report", action="store_true", help="print QA + cross-check stats")
    args = p.parse_args()

    tickers = read_universe(args.universe or DEFAULT_UNIVERSE)
    if not tickers:
        print("[fatal] empty universe", file=sys.stderr)
        return 2

    cache = args.cache or (args.out.parent / ".cache")
    edgar_ckpt = cache / "edgar_raw.jsonl"
    yf_ckpt = cache / "earnings_raw.jsonl"  # name kept: reuses an existing yfinance cache
    cikcache = cache / "company_tickers.json"
    refresh = {t.strip().upper() for t in args.refresh.split(",") if t.strip()}
    sources = {s.strip().lower() for s in args.sources.split(",") if s.strip()}

    if not args.build_only:
        if "edgar" in sources:
            run_edgar(tickers, edgar_ckpt, cikcache, args.start, refresh)
        if "yfinance" in sources:
            run_yfinance(tickers, yf_ckpt, args.sleep, refresh)

    rows, meta = build_tsv(tickers, edgar_ckpt, yf_ckpt, args.out, args.start, args.end)
    if args.report:
        report(tickers, rows, edgar_ckpt, yf_ckpt, args.start, args.end, meta)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
