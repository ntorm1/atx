"""The statistics tie fixture (P9 lane T1; contract K-P9-5, gate G-P7): today's Python statistics of record on a
committed synthetic panel, stored so the engine `eval` verb (lane B2, wave 2) can be tied to them before the Python is
deleted (plan DEC-6: the tie lands first).

  python generate_eval_tie.py --write    regenerate every file below from SERIES_SEED (a deliberate fixture change)
  python generate_eval_tie.py --check    recompute the values from the committed inputs and compare (exit 0 = tied)

Files (all pinned by SHA-256 in expected.json; .gitattributes keeps their bytes):
  series.f64            T x K date-major little-endian float64 daily returns: T = 500 sessions (not a multiple of 16
                        or 21, so the CSCV tail drop and the last CBB block's truncation are exercised), K = 12 trial
                        series in 3 correlated groups of 4 (so ONC has clusters to find); trial_00 is the book,
                        trial_01 the paired reference (same group)
  ledger_records.json   12 synthetic construction lines (one per trial; s2_net_sr = the trial's annualized Sharpe by
                        nav_summ.sharpe), window_id "eval-tie-v1": the input of the house DSR (house_v1: N = the
                        construction count by backtest_integrity.ledger_n, V = dsr_variance's window cell variance)
  cbb_starts.u16        draws x ceil(T / 21) little-endian uint16: the circular block bootstrap block starts that
                        nav_summ.paired_stats draws (numpy default_rng(20260929).integers(0, T, (4999, 24))); with them
                        the paired dSR's resamples need no port of numpy's PCG64 (P9 ruling P11)
  expected.json         the values (below), the ONC draw trace (every k-means++ pick of
                        backtest_integrity.onc in call order), the parameters and a tie rule per value

Values, each from today's Python called as a library (nothing here re-implements a statistic):
  moments / net_moments   backtest_integrity.moments, nav_summ.net_moments of the book
  psr                     backtest_integrity.psr_report(book, SR* in {0, .5} annual, alpha .05): PSR and MinTRL
  norm_ppf                backtest_integrity.norm_ppf at every quantile the DSR rows use
  dsr_cross               nav_summ.dsr_rows(net_moments of the 12 trials, N = 10): cross-cell variance DSR per trial
  dsr_lo                  backtest_integrity.lo_null_dsr(book, N = 10): single-cell Lo (2002) variance
  dsr_house_v1            dsr_total.ledger_rows(book, ledger_records, in_ledger, "eval-tie-v1"): its "v8" row is
                          house_v1 (N = construction count, V = window cell variance), "tot" the total-count row
  pbo                     backtest_integrity.cscv_pbo(series, 16 blocks, exhaustive): everything but the 12,870 raw
                          logits (a logit is a function of the OOS rank alone, so the rank histogram carries them)
  pbo_margins             the smallest IS gap between the winner and the runner-up and the smallest OOS gap between
                          the winner and another trial, over every split (diagnostic: counts tie at 0 only while these
                          margins stay far above the engine's rounding)
  onc / dsr_onc           backtest_integrity.effective_trials(series, n_init 10, seed 20260927) and effective_n_dsr
  onc_replay              backtest_integrity.onc on the same correlation fed the recorded picks: clusters and
                          silhouettes (members in trial order)
  paired                  nav_summ.paired_stats(book, reference, draws 4999, block 21, seed 20260929, one-sided):
                          dSR, rho, Memmel SE, t, CBB percentile CI, Ledoit-Wolf studentized CI and p-values
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import platform
import sys

import numpy as np

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[3]
TOOLS = REPO / "atx-impl" / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))
import backtest_integrity as BI  # noqa: E402
import dsr_total as DT  # noqa: E402
import nav_summ as NS  # noqa: E402

SCHEMA = "atx.eval-tie/v1"
GENERATOR = "atx-engine/tests/fixtures/eval_tie/generate_eval_tie.py"
SERIES, LEDGER, STARTS, EXPECTED = "series.f64", "ledger_records.json", "cbb_starts.u16", "expected.json"
SERIES_SEED = 20261003
T, K, GROUPS = 500, 12, 3
NAMES = tuple(f"trial_{k:02d}" for k in range(K))
BOOK, REFERENCE = 0, 1
WINDOW_ID = "eval-tie-v1"
SR_STARS = (0.0, 0.5)
ALPHA = 0.05
DSR_N = NS.DEFAULT_DSR_N                     # 10 (R6')
PBO_BLOCKS = BI.PBO_BLOCKS                   # 16
ONC_N_INIT, ONC_SEED = 10, BI.DEFAULT_SEED   # effective_trials' defaults (seed 20260927)
DRAWS, BLOCK, PAIRED_SEED = NS.V8_DRAWS, NS.DEFAULT_BLOCK, NS.V8_SEED   # 4999, 21, 20260929 (v8-prereg item 4)
CBB_BLOCKS = -(-T // BLOCK)                  # 24

# Tie rules for the engine side (lane B2): tolerance 0 where the reduction order can match, else 1e-12, with the reason.
TIE_RULES = {
    "moments": [1e-12, "numpy mean / std use pairwise summation; an engine sequential sum differs in the last bits"],
    "net_moments": [1e-12, "as moments"],
    "psr": [1e-12, "moments' summation order, and norm_cdf via erfc in both; MinTRL adds norm_ppf (below)"],
    "norm_ppf": [1e-12, "Python bisects norm_cdf to 1e-15 relative; the engine uses Acklam plus one Halley step "
                        "(eval/stats_ext.hpp): equal to rounding, not to the bit"],
    "dsr_cross": [1e-12, "moments, the ddof-1 variance of the SRs and norm_ppf"],
    "dsr_lo": [1e-12, "moments and norm_ppf"],
    "dsr_house_v1": [1e-12, "N and the cell counts are integers (tie at 0); V divides s2_net_sr by sqrt(252) and takes "
                            "a ddof-1 variance (numpy), SR0 uses norm_ppf"],
    "pbo": [0, "pbo, splits, winner_counts and the OOS rank histogram are integer counts: 0 while pbo_margins stay far "
               "above rounding; logit_mean, quantiles, means and the slope 1e-12 (matrix products on block sums)"],
    "onc": [0, "given the recorded picks, n_eff and the clusters are exact; variance_sr, cluster SRs and the "
               "correlation mean 1e-12 (numpy corrcoef and moments)"],
    "onc_replay": [0, "clusters exact given the picks; silhouettes 1e-12 (pairwise distances)"],
    "dsr_onc": [1e-12, "variance_sr and norm_ppf"],
    "paired": [1e-12, "Memmel SE, rho and the SRs from numpy moments; with the committed CBB starts the resamples are "
                      "exact, the bootstrap SRs and LW sums 1e-12; the p-values and valid-draw counts are integer ratios "
                      "(0 unless a studentized draw sits within rounding of its threshold)"],
}


# ------------------------------------------------------------------ bytes
def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def plain(x):
    """JSON-ready copy: numpy scalars and arrays to Python numbers and lists, tuples to lists, dict keys to str."""
    if isinstance(x, dict):
        return {str(k): plain(v) for k, v in x.items()}
    if isinstance(x, (list, tuple)):
        return [plain(v) for v in x]
    if isinstance(x, np.ndarray):
        return [plain(v) for v in x.tolist()]
    if isinstance(x, (bool, np.bool_)):
        return bool(x)
    if isinstance(x, np.integer):
        return int(x)
    if isinstance(x, (float, np.floating)):
        v = float(x)
        if not math.isfinite(v):
            raise ValueError(f"eval_tie: a non-finite value {v!r} cannot be stored")
        return v
    return x


def encode(doc) -> bytes:
    return (json.dumps(doc, indent=1, sort_keys=True, allow_nan=False) + "\n").encode("utf-8")


# ------------------------------------------------------------------ inputs
def make_series(seed: int = SERIES_SEED) -> np.ndarray:
    """T x K daily returns: 3 groups of 4 trials on a shared group factor, Student-t(5) idiosyncratic noise (fat
    tails, so skewness and kurtosis matter), drifts rising with the trial index (distinct SRs)."""
    rng = np.random.Generator(np.random.PCG64(seed))
    factors = 0.008 * rng.standard_normal((T, GROUPS))
    noise = 0.006 * math.sqrt(3.0 / 5.0) * rng.standard_t(5, size=(T, K))
    loads = 0.8 + 0.4 * rng.random(K)
    drift = 0.0002 + 0.00004 * np.arange(K)
    group = np.arange(K) // (K // GROUPS)
    return drift[None, :] + loads[None, :] * factors[:, group] + noise


def make_records(series: np.ndarray) -> list[dict]:
    """One construction line per trial (the fields backtest_integrity's N and V rules read)."""
    return [{"kind": "construction", "trial_id": f"{k + 1:016x}", "cell": NAMES[k], "window_id": WINDOW_ID,
             "count": 1, "s2_net_sr": float(NS.sharpe(series[:, k]))} for k in range(K)]


def make_starts() -> np.ndarray:
    """The CBB block starts paired_stats draws first from its generator (cbb_indices: rng.integers(0, t, ...))."""
    return np.random.default_rng(PAIRED_SEED).integers(0, T, size=(DRAWS, CBB_BLOCKS))


def read_series(directory: Path = HERE) -> np.ndarray:
    return np.frombuffer((directory / SERIES).read_bytes(), dtype="<f8").reshape(T, K).astype(np.float64)


def read_starts(directory: Path = HERE) -> np.ndarray:
    return np.frombuffer((directory / STARTS).read_bytes(), dtype="<u2").reshape(DRAWS, CBB_BLOCKS).astype(np.int64)


def read_records(directory: Path = HERE) -> list[dict]:
    return json.loads((directory / LEDGER).read_bytes())["records"]


def cbb_from_starts(starts: np.ndarray) -> np.ndarray:
    """The (draws, T) resample indices that nav_summ.cbb_indices builds from these starts."""
    idx = (starts[:, :, None] + np.arange(BLOCK)[None, None, :]).reshape(DRAWS, CBB_BLOCKS * BLOCK)[:, :T]
    return idx % T


# ------------------------------------------------------------------ ONC draw trace
class TraceRng:
    """Delegates backtest_integrity.kmeans' two draws to a numpy Generator and records each pick in call order."""

    def __init__(self, rng: np.random.Generator):
        self.rng, self.picks = rng, []

    def integers(self, n):
        v = int(self.rng.integers(n))
        self.picks.append(["integers", int(n), v])
        return v

    def choice(self, n, p=None):
        v = int(self.rng.choice(n, p=p))
        self.picks.append(["choice", int(n), v])
        return v


class ReplayRng:
    """Feeds recorded picks back in the same order; refuses a call that differs from the record."""

    def __init__(self, picks: list):
        self.picks, self.used = list(picks), 0

    def _next(self, kind: str, n: int) -> int:
        if self.used >= len(self.picks):
            raise AssertionError("onc replay: more draws than recorded")
        want_kind, want_n, value = self.picks[self.used]
        if (want_kind, want_n) != (kind, int(n)):
            raise AssertionError(f"onc replay: call {self.used} is {kind}({n}), the record has {want_kind}({want_n})")
        self.used += 1
        return int(value)

    def integers(self, n):
        return self._next("integers", n)

    def choice(self, n, p=None):
        return self._next("choice", n)


def onc_correlation(series: np.ndarray) -> np.ndarray:
    """The correlation effective_trials hands to onc (its own three lines, repeated so the trace sees the same input)."""
    with np.errstate(invalid="ignore", divide="ignore"):
        corr = np.corrcoef(series.T)
    corr = np.nan_to_num(np.atleast_2d(corr), nan=0.0)
    np.fill_diagonal(corr, 1.0)
    return corr


def onc_run(series: np.ndarray, rng) -> dict:
    res = BI.onc(onc_correlation(series), list(range(K)), n_init=ONC_N_INIT, _rng=rng)
    return {"clusters": [[NAMES[m] for m in c] for c in res["clusters"]],
            "silhouettes": [float(res["silhouettes"][m]) for m in range(K)]}


# ------------------------------------------------------------------ values
def pbo_margins(series: np.ndarray) -> dict:
    """Smallest IS winner margin and smallest OOS distance to the winner over every split, from
    backtest_integrity's own CSCV helpers (split_matrix, _subset_sr) on cscv_pbo's centred block sums."""
    width = T // PBO_BLOCKS
    x = series[:width * PBO_BLOCKS]
    offset = x.mean(axis=0)
    xc = (x - offset).reshape(PBO_BLOCKS, width, K)
    block_sum, block_sq = xc.sum(axis=1), (xc * xc).sum(axis=1)
    masks = BI.split_matrix(PBO_BLOCKS)
    count = (PBO_BLOCKS // 2) * width
    is_sr = BI._subset_sr(block_sum, block_sq, masks, count, offset)
    oos_sr = BI._subset_sr(block_sum, block_sq, ~masks, count, offset)
    top2 = np.sort(is_sr, axis=1)[:, -2:]
    winner = np.argmax(is_sr, axis=1)
    rows = np.arange(masks.shape[0])
    gap = np.abs(oos_sr - oos_sr[rows, winner][:, None])
    gap[rows, winner] = np.inf
    return {"is_winner_margin": float((top2[:, 1] - top2[:, 0]).min()), "oos_winner_distance": float(gap.min())}


def norm_ppf_block(n_values: dict) -> dict:
    out = {"z_one_minus_alpha": BI.norm_ppf(1.0 - ALPHA)}
    for label, n in n_values.items():
        out[f"{label}_n"] = int(n)
        out[f"{label}_one_minus_1_over_n"] = BI.norm_ppf(1.0 - 1.0 / n)
        out[f"{label}_one_minus_1_over_ne"] = BI.norm_ppf(1.0 - 1.0 / (n * math.e))
    return out


def compute_values(series: np.ndarray, records: list[dict], onc_picks: list | None = None) -> dict:
    """Every value of the fixture from today's Python; with ``onc_picks`` the ONC replay uses them."""
    book, ref = series[:, BOOK], series[:, REFERENCE]
    m_book = BI.moments(book)
    house = DT.ledger_rows(m_book, records, True, WINDOW_ID)
    pbo = BI.cscv_pbo(series, PBO_BLOCKS)
    onc = BI.effective_trials(series, list(NAMES), ONC_N_INIT, ONC_SEED)
    replay = onc_run(series, ReplayRng(onc_picks) if onc_picks is not None else
                     TraceRng(np.random.default_rng(ONC_SEED)))
    values = {
        "moments": m_book,
        "net_moments": NS.net_moments(book),
        "psr": BI.psr_report(book, SR_STARS, ALPHA),
        "norm_ppf": norm_ppf_block({"dsr": DSR_N, "house": house["v8"]["n"], "onc": onc["n_eff"]}),
        "dsr_cross": NS.dsr_rows([NS.net_moments(series[:, k]) for k in range(K)], DSR_N),
        "dsr_lo": BI.lo_null_dsr(m_book, DSR_N),
        "dsr_house_v1": house,
        "pbo": {k: v for k, v in pbo.items() if k != "logits"},
        "pbo_margins": pbo_margins(series),
        "onc": onc,
        "onc_replay": replay,
        "dsr_onc": BI.effective_n_dsr(m_book, onc),
        "paired": NS.paired_stats(book, ref, DRAWS, BLOCK, PAIRED_SEED, one_sided=True),
    }
    return plain(values)


def trace_onc(series: np.ndarray) -> tuple[list, dict]:
    rng = TraceRng(np.random.default_rng(ONC_SEED))
    result = onc_run(series, rng)
    return rng.picks, result


# ------------------------------------------------------------------ write / check
def file_entry(directory: Path, name: str, **extra) -> dict:
    return dict(extra, file=name, sha256=sha256_bytes((directory / name).read_bytes()))


def host_identity() -> dict:
    """What decides the last bit of numpy's BLAS-backed products (corrcoef, @): the numpy build, its BLAS and the
    CPU (OpenBLAS picks its kernel per CPU, DYNAMIC_ARCH)."""
    try:
        blas = (np.show_config(mode="dicts") or {}).get("Build Dependencies", {}).get("blas", {}) or {}
    except TypeError:                     # numpy < 1.25: show_config has no mode
        blas = {}
    return {"numpy": np.__version__, "blas": f"{blas.get('name')} {blas.get('version')}",
            "machine": platform.machine(), "processor": platform.processor()}


def document(directory: Path, series: np.ndarray, records: list[dict], picks: list) -> dict:
    values = compute_values(series, records, picks)
    return {
        "schema": SCHEMA, "generator": GENERATOR,
        "python": sys.version.split()[0], "numpy": np.__version__, "host": host_identity(),
        "inputs": {
            "series": file_entry(directory, SERIES, dtype="<f8", layout="date-major", shape=[T, K],
                                 names=list(NAMES), book=NAMES[BOOK], reference=NAMES[REFERENCE], seed=SERIES_SEED,
                                 recipe=make_series.__doc__.strip()),
            "ledger": file_entry(directory, LEDGER, window_id=WINDOW_ID, in_ledger=True),
            "cbb_starts": file_entry(directory, STARTS, dtype="<u2", shape=[DRAWS, CBB_BLOCKS],
                                     rule=f"numpy default_rng({PAIRED_SEED}).integers(0, {T}, size=({DRAWS}, "
                                          f"{CBB_BLOCKS})); index (start + j) mod T, j < {BLOCK}, first {T} kept"),
            "onc_picks": {"rule": "backtest_integrity.kmeans draws in call order: [integers, n, pick] seeds the first "
                                  "center, [choice, n, pick] each next k-means++ center (p = d^2 / sum d^2); onc runs "
                                  f"n_init {ONC_N_INIT} x k = 2..N-1, then its recursion, on one generator "
                                  f"(default_rng({ONC_SEED}))", "picks": picks},
        },
        "parameters": {"psr_sr_stars_annual": list(SR_STARS), "alpha": ALPHA, "dsr_n": DSR_N,
                       "pbo_blocks": PBO_BLOCKS, "pbo_exhaustive": True, "onc_n_init": ONC_N_INIT,
                       "onc_seed": ONC_SEED, "annualization": NS.ANNUAL,
                       "paired": {"draws": DRAWS, "block": BLOCK, "seed": PAIRED_SEED, "one_sided": True}},
        "house_v1": "values.dsr_house_v1.v8 (N = construction count by ledger_n, V = dsr_variance of the window's "
                    "construction lines; K-P9-5 dsr.house_v1); values.dsr_house_v1.tot is the total-count row",
        "tie_rules": {k: {"tolerance": v[0], "reason": v[1]} for k, v in TIE_RULES.items()},
        "values": values,
    }


def write(directory: Path = HERE) -> dict:
    series = make_series()
    (directory / SERIES).write_bytes(series.astype("<f8").tobytes())
    series = read_series(directory)
    records = make_records(series)
    (directory / LEDGER).write_bytes(encode({"schema": "atx.eval-tie-ledger/v1", "window_id": WINDOW_ID,
                                             "note": "synthetic construction lines for the house DSR; not a ledger "
                                                     "of record (no hash chain)", "records": records}))
    starts = make_starts()
    if int(starts.max()) >= 1 << 16:
        raise ValueError("eval_tie: a CBB start does not fit uint16")
    (directory / STARTS).write_bytes(starts.astype("<u2").tobytes())
    picks, traced = trace_onc(series)
    doc = document(directory, series, read_records(directory), picks)
    if doc["values"]["onc_replay"] != traced or doc["values"]["onc_replay"]["clusters"] != doc["values"]["onc"][
            "clusters"]:
        raise ValueError("eval_tie: the traced ONC run differs from effective_trials")
    (directory / EXPECTED).write_bytes(encode(doc))
    return doc


def check(directory: Path = HERE) -> list[str]:
    """Problems of the committed fixture against today's Python (empty when tied)."""
    want = json.loads((directory / EXPECTED).read_bytes())
    problems = []
    for key in ("series", "ledger", "cbb_starts"):
        entry = want["inputs"][key]
        if sha256_bytes((directory / entry["file"]).read_bytes()) != entry["sha256"]:
            problems.append(f"{entry['file']}: SHA-256 differs from expected.json")
    series, records = read_series(directory), read_records(directory)
    got = compute_values(series, records, want["inputs"]["onc_picks"]["picks"])
    problems += compare(want["values"], got, exact=want.get("host") == host_identity())
    if not np.array_equal(read_starts(directory), make_starts()):
        problems.append("cbb_starts.u16 is not default_rng(20260929)'s first draw")
    return problems


def compare(want, got, exact: bool, path: str = "values") -> list[str]:
    """Deep comparison: equal structure and integers; floats equal (``exact``: the recorded host identity, i.e. numpy
    build, BLAS and CPU) or within 1e-12 relative (another numpy, BLAS or CPU kernel may sum in another order)."""
    if isinstance(want, dict) and isinstance(got, dict):
        if set(want) != set(got):
            return [f"{path}: keys {sorted(set(want) ^ set(got))} differ"]
        return [p for k in want for p in compare(want[k], got[k], exact, f"{path}.{k}")]
    if isinstance(want, list) and isinstance(got, list):
        if len(want) != len(got):
            return [f"{path}: length {len(got)} != {len(want)}"]
        return [p for k, (a, b) in enumerate(zip(want, got)) for p in compare(a, b, exact, f"{path}[{k}]")]
    if isinstance(want, float) and isinstance(got, float):
        if want == got or (not exact and math.isclose(want, got, rel_tol=1e-12, abs_tol=1e-300)):
            return []
        return [f"{path}: {got!r} != {want!r}"]
    return [] if want == got and type(want) is type(got) else [f"{path}: {got!r} != {want!r}"]


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="the statistics tie fixture (P9 T1, K-P9-5)")
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true", help="regenerate every fixture file")
    mode.add_argument("--check", action="store_true", help="verify the committed fixture against today's Python")
    a = ap.parse_args(argv)
    if a.write:
        doc = write()
        print(json.dumps({k: doc["inputs"][k]["sha256"] for k in ("series", "ledger", "cbb_starts")}, indent=1))
        return 0
    problems = check()
    for p in problems:
        print(f"eval_tie: {p}", file=sys.stderr)
    print("eval_tie: tied" if not problems else f"eval_tie: {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
