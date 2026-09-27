"""Point-in-time research fields aligned EXACTLY to an existing recent-research role payload.

Reads a published ``atx.recent-research-role/v1`` directory (sessions.i64, ids.u64, member.u8)
and writes one date-major little-endian f64 file per field, shape role dates x role ids, NaN
where not visible or absent, plus a publish-last ``atx.research-role-fields/v1`` manifest.
Nothing is re-projected: the role's own session labels and securityID axis are the only axes.

Sources (all read-only; exact bytes hashed, pinned against their producers' receipts):
* FINRA short interest as-of CSVs (``security_id,available_at,value``): the join replicates
  ``atx-impl/src/asof_field.cpp`` exactly: strict available_at < date(session), latest row wins
  (a visible NaN stays NaN), age > 45 calendar days -> NaN.
* TickerHistory3 vendor rows (must be the role's own source file, same SHA-256): a row dated d
  is the end-of-day mark known at d 22:00 UTC, i.e. the same clock as the role's close. Same-date
  only, no fill. ``shares_out`` instead follows the house A8 rule (vendor share runs start at the
  filing cover date): the share count of the line's last row dated <= date(session) - 90 days,
  restated through cumulReturnFactor to the session's share basis.
* research-lake spine_monthly: value of the latest monthly formation session STRICTLY before
  date(session); line_types: static (non point-in-time) line classification.
* the role payload itself (``mkt_ret``): equal-weight mean of guarded adjusted close-to-close
  returns of the prior session's decision members, broadcast to every present cell of the session.

Everything available on or after 2025-01-01 is excluded; the role itself must end before it.
Row groups of the vendor file mix all dates: only needed columns are decoded, rows are filtered to
the role ids/dates before any statistic. No warehouse access. Outputs are exclusive and
deterministic (no wall-clock value in any output byte).
"""
from __future__ import annotations

import argparse
import csv
import datetime as dt
import hashlib
import io
import json
import math
import os
from pathlib import Path
import time

import numpy as np
import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.csv as pacsv
import pyarrow.parquet as pq

SCHEMA = "atx.research-role-fields/v1"
ROLE_SCHEMA = "atx.recent-research-role/v1"
DAY_NS = 86_400_000_000_000
EPOCH = dt.date(1970, 1, 1)
SEAL = dt.date(2025, 1, 1)
FINRA_MAX_STALE_DAYS = 45
FINRA_HEADER = b"security_id,available_at,value"
FINRA_REPUBLICATION_SETTLEMENT_BEFORE = dt.date(2021, 6, 1)
SHARES_LAG_DAYS = 90            # atx_db.research.spine.SHARES_LAG_DAYS (A8)
SHARES_MAX_AGE_DAYS = 400       # atx_db.research.spine.SHARES_MAX_AGE_DAYS
SHARES_UNIT = 1000.0            # A9: the parquet format's `shares` means thousands
THOUSANDS_ROW_CEILING = 100_000_000  # atx_db.migrations.bodies_0327 (1e11 plausible max / 1000)
LINE_PREFIX = "TBLTICKERHISTORY-"
ELIGIBLE_SECURITY_TYPES = ("common", "common_unverified")  # atx_db.research.spine
SIZE_CODES = {"micro": 0.0, "small": 1.0, "large": 2.0, "mega": 3.0}
EARN_CODES = {"N": 0, "-1": 0, "0": 1, "1": 1}  # -1 presumes a known future date: same as N

DEFAULT_FINRA = Path("C:/atx/data/finra_short_interest")
DEFAULT_TICKERHISTORY = Path("C:/Users/natha/Downloads/TickerHistory3.parquet")
DEFAULT_LAKE = Path("C:/atx/atx-db/data/research/lake/price-wave-0ed96b2696f1-5b596288cf23")

TH_TYPES = {"tradingDate": pa.date32(), "securityID": pa.int64(), "shares": pa.int64(),
            "earnFlag": pa.string(), "cumulReturnFactor": pa.float64(),
            "atmCenI_21d": pa.float32(), "atmCenI_63d": pa.float32(), "atmCenI_126d": pa.float32()}
TH_CLOCK = "vendor-eod-row-date==session-date;known-at-session+22h-mark;same-date-only-v1"

FIELDS = {
    "si_shares": {
        "group": "finra", "source_file": "si_shares.csv", "source_columns": ["value"],
        "units": "shares short (FINRA consolidated currentShortPositionQuantity)",
        "clock": "finra-asof:latest-row-with-available_at<date(session)-strict;available_at=official-dissemination-date;replicates-atx-impl-build_asof_column",
        "staleness": "age=date(session)-available_at calendar days; age>45 -> NaN; before first visible row -> NaN; visible NaN stays NaN (no skip-back)",
        "caveats": ["settlements before 2021-06 come from FINRA's later consolidated republication, not the bytes the exchanges disseminated (vintage risk; see vintage_risk counts)",
                    "securityID mapping by ORATS ticker_tk on the last trading day <= settlement (producer mapping_report.json)"]},
    "si_dtc": {
        "group": "finra", "source_file": "si_dtc.csv", "source_columns": ["value"],
        "units": "days to cover (FINRA daysToCoverQuantity; FINRA floors the ratio at 1.00)",
        "clock": "finra-asof:latest-row-with-available_at<date(session)-strict;available_at=official-dissemination-date;replicates-atx-impl-build_asof_column",
        "staleness": "age=date(session)-available_at calendar days; age>45 -> NaN; before first visible row -> NaN; visible NaN stays NaN (no skip-back)",
        "caveats": ["FINRA reports days-to-cover floored at 1.00 (about 40% of producer rows equal 1); si_shares / volume is the unfloored alternative",
                    "settlements before 2021-06 come from FINRA's later consolidated republication (vintage risk)"]},
    "iv_atm_21d": {
        "group": "th", "column": "atmCenI_21d", "source_columns": ["atmCenI_21d"],
        "units": "annualized ATM implied volatility, decimal (0.30 = 30%), 21-session constant maturity",
        "clock": TH_CLOCK, "staleness": "same-date vendor row only; null, non-finite or <= 0 -> NaN",
        "caveats": ["vendor clean IV: evidence suggests the earnings effect is removed using the vendor earnings calendar (nEarnCnt_*, forward-looking by construction); calendar vintage unproven",
                    "often null (about half of all vendor rows; about 6.5% of high-volume rows in a sample)", "no vintage proof"]},
    "iv_atm_63d": {
        "group": "th", "column": "atmCenI_63d", "source_columns": ["atmCenI_63d"],
        "units": "annualized ATM implied volatility, decimal, 63-session constant maturity",
        "clock": TH_CLOCK, "staleness": "same-date vendor row only; null, non-finite or <= 0 -> NaN",
        "caveats": ["vendor clean IV (earnings-calendar adjusted; calendar vintage unproven)", "often null", "no vintage proof"]},
    "iv_atm_126d": {
        "group": "th", "column": "atmCenI_126d", "source_columns": ["atmCenI_126d"],
        "units": "annualized ATM implied volatility, decimal, 126-session constant maturity",
        "clock": TH_CLOCK, "staleness": "same-date vendor row only; null, non-finite or <= 0 -> NaN",
        "caveats": ["vendor clean IV (earnings-calendar adjusted; calendar vintage unproven)", "often null", "no vintage proof"]},
    "earn_recent": {
        "group": "th", "column": "earnFlag", "source_columns": ["earnFlag"],
        "units": "indicator: 1 when the session is the vendor earnings reaction day (earnFlag 0) or the day after (1); 0 for N and -1",
        "clock": TH_CLOCK,
        "staleness": "same-date vendor row only; null or unexpected earnFlag -> NaN",
        "caveats": ["earnFlag -1 (the session before a known future event) is deliberately mapped to 0, indistinguishable from N",
                    "earnFlag 0 is the price-reaction session (median |return| 3.9% vs 1.2% baseline in samples), so the event is public by its close",
                    "vendor calendar vintage unproven"]},
    "shares_out": {
        "group": "th", "column": "shares", "source_columns": ["shares", "cumulReturnFactor"],
        "units": "shares outstanding (vendor thousands x 1000), restated to the session's share basis",
        "clock": "A8-vendor-shares-lag90-restated-v1: last vendor row of the line dated <= date(session)-90 calendar days with 0 < shares <= 1e8 (A9 thousands ceiling) and cumulReturnFactor > 0, times 1000 x cumulReturnFactor(session row)/cumulReturnFactor(lag row)",
        "staleness": "lag row older than date(session)-90-400 days -> NaN; no same-date vendor row (or its factor <= 0) -> NaN; a line with any row above the A9 ceiling in the scanned window is withheld entirely (ruling C-81)",
        "caveats": ["A8: vendor share runs start at the filing cover date, so same-date vendor shares would leak ~2 weeks; the 90-day modeled lag follows the research spine",
                    "restatement uses the raw vendor cumulReturnFactor ratio (not the VA1-repaired factor; vendor artifact breaks are not detected); dividends move it by a few tenths of a percent",
                    "one price line's count, not the issuer total across share classes; an ADR line counts ADS"]},
    "mktcap_lagged": {
        "group": "lake", "column": "me_line", "source_columns": ["me_line", "formation_date", "line_id"],
        "units": "USD: formation-session raw close x vendor shares lagged 90 days (spine me_line, me_basis vendor_shares_lag90)",
        "clock": "spine-formation<date(session)-strict-v1: the row of the latest monthly formation session strictly before date(session)",
        "staleness": "one formation only: a line without a spine row (or with NULL me_line) at that formation -> NaN",
        "caveats": ["spine universe gate uses vendor earnings evidence (nEarnCnt_504d > 0, which counts FUTURE events) and a 2026 directory snapshot",
                    "one price line's value, not the issuer total"]},
    "size_grp": {
        "group": "lake", "column": "size_grp", "source_columns": ["size_grp", "formation_date", "line_id"],
        "units": "ordinal code micro=0, small=1, large=2, mega=3 (Fama-French NYSE ME breakpoints p20/p50/p80; micro includes nano)",
        "clock": "spine-formation<date(session)-strict-v1: the row of the latest monthly formation session strictly before date(session)",
        "staleness": "one formation only: absent line or NULL size_grp -> NaN",
        "caveats": ["same spine universe caveats as mktcap_lagged"]},
    "is_common": {
        "group": "lake", "column": "security_type", "source_columns": ["security_type", "line_id"],
        "units": "indicator: 1 when line_types.security_type is common or common_unverified (the research universe's eligible types), 0 for every other type (ETF, ADR, fund, unknown, ...)",
        "clock": "static-line-classification;NOT-point-in-time",
        "staleness": "constant across sessions; NaN only when the line has no line_types row",
        "caveats": ["classification uses a 2026-09-18 Nasdaq Trader directory snapshot (listed lines) and whole-history vendor earnings evidence (delisted lines): information after 2025-01-01 by construction; use as a coarse ETF/fund filter, not as a PIT signal",
                    "ADR is typed 0"]},
    "mkt_ret": {
        "group": "role", "source_columns": ["close.f64", "raw_close.f64", "present.u8", "member.u8"],
        "units": "simple return, decimal: equal-weight mean of adjusted close-to-close returns (broadcast)",
        "definition": "row d >= 1: mean over instruments i with member[d-1,i]==1, present[d-1,i]==present[d,i]==1, finite positive close and raw_close at d-1 and d, and not guarded, of close[d,i]/close[d-1,i]-1; guarded (atx-impl strategy_target_replay.cpp rough_return) = non-finite r or |log adj ratio| > 1.5 or |log adj ratio| > |log raw ratio| + 0.10; the scalar fills every cell of row d with present[d,i]==1, NaN elsewhere; row 0 and sessions without contributors are NaN; sum is math.fsum (order independent)",
        "clock": "role-close-mark: row d uses role closes at sessions d-1 and d (known at the d 22:00 UTC mark, the close clock) and decision membership of d-1",
        "staleness": "no fill: row 0, cells not present at d, and sessions with no contributing name are NaN",
        "caveats": ["universe is the role's prior-63-session ADV top-N (ETFs included; common_stock_verified false): an ADV-universe equal-weight market, not a cap-weighted index",
                    "a broadcast field: identical across present names, so it avoids the runner's member-masked vec_avg blanking market templates for boundary names"]},
}
DEFAULT_FIELDS = tuple(FIELDS)

EXCLUDED_SOURCE_COLUMNS = [
    {"columns": "atmCenH_* (proposed hv_<tenor>)",
     "reason": "not realized historical volatility: equals atmCenI_<tenor> exactly whenever nEarnCnt_<tenor> == 0 (sample H/I p10=p50=p90=1.000) and differs only when a scheduled earnings event falls inside the tenor (corr with atmCenI_21d 0.95); an alternate earnings adjustment of the same IV built from the forward-looking earnings calendar and *EMove family. Realized volatility is computable from close in the DSL."},
    {"columns": "nEarnCnt_5d..nEarnCnt_504d",
     "reason": "count FUTURE earnings events within the next N sessions (nEarnCnt_5d is 1 on the five sessions up to and including earnFlag -1 and 0 from the reaction session on): forward-looking"},
    {"columns": "nEarnCnt", "reason": "semantics unverified (typically 8, sometimes 7 on event sessions)"},
    {"columns": "hEMove, iEMove, wkD1, shD1, qtrD1, lnD1", "reason": "forward-horizon moves; excluded as look-ahead by the ORATS history loader design (2026-06-16)"},
    {"columns": "earnFlag == -1", "reason": "presumes a known future event date; mapped to 0 (same as N)"},
    {"columns": "same-date vendor shares", "reason": "A8: vendor runs start at the filing cover date; replaced by the 90-day lag (shares_out)"},
]


def canonical(value) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)


def day_of(value: dt.date) -> int:
    return (value - EPOCH).days


def date_of(day: int) -> str:
    return (EPOCH + dt.timedelta(days=int(day))).isoformat()


def sha_bytes(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def sha_file(path: Path, budget: "Budget | None" = None) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while chunk := f.read(8 << 20):
            h.update(chunk)
            if budget:
                budget.check("hash")
    return h.hexdigest()


def identity(path: Path):
    s = path.stat()
    return (s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns)


class Budget:
    """Cooperative time/RSS guard polled at batch and date boundaries (not a kernel quota)."""

    def __init__(self, max_rss_mib=700, max_seconds=1800.0):
        if max_rss_mib < 128 or not 0 < max_seconds <= 7200:
            raise ValueError("invalid resource budget")
        self.max_rss = int(max_rss_mib) << 20
        self.deadline = time.monotonic() + max_seconds
        self.peak = 0
        try:
            import psutil
            self._rss = psutil.Process().memory_info
        except ImportError:  # pragma: no cover - psutil ships with the admitted interpreter
            self._rss = None

    def rss(self) -> int:
        return int(self._rss().rss) if self._rss else 0

    def check(self, stage: str):
        if time.monotonic() > self.deadline:
            raise TimeoutError(f"time budget exceeded at {stage}; partial output kept unpublished")
        rss = self.rss()
        self.peak = max(self.peak, rss)
        if rss > self.max_rss:
            raise MemoryError(f"RSS {rss >> 20} MiB exceeds --max-rss-mib at {stage}; partial output kept unpublished")

    def admit(self, planned: int, stage: str):
        if self.rss() + planned > self.max_rss:
            raise ValueError(f"{stage}: planned {planned >> 20} MiB + current RSS exceeds --max-rss-mib")

    def report(self, stage: str, **values):
        self.check(stage)
        print(canonical({"stage": stage, "rss_mib": self.rss() >> 20, **values}), flush=True)


def publish(path: Path, value):
    """Exclusive publish-last manifest: a reader sees no manifest or its complete fsynced bytes."""
    content = (json.dumps(value, sort_keys=True, indent=2, allow_nan=False) + "\n").encode("utf-8")
    pending = path.with_name("." + path.name + ".pending")
    with pending.open("xb") as f:
        f.write(content)
        f.flush()
        os.fsync(f.fileno())
    os.link(pending, path)
    pending.unlink()
    return content


# ---------------------------------------------------------------------------
# Role axes
# ---------------------------------------------------------------------------

class Role:
    def __init__(self, directory: Path, expected_sha256: str):
        self.dir = directory
        manifest_bytes = (directory / "manifest.json").read_bytes()
        self.manifest_sha256 = sha_bytes(manifest_bytes)
        if self.manifest_sha256 != expected_sha256.lower():
            raise ValueError("role manifest SHA-256 does not match --role-sha256")
        m = json.loads(manifest_bytes)
        if m.get("schema") != ROLE_SCHEMA or m.get("status") != "complete":
            raise ValueError("role is not a complete atx.recent-research-role/v1 payload")
        if m.get("instrument_namespace") != "spiderrock.securityID":
            raise ValueError("role instrument namespace is not spiderrock.securityID")
        self.manifest = m
        blobs = {}
        for name in ("sessions.i64", "ids.u64", "member.u8"):
            blob = (directory / name).read_bytes()
            entry = m["files"][name]
            if len(blob) != entry["bytes"] or sha_bytes(blob) != entry["sha256"]:
                raise ValueError(f"role {name} bytes do not match the role manifest")
            blobs[name] = blob
        self.sessions_sha256 = m["files"]["sessions.i64"]["sha256"]
        self.ids_sha256 = m["files"]["ids.u64"]["sha256"]
        sessions = np.frombuffer(blobs["sessions.i64"], dtype="<i8").astype(np.int64)
        ids = np.frombuffer(blobs["ids.u64"], dtype="<u8")
        self.n_dates, self.n = int(m["dates"]), int(m["instruments"])
        if len(sessions) != self.n_dates or len(ids) != self.n or not self.n_dates or not self.n:
            raise ValueError("role axes disagree with the manifest shape")
        if np.any(sessions % DAY_NS != 0) or np.any(np.diff(sessions) <= 0):
            raise ValueError("role sessions are not strictly increasing midnight labels")
        if np.any(ids == 0) or np.any(ids >= np.uint64(1 << 63)) or np.any(np.diff(ids.astype(np.int64)) <= 0):
            raise ValueError("role ids are not strictly increasing positive i64 securityIDs")
        self.days = sessions // DAY_NS
        self.ids = ids.astype(np.int64)
        # The roles end 2024; assert it rather than trust it.
        if int(self.days[-1]) >= day_of(SEAL):
            raise ValueError("role contains a session on or after 2025-01-01; refusing (sealed)")
        self.member = np.frombuffer(blobs["member.u8"], dtype="u1").reshape(self.n_dates, self.n)
        years = self.days.astype("datetime64[D]").astype("datetime64[Y]").astype(np.int64) + 1970
        self.years = years
        self.score_begin = int(m.get("score_begin", 0))
        self.score_end = int(m.get("score_end", self.n_dates))
        self.source_sha256 = m.get("source_sha256")

    def columns_of(self, sid: np.ndarray):
        """Positions of `sid` on the role id axis and a mask of which are on it."""
        pos = np.minimum(np.searchsorted(self.ids, sid), self.n - 1)
        return pos, self.ids[pos] == sid


class FieldWriter:
    """Streams one date-major f64 field and accumulates member-cell coverage/value statistics."""

    def __init__(self, output: Path, name: str, role: Role):
        self.name, self.role = name, role
        self.path = output / f"{name}.f64"
        self.f = self.path.open("xb")
        self.t = 0
        self.year = {}
        self.score = [0, 0]
        self.finite_all = 0
        self.vmin, self.vmax, self.vsum, self.vcount = np.inf, -np.inf, 0.0, 0

    def write(self, row: np.ndarray):
        if row.shape != (self.role.n,):
            raise ValueError(f"{self.name}: row shape mismatch")
        row = np.where(np.isfinite(row), row, np.nan).astype("<f8")  # canonical quiet NaN bytes
        self.f.write(row.tobytes())
        t = self.t
        member = self.role.member[t] != 0
        finite = np.isfinite(row)
        fm = member & finite
        mc, fc = int(np.count_nonzero(member)), int(np.count_nonzero(fm))
        y = self.year.setdefault(int(self.role.years[t]), [0, 0])
        y[0] += mc
        y[1] += fc
        if self.role.score_begin <= t < self.role.score_end:
            self.score[0] += mc
            self.score[1] += fc
        self.finite_all += int(np.count_nonzero(finite))
        if fc:
            v = row[fm]
            self.vmin, self.vmax = min(self.vmin, float(v.min())), max(self.vmax, float(v.max()))
            self.vsum += float(v.sum())
            self.vcount += fc
        self.t += 1

    def close(self):
        self.f.flush()
        os.fsync(self.f.fileno())
        self.f.close()
        if self.t != self.role.n_dates:
            raise ValueError(f"{self.name}: wrote {self.t} of {self.role.n_dates} dates")

    def coverage(self):
        frac = lambda f, m: round(f / m, 6) if m else None
        member = sum(v[0] for v in self.year.values())
        finite = sum(v[1] for v in self.year.values())
        return {"member_cells": member, "finite_member_cells": finite, "finite_member_frac": frac(finite, member),
                "score_window": {"member_cells": self.score[0], "finite_member_cells": self.score[1],
                                 "finite_member_frac": frac(self.score[1], self.score[0])},
                "per_year": {str(y): {"member_cells": v[0], "finite_member_cells": v[1],
                                      "finite_member_frac": frac(v[1], v[0])} for y, v in sorted(self.year.items())},
                "finite_cells_all": self.finite_all,
                "member_finite_min": self.vmin if self.vcount else None,
                "member_finite_max": self.vmax if self.vcount else None,
                "member_finite_mean": (self.vsum / self.vcount) if self.vcount else None}


# ---------------------------------------------------------------------------
# FINRA short interest (as-of join replicating atx-impl asof_field.cpp)
# ---------------------------------------------------------------------------

def read_schedule(finra: Path):
    blob = (finra / "dissemination_schedule.csv").read_bytes()
    rows = list(csv.DictReader(io.StringIO(blob.decode("utf-8"))))
    dissemination = np.array(sorted({day_of(dt.date.fromisoformat(r["dissemination_date"])) for r in rows}), dtype=np.int64)
    before = [dt.date.fromisoformat(r["dissemination_date"]) for r in rows
              if dt.date.fromisoformat(r["settlement_date"]) < FINRA_REPUBLICATION_SETTLEMENT_BEFORE]
    cutoff = day_of(max(before)) if before else None
    source = {"path": str((finra / "dissemination_schedule.csv").resolve()), "bytes": len(blob), "sha256": sha_bytes(blob)}
    return dissemination, cutoff, source


def parse_asof_csv(blob: bytes):
    """Strict CSV contract of atx-impl parse_asof_csv; returns (ids i64, days i64, values f64)."""
    first = blob.split(b"\n", 1)[0]
    if (first[:-1] if first.endswith(b"\r") else first) != FINRA_HEADER:
        raise ValueError("asof csv: header must be exactly 'security_id,available_at,value'")
    # One optional trailing newline; any empty line (or a bare CR line) is a C++ parse error.
    if b"\n\n" in blob or b"\n\r\n" in blob or b"\r\r" in blob or blob.endswith(b"\n\r"):
        raise ValueError("asof csv: empty line")
    table = pacsv.read_csv(
        io.BytesIO(blob),
        read_options=pacsv.ReadOptions(use_threads=False, block_size=1 << 22),
        parse_options=pacsv.ParseOptions(delimiter=",", quote_char=False, double_quote=False,
                                         escape_char=False, newlines_in_values=False),
        convert_options=pacsv.ConvertOptions(column_types={c: pa.string() for c in ("security_id", "available_at", "value")},
                                             strings_can_be_null=False, include_columns=["security_id", "available_at", "value"]))
    if table.column_names != ["security_id", "available_at", "value"]:
        raise ValueError("asof csv: unexpected columns")
    sid_text, date_text, value_text = (table.column(i).combine_chunks() for i in range(3))
    if not pc.all(pc.match_substring_regex(sid_text, r"^[0-9]+$")).as_py():
        raise ValueError("asof csv: security_id is not a positive i64")
    ids = pc.cast(sid_text, pa.int64()).to_numpy()
    if np.any(ids <= 0):
        raise ValueError("asof csv: security_id is not a positive i64")
    if not pc.all(pc.match_substring_regex(date_text, r"^[0-9]{4}-[0-9]{2}-[0-9]{2}$")).as_py():
        raise ValueError("asof csv: available_at is not YYYY-MM-DD")
    days = pc.cast(pc.cast(pc.strptime(date_text, format="%Y-%m-%d", unit="s"), pa.date32()), pa.int32()).to_numpy().astype(np.int64)
    lower = pc.utf8_lower(value_text)
    nan_literal = pc.or_(pc.equal(lower, "nan"), pc.equal(pc.utf8_length(value_text), 0)).to_numpy(zero_copy_only=False)
    decimal = pc.match_substring_regex(value_text, r"^-?([0-9]+\.?[0-9]*|\.[0-9]+)([eE][-+]?[0-9]+)?$").to_numpy(zero_copy_only=False)
    if np.any(~nan_literal & ~decimal):
        raise ValueError("asof csv: value is not a finite decimal")
    values = np.full(len(ids), np.nan)
    if decimal.any():
        parsed = pc.cast(pc.filter(value_text, pa.array(decimal)), pa.float64()).to_numpy()
        if not np.all(np.isfinite(parsed)):
            raise ValueError("asof csv: value is not a finite decimal")
        values[decimal] = parsed
    order = np.lexsort((days, ids))
    ids, days, values = ids[order], days[order], values[order]
    if np.any((ids[1:] == ids[:-1]) & (days[1:] == days[:-1])):
        raise ValueError("asof csv: duplicate (security_id, available_at)")
    return ids, days, values


def finra_field(name: str, finra: Path, role: Role, output: Path, schedule, budget: Budget):
    spec = FIELDS[name]
    path = finra / "asof" / spec["source_file"]
    captured = identity(path)
    blob = path.read_bytes()
    digest = sha_bytes(blob)
    receipt_bytes = (finra / "asof" / "manifest.json").read_bytes()
    receipt = json.loads(receipt_bytes)
    pinned = receipt.get("outputs", {}).get(name, {}).get("sha256")
    if pinned != digest:
        raise ValueError(f"{name}: CSV bytes do not match the as-of producer receipt (asof/manifest.json)")
    budget.admit(len(blob) * 6, f"{name}-parse")
    ids, days, values = parse_asof_csv(blob)
    del blob
    dissemination, vintage_cutoff, schedule_source = schedule
    if not np.all(np.isin(days, dissemination)):
        raise ValueError(f"{name}: an available_at is not an official FINRA dissemination date")
    sealed = days >= day_of(SEAL)
    rows_total, rows_sealed = len(ids), int(np.count_nonzero(sealed))
    ids, days, values = ids[~sealed], days[~sealed], values[~sealed]
    pos, on = role.columns_of(ids)
    rows_matched = int(np.count_nonzero(on))
    col, days, values = pos[on].astype(np.int64), days[on], values[on]
    key = (col << 32) | days  # rows are sorted by (id, day) and the axis is sorted: keys ascend
    if np.any(np.diff(key) <= 0):
        raise ValueError(f"{name}: internal as-of key order violated")
    # Sentinel -1 (column -1) keeps every lookup in bounds and is never visible.
    key, values = np.concatenate(([-1], key)), np.concatenate(([np.nan], values))
    del ids, pos, on, col
    budget.report(f"{name}-parsed", rows=rows_total, matched=rows_matched, sealed=rows_sealed)
    writer = FieldWriter(output, name, role)
    columns = np.arange(role.n, dtype=np.int64)
    vintage_cells = 0
    for t in range(role.n_dates):
        d = int(role.days[t])
        found = np.searchsorted(key, (columns << 32) | d, side="left") - 1  # last key < (col, d): strict
        visible = (key[found] >> 32) == columns
        src_day = key[found] & 0xFFFFFFFF
        safe = found
        visible &= (d - src_day) <= FINRA_MAX_STALE_DAYS
        row = np.where(visible, values[safe], np.nan)
        writer.write(row)
        if vintage_cutoff is not None:
            vintage_cells += int(np.count_nonzero(visible & np.isfinite(row) & (role.member[t] != 0) & (src_day <= vintage_cutoff)))
        if t % 256 == 0:
            budget.check(f"{name}-join")
    writer.close()
    if identity(path) != captured:
        raise ValueError(f"{name}: source changed while reading")
    coverage = writer.coverage()
    coverage["vintage_risk"] = {
        "rule": f"visible row disseminated on or before {date_of(vintage_cutoff) if vintage_cutoff is not None else None} (settlement before {FINRA_REPUBLICATION_SETTLEMENT_BEFORE.isoformat()}): later FINRA republication",
        "finite_member_cells": vintage_cells}
    sources = [{"path": str(path.resolve()), "bytes": captured[2], "sha256": digest},
               {"path": str((finra / "asof" / "manifest.json").resolve()), "bytes": len(receipt_bytes), "sha256": sha_bytes(receipt_bytes)},
               schedule_source]
    stats = {"rows_total": rows_total, "rows_available_on_or_after_2025_dropped": rows_sealed,
             "rows_matched_axis": rows_matched, "rows_ignored_unknown_id": rows_total - rows_sealed - rows_matched,
             "max_stale_days": FINRA_MAX_STALE_DAYS}
    return writer, sources, coverage, stats


# ---------------------------------------------------------------------------
# TickerHistory3 vendor rows
# ---------------------------------------------------------------------------

def tickerhistory_fields(names, th: Path, role: Role, output: Path, budget: Budget):
    iv_names = [n for n in names if FIELDS[n]["column"].startswith("atmCenI_")]
    want_earn, want_shares = "earn_recent" in names, "shares_out" in names
    captured = identity(th)
    pf = pq.ParquetFile(th, memory_map=False)
    columns = ["tradingDate", "securityID"] + [FIELDS[n]["column"] for n in iv_names]
    columns += ["earnFlag"] if want_earn else []
    columns += ["shares", "cumulReturnFactor"] if want_shares else []
    schema = pf.schema_arrow
    for c in columns:
        if schema.field(c).type != TH_TYPES[c]:
            raise ValueError(f"TickerHistory column {c} is {schema.field(c).type}, expected {TH_TYPES[c]}")
    budget.report("th-hash-start", bytes=captured[2])
    digest = sha_file(th, budget)
    if role.source_sha256 is None or digest != role.source_sha256:
        raise ValueError("TickerHistory SHA-256 differs from the role's source_sha256 (a different vendor file)")
    if identity(th) != captured:
        raise ValueError("TickerHistory changed during hashing")
    budget.report("th-hash-complete", sha256=digest)
    n, nd = role.n, role.n_dates
    first, last = int(role.days[0]), int(role.days[-1])
    pre = SHARES_LAG_DAYS + SHARES_MAX_AGE_DAYS if want_shares else 0
    # Extended date axis: calendar days before the role (shares lag only) then the role sessions.
    ext_days = np.concatenate((np.arange(first - pre, first, dtype=np.int64), role.days))
    n_ext = len(ext_days)
    planned = n_ext * n * 2 + nd * n * (4 * len(iv_names) + (1 if want_earn else 0))
    planned += (n_ext * n * 8 + nd * n * 8) if want_shares else 0
    budget.admit(planned + (64 << 20), "tickerhistory-matrices")
    counts = np.zeros((n_ext, n), dtype=np.uint16)
    iv = {k: np.full((nd, n), np.nan, dtype=np.float32) for k in iv_names}
    earn = np.full((nd, n), -1, dtype=np.int8) if want_earn else None
    q = np.full((n_ext, n), np.nan) if want_shares else None       # shares / cumulReturnFactor (file basis)
    crf = np.full((nd, n), np.nan) if want_shares else None        # cumulReturnFactor at the session
    suspect = np.zeros(n, dtype=bool)
    st = {"rows_scanned": 0, "rows_on_or_after_2025_skipped": 0, "rows_selected": 0, "rows_off_role_calendar": 0,
          "earnflag_null": 0, "earnflag_unexpected": 0, "iv_nonpositive_or_nonfinite": 0,
          "shares_rows_above_a9_ceiling": 0}
    for b, batch in enumerate(pf.iter_batches(batch_size=65536, columns=columns, use_threads=False)):
        budget.check("th-batch")
        st["rows_scanned"] += batch.num_rows
        d = pc.fill_null(batch.column("tradingDate").cast(pa.int32()), -1).to_numpy().astype(np.int64)
        st["rows_on_or_after_2025_skipped"] += int(np.count_nonzero(d >= day_of(SEAL)))
        idx = np.flatnonzero((d >= first - pre) & (d <= last))
        if not len(idx):
            continue
        sid = pc.fill_null(batch.column("securityID"), 0).to_numpy()[idx]
        pos, on = role.columns_of(sid)
        idx, j, dd = idx[on], pos[on], d[idx][on]
        e = np.minimum(np.searchsorted(ext_days, dd), n_ext - 1)
        cal = ext_days[e] == dd
        st["rows_off_role_calendar"] += int(np.count_nonzero(~cal))
        idx, j, e = idx[cal], j[cal], e[cal]
        if not len(idx):
            continue
        st["rows_selected"] += len(idx)
        np.add.at(counts, (e, j), 1)
        sub = batch.take(pa.array(idx, type=pa.int64()))
        role_rows = e >= pre
        t, jr = e[role_rows] - pre, j[role_rows]
        for k in iv_names:
            v = sub.column(FIELDS[k]["column"]).to_numpy(zero_copy_only=False).astype(np.float32)[role_rows]
            bad = ~(np.isfinite(v) & (v > 0))
            st["iv_nonpositive_or_nonfinite"] += int(np.count_nonzero(bad & ~np.isnan(v)))
            iv[k][t, jr] = np.where(bad, np.float32(np.nan), v)
        if want_earn:
            flag = sub.column("earnFlag").filter(pa.array(role_rows))
            code = np.full(len(flag), -1, dtype=np.int8)
            known = np.zeros(len(flag), dtype=bool)
            for text, value in EARN_CODES.items():
                hit = pc.fill_null(pc.equal(flag, text), False).to_numpy(zero_copy_only=False)
                code[hit] = value
                known |= hit
            null = flag.is_null().to_numpy(zero_copy_only=False)
            st["earnflag_null"] += int(np.count_nonzero(null))
            st["earnflag_unexpected"] += int(np.count_nonzero(~known & ~null))
            earn[t, jr] = code
        if want_shares:
            shares = pc.fill_null(sub.column("shares"), 0).to_numpy().astype(np.float64)
            factor = sub.column("cumulReturnFactor").to_numpy(zero_copy_only=False).astype(np.float64)
            above = shares > THOUSANDS_ROW_CEILING
            st["shares_rows_above_a9_ceiling"] += int(np.count_nonzero(above))
            suspect[j[above]] = True
            good_factor = np.isfinite(factor) & (factor > 0)
            with np.errstate(divide="ignore", invalid="ignore"):
                q[e, j] = np.where((shares > 0) & ~above & good_factor, shares / factor, np.nan)
            crf[t, jr] = np.where(good_factor, factor, np.nan)[role_rows]
        if b % 64 == 0:
            budget.report("th-batch", batch=b, rows_scanned=st["rows_scanned"], rows_selected=st["rows_selected"])
    if identity(th) != captured:
        raise ValueError("TickerHistory changed while reading")
    # All positive duplicate (date, securityID) keys are quarantined, never picked.
    dup = counts > 1
    st["duplicate_keys_quarantined"] = int(np.count_nonzero(dup))
    dup_role = dup[pre:]
    for k in iv_names:
        iv[k][dup_role] = np.nan
    if want_earn:
        earn[dup_role] = -1
    if want_shares:
        q[dup] = np.nan
        crf[dup_role] = np.nan
        st["shares_lines_withheld_c81"] = int(np.count_nonzero(suspect))
    del counts, dup, dup_role
    source = [{"path": str(th.resolve()), "bytes": captured[2], "sha256": digest,
               "row_groups": pf.metadata.num_row_groups, "rows": pf.metadata.num_rows}]
    results = {}
    for k in iv_names:
        w = FieldWriter(output, k, role)
        for t in range(nd):
            w.write(iv[k][t].astype(np.float64))
        w.close()
        results[k] = w
        del iv[k]
        budget.check(f"{k}-write")
    if want_earn:
        w = FieldWriter(output, "earn_recent", role)
        for t in range(nd):
            w.write(np.where(earn[t] < 0, np.nan, earn[t].astype(np.float64)))
        w.close()
        results["earn_recent"] = w
        del earn
    if want_shares:
        w = FieldWriter(output, "shares_out", role)
        last_valid = np.full(n, -1, dtype=np.int64)
        cols = np.arange(n)
        p = 0
        for t in range(nd):
            lag = int(role.days[t]) - SHARES_LAG_DAYS
            while p < n_ext and ext_days[p] <= lag:
                last_valid[np.isfinite(q[p])] = p
                p += 1
            src = np.maximum(last_valid, 0)
            fresh = (last_valid >= 0) & (ext_days[src] >= lag - SHARES_MAX_AGE_DAYS)
            row = np.where(fresh, q[src, cols] * crf[t] * SHARES_UNIT, np.nan)
            row[suspect] = np.nan
            w.write(row)
            if t % 256 == 0:
                budget.check("shares_out-write")
        w.close()
        results["shares_out"] = w
    return results, source, st, digest


# ---------------------------------------------------------------------------
# Research lake (spine_monthly, line_types)
# ---------------------------------------------------------------------------

def lake_file(lake: Path, listed: dict, rel: str):
    entry = listed.get(rel)
    if entry is None:
        raise ValueError(f"lake file {rel} is not listed in the lake manifest")
    blob = (lake / rel).read_bytes()
    digest = sha_bytes(blob)
    if len(blob) != entry["bytes"] or digest != entry["sha256"]:
        raise ValueError(f"lake file {rel} does not match the lake manifest (being rewritten?)")
    table = pq.read_table(pa.BufferReader(blob))
    return table, {"path": str((lake / rel).resolve()), "bytes": len(blob), "sha256": digest}


def line_ids(role: Role, line_id):
    line_id = line_id.combine_chunks() if isinstance(line_id, pa.ChunkedArray) else line_id
    if line_id.null_count or not pc.all(pc.starts_with(line_id, LINE_PREFIX)).as_py():
        raise ValueError("lake line_id is not TBLTICKERHISTORY-<securityID>")
    digits = pc.utf8_slice_codeunits(line_id, len(LINE_PREFIX))
    if not pc.all(pc.match_substring_regex(digits, r"^[0-9]+$")).as_py():
        raise ValueError("lake line_id suffix is not a securityID")
    sid = pc.cast(digits, pa.int64()).to_numpy()
    pos, on = role.columns_of(sid)
    return sid, pos, on


def lake_fields(names, lake: Path, role: Role, output: Path, budget: Budget):
    manifest_path = lake / "_manifest.json"
    manifest_bytes = manifest_path.read_bytes()
    m = json.loads(manifest_bytes)
    listed = {f["path"]: f for ds in ("spine_monthly", "line_types") for f in m["datasets"][ds]["files"]}
    sources = [{"path": str(manifest_path.resolve()), "bytes": len(manifest_bytes), "sha256": sha_bytes(manifest_bytes),
                "snapshot_id": m.get("snapshot_id"), "snapshot_sha256": m.get("snapshot_sha256")}]
    st = {}
    results = {}
    spine_names = [x for x in names if x in ("mktcap_lagged", "size_grp")]
    if spine_names:
        first_year = int(role.years[0]) - 1
        listed_years = sorted(int(p.split("year=")[1].split("/")[0]) for p in listed if p.startswith("spine_monthly/"))
        f_day, f_col, f_me, f_sg = [], [], [], []
        formations = set()
        for year in range(first_year, int(role.years[-1]) + 1):
            if year >= SEAL.year:
                raise ValueError("spine year on or after 2025 requested")
            rel = f"spine_monthly/year={year}/part-0.parquet"
            if rel not in listed:
                if listed_years and year < listed_years[0]:
                    continue
                raise ValueError(f"spine year {year} missing from the lake manifest")
            table, src = lake_file(lake, listed, rel)
            sources.append(src)
            for c, typ in (("formation_date", pa.date32()), ("me_line", pa.float64()), ("size_grp", pa.string()), ("line_id", pa.string())):
                if table.schema.field(c).type != typ:
                    raise ValueError(f"spine column {c} is {table.schema.field(c).type}, expected {typ}")
            fd = pc.fill_null(table.column("formation_date").cast(pa.int32()), -1).to_numpy().astype(np.int64)
            if np.any(fd < 0):
                raise ValueError("spine row without formation_date")
            keep = fd < day_of(SEAL)
            formations.update(np.unique(fd[keep]).tolist())
            _, pos, on = line_ids(role, table.column("line_id"))
            sel = keep & on
            f_day.append(fd[sel])
            f_col.append(pos[sel])
            f_me.append(table.column("me_line").to_numpy(zero_copy_only=False).astype(np.float64)[sel])
            sg_text = table.column("size_grp").combine_chunks().filter(pa.array(sel))
            sg = np.full(len(sg_text), np.nan)
            known = sg_text.is_null().to_numpy(zero_copy_only=False)
            for text, code in SIZE_CODES.items():
                hit = pc.fill_null(pc.equal(sg_text, text), False).to_numpy(zero_copy_only=False)
                sg[hit] = code
                known |= hit
            if not known.all():
                raise ValueError("unexpected spine size_grp value")
            f_sg.append(sg)
            budget.check(f"spine-{year}")
        fdays = np.array(sorted(formations), dtype=np.int64)
        months = [(EPOCH + dt.timedelta(days=int(x))) for x in fdays]
        if any((b.year * 12 + b.month) - (a.year * 12 + a.month) != 1 for a, b in zip(months, months[1:])):
            raise ValueError("spine formations are not month-contiguous (partial lake?)")
        day_all, col_all = np.concatenate(f_day), np.concatenate(f_col)
        k = np.searchsorted(fdays, day_all)
        flat = k * role.n + col_all
        if len(np.unique(flat)) != len(flat):
            raise ValueError("duplicate spine (formation, line) row")
        me = np.full((len(fdays), role.n), np.nan)
        sgm = np.full((len(fdays), role.n), np.nan)
        me_v = np.concatenate(f_me)
        me[k, col_all] = np.where(np.isfinite(me_v) & (me_v > 0), me_v, np.nan)
        sgm[k, col_all] = np.concatenate(f_sg)
        st["spine_formations"] = len(fdays)
        st["spine_first_formation"] = date_of(fdays[0]) if len(fdays) else None
        st["spine_last_formation"] = date_of(fdays[-1]) if len(fdays) else None
        st["spine_rows_on_axis"] = int(len(flat))
        writers = {x: FieldWriter(output, x, role) for x in spine_names}
        for t in range(role.n_dates):
            f = int(np.searchsorted(fdays, role.days[t], side="left")) - 1  # latest formation < session
            for x, w in writers.items():
                src = me if x == "mktcap_lagged" else sgm
                w.write(src[f] if f >= 0 else np.full(role.n, np.nan))
        for x, w in writers.items():
            w.close()
            results[x] = w
    if "is_common" in names:
        table, src = lake_file(lake, listed, "line_types/year=0/part-0.parquet")
        sources.append(src)
        if table.schema.field("security_type").type != pa.string():
            raise ValueError("line_types.security_type is not a string")
        sid, pos, on = line_ids(role, table.column("line_id"))
        if len(np.unique(sid)) != len(sid):
            raise ValueError("duplicate line_types line_id")
        stype = table.column("security_type").combine_chunks()
        eligible = pc.fill_null(pc.is_in(stype, value_set=pa.array(list(ELIGIBLE_SECURITY_TYPES))), False).to_numpy(zero_copy_only=False)
        vec = np.full(role.n, np.nan)
        vec[pos[on]] = np.where(eligible[on], 1.0, 0.0)
        types = stype.filter(pa.array(on)).to_pylist()
        st["line_types_security_type_on_axis"] = {str(k): types.count(k) for k in sorted(set(map(str, types)))}
        st["line_types_ids_missing_on_axis"] = int(np.count_nonzero(np.isnan(vec)))
        w = FieldWriter(output, "is_common", role)
        for t in range(role.n_dates):
            w.write(vec)
        w.close()
        results["is_common"] = w
    if sha_bytes(manifest_path.read_bytes()) != sources[0]["sha256"]:
        raise ValueError("lake manifest changed while reading")
    return results, sources, st


# ---------------------------------------------------------------------------
# Role-derived broadcast market return
# ---------------------------------------------------------------------------

def market_return_field(role: Role, output: Path, budget: Budget):
    """Stream close/raw_close/present row by row, hashing exactly the bytes used."""
    n = role.n
    specs = {"close.f64": "<f8", "raw_close.f64": "<f8", "present.u8": "u1"}
    handles, hashes, sources = {}, {}, []
    for name, dtype in specs.items():
        entry = role.manifest["files"][name]
        if entry["bytes"] != role.n_dates * n * np.dtype(dtype).itemsize:
            raise ValueError(f"role {name} size disagrees with the role shape")
        handles[name], hashes[name] = (role.dir / name).open("rb"), hashlib.sha256()

    def row_of(name):
        size = n * np.dtype(specs[name]).itemsize
        blob = handles[name].read(size)
        if len(blob) != size:
            raise ValueError(f"role {name} is truncated")
        hashes[name].update(blob)
        return np.frombuffer(blob, dtype=specs[name])

    writer = FieldWriter(output, "mkt_ret", role)
    counts, guarded_total, prev = [], 0, None
    try:
        for d in range(role.n_dates):
            close, raw, present = row_of("close.f64"), row_of("raw_close.f64"), row_of("present.u8") != 0
            row = np.full(n, np.nan)
            if prev is not None:
                c0, r0, p0 = prev
                with np.errstate(divide="ignore", invalid="ignore", over="ignore"):
                    priced = (p0 & present & np.isfinite(c0) & np.isfinite(close) & np.isfinite(r0) & np.isfinite(raw)
                              & (c0 > 0) & (close > 0) & (r0 > 0) & (raw > 0))
                    r = close / c0 - 1
                    log_return = np.log(close) - np.log(c0)
                    raw_log_return = np.log(raw) - np.log(r0)
                    guarded = priced & (~np.isfinite(r) | (np.abs(log_return) > 1.5)
                                        | (np.abs(log_return) > np.abs(raw_log_return) + .10))
                member = role.member[d - 1] != 0
                use = member & priced & ~guarded
                k = int(np.count_nonzero(use))
                counts.append(k)
                guarded_total += int(np.count_nonzero(member & guarded))
                if k:
                    row[present] = math.fsum(r[use].tolist()) / k
            writer.write(row)
            prev = (close, raw, present)
            if d % 256 == 0:
                budget.check("mkt_ret")
        for name in specs:
            if handles[name].read(1):
                raise ValueError(f"role {name} is longer than the role shape")
    finally:
        for h in handles.values():
            h.close()
    writer.close()
    for name in specs:
        if hashes[name].hexdigest() != role.manifest["files"][name]["sha256"]:
            raise ValueError(f"role {name} bytes do not match the role manifest")
        sources.append({"path": str((role.dir / name).resolve()), "bytes": role.manifest["files"][name]["bytes"],
                        "sha256": hashes[name].hexdigest()})
    sources.append({"path": str((role.dir / "member.u8").resolve()), "bytes": role.manifest["files"]["member.u8"]["bytes"],
                    "sha256": role.manifest["files"]["member.u8"]["sha256"]})
    c = np.array(counts, dtype=np.int64)
    score = c[max(role.score_begin - 1, 0):max(role.score_end - 1, 0)]
    stats = {"contributors_per_session": {
        "sessions": len(c), "min": int(c.min()) if len(c) else None, "median": float(np.median(c)) if len(c) else None,
        "max": int(c.max()) if len(c) else None, "zero_contributor_sessions": int(np.count_nonzero(c == 0)),
        "score_window_min": int(score.min()) if len(score) else None,
        "score_window_median": float(np.median(score)) if len(score) else None},
        "guarded_member_intervals": guarded_total}
    return writer, sources, stats


# ---------------------------------------------------------------------------
# Orchestration
# ---------------------------------------------------------------------------

def run(role_dir: Path, role_sha256: str, output: Path, fields=DEFAULT_FIELDS, *, max_rss_mib=700,
        max_seconds=1800.0, finra: Path = DEFAULT_FINRA, tickerhistory: Path = DEFAULT_TICKERHISTORY,
        lake: Path = DEFAULT_LAKE):
    fields = list(fields)
    if not fields or len(set(fields)) != len(fields) or any(f not in FIELDS for f in fields):
        raise ValueError(f"--fields must be distinct names from {', '.join(FIELDS)}")
    selected = [f for f in FIELDS if f in fields]  # registry order: stable manifests
    budget = Budget(max_rss_mib, max_seconds)
    role = Role(role_dir, role_sha256)
    budget.report("role-admitted", dates=role.n_dates, instruments=role.n)
    output.mkdir(parents=False, exist_ok=False)  # exclusive; never reuse or replace
    outcome = {}
    source_checks = {}
    groups = {g: [f for f in selected if FIELDS[f]["group"] == g] for g in ("role", "finra", "th", "lake")}
    field_stats = {}
    if groups["role"]:
        w, src, stats = market_return_field(role, output, budget)
        outcome["mkt_ret"] = (w, src, w.coverage())
        field_stats["mkt_ret"] = stats
        budget.report("mkt_ret-complete", **stats["contributors_per_session"])
    if groups["finra"]:
        schedule = read_schedule(finra)
        for name in groups["finra"]:
            w, src, cov, stats = finra_field(name, finra, role, output, schedule, budget)
            outcome[name] = (w, src, cov)
            source_checks[name] = stats
            budget.report(f"{name}-complete", **{k: cov[k] for k in ("finite_member_frac",)})
    if groups["th"]:
        writers, src, stats, _ = tickerhistory_fields(groups["th"], tickerhistory, role, output, budget)
        source_checks["tickerhistory"] = stats
        for name, w in writers.items():
            outcome[name] = (w, src, w.coverage())
        budget.report("tickerhistory-complete", **stats)
    if groups["lake"]:
        writers, src, stats = lake_fields(groups["lake"], lake, role, output, budget)
        source_checks["lake"] = stats
        for name, w in writers.items():
            outcome[name] = (w, src, w.coverage())
        budget.report("lake-complete")
    files, entries = {}, []
    for name in selected:
        w, src, cov = outcome[name]
        files[w.path.name] = {"bytes": w.path.stat().st_size, "sha256": sha_file(w.path, budget)}
        if files[w.path.name]["bytes"] != role.n_dates * role.n * 8:
            raise ValueError(f"{name}: output size mismatch")
        spec = FIELDS[name]
        entry = {"name": name, "file": w.path.name, "dtype": "<f8", "layout": "date-major",
                 "shape": [role.n_dates, role.n], "units": spec["units"], "clock": spec["clock"],
                 "staleness": spec["staleness"], "source_columns": spec["source_columns"],
                 "sources": src, "caveats": spec["caveats"], "coverage": cov,
                 "sha256": files[w.path.name]["sha256"]}
        if "definition" in spec:
            entry["definition"] = spec["definition"]
        if name in field_stats:
            entry["stats"] = field_stats[name]
        entries.append(entry)
    # Re-pin the role: the axes must not have changed underneath the run.
    Role(role_dir, role_sha256)
    manifest = {
        "schema": SCHEMA, "status": "complete",
        "role": {"path": str(role_dir.resolve()), "manifest_sha256": role.manifest_sha256,
                 "sessions_sha256": role.sessions_sha256, "ids_sha256": role.ids_sha256,
                 "member_sha256": role.manifest["files"]["member.u8"]["sha256"],
                 "dates": role.n_dates, "instruments": role.n, "score_begin": role.score_begin,
                 "score_end": role.score_end, "first_session": date_of(role.days[0]),
                 "last_session": date_of(role.days[-1]), "source_sha256": role.source_sha256,
                 "clock_recipe": role.manifest.get("clock_recipe")},
        "instrument_namespace": "spiderrock.securityID",
        "seal": {"exclusive_end": SEAL.isoformat(),
                 "rule": "every source row available on or after 2025-01-01 is dropped before use; role sessions asserted < 2025-01-01"},
        "cell_rule": "NaN where the field is not visible at the session decision or the source is absent",
        "coverage_basis": "member.u8 cells of the role (member cells with a finite value / member cells)",
        "fields": entries, "files": files,
        "excluded_source_columns": EXCLUDED_SOURCE_COLUMNS,
        "source_checks": source_checks,
        "code_sha256": sha_file(Path(__file__)),
        "historical_vintage_verified": False, "common_stock_verified": False,
    }
    publish(output / "manifest.json", manifest)
    budget.report("fields-complete", fields=len(entries), peak_rss_mib=budget.peak >> 20)
    return manifest


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--role", required=True, type=Path, help="published recent-research role directory")
    p.add_argument("--role-sha256", required=True, help="SHA-256 of the role's manifest.json")
    p.add_argument("--output", required=True, type=Path, help="new exclusive output directory")
    p.add_argument("--fields", default=",".join(DEFAULT_FIELDS), help="comma-separated subset of: " + ",".join(FIELDS))
    p.add_argument("--max-rss-mib", type=int, default=700)
    p.add_argument("--max-seconds", type=float, default=1800.0)
    p.add_argument("--finra", type=Path, default=DEFAULT_FINRA, help="FINRA short-interest root (asof/, dissemination_schedule.csv)")
    p.add_argument("--tickerhistory", type=Path, default=DEFAULT_TICKERHISTORY)
    p.add_argument("--lake", type=Path, default=DEFAULT_LAKE, help="research lake snapshot directory")
    a = p.parse_args(argv)
    run(a.role, a.role_sha256, a.output, [x.strip() for x in a.fields.split(",") if x.strip()],
        max_rss_mib=a.max_rss_mib, max_seconds=a.max_seconds, finra=a.finra, tickerhistory=a.tickerhistory, lake=a.lake)


if __name__ == "__main__":
    main()
