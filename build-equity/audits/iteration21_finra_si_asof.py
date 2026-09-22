"""Iteration 21: FINRA consolidated short interest -> point-in-time as-of CSVs.

Outputs (for `atx-impl panel --asof-field <name>=<csv>`), header `security_id,available_at,value`:
  si_shares.csv  currentShortPositionQuantity
  si_dtc.csv     daysToCoverQuantity (rows with averageDailyVolumeQuantity == 0 dropped: DTC undefined)
  mapping_report.json, manifest.json

Rules
  * exchange-listed marketClassCode only (EXCHANGE_CLASSES); OTC/OTCBB excluded.
  * available_at = FINRA dissemination_date for the settlement (panel join is strict
    available_at < session_date, so no extra lag). Missing schedule row -> settlement + 7 US
    business days (flagged).
  * symbol -> ORATS securityID is date-varying: ticker_tk carried on the last ORATS trading day
    on/before settlement (within 7 calendar days). Canonicaliser: upper-case, strip . space / -
    Ambiguous canonical ticker (>1 securityID that day) -> dropped and counted.
    Two FINRA symbols -> one securityID on a settlement: keep the exact raw-ticker match, else drop.
  * duplicates: within one file keep the last occurrence; same settlement in >1 file keep the
    earliest-published file.
  * tickerhistory: accepted.zip (filtered, panel source) for dates <= its last date, full
    tbltickerhistory3_10y.zip for later dates.

Usage:
  python build-equity/audits/iteration21_finra_si_asof.py [--out C:/atx/data/finra_short_interest/asof]
      [--cache-dir DIR] [--rebuild-cache]
"""
from __future__ import annotations

import argparse
import bisect
import csv
import datetime as dt
import glob
import hashlib
import json
import os
import re
import sys
import tempfile
import zipfile
from collections import Counter, defaultdict

import pandas as pd

FINRA_DIR = r"C:\atx\data\finra_short_interest"
RAW_GLOB = os.path.join(FINRA_DIR, "raw", "si_*.csv")
SCHEDULE = os.path.join(FINRA_DIR, "dissemination_schedule.csv")
TH_ACCEPTED = r"C:\atx\data\tickerhistory_training_20120326_20191231_20260920\accepted.zip"
TH_FULL = r"C:\Users\natha\Downloads\tbltickerhistory3_10y.zip"
CTX = {
    2018: r"C:\atx\data\equity_scorecard16_ctx_2018_t1000_20260920\context.bin.manifest.json",
    2019: r"C:\atx\data\equity_scorecard16_ctx_2019_t1000_20260920\context.bin.manifest.json",
}
CTX_CUTOFF = {2018: "2019-01-02", 2019: "2020-01-01"}
EXCHANGE_CLASSES = ("AMEX", "ARCA", "BZX", "NNM", "NYSE", "SC")
LOOKBACK_DAYS = 7
_CANON_RE = re.compile(r"[.\s/\-]")


def canon(t: str) -> str:
    return _CANON_RE.sub("", t.strip().upper())


def sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 22), b""):
            h.update(b)
    return h.hexdigest()


# ---------------------------------------------------------------- FINRA
def load_finra():
    files = sorted(glob.glob(RAW_GLOB))
    first_file = {}  # (settle, symbol) -> file that first published it
    rows = {}
    stats = Counter()
    classes = Counter()
    for fp in files:
        fname = os.path.basename(fp)
        file_rows = {}
        with open(fp, newline="", encoding="utf-8") as f:
            # QUOTE_NONE: issueName carries unbalanced '"' (e.g. DOD 'ELEMENTS "Dogs of the Dow"')
            for r in csv.DictReader(f, delimiter="|", quoting=csv.QUOTE_NONE):
                stats["raw_rows"] += 1
                if None in r or r.get("settlementDate") is None:
                    stats["bad_field_count_rows_skipped"] += 1
                    continue
                classes[r["marketClassCode"]] += 1
                if r["marketClassCode"] not in EXCHANGE_CLASSES:
                    continue
                stats["exchange_rows"] += 1
                s = r["settlementDate"]
                if r["accountingYearMonthNumber"] != s.replace("-", ""):
                    stats["settlement_field_mismatch"] += 1
                if (r.get("revisionFlag") or "").strip() == "R":
                    stats["revision_flag_R_rows"] += 1
                key = (s, r["symbolCode"])
                if key in file_rows:
                    stats["dup_within_file_replaced"] += 1
                file_rows[key] = r  # latest row within the same file wins
        settles = {k[0].replace("-", "") for k in file_rows}
        if settles and settles != {fname[3:11]}:
            stats["file_name_vs_settlement_mismatch"] += 1
        for key, r in file_rows.items():
            if key in rows and first_file[key] != fname:
                stats["dup_cross_file_dropped_later"] += 1  # keep first-published file
                continue
            rows[key] = r
            first_file[key] = fname
    return files, rows, stats, classes


def load_schedule():
    with open(SCHEDULE, newline="") as f:
        return {r["settlement_date"]: r["dissemination_date"] for r in csv.DictReader(f)}


def fallback_dissemination(settle: str) -> str:
    from pandas.tseries.holiday import USFederalHolidayCalendar
    from pandas.tseries.offsets import CustomBusinessDay
    bd = CustomBusinessDay(calendar=USFederalHolidayCalendar())
    return (pd.Timestamp(settle) + 7 * bd).strftime("%Y-%m-%d")


# ---------------------------------------------------------------- ticker history
def stream_th(path, keep_dates, min_date_exclusive=None):
    out = []
    zf = zipfile.ZipFile(path)
    last_date = None
    with zf.open(zf.infolist()[0]) as fh:
        it = pd.read_csv(fh, sep="\t", usecols=["tradingDate", "securityID", "ticker_tk"],
                         dtype=str, chunksize=2_000_000, keep_default_na=False)
        for ch in it:
            last_date = ch["tradingDate"].iloc[-1]
            m = ch["tradingDate"].isin(keep_dates)
            if min_date_exclusive:
                m &= ch["tradingDate"] > min_date_exclusive
            if m.any():
                out.append(ch[m])
            print(f"  {os.path.basename(path)} ... {last_date}", file=sys.stderr, flush=True)
    cols = ["tradingDate", "securityID", "ticker_tk"]
    df = pd.concat(out, ignore_index=True) if out else pd.DataFrame(columns=cols)
    return df, last_date


def load_ticker_history(settles, cache_dir, rebuild):
    keep = set()
    for s in settles:
        d0 = dt.date.fromisoformat(s)
        for k in range(LOOKBACK_DAYS + 1):
            keep.add((d0 - dt.timedelta(days=k)).isoformat())
    os.makedirs(cache_dir, exist_ok=True)
    cpath = os.path.join(cache_dir, "th_candidates.pkl")
    meta_path = os.path.join(cache_dir, "th_meta.json")
    if os.path.exists(cpath) and os.path.exists(meta_path) and not rebuild:
        meta = json.load(open(meta_path))
        if meta.get("keep_n") == len(keep):
            return pd.read_pickle(cpath), meta
    acc, acc_last = stream_th(TH_ACCEPTED, keep)
    acc["source"] = "accepted"
    frames = [acc]
    full_last = None
    if os.path.exists(TH_FULL):
        full, full_last = stream_th(TH_FULL, keep, min_date_exclusive=acc_last)
        full["source"] = "full"
        frames.append(full)
    df = pd.concat(frames, ignore_index=True)
    df["securityID"] = pd.to_numeric(df["securityID"], errors="coerce")
    df = df[(df["securityID"] > 0) & (df["ticker_tk"].str.strip() != "")].copy()
    df["securityID"] = df["securityID"].astype("int64")
    meta = {"keep_n": len(keep), "accepted_last_date": acc_last, "full_last_date": full_last,
            "full_present": os.path.exists(TH_FULL)}
    df.to_pickle(cpath)
    json.dump(meta, open(meta_path, "w"), indent=1)
    return df, meta


def build_maps(th, settles):
    """settle -> (trading_date, {canon: sid}, {canon: raw tickers}, ambiguous canon set) or None."""
    dates = sorted(th["tradingDate"].unique())
    by_date = {d: g for d, g in th.groupby("tradingDate")}
    out = {}
    for s in settles:
        i = bisect.bisect_right(dates, s) - 1
        if i < 0 or (dt.date.fromisoformat(s) - dt.date.fromisoformat(dates[i])).days > LOOKBACK_DAYS:
            out[s] = None
            continue
        d = dates[i]
        g = by_date[d]
        ids = defaultdict(set)
        raws = defaultdict(set)
        for t, sid in zip(g["ticker_tk"].values, g["securityID"].values):
            c = canon(t)
            ids[c].add(int(sid))
            raws[c].add(t.strip().upper())
        amb = {c for c, v in ids.items() if len(v) > 1}
        out[s] = (d, {c: next(iter(v)) for c, v in ids.items() if len(v) == 1}, raws, amb)
    return out


# ---------------------------------------------------------------- main
def num(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def fmt(v: float) -> str:
    if v == int(v):
        return str(int(v))
    return f"{v:.6f}".rstrip("0").rstrip(".")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(FINRA_DIR, "asof"))
    ap.add_argument("--cache-dir", default=os.path.join(tempfile.gettempdir(), "atx_finra_si_th_cache"))
    ap.add_argument("--rebuild-cache", action="store_true")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    files, rows, fstats, classes = load_finra()
    sched = load_schedule()
    settles = sorted({k[0] for k in rows})
    th, th_meta = load_ticker_history(settles, a.cache_dir, a.rebuild_cache)
    maps = build_maps(th, settles)

    avail, fallback = {}, []
    for s in settles:
        if sched.get(s):
            avail[s] = sched[s]
        else:
            avail[s] = fallback_dissemination(s)
            fallback.append(s)
        assert avail[s] > s, s

    st = Counter()
    per_year = defaultdict(Counter)
    unmapped_adv = {}
    cand = defaultdict(list)  # (sid, settle) -> [(exact, symbol, row)]
    for (s, sym), r in rows.items():
        y = s[:4]
        per_year[y]["exchange_rows"] += 1
        m = maps.get(s)
        if m is None:
            per_year[y]["no_tickerhistory"] += 1
            st["no_tickerhistory_rows"] += 1
            continue
        per_year[y]["mappable_rows"] += 1
        c = canon(sym)
        if c in m[3]:
            per_year[y]["ambiguous"] += 1
            st["ambiguous_rows"] += 1
            continue
        sid = m[1].get(c)
        if sid is None:
            per_year[y]["unmapped"] += 1
            adv = num(r["averageDailyVolumeQuantity"]) or 0.0
            prev = unmapped_adv.get(sym)
            if prev is None or adv > prev[0]:
                unmapped_adv[sym] = (adv, r["issueName"], r["marketClassCode"], s)
            continue
        exact = sym.strip().upper() in m[2][c]
        if not exact:
            st["mapped_via_canonicaliser_only"] += 1
        cand[(sid, s)].append((exact, sym, r))

    shares, dtc = [], []
    for (sid, s), lst in cand.items():
        if len(lst) > 1:
            ex = [x for x in lst if x[0]]
            if len(ex) == 1:
                st["sid_collision_resolved_exact"] += 1
                lst = ex
            else:
                st["sid_collision_dropped_rows"] += len(lst)
                continue
        _, sym, r = lst[0]
        per_year[s[:4]]["mapped"] += 1
        q = num(r["currentShortPositionQuantity"])
        d = num(r["daysToCoverQuantity"])
        adv = num(r["averageDailyVolumeQuantity"])
        if q is None:
            st["shares_unparseable"] += 1
        else:
            st["shares_zero"] += q == 0
            st["shares_negative"] += q < 0
            shares.append((sid, avail[s], q))
        if d is None:
            st["dtc_unparseable"] += 1
        elif not adv:
            st["dtc_dropped_adv_zero"] += 1
        else:
            st["dtc_negative"] += d < 0
            st["dtc_zero"] += d == 0
            st["dtc_exactly_1"] += d == 1.0
            st["dtc_below_1_nonzero"] += 0 < d < 1.0
            dtc.append((sid, avail[s], d))

    def write(name, data):
        data.sort()
        keys = [(x[0], x[1]) for x in data]
        assert len(keys) == len(set(keys)), name
        p = os.path.join(a.out, name)
        with open(p, "w", newline="") as f:
            f.write("security_id,available_at,value\n")
            for sid, av, v in data:
                f.write(f"{sid},{av},{fmt(v)}\n")
        return p, {"rows": len(data), "ids": len({x[0] for x in data}),
                   "first_available_at": min(x[1] for x in data),
                   "last_available_at": max(x[1] for x in data)}

    p_sh, sum_sh = write("si_shares.csv", shares)
    p_dt, sum_dt = write("si_dtc.csv", dtc)

    avail_by_id = defaultdict(list)
    for sid, av, _ in shares:
        avail_by_id[sid].append(av)
    coverage = {}
    for y, mp in CTX.items():
        if not os.path.exists(mp):
            coverage[y] = "missing manifest"
            continue
        ids = [int(x) for x in json.load(open(mp))["axes"]["instrument_ids"]]
        cut, ystart = CTX_CUTOFF[y], f"{y}-01-01"
        before = {i for i in ids if any(av < cut for av in avail_by_id.get(i, []))}
        n_in_year = [sum(1 for av in avail_by_id.get(i, []) if ystart <= av < cut) for i in ids]
        coverage[y] = {
            "ids": len(ids), "cutoff_exclusive": cut,
            "ids_with_row_before_cutoff": len(before), "share": round(len(before) / len(ids), 4),
            "ids_with_row_published_in_year": sum(1 for n in n_in_year if n > 0),
            "ids_zero_rows_before_cutoff": len(ids) - len(before),
            "ids_zero_rows_any_date": sum(1 for i in ids if i not in avail_by_id),
            "median_rows_published_in_year": float(pd.Series(n_in_year).median()),
            "zero_row_id_examples": [i for i in ids if i not in before][:25],
        }

    yr = {}
    for y, c in sorted(per_year.items()):
        yr[y] = dict(c)
        yr[y]["mapped_frac_exchange_rows"] = round(c["mapped"] / c["exchange_rows"], 4) if c["exchange_rows"] else None
        yr[y]["mapped_frac_mappable_rows"] = round(c["mapped"] / c["mappable_rows"], 4) if c["mappable_rows"] else None
    unm = sorted(unmapped_adv.items(), key=lambda kv: -kv[1][0])[:10]
    report = {
        "canonicaliser": "upper-case, strip '.', whitespace, '/', '-' on both FINRA symbolCode and ORATS ticker_tk",
        "ticker_source": "ORATS ticker_tk on last trading day <= settlement within 7 calendar days",
        "tickerhistory": th_meta,
        "exchange_classes_kept": list(EXCHANGE_CLASSES),
        "market_class_counts_all_files": dict(classes),
        "finra_stats": dict(fstats),
        "mapping_stats": dict(st),
        "settlements": len(settles),
        "settlements_without_tickerhistory": [s for s in settles if maps.get(s) is None],
        "schedule_fallback_settlements": fallback,
        "per_year": yr,
        "floored_universe_coverage": coverage,
        "unmapped_top_adv": [{"symbol": k, "max_adv": v[0], "issue": v[1], "class": v[2], "settlement": v[3]}
                             for k, v in unm],
        "si_shares": sum_sh, "si_dtc": sum_dt,
    }
    json.dump(report, open(os.path.join(a.out, "mapping_report.json"), "w"), indent=1)

    inputs = [{"path": p, "sha256": sha256(p)} for p in files + [SCHEDULE]]
    acc_manifest = os.path.join(os.path.dirname(TH_ACCEPTED), "manifest.json")
    inputs.append({"path": TH_ACCEPTED, "sha256": json.load(open(acc_manifest))["accepted"]["sha256"],
                   "sha256_source": acc_manifest})
    if th_meta.get("full_present"):
        inputs.append({"path": TH_FULL, "sha256": sha256(TH_FULL)})
    manifest = {
        "script": os.path.abspath(__file__),
        "generated_at": dt.datetime.now().isoformat(timespec="seconds"),
        "inputs": inputs,
        "outputs": {n: {"path": p, "sha256": sha256(p), **smry}
                    for n, p, smry in (("si_shares", p_sh, sum_sh), ("si_dtc", p_dt, sum_dt))},
        "rows": {"si_shares": sum_sh["rows"], "si_dtc": sum_dt["rows"]},
        "ids": {"si_shares": sum_sh["ids"], "si_dtc": sum_dt["ids"]},
        "first_available_at": min(sum_sh["first_available_at"], sum_dt["first_available_at"]),
        "last_available_at": max(sum_sh["last_available_at"], sum_dt["last_available_at"]),
        "rule": "available_at=dissemination_date",
        "exchange_classes_kept": list(EXCHANGE_CLASSES),
        "revisions_dropped": fstats.get("dup_cross_file_dropped_later", 0) + fstats.get("dup_within_file_replaced", 0),
        "schedule_fallback_settlements": fallback,
    }
    json.dump(manifest, open(os.path.join(a.out, "manifest.json"), "w"), indent=1)
    summ = {k: report[k] for k in ("tickerhistory", "finra_stats", "mapping_stats", "per_year",
                                   "floored_universe_coverage", "unmapped_top_adv", "si_shares", "si_dtc",
                                   "schedule_fallback_settlements", "settlements_without_tickerhistory")}
    for v in summ["floored_universe_coverage"].values():
        if isinstance(v, dict):
            v.pop("zero_row_id_examples", None)
    print(json.dumps(summ, indent=1))


if __name__ == "__main__":
    main()
