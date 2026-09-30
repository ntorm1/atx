"""Loaders and derived series for the mega-alpha report.

Everything is read through ``Registry`` so every input file is hashed and listed in the report header, a missing
file is recorded (and rendered n/a by the caller) instead of raising, and a path that names hidden data is refused
before it is opened (``path_is_sealed``: validation / holdout / VAL, or a date-shaped path part in the first sealed year
of the research window or later; hash-named parts are ignored; the path is taken relative to the registry root;
tracked documents under ``DOCUMENT_ROOTS`` are exempt from the year rule only, ruling E-11). A file the report maps
instead of reading (a memmapped cache payload) passes the same check through ``Registry.sealed`` first.
Derived statistics mirror ``studies/nav_summ.py`` (return rows = return_observation
== 1 and not the first CSV row; Sharpe = mean / sd(ddof 1) x sqrt(sessions per year); tau_t over executed sessions
with positive pre-trade gross, deployment session excluded; Memmel (2003) SE; Lo (2002) single-cell DSR null).
"""
from __future__ import annotations

import csv
import datetime as dt
import hashlib
import io
import json
import math
import re
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from engine_tools import research_window as _window  # atx-impl/tools is on sys.path for every mega_report entry

# OD-5 (platform v8 E-4): the report seal check matches date-shaped path parts only (v7 defect P6-F1: a hex cache
# folder such as fp_2e2025f0aa11bb22 was refused as if it named 2025).
_HEX = re.compile(r'^(?:fp_|ic1_)?[0-9a-f]{16,64}$')
_NAMED = re.compile(r'(validation|holdout|(?<![A-Za-z])VAL(?![A-Za-z]))')
_YEAR = re.compile(r'(?<!\d)(20\d\d)')
FIRST_SEALED_YEAR = _window.FIRST_SEALED_YEAR
# Ruling E-11 (platform v8): tracked documents (sprint ledgers, plans, scorecards) carry run dates in their paths and
# are quoted by the report; they are exempt from the year rule by root class. The named pattern still applies to them.
DOCUMENT_ROOTS = ('.superpowers/sdd/', 'docs/plans/')
DAILY_COLS = ('session_ns', 'return_observation', 'executed', 'net_return', 'gross_return', 'pretrade_gross_dollars',
              'traded_dollars', 'one_way_turnover_gmv', 'gross_leverage', 'net_leverage', 'posttrade_nav')
EULER_GAMMA = 0.5772156649015329


# ----------------------------------------------------------------------------------------------- registry
def is_document_path(rel_path: str) -> bool:
    """True for a path under a tracked-document root (``DOCUMENT_ROOTS``, relative to the report root, no ``..``)."""
    p = rel_path.replace('\\', '/')
    return p.startswith(DOCUMENT_ROOTS) and '..' not in p.split('/')


def path_is_sealed(rel_path: str, first_sealed_year: int = FIRST_SEALED_YEAR) -> bool:
    """True when a path names hidden data. Hash-named parts are ignored; date-shaped parts are not, except in a
    tracked document (ruling E-11: exempt from the year rule by root class, never from the named pattern)."""
    if _NAMED.search(rel_path):
        return True
    if is_document_path(rel_path):
        return False
    for part in re.split(r'[\\/._-]', rel_path):
        if not part or _HEX.match(part):
            continue
        if any(int(y) >= first_sealed_year for y in _YEAR.findall(part)):
            return True
    return False


@dataclass
class Registry:
    """Reads files relative to ``root``; records path, bytes and SHA-256 (or the reason a file was not read)."""
    root: Path
    files: dict = field(default_factory=dict)

    def path(self, rel) -> Path:
        p = Path(rel)
        return p if p.is_absolute() else self.root / p

    def rel(self, p: Path) -> str:
        try:
            return Path(p).resolve().relative_to(self.root.resolve()).as_posix()
        except ValueError:
            return Path(p).as_posix()

    def sealed(self, rel) -> bool:
        """Seal check of ``rel`` (relative to the root) without opening it; a refusal is recorded in the manifest.
        Every read goes through it, and so must any file the caller maps or stats instead of reading."""
        key = self.rel(self.path(rel))  # relative to the research output root: a run-date-stamped root never enters
        if path_is_sealed(key):
            self.files[key] = {'status': 'refused (sealed)'}
            return True
        return False

    def read_bytes(self, rel) -> bytes | None:
        p = self.path(rel)
        key = self.rel(p)
        if self.sealed(p):
            return None
        if key in self.files and 'data' in self.files[key]:
            return self.files[key]['data']
        try:
            data = p.read_bytes()
        except OSError:
            self.files[key] = {'status': 'missing'}
            return None
        self.files[key] = {'status': 'read', 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(), 'data': data}
        return data

    def read_text(self, rel) -> str | None:
        b = self.read_bytes(rel)
        return None if b is None else b.decode('utf-8', errors='replace')

    def read_json(self, rel):
        t = self.read_text(rel)
        if t is None:
            return None
        try:
            return json.loads(t)
        except json.JSONDecodeError:
            self.files[self.rel(self.path(rel))]['status'] = 'unparseable json'
            return None

    def manifest(self) -> list[dict]:
        """One row per file touched: path, status, bytes, sha256 (sorted by path)."""
        return [{'path': k, 'status': v['status'], 'bytes': v.get('bytes'), 'sha256': v.get('sha256')}
                for k, v in sorted(self.files.items())]


def sha256_bytes(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def dig(obj, path: str):
    """``a.b.0.c`` lookup into nested dicts/lists; None when any step is missing."""
    cur = obj
    for part in path.split('.'):
        if cur is None:
            return None
        if isinstance(cur, list):
            try:
                cur = cur[int(part)]
            except (ValueError, IndexError):
                return None
        elif isinstance(cur, dict):
            cur = cur.get(part, cur.get(int(part)) if part.lstrip('-').isdigit() else None)
        else:
            return None
    return cur


# ----------------------------------------------------------------------------------------------- daily CSV
@dataclass
class Daily:
    """Numeric columns of one daily_<scenario>.csv (NaN where a cell does not parse)."""
    cols: dict
    source: str

    @property
    def rows(self) -> int:
        return int(self.cols['session_ns'].size) if 'session_ns' in self.cols else 0

    def has(self, *names) -> bool:
        return all(n in self.cols for n in names)

    def ret_mask(self) -> np.ndarray:
        m = self.cols['return_observation'] == 1
        m[:1] = False
        return m

    def dates(self, mask=None) -> list[dt.date]:
        ns = self.cols['session_ns'] if mask is None else self.cols['session_ns'][mask]
        return [ns_to_date(v) for v in ns]


def ns_to_date(ns) -> dt.date:
    return dt.datetime.fromtimestamp(int(ns) / 1e9, dt.timezone.utc).date()


def parse_daily(text: str, source: str, columns=DAILY_COLS) -> Daily | None:
    rows = list(csv.reader(io.StringIO(text)))
    if len(rows) < 2:
        return None
    header, body = rows[0], rows[1:]
    idx = {h: i for i, h in enumerate(header)}
    cols = {}
    for name in columns:
        if name not in idx:
            continue
        i = idx[name]
        vals = []
        for r in body:
            try:
                vals.append(float(r[i]))
            except (ValueError, IndexError):
                vals.append(float('nan'))
        cols[name] = np.array(vals, dtype=np.float64)
    if 'session_ns' in cols:
        cols['session_ns'] = np.array([int(r[idx['session_ns']]) for r in body], dtype=np.int64)
    if not {'session_ns', 'return_observation', 'net_return'} <= set(cols):
        return None
    return Daily(cols, source)


def parse_events(text: str) -> list[dict]:
    """events_<scenario>.csv rows as dicts (kind, pnl_dollars as float)."""
    out = []
    for r in csv.DictReader(io.StringIO(text)):
        try:
            r['pnl_dollars'] = float(r.get('pnl_dollars') or 0.0)
        except ValueError:
            r['pnl_dollars'] = float('nan')
        out.append(r)
    return out


# ----------------------------------------------------------------------------------------------- cell
@dataclass
class Cell:
    """One NAV cell dir: config metadata, summary.json, its nav_summ row, receipt, and lazily loaded dailies."""
    name: str
    rel_dir: str
    label: str
    group: str
    parent: str | None = None
    verdict: str | None = None
    lever: str | None = None
    exe_cfg: str | None = None
    summary: dict | None = None
    summ: dict | None = None
    receipt: dict | None = None
    reg: Registry | None = None
    _daily: dict = field(default_factory=dict)
    _events: dict = field(default_factory=dict)

    def scen(self, scenario_id: str) -> dict | None:
        for s in (self.summary or {}).get('scenarios') or []:
            if s.get('scenario') == scenario_id:
                return s
        return None

    def daily(self, scenario_id: str) -> Daily | None:
        if scenario_id not in self._daily:
            text = self.reg.read_text(f'{self.rel_dir}/daily_{scenario_id}.csv') if self.reg else None
            self._daily[scenario_id] = parse_daily(text, f'{self.rel_dir}/daily_{scenario_id}.csv') if text else None
        return self._daily[scenario_id]

    def events(self, scenario_id: str) -> list[dict] | None:
        if scenario_id not in self._events:
            text = self.reg.read_text(f'{self.rel_dir}/events_{scenario_id}.csv') if self.reg else None
            self._events[scenario_id] = parse_events(text) if text is not None else None
        return self._events[scenario_id]

    def exe(self, tags: dict) -> str | None:
        sha = (self.receipt or {}).get('executable_sha256')
        if sha:
            s8 = sha[:8]
            return f'{s8} ({tags[s8]})' if s8 in tags else s8
        return self.exe_cfg


def load_cells(reg: Registry, cfg: dict, summ_rows: dict) -> list[Cell]:
    """Cells in config order; ``summ_rows`` maps dir basename -> nav_summ row."""
    prefix = cfg.get('cell_prefix', '')
    suffix = cfg.get('receipt_suffix')
    out = []
    for c in cfg.get('cells', []):
        rel = c['dir'].rstrip('/')
        base = rel.split('/')[-1]
        name = base[len(prefix):] if prefix and base.startswith(prefix) else base
        cell = Cell(name=name, rel_dir=rel, label=c.get('label') or name, group=c.get('group', ''),
                    parent=c.get('parent'), verdict=c.get('verdict'), lever=c.get('lever'), exe_cfg=c.get('exe'), reg=reg)
        cell.summary = reg.read_json(f'{rel}/summary.json')
        cell.summ = summ_rows.get(base)
        if suffix:
            cell.receipt = reg.read_json(f'{rel}{suffix}')
        out.append(cell)
    return out


def summ_index(rows) -> dict:
    """nav_summ --json rows keyed by the dir basename (either path separator)."""
    out = {}
    for r in rows or []:
        d = str(r.get('dir', '')).replace('\\', '/').rstrip('/')
        out[d.split('/')[-1]] = r
    return out


# ----------------------------------------------------------------------------------------------- statistics
def sharpe(x: np.ndarray, annual: float) -> float | None:
    x = np.asarray(x, dtype=np.float64)
    if x.size < 2:
        return None
    sd = float(x.std(ddof=1))
    return float(x.mean() / sd * math.sqrt(annual)) if sd > 0 else None


def returns(d: Daily, col: str = 'net_return') -> tuple[list[dt.date], np.ndarray]:
    m = d.ret_mask()
    return d.dates(m), d.cols[col][m]


def nav_path(d: Daily, col: str = 'net_return') -> tuple[list[dt.date], np.ndarray]:
    """NAV = 1.0 on the session before the first return row, then the compounded return rows."""
    m = d.ret_mask()
    idx = np.flatnonzero(m)
    if idx.size == 0 or col not in d.cols:
        return [], np.array([])
    start = max(int(idx[0]) - 1, 0)
    dates = [ns_to_date(d.cols['session_ns'][start])] + d.dates(m)
    nav = np.concatenate([[1.0], np.cumprod(1.0 + d.cols[col][m])])
    return dates, nav


def drawdown(nav: np.ndarray) -> np.ndarray:
    return nav / np.maximum.accumulate(nav) - 1.0 if nav.size else nav


def period_returns(dates: list[dt.date], r: np.ndarray, key) -> dict:
    """Compounded returns grouped by ``key(date)`` (insertion order)."""
    acc: dict = {}
    for d, x in zip(dates, r):
        k = key(d)
        acc[k] = acc.get(k, 1.0) * (1.0 + float(x))
    return {k: v - 1.0 for k, v in acc.items()}


def monthly_returns(d: Daily) -> dict:
    dates, r = returns(d)
    return period_returns(dates, r, lambda x: (x.year, x.month))


def yearly_returns(d: Daily) -> dict:
    dates, r = returns(d)
    return period_returns(dates, r, lambda x: x.year)


def rolling_sharpe(r: np.ndarray, window: int, annual: float) -> np.ndarray:
    """Sharpe over each trailing ``window`` of return rows (NaN before the window fills)."""
    out = np.full(r.size, np.nan)
    if r.size < window or window < 2:
        return out
    c1 = np.concatenate([[0.0], np.cumsum(r)])
    c2 = np.concatenate([[0.0], np.cumsum(r * r)])
    s1 = c1[window:] - c1[:-window]
    s2 = c2[window:] - c2[:-window]
    mean = s1 / window
    var = (s2 - window * mean * mean) / (window - 1)
    with np.errstate(invalid='ignore', divide='ignore'):
        out[window - 1:] = np.where(var > 0, mean / np.sqrt(np.maximum(var, 0)) * math.sqrt(annual), np.nan)
    return out


def cost_drag(d: Daily) -> tuple[list[dt.date], np.ndarray]:
    """Gross NAV minus net NAV (both start 1.0): cumulative trading, financing and write-off drag."""
    dates, net = nav_path(d, 'net_return')
    if not d.has('gross_return'):
        return dates, np.array([])
    _, gross = nav_path(d, 'gross_return')
    return dates, gross - net


def tau_sessions(d: Daily) -> np.ndarray:
    """Daily one-way GMV turnover over executed sessions with positive pre-trade gross, deployment excluded."""
    if not d.has('executed', 'pretrade_gross_dollars', 'one_way_turnover_gmv', 'traded_dollars'):
        return np.array([])
    keep = (d.cols['executed'] == 1) & (d.cols['pretrade_gross_dollars'] > 0)
    fills = np.flatnonzero((d.cols['executed'] == 1) & (d.cols['traded_dollars'] > 0))
    if fills.size:
        keep[int(fills[0])] = False
    return d.cols['one_way_turnover_gmv'][keep]


def distribution(x: np.ndarray) -> dict | None:
    x = x[np.isfinite(x)] if x.size else x
    if x.size == 0:
        return None
    q = np.quantile(x, [0.05, 0.25, 0.5, 0.75, 0.95])
    return {'p5': float(q[0]), 'p25': float(q[1]), 'p50': float(q[2]), 'p75': float(q[3]), 'p95': float(q[4]),
            'mean': float(x.mean()), 'n': int(x.size)}


def memmel_se(sr_a: float, sr_b: float, rho: float, t: int, annual: float) -> float:
    a, b = sr_a / math.sqrt(annual), sr_b / math.sqrt(annual)
    var = (2.0 - 2.0 * rho + 0.5 * (a * a + b * b - 2.0 * a * b * rho * rho)) / t
    return math.sqrt(max(var, 0.0) * annual)


def paired(a: Daily | None, b: Daily | None, annual: float) -> dict | None:
    """Paired dSR(net) of ``a`` minus ``b`` on common return sessions with the Memmel (2003) SE."""
    if a is None or b is None:
        return None
    ma, mb = a.ret_mask(), b.ret_mask()
    ra = dict(zip(a.cols['session_ns'][ma].tolist(), a.cols['net_return'][ma].tolist()))
    rb = dict(zip(b.cols['session_ns'][mb].tolist(), b.cols['net_return'][mb].tolist()))
    common = sorted(set(ra) & set(rb))
    if len(common) < 3:
        return None
    x = np.array([ra[s] for s in common])
    y = np.array([rb[s] for s in common])
    sa, sb = sharpe(x, annual), sharpe(y, annual)
    if sa is None or sb is None:
        return None
    rho = float(np.corrcoef(x, y)[0, 1])
    se = memmel_se(sa, sb, rho, len(common), annual)
    return {'dsr': sa - sb, 'se': se, 'rho': rho, 't': (sa - sb) / se if se > 0 else None, 'sessions': len(common),
            'sr': sa, 'sr_ref': sb}


def norm_cdf(x: float) -> float:
    return 0.5 * math.erfc(-x / math.sqrt(2.0))


def norm_ppf(p: float) -> float:
    lo, hi = -40.0, 40.0
    for _ in range(200):
        mid = 0.5 * (lo + hi)
        if norm_cdf(mid) < p:
            lo = mid
        else:
            hi = mid
        if hi - lo <= 1e-15 * max(1.0, abs(mid)):
            break
    return 0.5 * (lo + hi)


def lo_dsr(moments: dict | None, n: int, annual: float) -> dict | None:
    """Single-cell deflated Sharpe with the Lo (2002) variance (1 + SR^2/2)/T and N trials (nav_summ formula)."""
    if not moments or n < 2:
        return None
    sr, t, g3, g4 = moments.get('sr_daily'), moments.get('sessions'), moments.get('skew'), moments.get('kurtosis')
    if not all(isinstance(v, (int, float)) for v in (sr, t, g3, g4)) or t < 2:
        return None
    var = (1.0 + sr * sr / 2.0) / t
    sr0 = math.sqrt(var) * ((1.0 - EULER_GAMMA) * norm_ppf(1.0 - 1.0 / n) + EULER_GAMMA * norm_ppf(1.0 - 1.0 / (n * math.e)))
    denom = 1.0 - g3 * sr + (g4 - 1.0) * sr * sr / 4.0
    dsr = norm_cdf((sr - sr0) * math.sqrt(t - 1) / math.sqrt(denom)) if denom > 0 else None
    return {'dsr': dsr, 'sr0_annual': sr0 * math.sqrt(annual), 'variance_sr': var, 'n': n}


# ----------------------------------------------------------------------------------------------- misc inputs
def quote_source(reg: Registry, rel: str, match: str, lines: int = 1) -> dict | None:
    """First line of ``rel`` containing ``match`` plus ``lines - 1`` following lines, with its 1-based number."""
    text = reg.read_text(rel)
    if text is None or not match:
        return None
    all_lines = text.splitlines()
    for i, ln in enumerate(all_lines):
        if match in ln:
            return {'path': rel, 'line': i + 1, 'text': '\n'.join(all_lines[i:i + max(lines, 1)])}
    return None


def git_head(where: Path) -> str | None:
    """``git rev-parse HEAD`` of the checkout containing ``where`` (None without git)."""
    import subprocess
    try:
        r = subprocess.run(['git', '-C', str(where), 'rev-parse', 'HEAD'], capture_output=True, text=True, timeout=20)
    except (OSError, subprocess.SubprocessError):
        return None
    return r.stdout.strip() or None if r.returncode == 0 else None
