#!/usr/bin/env python3
"""audit_fund_fields.py -- T25 fundamentals fields QA / point-in-time audit (TRAIN only; the root runs it).

Audits the library-v4 issuer fields (28 fund items, me_company, grp_sic2/ff12/ff49) of the TRAIN fields-v5 artifact
against the inputs they were built from: the TRAIN role, the T19 identity bridge, the T20 fundamental events artifact
and the raw sources behind the events (CompanyFacts CF-R batches, FSDS SUB acceptance clocks). Selection-neutral data
audit: no return, IC or performance statistic of any signal is computed; nothing is chosen from the results.

Checks (each prints PASS/FLAG with numbers; results merge into DIR/audit.json and DIR/audit.md is re-rendered; a
re-run of a check replaces only that check, and C2 merges per item):

  C1 coverage   per field x year coverage on member and common-stock member cells; an independent re-join (bridge ->
                events -> lagged filing clock -> staleness) must reproduce every issuer-field cell bit for bit; NaN
                reasons (no link / secondary line / no event / stale / item NaN or unmapped) vs the producer's counts
  C2 recon      --sample random (id, date) member cells per core item (be, at, cfo_ttm, ni_ttm, sale_ttm, shrs_q):
                field == the visible events row; row clock < mark(t - 1 session) and == FSDS SUB accepted_utc (or a
                labelled FC1 = filed + 46 h); a replay of the events producer on the CIK's raw CF-R facts reproduces
                the row (rel 1e-9); an independent witness derivation from raw facts equals the value (rel 1e-9) and
                was public before mark(t - 1 session)
  C3 vintage    restatement vintages per (CIK, period_end), in-place revisions seen by the fields, FC1 fallback
                counts, amendment rows, anchor monotonicity, staleness domain
  C4 split      split contamination: shrs_q vs shrs_q_lag4 and shares_out vs delay(shares_out, 252) on role names
                with a split (role close/raw_close factor step) inside the ratio's window
  C5 ratios     be/me_company, gp_ttm/at, cfo_ttm/at, sue, noa/at: quantiles, share |x| > 10, sign of me_company
  C6 groups     FF12 / FF49 (and SIC2) group sizes per day among finite-fundamental members
  C7 identity   TRAIN vs VAL fields-v5 field-definition identity (VAL manifest JSON text and code blob only)

Hygiene (TRAIN only): the role must end before 2023-01-01 (refused otherwise); events and SIC rows are filtered to
accepted_utc < 2023-01-01 at parquet decode; CF-R facts are decoded with filed_date < 2023-01-01; no FSDS SUB quarter
after 2022q4 is opened; the VAL fields manifest is read as JSON text only and projected to its definition keys and
code identity at once (its coverage/statistics keys are discarded unread); no VAL payload is opened; the events
manifest's all-years counts are discarded unread. Every input is pinned by SHA-256 (defaults: the v4 TRAIN run).

Suggested grouping (each invocation <= 180 s / <= 1536 MiB; cooperative --max-seconds / --max-rss-mib):
  1. --checks C1,C3,C7
  2. --checks C4,C5,C6
  3. --checks C2 --items be,at,shrs_q
  4. --checks C2 --items cfo_ttm,ni_ttm,sale_ttm
Exit codes: 0 done (PASS and FLAG are findings), 1 a check raised (recorded as ERROR), 2 usage / pin / seal refusal,
3 budget stop (checks completed before it are kept; re-run the rest).
"""
from __future__ import annotations

import argparse
import bisect
import csv
import datetime as dt
import hashlib
import io
import json
import math
import os
import sys
import time
import traceback
from pathlib import Path

import numpy as np
import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[3]
TOOLS_DIR = REPO_ROOT / "atx-engine" / "tools"
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import build_fundamental_events as bfe  # noqa: E402  events producer (replay, clock rules, concept map)
import prepare_research_fields as prf  # noqa: E402  fields producer (role reader, column parsers, SIC mapping)

SCHEMA = "atx.t25-fund-fields-audit/v1"
EPOCH = dt.date(1970, 1, 1)
UTC = dt.timezone.utc
DAY_NS = 86_400_000_000_000
DAY_US = 86_400_000_000
MARK_NS = 22 * 3_600_000_000_000
TRAIN_SEAL = dt.date(2023, 1, 1)                 # TRAIN ends 2022-12-30; validation starts 2023
TRAIN_SEAL_NS = (TRAIN_SEAL - EPOCH).days * DAY_NS
TRAIN_SEAL_DT = dt.datetime(2023, 1, 1, tzinfo=UTC)
LAST_TRAIN_SUB_QUARTER = "2022q4"

BUILD = Path("C:/atx-wt/pool-2/build-equity")
DEFAULTS = {
    "fields": BUILD / "recent-fast-train-2020-2022-v2-fields-v5",
    "fields_sha256": "4e02b7dbd644b1330e932505f8315a59b564f4458705198c76ae8926995defff",
    "role": BUILD / "recent-fast-train-2020-2022-v2",
    "role_sha256": "210fff9687aa6b16e74c65104d77a916d708dca6986e77b06bac27556c48d1de",
    "identity_bridge": BUILD / "identity-bridge-r4-v1",
    "identity_bridge_sha256": "ddf9716459a1116b85f713ca9cb788c3db753a6e1fea8eba335ed34320baebaa",
    "fund_events": BUILD / "fundamental-events-v1",
    "fund_events_sha256": "519ecc1ad9d5036cbf3dc4af440291dff3f54530b2f611a8d68da1c193ad97e5",
    "val_fields_manifest": BUILD / "recent-fast-validation-2023-2024-v1-fields-v5" / "manifest.json",
    "val_fields_sha256": "10719e2623c733b6d671936158643f9cfae99aacb04e0ffffac7dff19c42693b",
    "lake": prf.DEFAULT_LAKE,
    "lake_sha256": "c3a21f39b0490e38ea0ffbc07e583ac378df1ed7d8e745693c0928b398561eeb",
}

CHECKS = ("C1", "C2", "C3", "C4", "C5", "C6", "C7")
CHECK_TITLES = {"C1": "coverage, NaN reasons and join reproduction", "C2": "raw-source reconciliation of sampled cells",
                "C3": "restatement vintages and FC1 fallback", "C4": "split contamination",
                "C5": "ratio sanity distributions", "C6": "industry group sizes", "C7": "TRAIN/VAL field-definition identity"}
CORE_ITEMS = ("be", "at", "cfo_ttm", "ni_ttm", "sale_ttm", "shrs_q")
FUND_NAMES = tuple(x[0] for x in prf.FUND_ITEMS)
GRP_NAMES = ("grp_sic2", "grp_ff12", "grp_ff49")
ME_NAME = "me_company"

# Declared audit parameters (fixed before any real run; a change needs a fresh --output).
SAMPLE_CELLS = 50
SEED = 20260927
REL_TOL = 1e-9
COVERAGE_MIN_CORE_COMMON = 0.50   # C1: core-item coverage on common member cells, every TRAIN year
COVERAGE_MAX_YEAR_STEP = 0.10     # C1: max |coverage change| between consecutive TRAIN years (common member cells)
FC1_MAX_SHARE = 0.01              # C3: FC1-clock share of rows / of finite member cells
VINTAGE_MAX_SHARE = 0.25          # C3: share of (CIK, period_end) groups whose core item took >= 2 values
SPLIT_EVENT_LN = math.log(1.25)   # C4: role factor step |ln(f_t / f_prev)| that marks a split / consolidation
SPLIT_BIG_LN = math.log(1.5)      # C4: |log ratio| > ln 1.5 is a big share change (brief)
SPLIT_RESIDUAL_LN = math.log(1.25)  # C4: explained when |ln(ratio / split factor)| <= ln 1.25
SPLIT_FLAG_SHARE = 0.25           # C4: FLAG when >= 25% of big ratios on split names are the split factor
SHARES_DELAY = 252                # C4: delay(shares_out, 252) in role sessions
FB_JUMP_LN = math.log(1.5)        # C4 continuity (controller addendum b): |ln adj step| > ln 1.5 is a jump
FB_SMALL_JUMP_LN = 0.01           # C4 continuity: finer step (the 2021-01-04 artifact steps are ~3%)
FB_CONT_FLAG_EXCESS = 0.01        # C4: FLAG when repaired names' ln-1.5 jump share that day exceeds normal days by > 0.01
FB_CONT_SMALL_FLAG_EXCESS = 0.10  # C4: FLAG when their > 0.01 step share that day exceeds normal days by > 0.10
# C4 spot checks (controller addendum a): public split dates / ratios; lines are found through the bridge CIK.
SPOT_SPLITS = (("AAPL", 320193, "2020-08-31", 4.0), ("TSLA", 1318605, "2020-08-31", 5.0),
               ("TSLA", 1318605, "2022-08-25", 3.0), ("NVDA", 1045810, "2021-07-20", 4.0),
               ("AMZN", 1018724, "2022-06-06", 20.0), ("GOOGL", 1652044, "2022-07-18", 20.0))
SPOT_OFFSETS = (-1, 0, 1, 5, 21, 63, 126, 251)
SPOT_SEARCH_SESSIONS = 5          # a detected factor step within +-5 sessions of the public date
RATIO_TAIL_ABS = 10.0             # C5
RATIO_TAIL_MAX_SHARE = 0.01       # C5: FLAG when more than 1% of a ratio's finite cells have |x| > 10
GROUP_SMALL = 5                   # C6: a group with fewer than 5 finite-fundamental members is small
GROUP_SMALL_MAX_SHARE = 0.10      # C6: pooled share of small groups (FF12, FF49)
GROUP_SMALL_MEMBER_MAX_SHARE = 0.02  # C6: pooled share of finite-fundamental members sitting in small groups
FUNDAMENTAL_BASIS = ("be", "at")  # C6: finite-fundamental member = member with finite be and at
QUANTILES = (("p0.1", 0.001), ("p1", 0.01), ("p5", 0.05), ("p25", 0.25), ("p50", 0.5), ("p75", 0.75),
             ("p95", 0.95), ("p99", 0.99), ("p99.9", 0.999))
RATIOS = (("be/me_company", "be", "me_company"), ("gp_ttm/at", "gp_ttm", "at"), ("cfo_ttm/at", "cfo_ttm", "at"),
          ("sue", "sue", None), ("noa/at", "noa", "at"))
WITNESS_COMBO_CAP = 200_000
# C2 witness derivations: the canonical metrics each item may be derived from (fundamental_events_schema.md 3.1).
ITEM_METRICS = {
    "at": ("total_assets",),
    "be": ("stockholders_equity", "equity_incl_minority", "minority_int_bs", "pref_stock"),
    "shrs_q": ("shares_diluted_avg", "shares_basic_avg"),
    "ni_ttm": ("net_income",),
    "sale_ttm": ("revenue",),
    "cfo_ttm": ("operating_cash_flow", "cfo_continuing"),
}
# C7: definition-bearing keys of a field entry / of the manifest top level / of the issuer source checks.
DEF_KEYS = ("file", "dtype", "layout", "units", "clock", "staleness", "source_columns", "caveats", "definition",
            "depends_on", "point_in_time", "non_pit_aspects", "point_in_time_reason", "fund_lag_sessions")
TOP_KEYS = ("schema", "cell_rule", "coverage_basis", "visibility_mark", "point_in_time_definition", "seal",
            "excluded_source_columns", "instrument_namespace", "code_sha256_lf", "code_git_blob_sha1",
            "non_point_in_time_fields", "historical_vintage_verified", "common_stock_verified")

CRITERIA = {
    "C1": ("PASS iff (a) the independent re-join reproduces every cell of every issuer field (0 value and 0 NaN-pattern "
           "mismatches), (b) the independent NaN-reason and link counts equal the producer's manifest counts, (c) the "
           "declared lag is 1 session and the SIC mapping table equals the manifest's, (d) every core item's coverage "
           f"on common-stock member cells is >= {COVERAGE_MIN_CORE_COMMON} in every TRAIN year, and (e) no issuer "
           f"field's common-member coverage moves by more than {COVERAGE_MAX_YEAR_STEP} between consecutive TRAIN "
           "years; else FLAG"),
    "C2": (f"per item PASS iff every sampled cell (<= {SAMPLE_CELLS} random finite member cells of the TRAIN score "
           "window) passes: field == visible events row (exact), row fresh, row clock < mark(t-1) (the lagged 22:00 "
           "UTC mark), row clock == FSDS SUB accepted_utc of its accession (or FC1 = filed + 46 h with the accession "
           f"absent from SUB <= {LAST_TRAIN_SUB_QUARTER}), the producer replay on raw CF-R facts reproduces the row "
           f"(rel {REL_TOL}), and an independent witness derivation from raw facts equals the value (rel {REL_TOL}) "
           "with every term public before mark(t-1); the check is PASS iff all six core items pass and the replay "
           "code equals the events manifest's pinned code; INCOMPLETE while items are missing"),
    "C3": (f"PASS iff FC1-clock rows are <= {FC1_MAX_SHARE:.0%} of linked TRAIN rows and of the rows behind finite "
           "core member cells, period_end is monotone per CIK, staleness_days is 200/400 only, and for every core "
           f"item <= {VINTAGE_MAX_SHARE:.0%} of (CIK, period_end) groups carry >= 2 distinct values; else FLAG"),
    "C4": ("pairs shares_out/delay(shares_out,252), adj/delay(adj,252) with adj = shares_out*raw_close/close, and "
           "shrs_q/shrs_q_lag4; population = TRAIN member cells of a name with a split event (role close/raw_close "
           "factor step |ln| >= ln 1.25) inside the ratio's window ((t-252, t]; shrs_q: (period_end - 365 d, row "
           "clock]); big = |ln ratio| > ln 1.5; explained = big and |ln(ratio / window split factor)| <= ln 1.25. "
           f"FLAG when explained / big >= {SPLIT_FLAG_SHARE} for a pair (the DSL ratio carries the split), or when on a "
           "factor-break-v1 mass session the repaired names' adj step share exceeds the pooled normal-day share by "
           f"> {FB_CONT_FLAG_EXCESS} (|ln step| > ln 1.5) or > {FB_CONT_SMALL_FLAG_EXCESS} (|ln step| > "
           f"{FB_SMALL_JUMP_LN}); spot checks of known splitters and the shrs_q concept path are reported only"),
    "C5": (f"PASS iff every ratio has <= {RATIO_TAIL_MAX_SHARE:.0%} of its finite TRAIN member cells with |x| > "
           f"{RATIO_TAIL_ABS:g} and me_company has no finite member cell <= 0; else FLAG"),
    "C6": (f"per day of the TRAIN score window, groups of finite-fundamental members (member, finite "
           f"{' and '.join(FUNDAMENTAL_BASIS)}, finite label); PASS iff for FF12 and FF49 the pooled share of groups "
           f"with < {GROUP_SMALL} members is <= {GROUP_SMALL_MAX_SHARE} and the pooled share of members in such groups "
           f"is <= {GROUP_SMALL_MEMBER_MAX_SHARE}; SIC2 is reported only"),
    "C7": ("PASS iff the TRAIN and VAL fields-v5 manifests list the same fields and every field's definition keys "
           f"({', '.join(DEF_KEYS)}), its non-role source pins, the manifest-level definition keys and code identity, "
           "and the issuer source-check definitions (lag, events/bridge pins, link rule, SIC mapping) are identical; "
           "else FLAG"),
}


class AuditError(Exception):
    """Usage, pin or seal refusal (exit 2)."""


class BudgetStop(Exception):
    """Cooperative budget stop (exit 3)."""


def need(cond, msg: str):
    if not cond:
        raise AuditError(msg)


# ---------------------------------------------------------------------------
# Small utilities
# ---------------------------------------------------------------------------

def sha_bytes(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def canonical(value) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)


def read_pinned(path: Path, sha: str | None = None, size=None, what: str = "") -> bytes:
    path = Path(path)
    need(path.exists(), f"{what}: {path} does not exist")
    blob = path.read_bytes()
    if size is not None:
        need(len(blob) == int(size), f"{what}: {path} has {len(blob)} bytes, pinned {size}")
    if sha is not None:
        need(sha_bytes(blob) == str(sha).lower(), f"{what}: {path} SHA-256 does not match its pin")
    return blob


def day_of(value: dt.date) -> int:
    return (value - EPOCH).days


def date_of(day) -> str:
    return (EPOCH + dt.timedelta(days=int(day))).isoformat()


def iso_ns(ns) -> str:
    return (dt.datetime(1970, 1, 1, tzinfo=UTC) + dt.timedelta(microseconds=int(ns) // 1000)).isoformat()


def iso_us(us) -> str:
    return (dt.datetime(1970, 1, 1, tzinfo=UTC) + dt.timedelta(microseconds=int(us))).isoformat()


def close_rel(a: float, b: float, tol: float = REL_TOL) -> bool:
    if a == b:
        return True
    if not (math.isfinite(a) and math.isfinite(b)):
        return False
    return abs(a - b) <= tol * max(1.0, abs(a), abs(b))


def same_value(a: float, b: float) -> bool:
    return (math.isnan(a) and math.isnan(b)) or close_rel(a, b)


def frac(num, den):
    return round(float(num) / float(den), 6) if den else None


def clean(x):
    """JSON-safe copy: numpy scalars to Python, non-finite floats to None."""
    if isinstance(x, dict):
        return {str(k): clean(v) for k, v in x.items()}
    if isinstance(x, (list, tuple)):
        return [clean(v) for v in x]
    if isinstance(x, np.ndarray):
        return [clean(v) for v in x.tolist()]
    if isinstance(x, (bool, np.bool_)):
        return bool(x)
    if isinstance(x, (int, np.integer)):
        return int(x)
    if isinstance(x, (float, np.floating)):
        x = float(x)
        return x if math.isfinite(x) else None
    return x


def code_identity(path: Path) -> dict:
    raw = Path(path).read_bytes()
    lf = raw.replace(b"\r\n", b"\n")
    return {"sha256": sha_bytes(raw), "sha256_lf": sha_bytes(lf),
            "git_blob_sha1": hashlib.sha1(b"blob %d\0" % len(lf) + lf).hexdigest()}


def code_matches(path: Path, pinned: str | None) -> bool:
    """A pinned raw-bytes SHA-256 matches this file up to CRLF/LF checkout differences."""
    if not pinned:
        return False
    raw = Path(path).read_bytes()
    lf = raw.replace(b"\r\n", b"\n")
    return pinned in {sha_bytes(raw), sha_bytes(lf), sha_bytes(lf.replace(b"\n", b"\r\n"))}


def quantiles(values: np.ndarray) -> dict:
    if not len(values):
        return {k: None for k, _ in QUANTILES}
    got = np.quantile(values, [q for _, q in QUANTILES])
    return {k: float(v) for (k, _), v in zip(QUANTILES, got)}


class Budget:
    """Cooperative wall-time / RSS guard polled at stage boundaries (not a kernel quota)."""

    def __init__(self, max_seconds: float, max_rss_mib: int):
        need(10 <= max_seconds <= 600, "--max-seconds must be in [10, 600]")
        need(256 <= max_rss_mib <= 1536, "--max-rss-mib must be in [256, 1536]")
        self.t0 = time.monotonic()
        self.max_seconds = float(max_seconds)
        self.max_rss = int(max_rss_mib) << 20
        self.peak = 0
        try:
            import psutil
            self._proc = psutil.Process()
        except ImportError:  # pragma: no cover - psutil ships with the admitted interpreter
            self._proc = None

    def rss(self) -> int:
        value = int(self._proc.memory_info().rss) if self._proc else 0
        self.peak = max(self.peak, value)
        return value

    def elapsed(self) -> float:
        return time.monotonic() - self.t0

    def check(self, stage: str):
        if self.elapsed() > self.max_seconds:
            raise BudgetStop(f"time budget {self.max_seconds:.0f} s exceeded at {stage}")
        if self.rss() > self.max_rss:
            raise BudgetStop(f"RSS {self.rss() >> 20} MiB exceeds --max-rss-mib at {stage}")


# ---------------------------------------------------------------------------
# Pinned inputs
# ---------------------------------------------------------------------------

def _wrap(fn, what):
    try:
        return fn()
    except (ValueError, KeyError) as exc:
        raise AuditError(f"{what}: {exc}") from None


class Inputs:
    """Every pinned manifest, verified before any check; payload readers verify bytes against those manifests."""

    def __init__(self, a, budget: Budget):
        self.a, self.budget = a, budget
        self.fields_dir = Path(a.fields)
        blob = read_pinned(self.fields_dir / "manifest.json", a.fields_sha256, what="fields manifest")
        fm = json.loads(blob)
        need(fm.get("schema") == prf.SCHEMA and fm.get("status") == "complete",
             "fields manifest is not a complete atx.research-role-fields/v1 artifact")
        self.fm = fm
        self.entries = {e["name"]: e for e in fm["fields"]}
        self.role = _wrap(lambda: prf.Role(Path(a.role), a.role_sha256), "role")
        role = self.role
        need(int(role.days[-1]) < day_of(TRAIN_SEAL),
             "role has a session on or after 2023-01-01: this audit is TRAIN only (refused)")
        need(fm["role"]["manifest_sha256"] == role.manifest_sha256, "fields manifest was built on a different role")
        need(str(fm["role"].get("last_session", "9999")) < TRAIN_SEAL.isoformat(),
             "fields manifest role reaches 2023 (refused)")
        self.n_dates, self.n = role.n_dates, role.n
        self.marks = role.days.astype(np.int64) * DAY_NS + MARK_NS
        self.sb, self.se = role.score_begin, role.score_end
        need(0 <= self.sb < self.se <= self.n_dates, "role score window is empty")
        self.issuer = [x for x in prf.ISSUER_FIELDS if x in self.entries]
        need(self.issuer, "fields manifest has no issuer field")
        lags = {self.entries[x].get("fund_lag_sessions") for x in self.issuer if prf.ISSUER_FIELDS[x]["lagged"]}
        need(len(lags) == 1 and isinstance(next(iter(lags)), int), f"issuer fields disagree on fund_lag_sessions {lags}")
        self.lag = int(lags.pop())
        need(0 <= self.lag < self.n_dates, "fund_lag_sessions out of range")
        # T20 events (manifest projected: the all-years counts are dropped unread)
        self.events_dir = Path(a.fund_events)
        em = json.loads(read_pinned(self.events_dir / "manifest.json", a.fund_events_sha256, what="fund-events manifest"))
        need(em.get("schema") == bfe.SCHEMA and em.get("status") == "complete",
             "fund-events manifest is not a complete atx.fundamental-events/v1 artifact")
        self.em = {k: em[k] for k in ("schema", "status", "files", "items", "inputs", "code_sha256",
                                      "concept_map_sha256", "parameters", "values_label", "rehearsal_identity", "seal")
                   if k in em}
        del em
        # T19 bridge
        self.bridge_dir = Path(a.identity_bridge)
        bm = json.loads(read_pinned(self.bridge_dir / "manifest.json", a.identity_bridge_sha256,
                                    what="identity-bridge manifest"))
        need(bm.get("schema") == prf.BRIDGE_ADAPTER["schema"] and bm.get("status") == "complete",
             "identity-bridge manifest is not a complete atx.identity-bridge/v1 artifact")
        self.bm = bm
        # Cross pins: the fields were built from exactly these inputs.
        issuer_checks = (fm.get("source_checks") or {}).get("issuer") or {}
        if any(prf.ISSUER_FIELDS[x]["kind"] != "me" for x in self.issuer):
            got = (issuer_checks.get("fund_events_manifest") or {}).get("sha256")
            need(got == str(a.fund_events_sha256).lower(), "fields were not built from the pinned fund-events manifest")
        srcs = {s.get("sha256") for x in self.issuer for s in self.entries[x].get("sources", [])}
        need(str(a.identity_bridge_sha256).lower() in srcs, "fields were not built from the pinned identity-bridge manifest")
        self.issuer_checks = issuer_checks
        # Lake line_types (static common-stock classification: a coverage denominator only)
        self.lake_dir = Path(a.lake)
        self.lake_manifest = json.loads(read_pinned(self.lake_dir / "_manifest.json", a.lake_sha256,
                                                    what="lake manifest"))
        self.val_path = Path(a.val_fields_manifest)
        self.pins = {
            "fields_manifest_sha256": str(a.fields_sha256).lower(), "role_manifest_sha256": role.manifest_sha256,
            "identity_bridge_manifest_sha256": str(a.identity_bridge_sha256).lower(),
            "fund_events_manifest_sha256": str(a.fund_events_sha256).lower(),
            "val_fields_manifest_sha256": str(a.val_fields_sha256).lower(), "lake_manifest_sha256": str(a.lake_sha256).lower(),
        }
        self.paths = {"fields": str(self.fields_dir), "role": str(a.role), "identity_bridge": str(self.bridge_dir),
                      "fund_events": str(self.events_dir), "val_fields_manifest": str(self.val_path),
                      "lake": str(self.lake_dir)}

    def field(self, name: str) -> np.ndarray:
        e = self.entries.get(name)
        need(e is not None, f"field {name} is not in the fields manifest")
        need(list(e["shape"]) == [self.n_dates, self.n] and e["dtype"] == "<f8", f"field {name} shape/dtype")
        f = self.fm["files"][e["file"]]
        blob = read_pinned(self.fields_dir / e["file"], f["sha256"], f["bytes"], what=f"field {name}")
        self.budget.check(f"read {name}")
        return np.frombuffer(blob, dtype="<f8").reshape(self.n_dates, self.n)

    def role_payload(self, name: str, dtype: str) -> np.ndarray:
        f = self.role.manifest["files"].get(name)
        need(f is not None, f"role payload {name} is not in the role manifest")
        blob = read_pinned(self.role.dir / name, f["sha256"], f["bytes"], what=f"role {name}")
        self.budget.check(f"read role {name}")
        return np.frombuffer(blob, dtype=dtype).reshape(self.n_dates, self.n)

    def line_types(self):
        """The pinned lake line_types table and its rows' role columns (static, non point-in-time)."""
        m = self.lake_manifest
        listed = {f["path"]: f for f in m["datasets"]["line_types"]["files"]}
        table, _ = _wrap(lambda: prf.lake_file(self.lake_dir, listed, "line_types/year=0/part-0.parquet"), "lake")
        sid, pos, on = _wrap(lambda: prf.line_ids(self.role, table.column("line_id")), "lake line_types")
        return table, sid, pos, on

    def common(self):
        """(is_common, typed) per role column from the pinned lake line_types (static, non point-in-time)."""
        table, _, pos, on = self.line_types()
        stype = table.column("security_type").combine_chunks()
        eligible = pc.fill_null(pc.is_in(stype, value_set=pa.array(list(prf.ELIGIBLE_SECURITY_TYPES))),
                                False).to_numpy(zero_copy_only=False)
        common = np.zeros(self.n, dtype=bool)
        typed = np.zeros(self.n, dtype=bool)
        common[pos[on]] = eligible[on]
        typed[pos[on]] = True
        return common, typed


# ---------------------------------------------------------------------------
# Independent re-join (the fields producer's declared rules, re-implemented from the contracts)
# ---------------------------------------------------------------------------

class Join:
    """Line -> CIK links, the lagged latest visible events / SIC row per cell, and freshness.

    Link rule: a bridge row (P or J, basis not current_ticker_verified, sr_id on the role axis) qualifies at session t
    when start <= date(t) <= end_incl and available_at <= mark(t); two qualifying CIKs make the cell ambiguous; the
    cell is primary when a qualifying P row carries the cell's CIK. Row selection: the CIK's latest row (max
    accepted_utc, tie by accession) with accepted_utc < mark(t - lag); fresh when date(t) - period_end <=
    staleness_days (SIC: date(t) - date(accepted_utc) <= 550)."""

    def __init__(self, inp: Inputs, budget: Budget):
        role = inp.role
        nd, n = inp.n_dates, inp.n
        days = role.days.astype(np.int64)
        marks = inp.marks
        self.days = days
        # --- bridge ---
        entry = inp.bm["files"]["links.parquet"]
        blob = read_pinned(inp.bridge_dir / "links.parquet", entry["sha256"], entry["bytes"], "identity-bridge links")
        t = pq.read_table(pa.BufferReader(blob), columns=list(prf.BRIDGE_ADAPTER["columns"]))

        def parse():
            return (prf.as_ids(prf.column_of(t, "sr_id"), "sr_id"), prf.as_ids(prf.column_of(t, "cik"), "cik"),
                    prf.as_days(prf.column_of(t, "start"), "start"),
                    prf.as_days(prf.column_of(t, "end_incl"), "end_incl", null_day=prf.OPEN_END_DAY),
                    prf.as_instants_ns(prf.column_of(t, "available_at"), "available_at"))
        sid, cik, start, end, avail = _wrap(parse, "identity-bridge links")
        kind = prf.as_text(prf.column_of(t, "primary")).to_pylist()
        basis = prf.as_text(prf.column_of(t, "basis")).to_pylist()
        pos, on = role.columns_of(sid)
        keep = [k for k in range(len(sid)) if kind[k] in ("P", "J") and basis[k] not in prf.BRIDGE_EXCLUDED_BASES
                and bool(on[k]) and int(avail[k]) < prf.SEAL_NS]
        self.ciks = np.unique(cik[keep]) if keep else np.zeros(0, dtype=np.int64)
        nc = len(self.ciks)
        link = np.full((nd, n), -1, dtype=np.int32)
        qualifying = []
        for k in keep:
            mask = (days >= start[k]) & (days <= end[k]) & (marks >= avail[k])
            if not mask.any():
                continue
            c, j = int(np.searchsorted(self.ciks, cik[k])), int(pos[k])
            col = link[:, j]  # a view: writes land in link
            cur = col[mask]
            col[mask] = np.where((cur == -1) | (cur == c), c, -2)
            qualifying.append((k, mask, c, j))
        primary = np.zeros((nd, n), dtype=bool)
        for k, mask, c, j in qualifying:
            if kind[k] == "P":
                primary[mask, j] |= link[mask, j] == c
        self.link = link
        self.pline = (link >= 0) & primary
        self.bridge_rows = {"total": len(sid), "used": len(keep), "qualifying_on_role": len(qualifying)}
        budget.check("join-bridge")
        # --- events (TRAIN-sealed at decode) ---
        ent = inp.em["files"]["fundamental_events.parquet"]
        blob = read_pinned(inp.events_dir / "fundamental_events.parquet", ent["sha256"], ent["bytes"], "fund events")
        cols = ["cik", "accession", "accepted_utc", "clock_basis", "filed", "form", "period_end", "staleness_days",
                "n_restated"] + list(bfe.ITEMS)
        ev = pq.read_table(pa.BufferReader(blob), columns=cols, filters=[("accepted_utc", "<", TRAIN_SEAL_DT)])
        del blob
        self.ev_all = ev  # every TRAIN-sealed row (C3)
        clock = _wrap(lambda: prf.as_instants_ns(prf.column_of(ev, "accepted_utc"), "events accepted_utc"), "events")
        need(not len(clock) or int(clock.max()) < TRAIN_SEAL_NS, "events filter let a 2023+ row through")
        ev_cik = _wrap(lambda: prf.as_ids(prf.column_of(ev, "cik"), "events cik"), "events")
        order, cidx = self._linked_order(ev_cik, clock, prf.as_text(prf.column_of(ev, "accession")))
        self.ev_cidx = cidx
        self.ev_clock = clock[order]
        self.ev_cik = ev_cik[order]
        take = pa.array(order, type=pa.int64())
        self.ev_accession = prf.as_text(prf.column_of(ev, "accession")).take(take).to_pylist()
        self.ev_basis = prf.as_text(prf.column_of(ev, "clock_basis")).take(take).to_pylist()
        self.ev_form = prf.as_text(prf.column_of(ev, "form")).take(take).to_pylist()
        self.ev_filed = prf.as_days(prf.column_of(ev, "filed"), "filed", null_day=prf.NULL_PERIOD_DAY)[order]
        self.ev_pe = prf.as_days(prf.column_of(ev, "period_end"), "period_end", null_day=prf.NULL_PERIOD_DAY)[order]
        self.ev_sd = pc.cast(prf.column_of(ev, "staleness_days"), pa.int64()).to_numpy(zero_copy_only=False)[order]
        self.ev_restated = pc.fill_null(pc.cast(prf.column_of(ev, "n_restated"), pa.int64()), 0).to_numpy(
            zero_copy_only=False)[order]
        self.ev_fc1 = np.array([b == bfe.BASIS_FC1 for b in self.ev_basis], dtype=bool)
        self.items = {x: prf.as_f64(prf.column_of(ev, x), x)[order] for x in bfe.ITEMS}
        self.rix = self._cell_rows(self.ev_cidx, self.ev_clock, nc, marks, inp.lag)
        self.has = self.rix >= 0
        rs = np.maximum(self.rix, 0)
        if len(self.ev_clock):
            self.fresh = self.has & ((days[:, None] - self.ev_pe[rs]) <= self.ev_sd[rs])
        else:
            self.fresh = np.zeros((nd, n), dtype=bool)
        budget.check("join-events")
        # --- SIC events (TRAIN-sealed at decode; invalid SIC rows carry no SIC and are skipped) ---
        ent = inp.em["files"]["sic_events.parquet"]
        blob = read_pinned(inp.events_dir / "sic_events.parquet", ent["sha256"], ent["bytes"], "sic events")
        sv = pq.read_table(pa.BufferReader(blob), columns=["cik", "accession", "accepted_utc", "sic"],
                           filters=[("accepted_utc", "<", TRAIN_SEAL_DT)])
        del blob
        sclock = _wrap(lambda: prf.as_instants_ns(prf.column_of(sv, "accepted_utc"), "sic accepted_utc"), "sic")
        need(not len(sclock) or int(sclock.max()) < TRAIN_SEAL_NS, "sic filter let a 2023+ row through")
        sic = prf.as_f64(prf.column_of(sv, "sic"), "sic")
        valid = np.isfinite(sic) & (sic == np.floor(sic)) & (sic >= prf.SIC_RANGE[0]) & (sic <= prf.SIC_RANGE[1])
        self.sic_invalid_rows = int(np.count_nonzero(~valid))
        scik = _wrap(lambda: prf.as_ids(prf.column_of(sv, "cik"), "sic cik"), "sic")
        sacc = prf.as_text(prf.column_of(sv, "accession")).filter(pa.array(valid))
        sorder, scidx = self._linked_order(scik[valid], sclock[valid], sacc)
        self.sic_clock = sclock[valid][sorder]
        self.sic_val = sic[valid][sorder].astype(np.int64)
        self.srix = self._cell_rows(scidx, self.sic_clock, nc, marks, inp.lag)
        self.shas = self.srix >= 0
        ss = np.maximum(self.srix, 0)
        if len(self.sic_clock):
            self.sfresh = self.shas & ((days[:, None] - self.sic_clock[ss] // DAY_NS) <= prf.GRP_STALE_DAYS)
        else:
            self.sfresh = np.zeros((nd, n), dtype=bool)
        budget.check("join-sic")

    def _linked_order(self, cik: np.ndarray, clock: np.ndarray, accession: pa.Array):
        """Rows of linked CIKs ordered by (CIK index, clock, accession): the contract's latest-row order."""
        if not len(self.ciks) or not len(cik):
            return np.zeros(0, dtype=np.int64), np.zeros(0, dtype=np.int64)
        pos = np.minimum(np.searchsorted(self.ciks, cik), len(self.ciks) - 1)
        linked = self.ciks[pos] == cik
        idx = np.flatnonzero(linked)
        tbl = pa.table({"c": pos[idx], "k": clock[idx], "a": accession.take(pa.array(idx, type=pa.int64()))})
        o = pc.sort_indices(tbl, sort_keys=[("c", "ascending"), ("k", "ascending"), ("a", "ascending")])
        order = idx[o.to_numpy(zero_copy_only=False)]
        return order, pos[order].astype(np.int64)

    def _cell_rows(self, cidx: np.ndarray, clock: np.ndarray, nc: int, marks: np.ndarray, lag: int) -> np.ndarray:
        """Per cell: index of the linked CIK's latest row with clock < mark(t - lag) on primary lines, else -1."""
        nd, n = self.link.shape
        mk = np.full(nd, np.iinfo(np.int64).min, dtype=np.int64)
        if lag < nd:
            mk[lag:] = marks[:nd - lag]
        latest = np.full((nd, max(nc, 1)), -1, dtype=np.int64)
        bounds = np.searchsorted(cidx, np.arange(nc + 1))
        for c in range(nc):
            b, e = int(bounds[c]), int(bounds[c + 1])
            if b == e:
                continue
            k = np.searchsorted(clock[b:e], mk, side="left")
            latest[:, c] = np.where(k > 0, b + k - 1, -1)
        safe = np.maximum(self.link, 0)
        return np.where(self.pline, latest[np.arange(nd)[:, None], safe], -1)


class Audit:
    """Lazy shared state of one invocation."""

    def __init__(self, inp: Inputs, budget: Budget, a):
        self.inp, self.budget, self.a = inp, budget, a
        self._join = None
        self._common = None
        self._cmap = None

    def join(self) -> Join:
        if self._join is None:
            self._join = Join(self.inp, self.budget)
        return self._join

    def common(self):
        if self._common is None:
            self._common = self.inp.common()
        return self._common

    def cmap(self):
        if self._cmap is None:
            self._cmap = bfe.load_concept_map()
        return self._cmap

    def line_symbols(self) -> dict:
        """securityID -> lake line_types symbol (current, else last) of role lines, for readable spot checks."""
        table, sid, _, on = self.inp.line_types()
        names = table.schema.names
        cur = table.column("current_symbol").to_pylist() if "current_symbol" in names else [None] * table.num_rows
        last = table.column("last_symbol").to_pylist() if "last_symbol" in names else [None] * table.num_rows
        return {int(sid[k]): (cur[k] or last[k]) for k in np.flatnonzero(on).tolist()}


# ---------------------------------------------------------------------------
# C1 coverage, NaN reasons, join reproduction
# ---------------------------------------------------------------------------

def _year_rows(inp: Inputs):
    years = inp.role.years.astype(np.int64)
    uniq = sorted(set(years.tolist()))
    train_years = sorted(set(years[inp.sb:inp.se].tolist()))
    return years, uniq, train_years


def _coverage(fin: np.ndarray, dens: dict, years: np.ndarray, uniq, inp: Inputs) -> dict:
    """Finite / denominator cells per year and in the score window, for every denominator mask."""
    out = {"per_year": {}, "score_window": {}}
    for label, den in dens.items():
        d_rows = np.count_nonzero(den, axis=1)
        f_rows = np.count_nonzero(den & fin, axis=1)
        for y in uniq:
            sel = years == y
            dy, fy = int(d_rows[sel].sum()), int(f_rows[sel].sum())
            out["per_year"].setdefault(str(y), {})[label] = {"cells": dy, "finite": fy, "frac": frac(fy, dy)}
        ds, fs = int(d_rows[inp.sb:inp.se].sum()), int(f_rows[inp.sb:inp.se].sum())
        out["score_window"][label] = {"cells": ds, "finite": fs, "frac": frac(fs, ds)}
    return out


def _mismatch(actual: np.ndarray, expected: np.ndarray, inp: Inputs) -> dict:
    fa, fe = np.isfinite(actual), np.isfinite(expected)
    pattern = fa != fe
    both = fa & fe
    with np.errstate(invalid="ignore"):
        value = both & (actual.view(np.int64) != expected.view(np.int64))
    bad = pattern | value
    out = {"expected_finite_cells": int(np.count_nonzero(fe)), "actual_finite_cells": int(np.count_nonzero(fa)),
           "nan_pattern_mismatch": int(np.count_nonzero(pattern)), "value_mismatch": int(np.count_nonzero(value))}
    if bad.any():
        t, i = (int(v) for v in np.argwhere(bad)[0])
        out["first_mismatch"] = {"t": t, "session": date_of(inp.role.days[t]), "sid": int(inp.role.ids[i]),
                                 "actual": float(actual[t, i]), "expected": float(expected[t, i])}
    return out


def expected_grp(j: Join, kind: str, tables) -> np.ndarray:
    code = j.sic_val[np.maximum(j.srix, 0)] if len(j.sic_val) else np.zeros(j.srix.shape, dtype=np.int64)
    if kind == "grp_sic2":
        value = (code // 100).astype(np.float64)
    else:
        value = tables[kind][code]
    return np.where(j.sfresh, value, np.nan)


def expected_me(j: Join, inp: Inputs):
    """me_company recomputed from this run's shares_out and the role's raw_close / present (P and J lines)."""
    nd = inp.n_dates
    so = inp.field("shares_out")
    raw = inp.role_payload("raw_close.f64", "<f8")
    present = inp.role_payload("present.u8", "u1") != 0
    with np.errstate(invalid="ignore", over="ignore"):
        line_me = np.where(present, so * raw, np.nan)
    ok = np.isfinite(line_me) & (line_me > 0)
    del so, raw, present
    nc = max(len(j.ciks), 1)
    linked = j.link >= 0
    flat = (np.arange(nd, dtype=np.int64)[:, None] * nc + j.link)[linked]
    total = np.bincount(flat, weights=np.where(ok, line_me, 0.0)[linked], minlength=nd * nc).reshape(nd, nc)
    bad = np.bincount(flat, weights=(~ok)[linked].astype(np.float64), minlength=nd * nc).reshape(nd, nc)
    lines = np.bincount(flat, minlength=nd * nc).reshape(nd, nc)
    company = np.where(bad == 0, total, np.nan)
    rows = np.arange(nd)[:, None]
    safe = np.maximum(j.link, 0)
    expected = np.where(j.pline, company[rows, safe], np.nan)
    return expected, ok, lines[rows, safe]


def check_c1(audit: Audit) -> dict:
    inp, budget = audit.inp, audit.budget
    j = audit.join()
    member = inp.role.member != 0
    common, typed = audit.common()
    years, uniq, train_years = _year_rows(inp)
    cm = member & common[None, :]
    dens = {"member": member, "common_member": cm, "linked_primary_member": member & j.pline}
    link_mine = {"member_cells": int(np.count_nonzero(member)),
                 "unlinked": int(np.count_nonzero(member & (j.link == -1))),
                 "ambiguous": int(np.count_nonzero(member & (j.link == -2))),
                 "secondary": int(np.count_nonzero(member & (j.link >= 0) & ~j.pline)),
                 "primary": int(np.count_nonzero(member & j.pline))}
    link_producer = inp.issuer_checks.get("link_member_cells") or {}
    link_ok = all(link_producer.get(k) == v for k, v in link_mine.items())
    ff12, ff49, mapping = prf.sic_mapping()
    tables = {"grp_ff12": ff12, "grp_ff49": ff49}
    mapping_ok = mapping["table_sha256"] == ((inp.issuer_checks.get("sic_mapping") or {}).get("table_sha256"))
    rs = np.maximum(j.rix, 0)
    base = {"unlinked": member & (j.link == -1), "ambiguous": member & (j.link == -2),
            "secondary": member & (j.link >= 0) & ~j.pline}
    fields = {}
    for name in inp.issuer:
        spec = prf.ISSUER_FIELDS[name]
        actual = inp.field(name)
        extra = {}
        if spec["kind"] == "fund":
            vals = j.items[spec["item"]]
            item = vals[rs] if len(vals) else np.full(rs.shape, np.nan)
            expected = np.where(j.fresh, item, np.nan)
            fe = np.isfinite(expected)
            reasons = {"no_visible_row": member & j.pline & ~j.has, "stale": member & j.has & ~j.fresh,
                       "visible_nan": member & j.fresh & ~fe}
            extra["fc1_finite_member_cells"] = int(np.count_nonzero(member & fe & j.ev_fc1[rs])) if len(vals) else 0
        elif spec["kind"] == "grp":
            expected = expected_grp(j, name, tables)
            fe = np.isfinite(expected)
            reasons = {"no_visible_row": member & j.pline & ~j.shas, "stale": member & j.shas & ~j.sfresh,
                       "unmapped": member & j.sfresh & ~fe}
        else:
            expected, ok, nlines = expected_me(j, inp)
            fe = np.isfinite(expected)
            reasons = {"own_line_nan": member & j.pline & ~ok, "other_linked_line_nan": member & j.pline & ok & ~fe}
            extra["multi_line_finite"] = int(np.count_nonzero(member & fe & (nlines > 1)))
        counts = {k: int(np.count_nonzero(v)) for k, v in {**base, **reasons}.items()}
        counts.update(extra)
        counts["finite"] = int(np.count_nonzero(member & fe))
        producer = inp.entries[name].get("nan_reasons_member_cells") or {}
        consistent = all(counts.get(k) == v for k, v in producer.items()) and bool(producer)
        repro = _mismatch(actual, expected, inp)
        cov = _coverage(np.isfinite(actual), dens, years, uniq, inp)
        fields[name] = {"kind": spec["kind"], "reproduction": repro, "nan_reasons_member_cells": counts,
                        "producer_nan_reasons_member_cells": producer, "reasons_consistent": consistent,
                        "coverage": cov}
        del actual, expected
        budget.check(f"C1 {name}")
    # verdicts
    repro_bad = [x for x, r in fields.items()
                 if r["reproduction"]["value_mismatch"] or r["reproduction"]["nan_pattern_mismatch"]]
    reason_bad = [x for x, r in fields.items() if not r["reasons_consistent"]]
    low = []
    for x in CORE_ITEMS:
        if x in fields:
            for y in train_years:
                f = fields[x]["coverage"]["per_year"][str(y)]["common_member"]["frac"]
                if f is None or f < COVERAGE_MIN_CORE_COMMON:
                    low.append(f"{x}:{y}={f}")
    steps = []
    for x, r in fields.items():
        seq = [r["coverage"]["per_year"][str(y)]["common_member"]["frac"] for y in train_years]
        for y0, y1, a0, a1 in zip(train_years, train_years[1:], seq, seq[1:]):
            if a0 is not None and a1 is not None and abs(a1 - a0) > COVERAGE_MAX_YEAR_STEP:
                steps.append(f"{x}:{y0}->{y1} {a0:.3f}->{a1:.3f}")
    lag_ok = inp.lag == prf.FUND_LAG_SESSIONS_DECLARED
    flags = []
    if repro_bad:
        flags.append(f"re-join mismatches in {repro_bad}")
    if reason_bad:
        flags.append(f"NaN-reason counts differ from the producer in {reason_bad}")
    if not link_ok:
        flags.append("link member-cell counts differ from the producer")
    if not lag_ok:
        flags.append(f"fund_lag_sessions {inp.lag} != declared {prf.FUND_LAG_SESSIONS_DECLARED}")
    if not mapping_ok:
        flags.append("SIC mapping table differs from the manifest's")
    if low:
        flags.append(f"core coverage < {COVERAGE_MIN_CORE_COMMON}: {low[:8]}")
    if steps:
        flags.append(f"year-to-year coverage steps > {COVERAGE_MAX_YEAR_STEP}: {steps[:8]}")
    core = {x: fields[x]["coverage"]["score_window"]["common_member"]["frac"] for x in CORE_ITEMS if x in fields}
    summary = (f"{len(fields)} issuer fields re-joined: {len(fields) - len(repro_bad)} reproduce every cell; "
               f"reasons consistent {len(fields) - len(reason_bad)}/{len(fields)}; link counts "
               f"{'equal' if link_ok else 'DIFFER'}; TRAIN common-member coverage of core items {core}")
    return {"status": "FLAG" if flags else "PASS", "finding": summary + ("; FLAG: " + "; ".join(flags) if flags else ""),
            "criterion": CRITERIA["C1"],
            "metrics": {"lag_sessions": inp.lag, "train_years": train_years, "link_member_cells": link_mine,
                        "producer_link_member_cells": link_producer, "sic_mapping_table_sha256": mapping["table_sha256"],
                        "common_lines": {"common": int(common.sum()), "typed": int(typed.sum()), "role_lines": inp.n,
                                         "basis": "lake line_types security_type in (common, common_unverified): a "
                                                  "static non-point-in-time classification, used as a denominator only"},
                        "bridge_rows": j.bridge_rows, "fields": fields}}


# ---------------------------------------------------------------------------
# C2 raw-source reconciliation
# ---------------------------------------------------------------------------

class FactBook:
    """One CIK's raw CF-R facts per canonical metric: unique (start, end, value) with its first public clock.

    An entry is (value, first clock, its accession, its clock basis, concepts carrying that value for the period)."""

    def __init__(self):
        self.inst: dict = {}
        self.dur: dict = {}
        self._index: dict = {}

    def add(self, metric: str, start, end: int, value: float, clock: int, adsh: str, basis: str, concept: str):
        store = self.inst.setdefault(metric, {}) if start is None else self.dur.setdefault(metric, {})
        key = end if start is None else (start, end)
        lst = store.setdefault(key, [])
        for i, (v, c, a, b, cs) in enumerate(lst):
            if v == value:
                cs = cs | {concept}
                lst[i] = (value, clock, adsh, basis, cs) if (clock, adsh) < (c, a) else (v, c, a, b, cs)
                return
        lst.append((value, clock, adsh, basis, frozenset((concept,))))

    def _idx(self, metric: str):
        got = self._index.get(metric)
        if got is None:
            inst = self.inst.get(metric, {})
            dur = self.dur.get(metric, {})
            by_end, by_start = {}, {}
            for s, e in dur:
                by_end.setdefault(e, []).append(s)
                by_start.setdefault(s, []).append(e)
            got = {"inst_ends": sorted(inst), "ends": sorted(by_end), "by_end": by_end, "starts": sorted(by_start),
                   "by_start": by_start}
            self._index[metric] = got
        return got

    def inst_near(self, metric: str, target: int, tol: int):
        """[(end, (value, clock, adsh, basis, concepts))] of instants ending within tol days of target."""
        ix = self._idx(metric)
        ends = ix["inst_ends"]
        store = self.inst.get(metric, {})
        return [(e, f) for e in ends[bisect.bisect_left(ends, target - tol):bisect.bisect_right(ends, target + tol)]
                for f in store[e]]

    def dur_near(self, metric: str, target: int, tol: int, dmin: int, dmax: int):
        """[(start, end, fact)] of durations ending within tol days of target, dmin <= days <= dmax."""
        ix = self._idx(metric)
        ends = ix["ends"]
        store = self.dur.get(metric, {})
        out = []
        for e in ends[bisect.bisect_left(ends, target - tol):bisect.bisect_right(ends, target + tol)]:
            for s in ix["by_end"][e]:
                if dmin <= e - s + 1 <= dmax:
                    out.extend((s, e, f) for f in store[(s, e)])
        return out

    def dur_start_near(self, metric: str, target: int, tol: int):
        """[(start, end, fact)] of durations starting within tol days of target."""
        ix = self._idx(metric)
        starts = ix["starts"]
        store = self.dur.get(metric, {})
        out = []
        for s in starts[bisect.bisect_left(starts, target - tol):bisect.bisect_right(starts, target + tol)]:
            for e in ix["by_start"][s]:
                out.extend((s, e, f) for f in store[(s, e)])
        return out


class Witness:
    """Best (earliest-public) derivation of a target value from raw facts, by the contract's item definitions."""

    def __init__(self, target: float):
        self.target = target
        self.best = None
        self.combos = 0
        self.capped = False

    def offer(self, value: float, clock: int, kind: str, terms: list):
        self.combos += 1
        if self.combos > WITNESS_COMBO_CAP:
            self.capped = True
            return
        if close_rel(value, self.target) and (self.best is None or clock < self.best[0]):
            self.best = (clock, kind, terms)


def _term(metric, s, e, f):
    """JSON term of a witness: [metric, start, end, value, accession, clock basis, first public clock, concepts]."""
    v, c, a, b, cs = f
    return [metric, None if s is None else date_of(s), date_of(e), v, a, b, iso_us(c), sorted(cs)]


def witness_shares(book: FactBook, anchor: int, target: float, tol: int, classes, metrics) -> Witness:
    w = Witness(target)
    for metric in metrics:
        for lo, hi in classes:
            for s, e, f in book.dur_near(metric, anchor, tol, lo, hi):
                w.offer(f[0], f[1], "weighted_shares", [_term(metric, s, e, f)])
    return w


def witness_item(book: FactBook, item: str, anchor: int, target: float) -> Witness:
    w = Witness(target)
    T, L, S = bfe.TOL_ANCHOR, bfe.TOL_LAG, bfe.TOL_SNAP
    if item == "at":
        for e, f in book.inst_near("total_assets", anchor, T):
            w.offer(f[0], f[1], "instant", [_term("total_assets", None, e, f)])
    elif item == "be":
        eqs = [(e, f[0], f[1], [_term("stockholders_equity", None, e, f)])
               for e, f in book.inst_near("stockholders_equity", anchor, T)]
        for e, f in book.inst_near("equity_incl_minority", anchor, T):
            base = [_term("equity_incl_minority", None, e, f)]
            eqs.append((e, f[0], f[1], base))
            for e2, f2 in book.inst_near("minority_int_bs", e, bfe.TOL_SAME_DATE):
                eqs.append((e, f[0] - f2[0], max(f[1], f2[1]), base + [_term("minority_int_bs", None, e2, f2)]))
        for e, v, c, terms in eqs:
            w.offer(v, c, "equity", terms)
            for e3, f3 in book.inst_near("pref_stock", e, bfe.TOL_SAME_DATE):
                w.offer(v - f3[0], max(c, f3[1]), "equity_minus_pref", terms + [_term("pref_stock", None, e3, f3)])
    elif item == "shrs_q":
        return witness_shares(book, anchor, target, T, ((bfe.Q_MIN, bfe.Q_MAX), (bfe.Y_MIN, bfe.Y_MAX)),
                              ITEM_METRICS["shrs_q"])
    else:
        for metric in ITEM_METRICS[item]:
            _witness_ttm(book, metric, anchor, w, T, L, S)
    return w


def _quarters(book: FactBook, metric: str, lo_end: int, hi_end: int) -> dict:
    """Discrete quarters ending in [lo_end, hi_end]: direct 80-100 d facts and same-start YTD differences."""
    store = book.dur.get(metric, {})
    ix = book._idx(metric)
    out: dict = {}
    for (s, e), lst in store.items():
        if lo_end <= e <= hi_end and bfe.Q_MIN <= e - s + 1 <= bfe.Q_MAX:
            for f in lst:
                out.setdefault(e, []).append((s, f[0], f[1], [_term(metric, s, e, f)]))
    for s, ends in ix["by_start"].items():
        for e1 in ends:
            d1 = e1 - s + 1
            if not lo_end <= e1 <= hi_end or d1 <= bfe.Q_MAX or d1 > bfe.Y_MAX:
                continue
            for e0 in ends:
                if bfe.Q_MIN <= e1 - e0 <= bfe.Q_MAX and e0 - s + 1 >= bfe.Q_MIN:
                    for f1 in store[(s, e1)]:
                        for f0 in store[(s, e0)]:
                            out.setdefault(e1, []).append((e0 + 1, f1[0] - f0[0], max(f1[1], f0[1]),
                                                           [_term(metric, s, e1, f1), _term(metric, s, e0, f0)]))
    return out


def _witness_ttm(book: FactBook, metric: str, anchor: int, w: Witness, T: int, L: int, S: int):
    for s, e, f in book.dur_near(metric, anchor, T, bfe.Y_MIN, bfe.Y_MAX):
        w.offer(f[0], f[1], "fy_direct", [_term(metric, s, e, f)])
    for s, e, f in book.dur_near(metric, anchor, T, bfe.Q_MIN, bfe.Y_MIN - 1):
        days = e - s + 1
        for fs, fe, ff in book.dur_near(metric, s - 1, S, bfe.Y_MIN, bfe.Y_MAX):
            for ps, pe, fp in book.dur_start_near(metric, fs, S):
                if abs(pe - (e - bfe.LAG4)) <= L and abs((pe - ps + 1) - days) <= L:
                    w.offer(f[0] + ff[0] - fp[0], max(f[1], ff[1], fp[1]), "ytd_plus_fy_minus_prior_ytd",
                            [_term(metric, s, e, f), _term(metric, fs, fe, ff), _term(metric, ps, pe, fp)])
            if w.capped:
                return
    quarters = _quarters(book, metric, anchor - 4 * bfe.Q_MAX - 2 * S - T, anchor + T)
    ends = sorted(quarters)

    def near(target, tol):
        return ends[bisect.bisect_left(ends, target - tol):bisect.bisect_right(ends, target + tol)]

    def dfs(start, total, clock, terms, depth):
        if w.capped:
            return
        if depth == 4:
            w.offer(total, clock, "four_quarters", terms)
            return
        for e in near(start - 1, S):
            for s1, v1, c1, t1 in quarters[e]:
                dfs(s1, total + v1, max(clock, c1), terms + t1, depth + 1)

    for e in near(anchor, T):
        for s0, v0, c0, t0 in quarters[e]:
            dfs(s0, v0, c0, t0, 1)


class RawSources:
    """CF-R batches and FSDS SUB quarters named by the events manifest (TRAIN-sealed reads, pins verified)."""

    def __init__(self, audit: Audit):
        inp = audit.inp
        self.budget = audit.budget
        cf = inp.em["inputs"]["companyfacts"]
        self.cf_dir = Path(cf["dir"])
        m = json.loads(read_pinned(self.cf_dir / "manifest.json", cf["manifest_sha256"], what="CF-R manifest"))
        listed = {b["file"]: b["sha256"] for b in cf["batches"]}
        self.batches = []
        for b in m["batches"]:
            need(listed.get(b["file"]) == b["parquet_sha256"], f"CF-R batch {b['file']} differs from the events pin")
            self.batches.append((int(b["first_cik"]), int(b["last_cik"]), str(b["file"]), str(b["parquet_sha256"])))
        fsds = inp.em["inputs"]["fsds"]
        self.fsds_dir = Path(fsds["dir"])
        read_pinned(self.fsds_dir / "fsds-staging-manifest.json", fsds["manifest_sha256"], what="FSDS staging manifest")
        self.quarters = [q for q in fsds["sub_quarters"] if q["quarter"] <= LAST_TRAIN_SUB_QUARTER]
        self.full_decodes = 0

    def facts(self, ciks, cmap) -> list:
        """Per touched batch: the CIKs' us-gaap periodic-form facts of mapped concepts, filed before 2023."""
        by_batch: dict = {}
        for c in ciks:
            hit = [b for b in self.batches if b[0] <= c <= b[1]]
            need(len(hit) == 1, f"CIK {c} is in {len(hit)} CF-R batches")
            by_batch.setdefault(hit[0], []).append(c)
        concepts = sorted(cmap.concept_to)
        filters = [("taxonomy", "=", "us-gaap"), ("filed_date", "<", TRAIN_SEAL),
                   ("form", "in", list(bfe.EVENT_FORMS)), ("unit", "in", ["USD", "shares"]), ("concept", "in", concepts)]
        tables = []
        for (lo, hi, file, sha), cs in sorted(by_batch.items()):
            self.budget.check(f"C2 CF-R {file}")
            blob = read_pinned(self.cf_dir / file, sha, what=f"CF-R {file}")
            # CF-R stores cik as a 10-digit zero-padded string and batches are CIK-sorted: the padded filter prunes
            # row groups. It is verified: a requested CIK without rows triggers a full decode of the batch.
            padded = filters + [("cik", "in", [f"{c:010d}" for c in cs])]
            t = self._decode(blob, padded, cs)
            if set(pc.unique(t.column("cik")).to_pylist()) != set(cs):
                t = self._decode(blob, filters, cs)
                self.full_decodes += 1
            del blob
            tables.append(t)
        return tables

    @staticmethod
    def _decode(blob: bytes, filters, ciks) -> pa.Table:
        t = pq.read_table(pa.BufferReader(blob), columns=bfe.CF_COLUMNS, filters=filters)
        t = t.set_column(t.schema.get_field_index("cik"), "cik", pc.cast(t.column("cik"), pa.int64()))
        return t.filter(pc.is_in(t.column("cik"), value_set=pa.array(sorted(ciks), pa.int64()))).combine_chunks()

    def clock(self, adsh: set):
        """bfe.CLOCK_SCHEMA table and adsh -> (clock_us, basis) for the named accessions (SUB <= 2022q4)."""
        best: dict = {}
        value_set = pa.array(sorted(adsh), pa.string())
        for q in self.quarters:
            self.budget.check(f"C2 SUB {q['quarter']}")
            blob = read_pinned(self.fsds_dir / q["file"], q["sha256"], what=f"FSDS SUB {q['quarter']}")
            t = pq.read_table(pa.BufferReader(blob), columns=bfe.SUB_COLUMNS)
            del blob
            t = t.filter(pc.is_in(t.column("adsh"), value_set=value_set))
            if not t.num_rows:
                continue
            cols = {c: t.column(c).to_pylist() for c in ("adsh", "cik", "sic", "form", "fy", "fp", "fye")}
            period = pc.cast(t.column("period"), pa.int32()).to_pylist()
            filed = pc.cast(t.column("filed"), pa.int32()).to_pylist()
            acc = pc.cast(t.column("accepted_utc"), pa.int64()).to_pylist()
            for i in range(t.num_rows):
                if cols["adsh"][i] is None or cols["cik"][i] is None or filed[i] is None:
                    continue
                if acc[i] is None:
                    clock, basis = filed[i] * DAY_US + bfe.FC1_OFFSET_US, bfe.BASIS_FC1
                else:
                    clock, basis = int(acc[i]), bfe.BASIS_FSDS
                row = (cols["adsh"][i], int(cols["cik"][i]), clock, basis, filed[i], cols["form"][i], period[i],
                       cols["fy"][i], cols["fp"][i], cols["fye"][i], bfe._parse_sic(cols["sic"][i]))
                prev = best.get(row[0])
                if prev is not None and (prev[2], prev[1]) <= (clock, row[1]):
                    continue
                best[row[0]] = row
        rows = sorted(best.values())
        cols = list(zip(*rows)) if rows else [[] for _ in bfe.CLOCK_SCHEMA]
        table = pa.Table.from_arrays([pa.array(list(c), type=f.type) for c, f in zip(cols, bfe.CLOCK_SCHEMA)],
                                     schema=bfe.CLOCK_SCHEMA)
        return table, {r[0]: (r[2], r[3]) for r in rows}


def _books(tables, cmap, sub_map: dict, metrics) -> dict:
    """Per CIK FactBook over the wanted metrics (unit class matched, finite value, duration 1..380 d)."""
    wanted = {c for c, (mid, _rank, _u) in cmap.concept_to.items() if bfe.METRICS[mid] in metrics}
    books: dict = {}
    for t in tables:
        t = t.filter(pc.is_in(t.column("concept"), value_set=pa.array(sorted(wanted), pa.string())))
        cols = {c: t.column(c).to_pylist() for c in ("cik", "concept", "unit", "accession_number", "value")}
        start = pc.cast(t.column("period_start"), pa.int32()).to_pylist()
        end = pc.cast(t.column("period_end"), pa.int32()).to_pylist()
        filed = pc.cast(t.column("filed_date"), pa.int32()).to_pylist()
        for i in range(t.num_rows):
            mid, _rank, unit = cmap.concept_to[cols["concept"][i]]
            value = cols["value"][i]
            if cols["unit"][i] != unit or value is None or not math.isfinite(value) or end[i] is None:
                continue
            is_dur = mid >= bfe.N_INSTANT
            if is_dur and (start[i] is None or not 1 <= end[i] - start[i] + 1 <= bfe.Y_MAX):
                continue
            adsh = cols["accession_number"][i]
            got = sub_map.get(adsh)
            if got is None:
                clock, basis = filed[i] * DAY_US + bfe.FC1_OFFSET_US, bfe.BASIS_FC1
            else:
                clock, basis = got
            books.setdefault(int(cols["cik"][i]), FactBook()).add(
                bfe.METRICS[mid], start[i] if is_dur else None, end[i], float(value), clock, adsh, basis,
                cols["concept"][i])
    return books


def lag4_concept_check(records: list, books: dict, j: Join) -> dict:
    """shrs_q residual path on the C2 sample: do shrs_q and shrs_q_lag4 come from the same XBRL concept?

    The events table exposes no concept, so the concepts come from raw-fact witnesses: shrs_q's witness term, and a
    shrs_q_lag4 witness (same metric, same duration class, ending within TOL_LAG of period_end - 365 d). Mismatch =
    the two terms' concept sets are disjoint (a value reported identically under two concepts counts under both)."""
    out = {"cells": 0, "lag4_finite": 0, "lag4_witnessed": 0, "concept_mismatch": 0, "metric_mismatch": 0,
           "pairs": {}, "examples": []}
    for rec in records:
        wit = rec.get("witness") or {}
        if rec.get("row") is None or wit.get("status") != "found":
            continue
        out["cells"] += 1
        lag4 = float(j.items["shrs_q_lag4"][rec["row"]["index"]])
        if not math.isfinite(lag4):
            continue
        out["lag4_finite"] += 1
        term = wit["terms"][0]
        metric, start, end, cur_concepts = term[0], term[1], term[2], set(term[7])
        days = (dt.date.fromisoformat(end) - dt.date.fromisoformat(start)).days + 1
        cls = (bfe.Q_MIN, bfe.Q_MAX) if days <= bfe.Q_MAX else (bfe.Y_MIN, bfe.Y_MAX)
        anchor = day_of(dt.date.fromisoformat(rec["row"]["period_end"]))
        book = books.get(rec["cik"], FactBook())
        w = witness_shares(book, anchor - bfe.LAG4, lag4, bfe.TOL_LAG, (cls,), ITEM_METRICS["shrs_q"])
        if w.best is None:
            continue
        out["lag4_witnessed"] += 1
        lterm = w.best[2][0]
        key = f"{'/'.join(sorted(cur_concepts))} | {'/'.join(lterm[7])}"
        out["pairs"][key] = out["pairs"].get(key, 0) + 1
        if lterm[0] != metric:
            out["metric_mismatch"] += 1
        if not cur_concepts & set(lterm[7]):
            out["concept_mismatch"] += 1
            if len(out["examples"]) < 10:
                out["examples"].append({"session": rec["session"], "sid": rec["sid"], "cik": rec["cik"],
                                        "shrs_q": term, "shrs_q_lag4": lterm})
    return out


def recon_item(audit: Audit, raw: RawSources, item: str) -> dict:
    inp, budget, a = audit.inp, audit.budget, audit.a
    j = audit.join()
    n = inp.n
    x = inp.field(item)
    member = inp.role.member != 0
    sb, se = inp.sb, inp.se
    flat = np.flatnonzero(member[sb:se] & np.isfinite(x[sb:se]))
    k = min(int(a.sample), len(flat))
    rng = np.random.default_rng([int(a.seed), CORE_ITEMS.index(item)])
    pick = np.sort(flat[rng.choice(len(flat), size=k, replace=False)]) if k else np.zeros(0, dtype=np.int64)
    records = []
    for f in pick.tolist():
        t, i = sb + f // n, f % n
        v = float(x[t, i])
        lk = int(j.link[t, i])
        r = int(j.rix[t, i])
        mark = int(inp.marks[t - inp.lag]) if t >= inp.lag else None
        rec = {"t": t, "session": date_of(j.days[t]), "sid": int(inp.role.ids[i]), "value": v,
               "cik": int(j.ciks[lk]) if lk >= 0 else None, "primary": bool(j.pline[t, i]),
               "mark": iso_ns(mark) if mark is not None else None}
        if r >= 0:
            stored = float(j.items[item][r])
            clock_ns = int(j.ev_clock[r])
            rec["row"] = {"index": r, "accession": j.ev_accession[r], "accepted_utc": iso_ns(clock_ns),
                          "clock_basis": j.ev_basis[r], "period_end": date_of(j.ev_pe[r]), "form": j.ev_form[r],
                          "filed": date_of(j.ev_filed[r]), "staleness_days": int(j.ev_sd[r]), "stored": stored}
            rec["join_equal"] = bool(np.float64(stored).view(np.int64) == np.float64(v).view(np.int64))
            rec["fresh"] = bool(j.fresh[t, i])
            rec["row_before_mark"] = mark is not None and clock_ns < mark
            rec["row_slack_hours"] = round((mark - clock_ns) / 3.6e12, 3) if mark is not None else None
        else:
            rec["row"] = None
            rec["join_equal"] = rec["fresh"] = rec["row_before_mark"] = False
        records.append(rec)
    budget.check(f"C2 {item} sample")
    ciks = sorted({rec["cik"] for rec in records if rec["row"] is not None})
    cmap = audit.cmap()
    full0 = raw.full_decodes
    tables = raw.facts(ciks, cmap)
    adsh = {rec["row"]["accession"] for rec in records if rec["row"] is not None}
    for t in tables:
        adsh.update(pc.unique(t.column("accession_number")).to_pylist())
    clock_table, sub_map = raw.clock(adsh)
    # Replay of the events producer on the raw facts (filed < 2023: every row visible at a TRAIN mark is causal in it).
    counts: dict = {}
    replay: dict = {}
    for t in tables:
        per_cik = bfe.batch_accessions(t, cmap, clock_table, counts)
        for cik, accs in per_cik.items():
            replay[cik] = {row[1]: row for row in bfe.process_cik(cik, accs, counts)}
        budget.check(f"C2 {item} replay")
    books = _books(tables, cmap, sub_map, ITEM_METRICS[item])
    item_pos = 12 + bfe.ITEMS.index(item)
    kinds: dict = {}
    fails: dict = {}
    for rec in records:
        row = rec["row"]
        ok = rec["join_equal"] and rec["fresh"] and rec["row_before_mark"]
        if row is None:
            fails["no_row"] = fails.get("no_row", 0) + 1
            rec["pass"] = False
            continue
        clock_us = int(j.ev_clock[row["index"]]) // 1000
        got = sub_map.get(row["accession"])
        if row["clock_basis"] == bfe.BASIS_FSDS:
            sub = "match" if got is not None and got == (clock_us, bfe.BASIS_FSDS) else (
                "missing" if got is None else "mismatch")
        else:
            fc1 = day_of(dt.date.fromisoformat(row["filed"])) * DAY_US + bfe.FC1_OFFSET_US
            sub = "fc1_ok" if (got is None or got[1] == bfe.BASIS_FC1) and clock_us == fc1 else "fc1_inconsistent"
        rec["sub"] = {"status": sub, "sub_clock": iso_us(got[0]) if got else None}
        rp = replay.get(rec["cik"], {}).get(row["accession"])
        if rp is None:
            rec["replay"] = {"status": "row_missing"}
        else:
            differ = [name for pos, name in enumerate(bfe.ITEMS, start=12)
                      if not same_value(float(rp[pos]), float(j.items[name][row["index"]]))]
            meta = (rp[2] == clock_us and rp[3] == row["clock_basis"] and date_of(rp[7]) == row["period_end"]
                    and int(rp[11]) == row["staleness_days"])
            rec["replay"] = {"status": "equal" if not differ and meta else "differs", "item_value": float(rp[item_pos]),
                             "differing_items": differ, "clock_basis_period_staleness_equal": bool(meta)}
        anchor = day_of(dt.date.fromisoformat(row["period_end"]))
        w = witness_item(books.get(rec["cik"], FactBook()), item, anchor, rec["value"])
        mark_us = int(inp.marks[rec["t"] - inp.lag]) // 1000
        if w.best is None:
            rec["witness"] = {"status": "none", "combos": w.combos, "capped": w.capped}
        else:
            wc, kind, terms = w.best
            rec["witness"] = {"status": "found", "kind": kind, "public_by": iso_us(wc),
                              "slack_hours": round((mark_us - wc) / 3.6e9, 3), "before_mark": wc < mark_us,
                              "terms": terms, "combos": w.combos}
            kinds[kind] = kinds.get(kind, 0) + 1
        checks = {"join": ok, "sub": sub in ("match", "fc1_ok"), "replay": rec["replay"]["status"] == "equal",
                  "witness": w.best is not None and rec["witness"]["before_mark"]}
        rec["pass"] = all(checks.values())
        for name, good in checks.items():
            if not good:
                fails[name] = fails.get(name, 0) + 1
    slack = [rec["row_slack_hours"] for rec in records if rec.get("row_slack_hours") is not None]
    wslack = [rec["witness"]["slack_hours"] for rec in records if (rec.get("witness") or {}).get("status") == "found"]
    n_pass = sum(1 for rec in records if rec["pass"])
    basis = {}
    for rec in records:
        if rec["row"] is not None:
            basis[rec["row"]["clock_basis"]] = basis.get(rec["row"]["clock_basis"], 0) + 1
    status = "PASS" if records and n_pass == len(records) else "FLAG"
    out = {"status": status, "candidates": int(len(flat)), "sampled": len(records), "passed": n_pass,
           "failures": fails, "witness_kinds": kinds, "row_clock_basis": basis,
           "min_row_slack_hours": min(slack) if slack else None,
           "min_witness_slack_hours": min(wslack) if wslack else None,
           "ciks": len(ciks), "cf_batches_read": len(tables), "cf_full_decodes": raw.full_decodes - full0,
           "sub_quarters_read": len(raw.quarters),
           "replay_counts": {k: v for k, v in counts.items() if isinstance(v, int)}, "cells": records}
    if item == "shrs_q":
        out["lag4_concept_check"] = lag4_concept_check(records, books, j)  # informational (controller addendum c)
    return out


def check_c2(audit: Audit, items, doc: dict, save) -> dict:
    """Runs the requested items; the merged C2 entry is saved after every item (a budget stop keeps them)."""
    inp = audit.inp
    code = {rel: code_matches(REPO_ROOT / rel, (inp.em.get("code_sha256") or {}).get(rel))
            for rel in ("atx-engine/tools/build_fundamental_events.py", "atx-engine/tools/export_fundamental_fields.py",
                        "atx-db/src/atx_db/statement_map_seed.py", "atx-db/src/atx_db/seeds/statement_map.csv")}
    cmap_ok = audit.cmap().sha256 == inp.em.get("concept_map_sha256")
    raw = RawSources(audit)
    done = dict((((doc["checks"].get("C2") or {}).get("metrics")) or {}).get("items", {}))
    for item in items:
        tick = time.perf_counter()
        done[item] = recon_item(audit, raw, item)
        done[item]["elapsed_s"] = round(time.perf_counter() - tick, 2)
        r = done[item]
        print(f"[t25] C2 {item} {r['status']}: {r['passed']}/{r['sampled']} cells pass (failures {r['failures']}, "
              f"witness kinds {r['witness_kinds']}, min row slack {r['min_row_slack_hours']} h, "
              f"{r['elapsed_s']} s)", flush=True)
        doc["checks"]["C2"] = dict(_c2_summary(done, code, cmap_ok), elapsed_s=r["elapsed_s"],
                                   peak_rss_mib=audit.budget.peak >> 20)
        save()
    return _c2_summary(done, code, cmap_ok)


def _c2_summary(done: dict, code: dict, cmap_ok: bool) -> dict:
    code_ok = all(code.values()) and cmap_ok
    missing = [x for x in CORE_ITEMS if x not in done]
    bad = [x for x in CORE_ITEMS if x in done and done[x]["status"] != "PASS"]
    if bad or not code_ok:
        status = "FLAG"
    elif missing:
        status = "INCOMPLETE"
    else:
        status = "PASS"
    parts = [f"{x} {done[x]['passed']}/{done[x]['sampled']}" for x in CORE_ITEMS if x in done]
    finding = f"cells passing per item: {', '.join(parts) or 'none'}"
    if missing:
        finding += f"; items not yet run: {missing}"
    if not code_ok:
        finding += f"; FLAG: replay code/concept map differ from the events manifest pins ({code}, concept map {cmap_ok})"
    if bad:
        finding += f"; FLAG: items with failing cells {bad}"
    return {"status": status, "finding": finding, "criterion": CRITERIA["C2"],
            "metrics": {"replay_code_matches_events_pins": code, "concept_map_matches": cmap_ok, "items": done}}


# ---------------------------------------------------------------------------
# C3 vintages and FC1
# ---------------------------------------------------------------------------

def check_c3(audit: Audit) -> dict:
    inp, budget = audit.inp, audit.budget
    j = audit.join()
    ev = j.ev_all
    cik = prf.as_ids(prf.column_of(ev, "cik"), "cik")
    clock = prf.as_instants_ns(prf.column_of(ev, "accepted_utc"), "accepted_utc")
    basis = prf.as_text(prf.column_of(ev, "clock_basis")).to_pylist()
    form = prf.as_text(prf.column_of(ev, "form")).to_pylist()
    pe = prf.as_days(prf.column_of(ev, "period_end"), "period_end", null_day=prf.NULL_PERIOD_DAY)
    sd = pc.cast(prf.column_of(ev, "staleness_days"), pa.int64()).to_numpy(zero_copy_only=False)
    restated = pc.fill_null(pc.cast(prf.column_of(ev, "n_restated"), pa.int64()), 0).to_numpy(zero_copy_only=False)
    fc1 = np.array([b == bfe.BASIS_FC1 for b in basis], dtype=bool)
    amended = np.array([str(f).endswith("/A") for f in form], dtype=bool)
    years = (clock // 1000).astype("datetime64[us]").astype("datetime64[Y]").astype(np.int64) + 1970
    linked = np.isin(cik, j.ciks)
    role_first = int(inp.role.days[0])
    window = clock >= (role_first - 400) * DAY_NS  # rows that can be visible in the role (400 d staleness)
    per_year = {}
    for y in sorted(set(years.tolist())):
        s = years == y
        per_year[str(y)] = {"rows": int(s.sum()), "ciks": int(len(np.unique(cik[s]))), "fc1_rows": int((s & fc1).sum()),
                            "amended_rows": int((s & amended).sum()), "rows_with_restated_facts": int((s & (restated > 0)).sum()),
                            "restated_facts": int(restated[s].sum()), "linked_rows": int((s & linked).sum()),
                            "linked_fc1_rows": int((s & linked & fc1).sum())}
    lw = linked & window
    fc1_rows_share = frac((lw & fc1).sum(), lw.sum())
    # anchor monotonicity per CIK in (clock, accession) order
    acc = prf.as_text(prf.column_of(ev, "accession"))
    o = pc.sort_indices(pa.table({"c": cik, "k": clock, "a": acc}),
                        sort_keys=[("c", "ascending"), ("k", "ascending"), ("a", "ascending")]).to_numpy(zero_copy_only=False)
    c_o, pe_o = cik[o], pe[o]
    same = c_o[1:] == c_o[:-1]
    valid = (pe_o[1:] != prf.NULL_PERIOD_DAY) & (pe_o[:-1] != prf.NULL_PERIOD_DAY)
    monotone_violations = int(np.count_nonzero(same & valid & (pe_o[1:] < pe_o[:-1])))
    stale_values = sorted(set(sd.tolist()))
    # vintages per (CIK, period_end) of linked role-window rows
    vint = {}
    sel = np.flatnonzero(lw & (pe != prf.NULL_PERIOD_DAY))
    key_o = np.lexsort((clock[sel], pe[sel], cik[sel]))
    s_idx = sel[key_o]
    gk = np.stack([cik[s_idx], pe[s_idx]], axis=1)
    starts = np.flatnonzero(np.r_[True, np.any(gk[1:] != gk[:-1], axis=1)]) if len(s_idx) else np.zeros(0, np.int64)
    bounds = np.r_[starts, len(s_idx)]
    gid = np.repeat(np.arange(len(bounds) - 1), np.diff(bounds))  # (CIK, period_end) group of each row
    for item in CORE_ITEMS:
        vals = prf.as_f64(prf.column_of(ev, item), item)[s_idx]
        fin = np.isfinite(vals)
        g, v = gid[fin], vals[fin]
        o = np.lexsort((v, g))
        g, v = g[o], v[o]
        new = np.r_[True, (g[1:] != g[:-1]) | (v[1:] != v[:-1])] if len(g) else np.zeros(0, dtype=bool)
        distinct = np.bincount(g[new], minlength=len(bounds) - 1)
        hist = {"1": int((distinct == 1).sum()), "2": int((distinct == 2).sum()), "3+": int((distinct >= 3).sum())}
        groups = int((distinct > 0).sum())
        vint[item] = {"groups_with_value": groups, "vintages": hist, "revised_share": frac(hist["2"] + hist["3+"], groups)}
    budget.check("C3 vintages")
    # member-cell level (score window): rows behind finite core cells, and in-place revisions seen by the fields
    member = inp.role.member != 0
    sb, se = inp.sb, inp.se
    rs = np.maximum(j.rix[sb:se], 0)
    cell = {}
    amended_s = np.array([str(f).endswith("/A") for f in j.ev_form], dtype=bool)
    for item in CORE_ITEMS:
        x = inp.field(item)
        fin = member[sb:se] & np.isfinite(x[sb:se]) & (j.rix[sb:se] >= 0)
        nfin = int(fin.sum())
        rev = adv = 0
        if len(j.ev_clock):
            xs = x[sb - 1 if sb > 0 else sb:se]
            ps = j.ev_pe[np.maximum(j.rix[sb - 1 if sb > 0 else sb:se], 0)]
            ls = j.link[sb - 1 if sb > 0 else sb:se]
            both = np.isfinite(xs[1:]) & np.isfinite(xs[:-1]) & (ls[1:] == ls[:-1]) & (ls[1:] >= 0)
            rev = int(np.count_nonzero(both & (ps[1:] == ps[:-1]) & (xs[1:] != xs[:-1])))
            adv = int(np.count_nonzero(both & (ps[1:] != ps[:-1])))
        cell[item] = {"finite_member_cells": nfin,
                      "fc1_row_cells": int((fin & j.ev_fc1[rs]).sum()) if len(j.ev_clock) else 0,
                      "amended_row_cells": int((fin & amended_s[rs]).sum()) if len(j.ev_clock) else 0,
                      "restated_row_cells": int((fin & (j.ev_restated[rs] > 0)).sum()) if len(j.ev_clock) else 0,
                      "in_place_revisions": rev, "anchor_advances": adv}
        cell[item]["fc1_share"] = frac(cell[item]["fc1_row_cells"], nfin)
        del x
        budget.check(f"C3 {item}")
    flags = []
    if fc1_rows_share is not None and fc1_rows_share > FC1_MAX_SHARE:
        flags.append(f"FC1 share of linked rows {fc1_rows_share}")
    hi_cell = {x: c["fc1_share"] for x, c in cell.items() if c["fc1_share"] is not None and c["fc1_share"] > FC1_MAX_SHARE}
    if hi_cell:
        flags.append(f"FC1 share of finite member cells {hi_cell}")
    if monotone_violations:
        flags.append(f"{monotone_violations} period_end decreases")
    if not set(stale_values) <= set(prf.STALENESS_DAYS_ALLOWED):
        flags.append(f"staleness_days values {stale_values}")
    hi_v = {x: v["revised_share"] for x, v in vint.items() if (v["revised_share"] or 0) > VINTAGE_MAX_SHARE}
    if hi_v:
        flags.append(f"revised-vintage share {hi_v}")
    finding = (f"{int(lw.sum())} linked role-window rows (TRAIN-sealed), FC1 {int((lw & fc1).sum())} "
               f"({fc1_rows_share}); amended {int((lw & amended).sum())}; rows with restated facts "
               f"{int((lw & (restated > 0)).sum())}; revised-vintage share "
               f"{ {x: v['revised_share'] for x, v in vint.items()} }; in-place field revisions "
               f"{ {x: c['in_place_revisions'] for x, c in cell.items()} }")
    return {"status": "FLAG" if flags else "PASS", "finding": finding + ("; FLAG: " + "; ".join(flags) if flags else ""),
            "criterion": CRITERIA["C3"],
            "metrics": {"rows_train_sealed": int(len(cik)), "linked_role_window_rows": int(lw.sum()),
                        "fc1_share_linked_role_window_rows": fc1_rows_share, "per_clock_year": per_year,
                        "period_end_monotone_violations": monotone_violations, "staleness_days_values": stale_values,
                        "vintages_linked_role_window": vint, "member_cells_score_window": cell,
                        "sic_rows_invalid_skipped": j.sic_invalid_rows}}


# ---------------------------------------------------------------------------
# C4 split contamination
# ---------------------------------------------------------------------------

def _split_stats(ok, lnr, lnk, insplit, ids) -> dict:
    pop = ok & insplit
    base = ok & ~insplit
    with np.errstate(invalid="ignore"):
        big = np.abs(lnr) > SPLIT_BIG_LN
        explained = pop & big & (np.abs(lnr - lnk) <= SPLIT_RESIDUAL_LN)
        factor = pop & (np.abs(lnk) >= SPLIT_BIG_LN)
        contaminated = factor & (np.abs(lnr - lnk) <= SPLIT_RESIDUAL_LN)
        neutral = factor & (np.abs(lnr) <= SPLIT_RESIDUAL_LN)
    nbig = int((pop & big).sum())
    return {"population_cells": int(pop.sum()), "population_names": int(np.count_nonzero(pop.any(axis=0))),
            "big_cells": nbig, "explained_cells": int(explained.sum()),
            "explained_names": int(np.count_nonzero(explained.any(axis=0))),
            "share_explained": frac(explained.sum(), nbig),
            "big_split_factor_cells": int(factor.sum()), "moved_with_split_cells": int(contaminated.sum()),
            "split_neutral_cells": int(neutral.sum()),
            "explained_examples": [int(ids[i]) for i in np.flatnonzero(explained.any(axis=0))[:10]],
            "baseline_cells_no_split": int(base.sum()), "baseline_big_cells": int((base & big).sum()),
            "baseline_big_share": frac((base & big).sum(), base.sum())}


def _prev_valid(valid: np.ndarray) -> np.ndarray:
    """Per cell: index of the column's previous valid session (-1 when none)."""
    nd, n = valid.shape
    last = np.maximum.accumulate(np.where(valid, np.arange(nd)[:, None], -1), axis=0)
    return np.vstack([np.full((1, n), -1, dtype=last.dtype), last[:-1]])


def _log_steps(x: np.ndarray):
    """(step, ln step) between consecutive finite positive observations of each column."""
    valid = np.isfinite(x) & (x > 0)
    prev = _prev_valid(valid)
    step = valid & (prev >= 0)
    with np.errstate(invalid="ignore", divide="ignore"):
        ln = np.where(step, np.log(x / x[np.maximum(prev, 0), np.arange(x.shape[1])[None, :]]), 0.0)
    return step, ln


def _ln_ratio(cur: np.ndarray, old: np.ndarray, ok: np.ndarray) -> np.ndarray:
    with np.errstate(invalid="ignore", divide="ignore"):
        return np.where(ok, np.log(np.where(ok, cur, 1.0) / np.where(ok, old, 1.0)), np.nan)


def _delay_pair(x: np.ndarray, C, S, member, inp: Inputs, label: str) -> dict:
    sb, se, d = inp.sb, inp.se, SHARES_DELAY
    t0 = max(sb, d)
    if t0 >= se:
        return {"note": "role shorter than the delay"}
    cur, old = x[t0:se], x[t0 - d:se - d]
    ok = member[t0:se] & np.isfinite(cur) & np.isfinite(old) & (cur > 0) & (old > 0)
    res = _split_stats(ok, _ln_ratio(cur, old, ok), C[t0:se] - C[t0 - d:se - d], (S[t0:se] - S[t0 - d:se - d]) > 0,
                       inp.role.ids)
    res["window"] = f"(t-252, t] role sessions; ratio {label}[t] / {label}[t-252]"
    return res


def _fb_continuity(inp: Inputs, series: dict) -> dict:
    """factor-break-v1 mass sessions of the role: jump shares of repaired names that day vs the other role days."""
    rep = inp.role.manifest.get("repair")
    if not rep or not rep.get("mass_sessions"):
        return {"status": "no factor-break repair recorded in the role manifest"}
    cells = rep["cells"]
    blob = read_pinned(inp.role.dir / cells["file"], cells["sha256"], cells["bytes"], what="role repair cells")
    rows = list(csv.DictReader(io.StringIO(blob.decode("utf-8"))))
    del blob
    nd = inp.n_dates
    out = {}
    for ms in rep["mass_sessions"]:
        sess = str(ms["session"])
        tm = int(np.searchsorted(inp.role.days, day_of(dt.date.fromisoformat(sess))))
        if tm >= nd or int(inp.role.days[tm]) != day_of(dt.date.fromisoformat(sess)):
            out[sess] = {"status": "mass session not on the role calendar"}
            continue
        sids = np.array(sorted({int(r["security_id"]) for r in rows
                                if r.get("mass_session") == sess and r.get("action") == "repaired"}), dtype=np.int64)
        pos, on = inp.role.columns_of(sids) if len(sids) else (np.zeros(0, np.int64), np.zeros(0, bool))
        cols = pos[on]
        other = np.ones(nd, dtype=bool)
        other[tm] = False
        res = {"repaired_names": int(len(sids)), "repaired_names_on_axis": int(len(cols))}
        for name, x in series.items():
            step, ln = _log_steps(x)
            a = np.abs(ln)
            r = {}
            for tag, thr in (("ln1.5", FB_JUMP_LN), ("ln_0.01", FB_SMALL_JUMP_LN)):
                jump = step & (a > thr)
                ns_r = int(step[tm, cols].sum())
                per_day = jump.sum(axis=1) / np.maximum(step.sum(axis=1), 1)
                days_ok = other & (step.sum(axis=1) > 0)
                r[tag] = {"repaired_steps_that_day": ns_r, "repaired_jumps_that_day": int(jump[tm, cols].sum()),
                          "repaired_share_that_day": frac(jump[tm, cols].sum(), ns_r),
                          "all_names_share_that_day": frac(jump[tm].sum(), step[tm].sum()),
                          "normal_day_pooled_share": frac(jump[other].sum(), step[other].sum()),
                          "normal_day_median_share": float(np.median(per_day[days_ok])) if days_ok.any() else None,
                          "normal_day_p99_share": float(np.quantile(per_day[days_ok], 0.99)) if days_ok.any() else None,
                          "day_before_share": frac(jump[tm - 1].sum(), step[tm - 1].sum()) if tm > 0 else None,
                          "day_after_share": frac(jump[tm + 1].sum(), step[tm + 1].sum()) if tm + 1 < nd else None}
            steps_r = step[tm, cols]
            r["repaired_median_abs_ln_step_that_day"] = float(np.median(a[tm, cols][steps_r])) if steps_r.any() else None
            r["normal_day_median_abs_ln_step"] = float(np.median(a[other][step[other]])) if step[other].any() else None
            big = cols[step[tm, cols] & (a[tm, cols] > FB_JUMP_LN)]
            r["repaired_big_jump_examples"] = [[int(inp.role.ids[c]), float(ln[tm, c])] for c in big[:10]]
            res[name] = r
            del step, ln, a
        out[sess] = res
    return out


def _spot_splits(inp: Inputs, j: Join, split, s, C, series: dict, lines_meta: dict) -> list:
    """Known TRAIN-window splitters (public split dates; lines found through the bridge CIK): log ratios around
    the split for every pair."""
    days = j.days
    nd = inp.n_dates
    out = []
    for ticker, cik, date_text, ratio in SPOT_SPLITS:
        day = day_of(dt.date.fromisoformat(date_text))
        te = int(np.searchsorted(days, day))
        rec = {"ticker": ticker, "cik": cik, "expected_session": date_text, "expected_factor": ratio, "lines": []}
        c = int(np.searchsorted(j.ciks, cik)) if len(j.ciks) else 0
        if not int(days[0]) <= day <= int(days[-1]):
            rec["status"] = "split date outside the role"
            out.append(rec)
            continue
        if not len(j.ciks) or c >= len(j.ciks) or int(j.ciks[c]) != cik:
            rec["status"] = "CIK not linked on the role"
            out.append(rec)
            continue
        cols = np.flatnonzero((j.link[max(te - SPOT_SEARCH_SESSIONS, 0):te + SPOT_SEARCH_SESSIONS + 1] == c).any(axis=0))
        for i in cols.tolist():
            lo, hi = max(te - SPOT_SEARCH_SESSIONS, 0), min(te + SPOT_SEARCH_SESSIONS + 1, nd)
            hits = [t for t in range(lo, hi) if split[t, i]]
            ts = hits[0] if hits else te
            line = {"sid": int(inp.role.ids[i]), "symbol": lines_meta.get(int(inp.role.ids[i])),
                    "primary": bool(j.pline[min(ts, nd - 1), i]),
                    "detected": [{"session": date_of(days[t]), "factor": round(float(math.exp(s[t, i])), 6)} for t in hits],
                    "at": []}
            for off in SPOT_OFFSETS:
                t = ts + off
                if not 0 <= t < nd:
                    continue
                row = {"offset": off, "session": date_of(days[t]), "member": bool(inp.role.member[t, i])}
                if t >= SHARES_DELAY:
                    row["ln_split_factor_252"] = float(C[t, i] - C[t - SHARES_DELAY, i])
                    for name in ("shares_out", "adj"):
                        x = series[name]
                        cur, old = x[t, i], x[t - SHARES_DELAY, i]
                        row[f"ln_{name}_over_delay252"] = (float(math.log(cur / old)) if math.isfinite(cur)
                                                           and math.isfinite(old) and cur > 0 and old > 0 else None)
                q, q4 = series["shrs_q"][t, i], series["shrs_q_lag4"][t, i]
                row["ln_shrs_q_over_lag4"] = (float(math.log(q / q4)) if math.isfinite(q) and math.isfinite(q4)
                                              and q > 0 and q4 > 0 else None)
                r = int(j.rix[t, i])
                row["shrs_q_period_end"] = date_of(j.ev_pe[r]) if r >= 0 else None
                line["at"].append(row)
            rec["lines"].append(line)
        out.append(rec)
    return out


def check_c4(audit: Audit) -> dict:
    inp, budget = audit.inp, audit.budget
    j = audit.join()
    n = inp.n
    sb, se = inp.sb, inp.se
    close = inp.role_payload("close.f64", "<f8")
    raw = inp.role_payload("raw_close.f64", "<f8")
    present = inp.role_payload("present.u8", "u1") != 0
    with np.errstate(invalid="ignore", divide="ignore"):
        good = present & np.isfinite(close) & np.isfinite(raw) & (close > 0) & (raw > 0)
        f = np.where(good, close / np.where(good, raw, 1.0), np.nan)  # the role's chained share factor close/raw
    del close, raw, present, good
    step, s = _log_steps(f)
    split = step & (np.abs(s) >= SPLIT_EVENT_LN)
    C = np.cumsum(np.where(split, s, 0.0), axis=0)
    S = np.cumsum(split, axis=0, dtype=np.int32)
    del step
    budget.check("C4 factors")
    member = inp.role.member != 0
    ev_split = np.argwhere(split)
    in_window = (ev_split[:, 0] >= sb) & (ev_split[:, 0] < se) if len(ev_split) else np.zeros(0, dtype=bool)
    events = {"split_events_role": int(len(ev_split)), "split_events_score_window": int(in_window.sum()),
              "names_with_split_score_window": int(len(np.unique(ev_split[in_window, 1]))) if len(ev_split) else 0}
    so = inp.field("shares_out")
    with np.errstate(invalid="ignore", divide="ignore"):
        adj = np.where(np.isfinite(f) & np.isfinite(so) & (so > 0), so / f, np.nan)  # shares_out x raw_close / close
    del f
    pairs = {"shares_out/delay(shares_out,252)": _delay_pair(so, C, S, member, inp, "shares_out"),
             "adj/delay(adj,252)": _delay_pair(adj, C, S, member, inp, "adj")}
    pairs["adj/delay(adj,252)"]["definition"] = "adj = shares_out * raw_close / close (the library's split-safe form)"
    budget.check("C4 delay pairs")
    # shrs_q vs shrs_q_lag4 (the pair of the visible events row)
    qf, q4f = inp.field("shrs_q"), inp.field("shrs_q_lag4")
    q, q4 = qf[sb:se], q4f[sb:se]
    rs = np.maximum(j.rix[sb:se], 0)
    okq = member[sb:se] & j.fresh[sb:se] & np.isfinite(q) & np.isfinite(q4) & (q > 0) & (q4 > 0)
    if len(j.ev_clock):
        A, cday = j.ev_pe[rs], j.ev_clock[rs] // DAY_NS
    else:
        A = cday = np.zeros(rs.shape, dtype=np.int64)
    tA = np.searchsorted(j.days, A - bfe.LAG4, side="right") - 1
    tC = np.searchsorted(j.days, cday, side="right") - 1
    truncated = okq & (tA < 0)
    okq &= (tA >= 0) & (tC >= 0)
    col = np.arange(n)[None, :]
    tA0, tC0 = np.maximum(tA, 0), np.maximum(tC, 0)
    shrs_res = _split_stats(okq, _ln_ratio(q, q4, okq), C[tC0, col] - C[tA0, col], (S[tC0, col] - S[tA0, col]) > 0,
                            inp.role.ids)
    shrs_res["window"] = "(period_end - 365 d, row clock]; ratio shrs_q / shrs_q_lag4 of the visible events row"
    shrs_res["cells_window_before_role_excluded"] = int(truncated.sum())
    shrs_res["concept_path"] = ("not checkable on the full population: atx.fundamental-events/v1 exposes no concept "
                                "column; C2 shrs_q runs a raw-fact sample check (lag4_concept_check)")
    pairs["shrs_q/shrs_q_lag4"] = shrs_res
    del tA, tC, tA0, tC0, A, cday, rs, okq
    budget.check("C4 shrs_q")
    common_table = audit.line_symbols()
    spots = _spot_splits(inp, j, split, s, C, {"shares_out": so, "adj": adj, "shrs_q": qf, "shrs_q_lag4": q4f},
                         common_table)
    del s, C, S, split
    budget.check("C4 spot checks")
    continuity = _fb_continuity(inp, {"adj": adj, "shares_out": so})
    budget.check("C4 factor-break continuity")
    flags = [name for name, r in pairs.items()
             if (r.get("share_explained") or 0) >= SPLIT_FLAG_SHARE and r.get("big_cells", 0) > 0]
    fb_flags = []
    for sess, r in continuity.items():
        a = r.get("adj") if isinstance(r, dict) else None
        if not a:
            continue
        for tag, excess in (("ln1.5", FB_CONT_FLAG_EXCESS), ("ln_0.01", FB_CONT_SMALL_FLAG_EXCESS)):
            got, base = a[tag]["repaired_share_that_day"], a[tag]["normal_day_pooled_share"]
            if got is not None and base is not None and got - base > excess:
                fb_flags.append(f"adj jumps on {sess} ({tag}): repaired {got} vs normal {base}")
    parts = [f"{name}: {r.get('explained_cells')} of {r.get('big_cells')} big ratios on split names are the split "
             f"factor (share {r.get('share_explained')})" for name, r in pairs.items()]
    cont = []
    for sess, r in continuity.items():
        a = (r.get("adj") or {}) if isinstance(r, dict) else {}
        if a:
            cont.append(f"{sess} adj |jump|>ln1.5 repaired {a['ln1.5']['repaired_share_that_day']} vs normal "
                        f"{a['ln1.5']['normal_day_pooled_share']}, |jump|>0.01 repaired "
                        f"{a['ln_0.01']['repaired_share_that_day']} vs normal {a['ln_0.01']['normal_day_pooled_share']}")
    finding = (f"{events['split_events_score_window']} split events on {events['names_with_split_score_window']} names "
               f"in the TRAIN window; " + "; ".join(parts) + ("; factor-break continuity: " + "; ".join(cont) if cont else ""))
    flags_all = [f"split-contaminated {flags}"] if flags else []
    flags_all += fb_flags
    return {"status": "FLAG" if flags_all else "PASS",
            "finding": finding + ("; FLAG: " + "; ".join(flags_all) if flags_all else ""),
            "criterion": CRITERIA["C4"],
            "metrics": {"split_events": events, "pairs": pairs,
                        "pair_status": {k: ("FLAG" if k in flags else "PASS") for k in pairs},
                        "spot_splits": spots, "factor_break_continuity": continuity}}


# ---------------------------------------------------------------------------
# C5 ratios, C6 groups
# ---------------------------------------------------------------------------

def check_c5(audit: Audit) -> dict:
    inp, budget = audit.inp, audit.budget
    member = inp.role.member != 0
    sb, se = inp.sb, inp.se
    m = member[sb:se]
    years = inp.role.years[sb:se]
    cache = {}

    def get(name):
        if name not in cache:
            cache[name] = np.array(inp.field(name)[sb:se]) if name in inp.entries else None
        return cache[name]

    out, flags = {}, []
    for label, num, den in RATIOS:
        a = get(num)
        b = get(den) if den else None
        if a is None or (den and b is None):
            out[label] = {"status": "missing field"}
            continue
        if b is None:
            ok = m & np.isfinite(a)
            x = a[ok]
            den_nonpos = None
        else:
            both = m & np.isfinite(a) & np.isfinite(b)
            den_nonpos = int((both & (b <= 0)).sum())
            ok = both & (b != 0)
            x = a[ok] / b[ok]
        tail = int(np.count_nonzero(np.abs(x) > RATIO_TAIL_ABS))
        per_year = {}
        for y in sorted(set(years.tolist())):
            sel = ok[years == y]
            xy = (a[years == y][sel] / b[years == y][sel]) if b is not None else a[years == y][sel]
            per_year[str(y)] = {"n": int(len(xy)), "tail_share": frac(np.count_nonzero(np.abs(xy) > RATIO_TAIL_ABS), len(xy))}
        out[label] = {"n": int(len(x)), "quantiles": quantiles(x), "tail_cells": tail,
                      "tail_share": frac(tail, len(x)), "negative_share": frac(np.count_nonzero(x < 0), len(x)),
                      "denominator_nonpositive_cells": den_nonpos, "per_year": per_year}
        if out[label]["tail_share"] is not None and out[label]["tail_share"] > RATIO_TAIL_MAX_SHARE:
            flags.append(f"{label} |x|>{RATIO_TAIL_ABS:g} share {out[label]['tail_share']}")
        budget.check(f"C5 {label}")
    me = get(ME_NAME)
    sign = None
    if me is not None:
        fin = m & np.isfinite(me)
        sign = {"finite_member_cells": int(fin.sum()), "nonpositive_cells": int((fin & (me <= 0)).sum())}
        if sign["nonpositive_cells"]:
            flags.append(f"me_company non-positive cells {sign['nonpositive_cells']}")
    be = get("be")
    be_sign = None
    if be is not None:
        fin = m & np.isfinite(be)
        be_sign = {"finite_member_cells": int(fin.sum()), "negative_share": frac((fin & (be < 0)).sum(), fin.sum())}
    finding = "; ".join(f"{k} n={v.get('n')} p1/p50/p99={_q3(v)} |x|>10 {v.get('tail_share')}" for k, v in out.items())
    return {"status": "FLAG" if flags else "PASS", "finding": finding + ("; FLAG: " + "; ".join(flags) if flags else ""),
            "criterion": CRITERIA["C5"],
            "metrics": {"ratios": out, "me_company_sign": sign, "be_sign": be_sign,
                        "cells": "TRAIN score-window member cells with finite numerator and denominator (denominator != 0)"}}


def _q3(v):
    q = v.get("quantiles") or {}
    fmt = lambda z: "nan" if z is None else f"{z:.3g}"  # noqa: E731
    return "/".join(fmt(q.get(k)) for k in ("p1", "p50", "p99"))


def check_c6(audit: Audit) -> dict:
    inp, budget = audit.inp, audit.budget
    member = inp.role.member != 0
    sb, se = inp.sb, inp.se
    fin = member[sb:se].copy()
    for name in FUNDAMENTAL_BASIS:
        fin &= np.isfinite(inp.field(name)[sb:se])
    out, flags = {}, []
    for name in ("grp_ff12", "grp_ff49", "grp_sic2"):
        if name not in inp.entries:
            out[name] = {"status": "missing field"}
            continue
        lab = inp.field(name)[sb:se]
        mins, small_g, all_g, small_m, all_m, unlabeled, groups_per_day = [], 0, 0, 0, 0, 0, []
        for t in range(se - sb):
            mk = fin[t] & np.isfinite(lab[t])
            unlabeled += int(np.count_nonzero(fin[t] & ~np.isfinite(lab[t])))
            if not mk.any():
                continue
            counts = np.bincount(lab[t][mk].astype(np.int64))
            sizes = counts[counts > 0]
            mins.append(int(sizes.min()))
            groups_per_day.append(len(sizes))
            small = sizes < GROUP_SMALL
            small_g += int(small.sum())
            all_g += len(sizes)
            small_m += int(sizes[small].sum())
            all_m += int(sizes.sum())
        r = {"days": len(mins), "min_group_size": min(mins) if mins else None,
             "median_daily_min": float(np.median(mins)) if mins else None,
             "mean_groups_per_day": float(np.mean(groups_per_day)) if groups_per_day else None,
             "share_small_groups": frac(small_g, all_g), "share_members_in_small_groups": frac(small_m, all_m),
             "finite_fundamental_member_cells": all_m, "finite_fundamental_cells_unlabeled": unlabeled,
             "unlabeled_share": frac(unlabeled, unlabeled + all_m)}
        out[name] = r
        if name != "grp_sic2" and ((r["share_small_groups"] or 0) > GROUP_SMALL_MAX_SHARE
                                   or (r["share_members_in_small_groups"] or 0) > GROUP_SMALL_MEMBER_MAX_SHARE):
            flags.append(f"{name} small groups {r['share_small_groups']} / members {r['share_members_in_small_groups']}")
        budget.check(f"C6 {name}")
    finding = "; ".join(f"{k}: min {v.get('min_group_size')}, groups<{GROUP_SMALL} {v.get('share_small_groups')}, "
                        f"members in them {v.get('share_members_in_small_groups')}, groups/day "
                        f"{None if v.get('mean_groups_per_day') is None else round(v['mean_groups_per_day'], 1)}"
                        for k, v in out.items())
    return {"status": "FLAG" if flags else "PASS", "finding": finding + ("; FLAG: " + "; ".join(flags) if flags else ""),
            "criterion": CRITERIA["C6"], "metrics": {"groups": out}}


# ---------------------------------------------------------------------------
# C7 TRAIN/VAL definition identity (manifest text only)
# ---------------------------------------------------------------------------

def _norm_path(p) -> str:
    return str(p).replace("\\", "/").rstrip("/").lower()


def _project(manifest: dict) -> dict:
    """Definition keys only: coverage, statistics and every other data-derived key is dropped."""
    role_path = _norm_path((manifest.get("role") or {}).get("path", ""))
    fields = {}
    for e in manifest.get("fields", []):
        proj = {k: e.get(k) for k in DEF_KEYS if k in e}
        proj["non_role_sources"] = sorted({s.get("sha256") for s in e.get("sources", [])
                                           if role_path and not _norm_path(s.get("path", "")).startswith(role_path + "/")})
        fields[e["name"]] = proj
    iss = (manifest.get("source_checks") or {}).get("issuer") or {}
    fem, ib, sm = iss.get("fund_events_manifest") or {}, iss.get("identity_bridge") or {}, iss.get("sic_mapping") or {}
    issuer = {"fund_lag_sessions": iss.get("fund_lag_sessions"),
              "fund_events_manifest": {k: fem.get(k) for k in ("sha256", "items", "values_label", "rehearsal_identity")},
              "identity_bridge": {k: ib.get(k) for k in ("rule", "link_rule", "rehearsal_identity", "scope_complete")},
              "sic_mapping": {k: sm.get(k) for k in ("table_sha256", "versions", "code_sha256_lf", "code_git_blob_sha1",
                                                     "ff12_numbering", "table_rule")}}
    return {"top": {k: manifest.get(k) for k in TOP_KEYS}, "order": [e["name"] for e in manifest.get("fields", [])],
            "fields": fields, "issuer": issuer}


def check_c7(audit: Audit) -> dict:
    inp = audit.inp
    blob = read_pinned(inp.val_path, inp.a.val_fields_sha256, what="VAL fields manifest")
    val = _project(json.loads(blob))  # the parsed VAL manifest is projected at once; nothing else is kept
    del blob
    train = _project(inp.fm)
    need(val["top"].get("schema") == prf.SCHEMA, "VAL fields manifest schema")
    only_train = sorted(set(train["fields"]) - set(val["fields"]))
    only_val = sorted(set(val["fields"]) - set(train["fields"]))
    differ = {}
    for name in train["fields"]:
        if name in val["fields"]:
            keys = sorted(k for k in set(train["fields"][name]) | set(val["fields"][name])
                          if train["fields"][name].get(k) != val["fields"][name].get(k))
            if keys:
                differ[name] = keys
    top = sorted(k for k in TOP_KEYS if train["top"].get(k) != val["top"].get(k))
    issuer = sorted(k for k in train["issuer"] if train["issuer"][k] != val["issuer"][k])
    order_same = train["order"] == val["order"]
    flags = []
    if only_train or only_val:
        flags.append(f"field sets differ (TRAIN only {only_train}, VAL only {only_val})")
    if not order_same:
        flags.append("field order differs")
    if differ:
        flags.append(f"definition keys differ {differ}")
    if top:
        flags.append(f"manifest-level keys differ {top}")
    if issuer:
        flags.append(f"issuer source checks differ {issuer}")
    finding = (f"{len(set(train['fields']) & set(val['fields']))} fields compared; code LF sha "
               f"{'equal' if 'code_sha256_lf' not in top else 'DIFFERENT'}; issuer inputs "
               f"{'equal' if not issuer else 'DIFFERENT'}")
    return {"status": "FLAG" if flags else "PASS", "finding": finding + ("; FLAG: " + "; ".join(flags) if flags else ""),
            "criterion": CRITERIA["C7"],
            "metrics": {"fields_compared": len(set(train["fields"]) & set(val["fields"])), "only_train": only_train,
                        "only_val": only_val, "order_same": order_same, "differing_fields": differ,
                        "differing_top_keys": top, "differing_issuer_checks": issuer,
                        "train_code": {k: train["top"].get(k) for k in ("code_sha256_lf", "code_git_blob_sha1")},
                        "val_code": {k: val["top"].get(k) for k in ("code_sha256_lf", "code_git_blob_sha1")},
                        "val_read": "manifest.json text only, projected to definition keys"}}


# ---------------------------------------------------------------------------
# Document: merge, save, render
# ---------------------------------------------------------------------------

def run_key(inp: Inputs, a) -> str:
    params = {"train_seal": TRAIN_SEAL.isoformat(), "sample": int(a.sample), "seed": int(a.seed), "rel_tol": REL_TOL,
              "thresholds": [COVERAGE_MIN_CORE_COMMON, COVERAGE_MAX_YEAR_STEP, FC1_MAX_SHARE, VINTAGE_MAX_SHARE,
                             SPLIT_EVENT_LN, SPLIT_BIG_LN, SPLIT_RESIDUAL_LN, SPLIT_FLAG_SHARE, SHARES_DELAY,
                             FB_JUMP_LN, FB_SMALL_JUMP_LN, FB_CONT_FLAG_EXCESS, FB_CONT_SMALL_FLAG_EXCESS,
                             RATIO_TAIL_ABS, RATIO_TAIL_MAX_SHARE, GROUP_SMALL, GROUP_SMALL_MAX_SHARE,
                             GROUP_SMALL_MEMBER_MAX_SHARE], "spot_splits": SPOT_SPLITS}
    return sha_bytes(canonical({"pins": inp.pins, "params": params,
                                "code": code_identity(Path(__file__))["sha256_lf"]}).encode("utf-8"))


def load_doc(out: Path, key: str, inp: Inputs) -> dict:
    path = out / "audit.json"
    if path.exists():
        doc = json.loads(path.read_text(encoding="utf-8"))
        need(doc.get("schema") == SCHEMA and doc.get("run_key") == key,
             f"{path} holds an audit of different inputs, parameters or code; use a fresh --output")
        return doc
    return {"schema": SCHEMA, "run_key": key, "pins": inp.pins, "paths": inp.paths,
            "code": {"audit_fund_fields.py": code_identity(Path(__file__))},
            "parameters": {"train_seal": TRAIN_SEAL.isoformat(), "sample_cells": inp.a.sample, "seed": inp.a.seed,
                           "rel_tol": REL_TOL, "lag_sessions": inp.lag},
            "hygiene": ("TRAIN only: role < 2023-01-01 asserted; events/SIC rows filtered accepted_utc < 2023-01-01 at "
                        "decode; CF-R facts filed_date < 2023-01-01; FSDS SUB quarters <= 2022q4; VAL manifest text "
                        "projected to definitions only; no return/IC/performance statistic computed"),
            "checks": {}}


def write_atomic(path: Path, data: bytes):
    tmp = path.with_name("." + path.name + ".tmp")
    with tmp.open("wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def save_doc(out: Path, doc: dict):
    doc = clean(doc)
    write_atomic(out / "audit.json", (json.dumps(doc, indent=1, sort_keys=True, allow_nan=False) + "\n").encode("utf-8"))
    write_atomic(out / "audit.md", render_md(doc).encode("utf-8"))


def _fmt(v, nd=3):
    if v is None:
        return "-"
    if isinstance(v, float):
        return f"{v:.{nd}f}"
    return str(v)


def render_md(doc: dict) -> str:
    L = ["# T25 fundamentals fields audit (TRAIN only)", "",
         "Selection-neutral data audit of the library-v4 issuer fields. No return, IC or performance statistic "
         "is computed.", "", f"- run_key `{doc['run_key']}`", f"- hygiene: {doc['hygiene']}", "", "## Inputs", "",
         "| input | path | SHA-256 |", "|---|---|---|"]
    names = {"fields": "fields_manifest_sha256", "role": "role_manifest_sha256",
             "identity_bridge": "identity_bridge_manifest_sha256", "fund_events": "fund_events_manifest_sha256",
             "val_fields_manifest": "val_fields_manifest_sha256", "lake": "lake_manifest_sha256"}
    for k, pin in names.items():
        L.append(f"| {k} | `{doc['paths'].get(k)}` | `{doc['pins'].get(pin)}` |")
    L += ["", "## Summary", "", "| check | status | finding |", "|---|---|---|"]
    for c in CHECKS:
        r = doc["checks"].get(c)
        L.append(f"| {c} {CHECK_TITLES[c]} | {r['status'] if r else 'not run'} | "
                 f"{(r or {}).get('finding', '').replace('|', '/')} |")
    for c in CHECKS:
        r = doc["checks"].get(c)
        if not r:
            continue
        L += ["", f"## {c} {CHECK_TITLES[c]}: {r['status']}", "", f"Criterion: {r['criterion']}", "",
              f"Finding: {r['finding']}", ""]
        if "elapsed_s" in r:
            L.append(f"(elapsed {r['elapsed_s']} s, peak RSS {r.get('peak_rss_mib')} MiB)")
            L.append("")
        L += RENDER.get(c, lambda m: [])(r.get("metrics") or {})
    return "\n".join(L) + "\n"


def _render_c1(m):
    years = m.get("train_years", [])
    L = ["| field | re-join mismatches (value / NaN) | reasons = producer | TRAIN member cov | TRAIN common cov | "
         + " | ".join(f"common {y}" for y in years) + " |", "|---|---|---|---|---|" + "---|" * len(years)]
    for name, r in (m.get("fields") or {}).items():
        cov = r["coverage"]
        row = [name, f"{r['reproduction']['value_mismatch']} / {r['reproduction']['nan_pattern_mismatch']}",
               "yes" if r["reasons_consistent"] else "NO", _fmt(cov["score_window"]["member"]["frac"]),
               _fmt(cov["score_window"]["common_member"]["frac"])]
        row += [_fmt(cov["per_year"].get(str(y), {}).get("common_member", {}).get("frac")) for y in years]
        L.append("| " + " | ".join(row) + " |")
    L += ["", "NaN reasons on member cells (all role sessions):", "",
          "| field | unlinked | secondary | ambiguous | no visible row | stale | item NaN / unmapped / line NaN | finite |",
          "|---|---|---|---|---|---|---|---|"]
    for name, r in (m.get("fields") or {}).items():
        c = r["nan_reasons_member_cells"]
        other = c.get("visible_nan", c.get("unmapped", (c.get("own_line_nan", 0) or 0) + (c.get("other_linked_line_nan", 0) or 0)))
        L.append(f"| {name} | {c.get('unlinked')} | {c.get('secondary')} | {c.get('ambiguous')} | "
                 f"{c.get('no_visible_row', '-')} | {c.get('stale', '-')} | {other} | {c.get('finite')} |")
    L += ["", f"Link member cells: {m.get('link_member_cells')} (producer {m.get('producer_link_member_cells')})", ""]
    return L


def _render_c2(m):
    L = [f"Replay code matches the events manifest pins: {m.get('replay_code_matches_events_pins')}; concept map "
         f"matches: {m.get('concept_map_matches')}", "",
         "| item | status | passed / sampled | candidates | failures | witness kinds | row clock basis | min row slack h | "
         "min witness slack h |", "|---|---|---|---|---|---|---|---|---|"]
    items = m.get("items") or {}
    for x in CORE_ITEMS:
        r = items.get(x)
        if r:
            L.append(f"| {x} | {r['status']} | {r['passed']} / {r['sampled']} | {r['candidates']} | {r['failures']} | "
                     f"{r['witness_kinds']} | {r['row_clock_basis']} | {_fmt(r['min_row_slack_hours'], 1)} | "
                     f"{_fmt(r['min_witness_slack_hours'], 1)} |")
    bad = [(x, c) for x in CORE_ITEMS for c in (items.get(x) or {}).get("cells", []) if not c.get("pass")]
    if bad:
        L += ["", "Failing cells (first 20):", ""]
        for x, c in bad[:20]:
            L.append(f"- {x} {c['session']} sid {c['sid']} cik {c.get('cik')}: value {c['value']}, row "
                     f"{(c.get('row') or {}).get('accession')}, join {c.get('join_equal')}, fresh {c.get('fresh')}, "
                     f"before mark {c.get('row_before_mark')}, sub {(c.get('sub') or {}).get('status')}, replay "
                     f"{(c.get('replay') or {}).get('status')}, witness {(c.get('witness') or {}).get('status')}")
    return L + [""]


def _render_c3(m):
    L = ["| clock year | rows | CIKs | FC1 | amended | rows w/ restated facts | restated facts | linked rows | linked FC1 |",
         "|---|---|---|---|---|---|---|---|---|"]
    for y, r in (m.get("per_clock_year") or {}).items():
        L.append(f"| {y} | {r['rows']} | {r['ciks']} | {r['fc1_rows']} | {r['amended_rows']} | "
                 f"{r['rows_with_restated_facts']} | {r['restated_facts']} | {r['linked_rows']} | {r['linked_fc1_rows']} |")
    L += ["", "| core item | (CIK, period_end) groups | 1 value | 2 values | 3+ values | revised share | finite member "
          "cells | FC1-row cells | amended-row cells | in-place revisions | anchor advances |",
          "|---|---|---|---|---|---|---|---|---|---|---|"]
    cells = m.get("member_cells_score_window") or {}
    for x, v in (m.get("vintages_linked_role_window") or {}).items():
        c = cells.get(x, {})
        L.append(f"| {x} | {v['groups_with_value']} | {v['vintages']['1']} | {v['vintages']['2']} | {v['vintages']['3+']} | "
                 f"{_fmt(v['revised_share'])} | {c.get('finite_member_cells')} | {c.get('fc1_row_cells')} | "
                 f"{c.get('amended_row_cells')} | {c.get('in_place_revisions')} | {c.get('anchor_advances')} |")
    return L + [""]


def _render_c4(m):
    L = [f"Split events: {m.get('split_events')}", "",
         "| pair | status | population cells (names) | big | explained (share) | big split factor | moved with split | "
         "split-neutral | baseline big share (no split) |", "|---|---|---|---|---|---|---|---|---|"]
    for label, r in (m.get("pairs") or {}).items():
        L.append(f"| {label} | {(m.get('pair_status') or {}).get(label)} | {r.get('population_cells')} "
                 f"({r.get('population_names')}) | {r.get('big_cells')} | {r.get('explained_cells')} "
                 f"({_fmt(r.get('share_explained'))}) | {r.get('big_split_factor_cells')} | {r.get('moved_with_split_cells')} | "
                 f"{r.get('split_neutral_cells')} | {_fmt(r.get('baseline_big_share'))} |")
    shrs = (m.get("pairs") or {}).get("shrs_q/shrs_q_lag4") or {}
    L += ["", f"shrs_q concept path: {shrs.get('concept_path')}", "", "Factor-break continuity (adj and shares_out):", ""]
    for sess, r in (m.get("factor_break_continuity") or {}).items():
        if not isinstance(r, dict) or "adj" not in r:
            L.append(f"- {sess}: {r}")
            continue
        for name in ("adj", "shares_out"):
            for tag, text in (("ln1.5", "ln 1.5"), ("ln_0.01", "0.01")):
                x = r[name][tag]
                L.append(f"- {sess} {name} |ln step| > {text}: repaired names {_fmt(x['repaired_share_that_day'], 4)} "
                         f"({x['repaired_jumps_that_day']}/{x['repaired_steps_that_day']}), all names "
                         f"{_fmt(x['all_names_share_that_day'], 4)}, normal day pooled {_fmt(x['normal_day_pooled_share'], 4)} "
                         f"(median {_fmt(x['normal_day_median_share'], 4)}, p99 {_fmt(x['normal_day_p99_share'], 4)})")
    L += ["", "Spot checks of known splitters (ln ratios; offsets in role sessions from the detected split):", "",
          "| ticker | sid (symbol) | detected | offset | session | ln K252 | ln shares_out/d252 | ln adj/d252 | "
          "ln shrs_q/lag4 |", "|---|---|---|---|---|---|---|---|---|"]
    for rec in m.get("spot_splits") or []:
        if not rec.get("lines"):
            L.append(f"| {rec['ticker']} {rec['expected_session']} | {rec.get('status', 'no line')} | | | | | | | |")
        for line in rec.get("lines", []):
            det = ", ".join(f"{d['session']} x{d['factor']:g}" for d in line["detected"]) or "none"
            for row in line["at"]:
                L.append(f"| {rec['ticker']} {rec['expected_session']} x{rec['expected_factor']:g} | {line['sid']} "
                         f"({line.get('symbol')}) | {det} | {row['offset']} | {row['session']} | "
                         f"{_fmt(row.get('ln_split_factor_252'))} | {_fmt(row.get('ln_shares_out_over_delay252'))} | "
                         f"{_fmt(row.get('ln_adj_over_delay252'))} | {_fmt(row.get('ln_shrs_q_over_lag4'))} |")
    return L + [""]


def _render_c5(m):
    ks = [k for k, _ in QUANTILES]
    L = ["| ratio | n | " + " | ".join(ks) + " | |x|>10 share | negative share | den <= 0 |",
         "|---|---|" + "---|" * len(ks) + "---|---|---|"]
    for label, r in (m.get("ratios") or {}).items():
        if "quantiles" not in r:
            L.append(f"| {label} | {r.get('status')} |")
            continue
        L.append(f"| {label} | {r['n']} | " + " | ".join(_fmt(r["quantiles"][k], 4) for k in ks)
                 + f" | {_fmt(r['tail_share'], 4)} | {_fmt(r['negative_share'], 4)} | {r['denominator_nonpositive_cells']} |")
    L += ["", f"me_company sign: {m.get('me_company_sign')}; be sign: {m.get('be_sign')}", ""]
    return L


def _render_c6(m):
    L = ["| label | days | min size | median daily min | groups/day | share groups < 5 | share members in them | "
         "unlabeled share |", "|---|---|---|---|---|---|---|---|"]
    for name, r in (m.get("groups") or {}).items():
        L.append(f"| {name} | {r.get('days')} | {r.get('min_group_size')} | {_fmt(r.get('median_daily_min'), 1)} | "
                 f"{_fmt(r.get('mean_groups_per_day'), 1)} | {_fmt(r.get('share_small_groups'), 4)} | "
                 f"{_fmt(r.get('share_members_in_small_groups'), 4)} | {_fmt(r.get('unlabeled_share'), 4)} |")
    return L + [""]


def _render_c7(m):
    return [f"- fields compared {m.get('fields_compared')}; order same {m.get('order_same')}",
            f"- differing fields: {m.get('differing_fields') or 'none'}",
            f"- differing manifest keys: {m.get('differing_top_keys') or 'none'}; issuer checks: "
            f"{m.get('differing_issuer_checks') or 'none'}",
            f"- code TRAIN {m.get('train_code')} / VAL {m.get('val_code')}", ""]


RENDER = {"C1": _render_c1, "C2": _render_c2, "C3": _render_c3, "C4": _render_c4, "C5": _render_c5, "C6": _render_c6,
          "C7": _render_c7}
FUNCS = {"C1": check_c1, "C3": check_c3, "C4": check_c4, "C5": check_c5, "C6": check_c6, "C7": check_c7}


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--output", required=True, type=Path, help="audit directory (audit.json + audit.md; merged per check)")
    p.add_argument("--checks", required=True, help=f"comma-separated subset of {','.join(CHECKS)}")
    p.add_argument("--items", default=",".join(CORE_ITEMS), help="C2 only: comma-separated core items")
    p.add_argument("--sample", type=int, default=SAMPLE_CELLS, help="C2 cells per item (declared 50)")
    p.add_argument("--seed", type=int, default=SEED, help="C2 sampling seed (declared)")
    for key in ("fields", "role", "identity_bridge", "fund_events", "val_fields_manifest", "lake"):
        p.add_argument("--" + key.replace("_", "-"), type=Path, default=DEFAULTS[key])
    for key in ("fields_sha256", "role_sha256", "identity_bridge_sha256", "fund_events_sha256", "val_fields_sha256",
                "lake_sha256"):
        p.add_argument("--" + key.replace("_", "-"), default=DEFAULTS[key])
    p.add_argument("--max-seconds", type=float, default=170.0)
    p.add_argument("--max-rss-mib", type=int, default=1400)
    return p.parse_args(argv)


def main(argv=None) -> int:
    a = parse_args(argv)
    checks = [c.strip().upper() for c in a.checks.split(",") if c.strip()]
    items = [x.strip() for x in a.items.split(",") if x.strip()]
    if not checks or any(c not in CHECKS for c in checks) or len(set(checks)) != len(checks):
        print(f"[t25] --checks must be distinct names from {','.join(CHECKS)}", file=sys.stderr)
        return 2
    if not items or any(x not in CORE_ITEMS for x in items) or len(set(items)) != len(items):
        print(f"[t25] --items must be distinct names from {','.join(CORE_ITEMS)}", file=sys.stderr)
        return 2
    if not 1 <= a.sample <= 1000:
        print("[t25] --sample must be in [1, 1000]", file=sys.stderr)
        return 2
    out = Path(a.output)
    try:
        budget = Budget(a.max_seconds, a.max_rss_mib)
        inp = Inputs(a, budget)
        out.mkdir(parents=True, exist_ok=True)
        doc = load_doc(out, run_key(inp, a), inp)
        save_doc(out, doc)
    except AuditError as exc:
        print(f"[t25] refused: {exc}", file=sys.stderr)
        return 2
    audit = Audit(inp, budget, a)
    rc = 0
    for c in CHECKS:
        if c not in checks:
            continue
        tick = time.perf_counter()
        try:
            result = (check_c2(audit, items, doc, lambda: save_doc(out, doc)) if c == "C2" else FUNCS[c](audit))
        except BudgetStop as exc:
            print(f"[t25] {c} budget stop: {exc}; completed checks are saved in {out / 'audit.json'}; re-run the rest",
                  file=sys.stderr)
            return 3
        except AuditError as exc:
            print(f"[t25] {c} refused: {exc}", file=sys.stderr)
            return 2
        except Exception as exc:  # recorded, loud, non-zero
            traceback.print_exc()
            result = {"status": "ERROR", "finding": f"{type(exc).__name__}: {exc}", "criterion": CRITERIA[c],
                      "metrics": {}}
            rc = 1
        result["elapsed_s"] = round(time.perf_counter() - tick, 2)
        result["peak_rss_mib"] = budget.peak >> 20
        doc["checks"][c] = result
        save_doc(out, doc)
        print(f"[t25] {c} {result['status']} ({result['elapsed_s']:.1f} s, peak RSS {result['peak_rss_mib']} MiB): "
              f"{result['finding'][:400]}", flush=True)
    print(f"[t25] total {budget.elapsed():.1f} s, peak RSS {budget.peak >> 20} MiB; wrote {out / 'audit.json'} and "
          f"{out / 'audit.md'}", flush=True)
    return rc


if __name__ == "__main__":
    sys.exit(main())
