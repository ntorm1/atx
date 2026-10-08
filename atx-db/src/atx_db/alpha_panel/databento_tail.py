"""Isolated, capped Databento daily-price acquisition; never promotes total returns.

The sole paid request is the explicitly authorized EQUS.SUMMARY daily scope.
An exclusive attempt receipt prevents a failed/partial request being rebilled on
restart. Metadata and dated symbology are free. No reference allocations, SDK,
definitions, statistics, or canonical panel mutations are used.
"""
from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import os
from decimal import Decimal
from pathlib import Path

import requests

from . import common as C

DATASET = "EQUS.SUMMARY"
SCHEMA = "ohlcv-1d"
START = "2026-09-14"
END = "2026-10-01"  # exclusive: Oct 1 daily close is not yet published
CAP = Decimal("0.30")
BASE = "https://hist.databento.com/v0/"
SCOPE = dict(dataset=DATASET, schema=SCHEMA, symbols="ALL_SYMBOLS", start=START,
             end=END, stype_in="raw_symbol", stype_out="instrument_id")
DEFAULT_BRONZE = C.PACKAGE_ROOT / "data/raw/databento/equs_summary_ohlcv_1d" / f"{START}_{END}"
SILVER_STAGE = "provider_prices_databento_tail"


def now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat()


def sql_path(path: Path) -> str:
    return "'" + path.as_posix().replace("'", "''") + "'"


def api_key(env_file: Path | None = None) -> str:
    key = os.environ.get("DATABENTO_API_KEY")
    if not key:
        env_file = env_file or C.PACKAGE_ROOT.parent / ".env"
        for line in env_file.read_text(encoding="utf-8-sig").splitlines():
            name, sep, value = line.partition("=")
            if sep and name.strip() == "DATABENTO_API_KEY":
                key = value.strip().strip("\"'")
                break
    if not key:
        raise RuntimeError("DATABENTO_API_KEY unavailable")
    return key


def check_quote(value) -> str:
    amount = Decimal(str(value))
    if not amount.is_finite() or amount < 0 or amount > CAP:
        raise RuntimeError("Market-data quote exceeds the authorized USD 0.30 cap")
    return str(amount)


class Client:
    def __init__(self, key: str):
        self.session = requests.Session()
        self.session.auth = (key, "")
        # requests' default adapter does not retry; never retry a paid stream.

    def json(self, method: str, params: dict, *, post=False):
        try:
            response = (self.session.post(BASE + method, data=params, timeout=(15, 90))
                        if post else self.session.get(BASE + method, params=params, timeout=(15, 90)))
        except requests.RequestException:
            raise RuntimeError(f"Databento {method}: transport failure") from None
        if response.status_code != 200:
            raise RuntimeError(f"Databento {method}: HTTP {response.status_code}")
        try:
            return response.json()
        except ValueError:
            raise RuntimeError(f"Databento {method}: invalid JSON") from None

    def download(self, dest: Path):
        params = dict(SCOPE, encoding="csv", compression="none", pretty_px="true",
                      pretty_ts="true", map_symbols="false")
        try:
            with self.session.post(BASE + "timeseries.get_range", data=params,
                                   timeout=(15, 120), stream=True) as response:
                if response.status_code != 200:
                    raise RuntimeError(f"Databento paid stream: HTTP {response.status_code}")
                with dest.open("xb") as output:
                    for chunk in response.iter_content(1 << 20):
                        output.write(chunk)
            return {"http_status": 200, "content_type": response.headers.get("content-type")}
        except requests.RequestException:
            raise RuntimeError("Databento paid stream transport failure; automatic rebilling prohibited") from None


def acquire(bronze: Path, client: Client) -> dict:
    bronze.mkdir(parents=True, exist_ok=True)
    raw, receipt = bronze / "bars.csv", bronze / "download_receipt.json"
    if receipt.exists():
        result = C.read_json(receipt)
        if result["scope"] != SCOPE or not raw.exists() or C.sha256_file(raw) != result["raw_sha256"]:
            raise RuntimeError("Immutable bronze receipt/raw mismatch")
        return result
    attempt = bronze / "paid_request_attempt.json"
    if attempt.exists() or raw.exists():
        raise RuntimeError("Paid request already attempted without a complete receipt; no automatic retry")
    availability = client.json("metadata.get_dataset_range", {"dataset": DATASET})
    count = client.json("metadata.get_record_count", SCOPE)
    billed_size = client.json("metadata.get_billable_size", SCOPE)
    # Recheck exact authorized scope immediately before the paid request.
    quote = check_quote(client.json("metadata.get_cost", SCOPE))
    quoted = dict(scope=SCOPE, checked_at=now(), quoted_usd=quote, authorized_cap_usd=str(CAP),
                  record_count=count, billable_bytes=billed_size, dataset_range=availability)
    C.write_json_atomic(bronze / "quote.json", quoted)
    # Exclusive-create prevents concurrent processes issuing this scope twice.
    with attempt.open("x", encoding="utf-8") as fh:
        json.dump(dict(quoted, status="request_started_no_automatic_retry"), fh, indent=2)
    headers = client.download(bronze / "bars.csv.partial")
    os.replace(bronze / "bars.csv.partial", raw)
    result = dict(quoted, completed_at=now(), raw_sha256=C.sha256_file(raw),
                  raw_bytes=raw.stat().st_size, response=headers,
                  actual_billed_usd=None, billing_note="Metadata quote; invoice/account debits not exposed by this API")
    C.write_json_atomic(receipt, result)
    return result


def instrument_ids(raw: Path) -> list[int]:
    with raw.open(newline="", encoding="utf-8") as fh:
        return sorted({int(row["instrument_id"]) for row in csv.DictReader(fh)})


def resolve(bronze: Path, client: Client, ids: list[int]) -> list[dict]:
    rows = []
    for i in range(0, len(ids), 2000):
        chunk = ids[i:i + 2000]
        params = dict(dataset=DATASET, symbols=",".join(map(str, chunk)), stype_in="instrument_id",
                      stype_out="raw_symbol", start_date=START, end_date=END)
        dest = bronze / "symbology" / f"chunk-{i // 2000:03}.json"
        if dest.exists():
            saved = C.read_json(dest)
            if saved["request"] != params:
                raise RuntimeError("Cached dated symbology scope mismatch")
        else:
            saved = dict(request=params, fetched_at=now(), response=client.json("symbology.resolve", params, post=True))
            C.write_json_atomic(dest, saved)
        for instrument, intervals in saved["response"]["result"].items():
            for interval in intervals:
                rows.append(dict(instrument_id=int(instrument), start_date=interval["d0"],
                                 end_date=interval["d1"], provider_symbol=interval["s"]))
    return rows


def normalize(raw: Path, mappings: list[dict], silver: Path, receipt: dict) -> dict:
    """Preserve every provider record; flag invalid bars rather than invent returns."""
    silver.mkdir(parents=True, exist_ok=True)
    con = C.connect(memory="220MB", threads=1, temp_dir=silver / "_tmp")
    try:
        con.execute("CREATE TABLE symbols(instrument_id BIGINT,start_date DATE,end_date DATE,provider_symbol VARCHAR)")
        if mappings:
            con.executemany("INSERT INTO symbols VALUES (?,?,?,?)", [(r["instrument_id"], r["start_date"], r["end_date"], r["provider_symbol"]) for r in mappings])
        overlap = con.execute("""SELECT count(*) FROM symbols a JOIN symbols b ON a.instrument_id=b.instrument_id
            AND a.start_date<b.end_date AND b.start_date<a.end_date AND
            (a.start_date<>b.start_date OR a.end_date<>b.end_date OR a.provider_symbol<>b.provider_symbol)""").fetchone()[0]
        if overlap:
            raise RuntimeError("Ambiguous provider dated-symbol intervals")
        con.execute("CREATE VIEW bars AS SELECT CAST(ts_event AS TIMESTAMPTZ)::DATE session_date, "
                    "CAST(ts_event AS TIMESTAMPTZ) ts_event,CAST(rtype AS INTEGER) rtype, "
                    "CAST(publisher_id AS INTEGER) publisher_id,CAST(instrument_id AS BIGINT) instrument_id, "
                    "CAST(open AS DOUBLE) open,CAST(high AS DOUBLE) high,CAST(low AS DOUBLE) low, "
                    'CAST(close AS DOUBLE) AS "close",CAST(volume AS UBIGINT) volume '
                    f"FROM read_csv({sql_path(raw)},header=true,all_varchar=true)")
        n, outside, duplicates = con.execute("""SELECT count(*), count(*) FILTER(WHERE session_date<DATE '2026-09-14'
            OR session_date>=DATE '2026-10-01'),count(*)-count(DISTINCT(session_date,publisher_id,instrument_id)) FROM bars""").fetchone()
        if outside or duplicates or not n:
            raise RuntimeError("Provider bar scope/key validation failed")
        if n != int(receipt["record_count"]):
            raise RuntimeError("Downloaded row count differs from metadata quote")
        sql = """SELECT b.*,s.provider_symbol, s.provider_symbol IS NULL symbol_missing,
            NOT(coalesce(isfinite(open) AND isfinite(high) AND isfinite(low) AND isfinite(close)
                AND open>0 AND low>0 AND close>0 AND high>=greatest(open,close,low)
                AND low<=least(open,close),false)) invalid_ohlc,
            'Databento' provider,'EQUS.SUMMARY' source_dataset,'unadjusted' adjustment_basis
            FROM bars b LEFT JOIN symbols s ON b.instrument_id=s.instrument_id
                AND b.session_date>=s.start_date AND b.session_date<s.end_date
            ORDER BY session_date,publisher_id,instrument_id"""
        out = silver / "year=2026" / "raw_bars.parquet"
        written = C.copy_to_parquet(con, sql, out, row_group_size=32768)
        if written != n:
            raise RuntimeError("Dated symbology duplicated provider bars")
        con.execute(f"CREATE VIEW normalized AS SELECT * FROM read_parquet({sql_path(out)})")
        counts = con.execute("SELECT session_date,count(*),count(DISTINCT instrument_id),"
                             "count(*) FILTER(WHERE symbol_missing),count(*) FILTER(WHERE invalid_ohlc) "
                             "FROM normalized GROUP BY session_date ORDER BY session_date").fetchall()
        manifest = dict(schema_version=1, status="complete", stage=SILVER_STAGE, scope=SCOPE,
                        created_at=now(), rows=n, sessions=[dict(date=str(d), rows=r, instruments=i,
                            symbol_missing=m, invalid_ohlc=q) for d,r,i,m,q in counts],
                        download_receipt=receipt, raw_path=str(raw),
                        code=C.code_identity("databento_tail", "common"),
                        output=dict(path=str(out), sha256=C.sha256_file(out), bytes=out.stat().st_size),
                        quality=dict(adjustment_basis="unadjusted", total_return_compatible=False,
                            identity_namespace="Databento EQUS.SUMMARY instrument_id; not SpiderRock security_id",
                            point_in_time_vintage="Retrieved now; historic original-publication vintage unverified",
                            canonical_sources_modified=False, promotion_status="blocked_pending_bridge_adjudication",
                            blockers=["No verified split/dividend factors for the tail", "Dated cross-provider security IDs require adjudication", "Session/volume conventions differ by provider", "Oct 1 daily bar unavailable at acquisition"]))
        return manifest
    finally:
        con.close()


def overlap_audit(silver: Path, prices: Path) -> dict:
    """Exact observed dated ticker match only, excluding ambiguous/repaired ORATS cells."""
    con = C.connect(memory="220MB", threads=1, temp_dir=silver / "_tmp")
    try:
        con.execute(f"CREATE VIEW provider AS SELECT * FROM read_parquet({sql_path(silver / 'year=2026/raw_bars.parquet')}) WHERE session_date<DATE '2026-09-19'")
        con.execute("CREATE TABLE orats AS SELECT session_date,ticker,security_id,close,volume,sid0_repaired "
                    "FROM read_parquet(?) WHERE session_date BETWEEN DATE '2026-09-14' AND DATE '2026-09-18'", [str(prices / "year=2026" / "*.parquet")])
        con.execute("CREATE VIEW usable_orats AS SELECT * FROM orats WHERE NOT coalesce(sid0_repaired,false) "
                    "AND close>0 QUALIFY count(*) OVER(PARTITION BY session_date,ticker)=1")
        con.execute("CREATE VIEW comparison AS SELECT p.session_date,p.instrument_id,p.provider_symbol,o.security_id, "
                    "p.close provider_close,o.close orats_close,p.volume provider_volume,o.volume orats_volume, "
                    "abs(p.close/o.close-1) close_relative_difference, "
                    "CASE WHEN o.volume>0 THEN abs(p.volume::DOUBLE/o.volume-1) END volume_relative_difference "
                    "FROM provider p JOIN usable_orats o ON p.session_date=o.session_date AND p.provider_symbol=o.ticker "
                    "WHERE NOT p.invalid_ohlc AND NOT p.symbol_missing "
                    "QUALIFY count(*) OVER(PARTITION BY p.session_date,p.provider_symbol)=1")
        columns = ["date", "matched_rows", "unique_orats_lines", "close_within_1bp", "close_within_10bp", "close_within_1pct", "median_close_diff", "p95_close_diff", "volume_comparable_rows", "volume_within_1pct", "volume_within_5pct", "median_volume_diff", "p95_volume_diff"]
        rows = con.execute("""SELECT session_date,count(*),count(DISTINCT security_id),
            count(*) FILTER(WHERE close_relative_difference<=0.0001),
            count(*) FILTER(WHERE close_relative_difference<=0.001),
            count(*) FILTER(WHERE close_relative_difference<=0.01),
            median(close_relative_difference),quantile_cont(close_relative_difference,0.95),
            count(volume_relative_difference),count(*) FILTER(WHERE volume_relative_difference<=0.01),
            count(*) FILTER(WHERE volume_relative_difference<=0.05),median(volume_relative_difference),
            quantile_cont(volume_relative_difference,0.95) FROM comparison GROUP BY session_date ORDER BY session_date""").fetchall()
        counts = dict(zip(["orats_overlap_rows", "orats_usable_rows", "provider_overlap_rows", "matched_rows"],
                          [con.execute(f"SELECT count(*) FROM {v}").fetchone()[0] for v in ["orats", "usable_orats", "provider", "comparison"]]))
        return dict(method="Exact session_date + observed raw ticker; exclude duplicate ticker cells and repaired ORATS rows; no static identity mapping", counts=counts,
                    days=[dict(zip(columns, [str(r[0]), *r[1:]])) for r in rows],
                    prices_manifest_sha256=C.sha256_file(prices / "manifest.json"),
                    conclusion="Diagnostic raw-bar bridge only; price/volume agreement cannot certify total returns or identity continuity")
    finally:
        con.close()


def run(bronze: Path = DEFAULT_BRONZE, silver: Path | None = None) -> dict:
    silver = silver or C.stage_dir(SILVER_STAGE)
    if (silver / "manifest.json").exists():
        manifest = C.read_json(silver / "manifest.json")
        if manifest["scope"] != SCOPE or C.sha256_file(Path(manifest["output"]["path"])) != manifest["output"]["sha256"]:
            raise RuntimeError("Immutable silver artifact mismatch")
        return manifest
    client = Client(api_key())
    receipt = acquire(bronze, client)
    print(f"Bronze complete: quoted USD {receipt['quoted_usd']}, {receipt['record_count']} rows, SHA256 {receipt['raw_sha256']}", flush=True)
    mappings = resolve(bronze, client, instrument_ids(bronze / "bars.csv"))
    manifest = normalize(bronze / "bars.csv", mappings, silver, receipt)
    audit = overlap_audit(silver, C.build_root() / "prices")
    C.write_json_atomic(silver / "overlap_audit.json", audit)
    manifest["overlap_audit_sha256"] = C.sha256_file(silver / "overlap_audit.json")
    manifest["symbology_sha256"] = {p.name: C.sha256_file(p) for p in sorted((bronze / "symbology").glob("*.json"))}
    C.write_json_atomic(silver / "manifest.json", manifest)
    print(f"Silver complete: {manifest['rows']} rows, {len(manifest['sessions'])} sessions; overlap {audit['counts']}", flush=True)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bronze", type=Path, default=DEFAULT_BRONZE)
    parser.add_argument("--silver", type=Path)
    args = parser.parse_args()
    try:
        run(args.bronze, args.silver)
    except (RuntimeError, requests.RequestException) as exc:
        # All network-facing errors are sanitized before reaching this handler.
        raise SystemExit(str(exc)) from None


if __name__ == "__main__":
    main()
