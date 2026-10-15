"""Compact summary of NAV replay output dirs (root helper): one line per scenario, plus (v5, T31) construction
stats, the netting ratio and the paired dSR(net) against a reference cell.

Usage:
  nav_summ.py NAV_DIR [NAV_DIR ...] [--weights W ...] [--reference REF_NAV_DIR] [--scenario NAME]
              [--draws 2000] [--block 21] [--seed 20260927] [--dsr-n 10] [--json OUT.json]

Per NAV dir, for the primary scenario (or --scenario), from its daily CSV (columns as strategy_nav_replay.cpp
writes them; rows and definitions mirror its summary):
  construction   mean gross / net / |net| leverage (the previous session's closing book over return rows, as the
                 summary's mean_gross_leverage), held_share (summary construction.v5.mean_held_share, T30; "na"
                 without it) and mean held names; tau_t = one_way_turnover_gmv over executed sessions with positive
                 pre-trade gross, the deployment session (first executed session with a fill) excluded: mean, p95
                 (numpy linear); cost per unit GMV turnover = sum over the same tau_t sessions s of
                 trade_cost_dollars_s / pretrade_nav_s (exactly the trade_cost_return MARK books into row s + 1, so
                 the deployment session's cost is excluded as it is from the denominator) / sum tau_t;
                 cost_bps_traded = 1e4 * sum trade_cost_dollars / sum traded_dollars (executed sessions).
  leverage gate  mean_gross_leverage_all_rows / mean_net_leverage_all_rows = mean of gross_leverage / net_leverage
                 over EVERY row of the daily CSV: the ruled D1/R6' definition the R6' mechanics gate uses (the
                 return-row means above are kept for continuity and equal the summary's mean_gross_leverage).
  post-ramp      (v6 prereg C4) mean_gross_leverage_post_ramp / mean_net_leverage_post_ramp = the same means over
                 the CSV rows after the first 63 (post_ramp_rows of them; null when there are none): the steady-state
                 book that L is calibrated on. The gate itself stays the all-rows mean.
  netting ratio  NR = tau mean / weighted_standalone_turnover of the weights file whose SHA-256 equals the NAV
                 summary's composition_weights_sha256 (--weights may repeat; a single unmatched file is used
                 with a warning). NR = tau_book / sum_k w_k tau_k (lane contract), reported on TRAIN.
  paired dSR     --reference: net returns of both cells on their common return sessions (session_ns);
                 dSR = SR(cell) - SR(ref), SR = mean / sd(ddof 1) * sqrt(252) (the summary's net_sharpe);
                 rho = Pearson correlation of the daily nets; Memmel (2003) SE:
                 Var = [2 - 2 rho + (SR_a^2 + SR_b^2 - 2 SR_a SR_b rho^2) / 2] / T with per-session SRs, x sqrt(252);
                 circular block bootstrap (Politis-Romano 1992; --draws, --block, fixed --seed) percentile 95% CI of dSR;
                 Ledoit-Wolf (2008) studentized circular block bootstrap 95% CI and p-value (R6'): delta-method SE of
                 dSR (population moments) with a Bartlett HAC (lag = block - 1) on the sample and the block
                 estimator Psi* = sum_blocks S_j S_j' / T on each resample, symmetric |t*| quantile.
  DSR            (R6', plan-s4 4.E; Bailey & Lopez de Prado 2014) for every NAV dir passed:
                 DSR = Phi[(SR - SR0) sqrt(T - 1) / sqrt(1 - g3 SR + (g4 - 1) SR^2 / 4)], SR the per-session net Sharpe
                 (ddof 1), T the return rows, g3 skewness and g4 (non-excess) kurtosis of the daily nets (population
                 moments); SR0 = sqrt(V[SR_n]) [(1 - gamma) PhiInv(1 - 1/N) + gamma PhiInv(1 - 1/(N e))],
                 gamma = 0.5772156649, N = --dsr-n (default 10). V[SR_n] = sample variance (ddof 1) of the per-session
                 SRs of the NAV dirs on the command line; with a single dir, the Lo (2002) sampling variance
                 (1 + SR^2 / 2) / T of that dir (flagged in the output). Phi via math.erfc, PhiInv by bisection on it.
                 Warns on stderr (never refuses) when the number of dirs with a defined SR differs from N, and when
                 two listed dirs have identical daily net series (both silently change V[SR_n], hence SR0).
  provenance     --json: every row carries nav_summ_run = {argv, script, script_sha256 (of this file), git_head
                 (git rev-parse HEAD of this file's checkout; null without git), warnings}. The top level stays the
                 per-dir list so existing consumers and field-by-field diffs are unchanged.

Backtest integrity (platform v7 lane L2; statistics in backtest_integrity.py, pure numpy). Every option below only
ADDS lines / JSON keys; without them the output is byte-identical to the v6.1 nav_summ (the numbers above never move):
  --ledger PATH          append each listed dir to the atx.trial-ledger/v1 JSONL (R5.1) as one trial of --ledger-kind
                         (admission|composition|construction|universe|data; default construction; --ledger-count per
                         line, --ledger-note), with its pins, TRAIN window, daily CSV path + SHA-256 and S2 net SR.
                         Idempotent on (kind, daily series SHA-256): an identity re-run adds no trial; a series with a
                         session outside the research window's TRAIN (research_window.py) is refused.
  --ledger-n PATH        print the Appendix A block: trials by kind and window from that ledger (no dirs needed).
  --effective-n SOURCE   effective-N DSR beside the cell-count DSR and the Lo null: ONC clusters (Lopez de Prado &
                         Lewis 2019) of the trial series of SOURCE = "dirs" (the listed dirs) or a ledger PATH (its
                         series lines), aligned on their common sessions; N_eff = clusters, V[SR_k] across the
                         clusters' inverse-variance series (--onc-init restarts, --onc-seed).
  --pbo [DIR ...]        CSCV PBO (Bailey et al. 2017) over the given cells (default: the listed dirs; >= 4 cells):
                         --pbo-blocks (16) blocks on the common sessions, all C(16,8) = 12,870 splits unless
                         --pbo-max-splits K (> 0) asks for a seeded subsample (then flagged); --pbo-json OUT.
  --psr                  PSR and MinTRL (Bailey & Lopez de Prado 2012) of every listed dir against --psr-benchmarks
                         (annualized SR*, default 0,0.5) at --psr-alpha (.05).

Validation kit (platform v8 V-1). Every option below is opt-in; without them the output is byte-identical to v7. The
research window (TRAIN and the seal) is read from atx-engine/tools/research_window.py; any NAV daily CSV with a session
at or after the seal is refused before a statistic is formed.
  --protocol v8          the v8 pre-registration (item 4): bootstrap seed 20260929 and 4,999 resamples (block 21) unless
                         --seed / --draws are given; a year table per dir; ledger lines carry origin (--origin required
                         with --ledger), window_id and the hash chain; --ledger-n adds the v8 Appendix A block.
  --origin CLASS         prior | grid | mined (contract K5), recorded in every appended ledger line.
  --rerun-of ID --rerun-basis window|blind|returns
                         the appended line re-runs trial ID: window = a ledgered cell re-run on a longer window (no new
                         trial); blind / returns = the defect rule (v8-prereg item 7): a re-run decided without seeing
                         returns replaces the invalid cell, one decided because the returns looked wrong is a new trial.
  --ledger-defect REASON the appended line is an invalid cell: logged and excluded from N.
  protocol lines         (W0-3; written by `research_cycle.py ledger-protocol`, lane A) kind protocol, no cell, count 0:
                         every reader here skips them when it lists cells or counts N, and the hash chain covers them.
  --year-table           per dir and calendar year: return rows, compounded net return, net Sharpe, volatility, mean
                         daily GMV turnover (the tau_t sessions) and cost per traded dollar (bps).
  --dsr-ledger PATH      DSR per dir with N = the ledger's construction trial count (+1 when the dir is not in it) and
                         V[SR] from the construction cells ledgered on the current window (OD-4; v8-prereg item 3); the
                         legacy variance (lines without a window_id) is reported beside it and gates nothing.
  --bundle BASE FINAL    the cumulative paired test FINAL vs BASE (e.g. V8-F vs B0c): paired dSR(net) with Memmel SE,
                         CBB percentile CI and the studentized CBB (Ledoit-Wolf) two-sided and one-sided p, a per-year
                         dSR table and the verdict dSR > 0 and one-sided p < --bundle-alpha (.10, the freeze gate);
                         --bundle-json OUT writes it.

Expansion X (v8x-prereg.md section 4, precondition P5; Rulings PM7-1, PM7-2, PM7-7; opt-in, logic in dsr_total.py):
  --dsr-total LEDGER     total-count DSR of every listed dir. N_tot = every trial the defect rule counts in LEDGER (any
                         kind) + the mining campaigns' registry counts (+1 for a dir not in the ledger). V[SR] is the
                         ddof-1 variance from dsr_variance (construction cells on the current window). The v8-count
                         value (N = the construction trials, as --dsr-ledger) is printed beside it, and DSR_hand at
                         N = N_tot - M (P9 ruling PRE-15: the campaigns' registry counts M left out, the same V; JSON
                         deflated_total.tot_minus_m). A missing ledger is refused.
  --dsr-hand DIR         with --dsr-total; repeatable. DSR_hand of a ledgered book by the prefix rule (v8x PM7-2;
                         printed "prefix rule"): N_tot and V of the ledger prefix that ends at DIR's own construction
                         line. A dir that is missing or not ledgered is refused.

Era pools (platform v8 H-1; opt-in, without --pool nothing above moves):
  --pool DIR ... [--pool-ids ID,...] [--pool-reference DIR ...]
                         one pooled row of era NAV dirs in date order (era_pool.pool_rows), after the listed dirs:
                         return rows, deployment, tau sessions and the post-ramp rows are taken per era (each era
                         deploys afresh), the eras must share one scenario and one rule, and their windows pass
                         era_pool.check_windows (no overlap, date order, nothing sealed or straddling the TRAIN begin).
                         One era: the era's own row (pooled == single, apart from dir and the pool block). Several:
                         net and gross Sharpe recomputed over the pooled return rows (mean / sd ddof 1 x sqrt 252),
                         hac_t and held_share null, the per-era rows in row["pool"]["eras"]; the paired block against
                         --pool-reference (pooled) or --reference is skipped with a note below 3 common sessions.
                         With --ledger: one line per era (adds no trial) and one pooled line (window POOL, one trial);
                         a one-era pool is the era's own line (backtest_integrity.ledger_pool_records). A history
                         read (one era or a pool scored before TRAIN) adds no trial and counts as a history read in
                         the Appendix A block (Ruling E-41); its --dsr-ledger N has no + 1.
  --era-of TRIAL_ID      a one-era history pool (sessions before TRAIN) with --ledger: the trial_id of the TRAIN cell
                         it re-reads, required (review P-1). Its line carries era_of: an era line, adding no trial,
                         never read as a TRAIN cell; refused for a pool of two or more eras or one inside TRAIN.
  --pool-one-sided       (OD-3 T2; P9 ruling PRE-21) the pooled row's paired block is the engine verb's with --one-sided
                         (lw.p_one_sided, from the same resamples) plus the bundle's verdict dict (dSR > 0 and one-sided
                         p < --bundle-alpha); one more printed line. Needs --pool and a reference (--pool-reference or
                         --reference). The listed dirs' paired blocks are unchanged; without the flag nothing moves.
                         The verdict is labelled "bundle rule, not G-B8" (P9 ruling OD3P-T2V): G-B8's read is the
                         prereg's own, made by the PM from the printed p values; no second verdict rule is coded.

Engine statistics (platform P9 B2, contract K-P9-5):
  every statistic of record (net moments, paired dSR with Memmel SE / CBB / Ledoit-Wolf, the DSR rows, PSR and MinTRL,
  ONC effective N, CSCV PBO, the ledger DSR, DSR_tot and DSR_hand) is computed by the engine verb atx-research-eval on
  the same net series, written as session_ns,net_return CSVs to a private temp dir; the verb's JSON blocks carry the
  keys printed here. The Python copies were deleted after root's identity run (values agreed to 1e-12, the P12
  tolerance). The verb is $ATX_EQUITY_BIN/atx-research-eval.exe when ATX_EQUITY_BIN is set (refused when that bin
  lacks it: no fallback), else build-equity/bin of this checkout; --engine-stats EXE names another. It is opened on
  first use, so a run that computes no statistic (--ledger-n alone) needs none. Every nav_summ_run of a run that used
  it records engine_exe and engine_exe_sha256 (ruling B2-R3). --pbo-max-splits is refused (the engine CSCV is
  exhaustive). Ledgers are read, never written, by the verb.
"""
from __future__ import annotations

import argparse
import atexit
import csv
import hashlib
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

import backtest_integrity as BI
import dsr_total as DT

EP = BI.era_pool()  # task H-1: the era pooling rules (atx-engine/tools/era_pool.py; standard library)
ANNUAL = 252
DEFAULT_DRAWS, DEFAULT_BLOCK, DEFAULT_SEED = 2000, 21, 20260927
V8_DRAWS, V8_SEED = 4999, 20260929  # v8-prereg item 4 (block stays DEFAULT_BLOCK = 21)
PROTOCOLS = ("v7", "v8")
BUNDLE_ALPHA = 0.10  # v8-prereg item 9: the freeze gate's cumulative paired test, bootstrap p < .10
DEFAULT_DSR_N = 10  # R6': N = 10 construction cells
LEVERAGE_GATE_BASIS = ("all_rows: the R6' mechanics gate (ruled D1/R6') reads mean_gross_leverage_all_rows / "
                       "mean_net_leverage_all_rows; mean_gross_leverage / mean_net_leverage are return-row means "
                       "(previous close), kept for continuity")
RAMP_ROWS = 63  # v6 prereg C4: the theta-ramp rows dropped from the post-ramp (L calibration) means
ENGINE_EXE = Path(__file__).resolve().parents[2] / "build-equity" / "bin" / "atx-research-eval.exe"
_ENGINE: dict = {}   # the default verb of this process per resolved path (engine_of), opened on first use


def default_engine_exe() -> Path:
    """$ATX_EQUITY_BIN/atx-research-eval.exe when ATX_EQUITY_BIN is set (the canary's bin), else ENGINE_EXE. Ruling
    B2-R3: a bin named by ATX_EQUITY_BIN without the verb fails closed (EngineStats refuses it); there is no fallback to
    this checkout's build-equity/bin, whose Debug verb may be stale."""
    env = os.environ.get("ATX_EQUITY_BIN")
    return Path(env) / ENGINE_EXE.name if env else ENGINE_EXE


def engine_of(args) -> "EngineStats":
    """The engine verb a run's statistics come from: --engine-stats EXE, else the default executable (looked up on each
    call, so a changed ATX_EQUITY_BIN is honoured); opened on first use (review B2 S8: a run that computes no
    statistic, e.g. --ledger-n alone, needs no verb) and kept on ``args``."""
    eng = getattr(args, "engine", None)
    if eng is not None:
        return eng
    exe = getattr(args, "engine_stats", None)
    if exe:
        eng = EngineStats(exe)
    else:
        path = default_engine_exe().resolve()
        if path not in _ENGINE:
            _ENGINE[path] = EngineStats(str(path))
        eng = _ENGINE[path]
    try:
        args.engine = eng
    except AttributeError:
        pass
    return eng


class EngineStats:
    """--engine-stats (P9 B2, K-P9-5): the statistics of record from the engine verb ``atx-research-eval``.

    Each method returns the dict the Python statistic it stands for returns (the same keys; NaN where the Python holds
    NaN, the verb's null), read from the verb's JSON document. Nothing here computes a statistic: the series go to the
    verb as ``session_ns,net_return`` CSVs (Python repr, read back exactly) in a private temp dir removed at exit."""

    PAIRED_EXTRA = ("se", "p_one", "p_two", "reference")      # K-P9-5 mirrors of the lw block
    PAIRED_NAN = ("sr", "sr_reference", "dsr", "rho", "memmel_se")
    PBO_EXTRA = ("labels", "common_sessions_ns", "engine")

    def __init__(self, exe: str):
        self.exe = Path(exe).resolve()
        if not self.exe.is_file():
            raise SystemExit(f"nav_summ: no engine statistics executable {self.exe}: build atx-research-eval "
                             "(scripts/research-build.ps1) or name one with --engine-stats EXE")
        self.sha256 = hashlib.sha256(self.exe.read_bytes()).hexdigest()   # ruling B2-R3: the producer of record
        self.dir = Path(tempfile.mkdtemp(prefix="nav_summ_engine_"))
        atexit.register(shutil.rmtree, self.dir, True)
        self._csv: dict[int, tuple[dict, str]] = {}

    def csv(self, nets: dict) -> str:
        """The CSV of a net series (session_ns -> net, file order), written once per series object."""
        hit = self._csv.get(id(nets))
        if hit is not None and hit[0] is nets:
            return hit[1]
        path = self.dir / f"s{len(self._csv)}.csv"
        path.write_text("session_ns,net_return\n" + "".join(f"{int(s)},{float(v)!r}\n" for s, v in nets.items()),
                        encoding="ascii")
        self._csv[id(nets)] = (nets, str(path))
        return str(path)

    def run(self, cells: list[dict], *extra) -> dict:
        """The verb's document for these cells (labels c0, c1, ... in order) and options."""
        argv = [str(self.exe)]
        for k, nets in enumerate(cells):
            argv += ["--daily", f"c{k}={self.csv(nets)}"]
        argv += [str(a) for a in extra] + ["--json"]
        with subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) as proc:
            out, err = proc.communicate()      # Popen, not run: a test may stand in for git through subprocess.run
        if proc.returncode != 0:
            raise SystemExit(f"nav_summ: engine statistics: {err.strip() or f'exit {proc.returncode}'}")
        return json.loads(out)

    def moments_of(self, nets: dict) -> dict:
        return self.run([nets])["cells"][0]["net_moments"]

    def paired_of(self, nets: dict, ref_nets: dict, args, one_sided: bool = False) -> dict | None:
        """nav_summ's paired block of a cell against its reference (None below 3 common sessions); ``args`` carries
        draws, block and seed."""
        doc = self.run([nets], "--paired", self.csv(ref_nets), "--draws", args.draws, "--block", args.block,
                       "--seed", args.seed, *(["--one-sided"] if one_sided else []))
        p = doc["cells"][0]["paired"]
        if p is None:
            return None
        for key in self.PAIRED_EXTRA:
            p.pop(key, None)
        omr = p.pop("one_minus_rho", None)
        # ruling B2-R1 (c): a degenerate pair (1 - rho < 1e-6) keeps its refused SE / t / p as null (printed "na")
        # and says so with its 1 - rho; any other pair carries the Python's keys only
        degenerate = bool(p.pop("degenerate", False))
        nan = float("nan")
        for key in self.PAIRED_NAN:
            if not (degenerate and key == "memmel_se"):
                p[key] = nan if p[key] is None else p[key]
        p["cbb_ci95"] = [nan if v is None else v for v in p["cbb_ci95"]]
        for key in ("dsr_population", "se"):
            if not (degenerate and key == "se"):
                p["lw"][key] = nan if p["lw"][key] is None else p["lw"][key]
        if degenerate:
            p.update(degenerate=True, one_minus_rho=omr)
        return p

    def deflated_rows(self, cells: list[dict], n: int) -> list[dict]:
        return [c["dsr"]["cross"] for c in self.run(cells, "--dsr-n", n)["cells"]]

    def psr_of(self, nets: dict, benchmarks: list[float], alpha: float) -> dict:
        doc = self.run([nets], "--psr-benchmarks", ",".join(repr(float(b)) for b in benchmarks),
                       "--psr-alpha", repr(float(alpha)))
        return doc["cells"][0]["psr"]

    def effective(self, cells: list[dict], names: list, series: list[dict], args) -> tuple[dict, list[dict]]:
        """(effective_trials of the series with their names, per cell (effective-N DSR row, Lo null row))."""
        extra = ["--onc", "--onc-init", args.onc_init, "--onc-seed", args.onc_seed, "--dsr-n", args.dsr_n]
        for k, nets in enumerate(series):
            extra += ["--onc-series", f"s{k}={self.csv(nets)}"]
        doc = self.run(cells, *extra)
        eff = doc["onc"]
        eff["clusters"] = [[names[int(label[1:])] for label in cl] for cl in eff["clusters"]]
        return eff, [(c["dsr"]["onc"], c["dsr"]["lo"]) for c in doc["cells"]]

    def cscv(self, series: list[dict], blocks: int) -> tuple[dict, list[int]]:
        """(the CSCV PBO block as cscv_pbo returns it, the first and last common session)."""
        extra = ["--pbo", "--pbo-blocks", blocks]
        for k, nets in enumerate(series):
            extra += ["--pbo-series", f"s{k}={self.csv(nets)}"]
        res = self.run(series[:1], *extra)["pbo"]
        common = [int(s) for s in res["common_sessions_ns"]]
        for key in self.PBO_EXTRA:
            res.pop(key, None)
        res["logits"] = np.array(res["logits"], dtype=np.float64)
        return res, common

    def ledger_rows_of(self, cells: list[dict], ledger: str, window: str, trial_ids: list[str],
                       history: list[bool], missing: str = "empty", hands: list[tuple[str, dict]] = ()) -> dict:
        """The verb's document with the ledger blocks (house_v1, tot per cell; the hand rows)."""
        extra = ["--ledger", ledger, "--ledger-missing", missing, "--window-id", window]
        for k, (tid, hist) in enumerate(zip(trial_ids, history)):
            extra += ["--trial", f"c{k}={tid}"] + (["--history", f"c{k}"] if hist else [])
        for tid, nets in hands:
            extra += ["--hand", f"{tid}={self.csv(nets)}"]
        return self.run(cells, *extra)


def fmt(v, spec: str) -> str:
    return format(v, spec) if isinstance(v, (int, float)) else "na"


def load_summary(d: Path) -> dict:
    return json.loads((Path(d) / "summary.json").read_text(encoding="utf-8"))


def scenario_of(summary: dict, name: str | None = None) -> dict:
    wanted = name or summary.get("primary_scenario")
    for v in summary["scenarios"]:
        if v["scenario"] == wanted:
            return v
    raise SystemExit(f"nav_summ: scenario {wanted!r} not in summary")


def print_scenarios(summary: dict) -> None:
    """The pre-T31 one-line-per-scenario view (same layout; a null statistic prints "na")."""
    print(f"rule={summary.get('rule')} status={summary.get('status')} primary={summary.get('primary_scenario')}")
    for v in summary["scenarios"]:
        yrs = v.get("calendar_year_returns") or []
        ys = " ".join(
            f"{y.get('year')}:{(y.get('net_compounded_return') or 0):+.3f}" if isinstance(y, dict) else str(y)
            for y in yrs
        )
        t = v.get("daily_turnover_gmv") or {}
        c = v.get("costs", {})
        fin = v.get("financing") or {}
        print(
            f"{v['scenario'][:34]:34s} net {fmt(v.get('net_sharpe'), '+.3f')} gross {fmt(v.get('gross_sharpe'), '+.3f')} "
            f"hac {fmt(v.get('hac_t'), '+.2f')} mu {fmt(v.get('ann_mean'), '+.4f')} vol {fmt(v.get('ann_vol'), '.4f')} "
            f"mdd {fmt(v.get('max_drawdown'), '.3f')} "
            f"tau_gmv mean {t.get('mean', float('nan')):.4f} p95 {t.get('p95', float('nan')):.4f} "
            f"tc {c.get('summed_trade_cost_return', 0):.4f} brw {c.get('summed_borrow_return', 0):.4f} "
            f"meets_d {v.get('meets_daily_turnover_mean')}/{v.get('meets_daily_turnover_p95')} | {ys}"
        )
        if fin:
            print("   financing:", json.dumps(fin)[:400])
        nz = v.get("neutralize") or v.get("neutralization")
        if nz:
            print("   neutralize:", json.dumps(nz)[:300])


# ------------------------------------------------------------------ daily CSV
def load_daily(d: Path, scenario: str) -> dict:
    """Columns of daily_<scenario>.csv: numeric columns as float arrays ("nan" -> NaN), others as lists."""
    return load_daily_csv(Path(d) / f"daily_{scenario}.csv")


def load_daily_csv(path: Path, *, allow_sealed: bool = False) -> dict:
    """``load_daily`` of an explicit CSV path (the trial ledger records the path).

    The session column is checked against the research seal before any other column is parsed: a session at or after
    the seal refuses the file (SystemExit). Only holdout_gate.py, the owner-ruled reader of the hidden blocks, passes
    ``allow_sealed``."""
    with Path(path).open(newline="", encoding="utf-8") as stream:
        rows = list(csv.reader(stream))
    header, body = rows[0], rows[1:]
    if "session_ns" in header and not allow_sealed:
        k = header.index("session_ns")
        try:
            BI.refuse_sealed([int(float(r[k])) for r in body], f"nav_summ: {path}")
        except ValueError as exc:
            raise SystemExit(str(exc)) from exc
    out: dict = {}
    for n, name in enumerate(header):
        col = [r[n] for r in body]
        try:
            out[name] = np.array([float(x) for x in col], dtype=np.float64)
        except ValueError:
            out[name] = col
    return out


def segments(daily: dict) -> list[tuple[int, int]]:
    """[(first row, end row)] of each era segment of a pooled daily dict (``era_pool.pool_rows``: key SEGMENTS); a
    single CSV is one segment. Every per-CSV rule below (first row, deployment, ramp) applies per segment."""
    rows = len(next(iter(daily.values())))       # any column (SEGMENTS is added after the columns)
    starts = list(daily.get(EP.SEGMENTS) or [0])
    return list(zip(starts, starts[1:] + [rows]))


def return_mask(daily: dict) -> np.ndarray:
    """Rows that enter the summary's return statistics: return_observation and not the first row (of each era)."""
    mask = daily["return_observation"] == 1
    mask[:1] = False
    for first, _ in segments(daily)[1:]:  # an era pool: each era's first row, as a single CSV's
        mask[first] = False
    return mask


def deployment_row(daily: dict) -> int | None:
    fills = np.flatnonzero((daily["executed"] == 1) & (daily["traded_dollars"] > 0))
    return int(fills[0]) if fills.size else None


def deployment_rows(daily: dict) -> list[int]:
    """The deployment row of each era segment (one per CSV; each era deploys afresh)."""
    out = []
    for first, end in segments(daily):
        dep = deployment_row({k: daily[k][first:end] for k in ("executed", "traded_dollars")})
        if dep is not None:
            out.append(first + dep)
    return out


def turnover_rows(daily: dict) -> np.ndarray:
    """The tau_t sessions: executed with positive pre-trade gross, the deployment session excluded (bool mask)."""
    keep = (daily["executed"] == 1) & (daily["pretrade_gross_dollars"] > 0)
    for dep in deployment_rows(daily):
        keep[dep] = False
    return keep


def post_ramp_rows(daily: dict):
    """Index of the rows after the first RAMP_ROWS: the slice of a single CSV, or a mask over each era segment."""
    if EP.SEGMENTS not in daily:
        return slice(RAMP_ROWS, None)
    mask = np.zeros(int(daily["gross_leverage"].size), dtype=bool)
    for first, end in segments(daily):
        mask[first + RAMP_ROWS:end] = True
    return mask


def turnover_gmv(daily: dict) -> np.ndarray:
    """Daily one-way GMV turnover over executed sessions with positive pre-trade gross, deployment excluded."""
    return daily["one_way_turnover_gmv"][turnover_rows(daily)]


def construction_stats(daily: dict, scenario: dict) -> dict:
    ret = return_mask(daily)
    prev = np.flatnonzero(ret) - 1  # exposure that earned a return row: the previous session's closing book
    keep = turnover_rows(daily)
    tau = daily["one_way_turnover_gmv"][keep]
    executed = daily["executed"] == 1
    v5 = (scenario.get("construction") or {}).get("v5") or {}
    traded = float(daily["traded_dollars"][executed].sum())
    tau_sum = float(tau.sum())
    # Session s's fill cost in NAV-return units, over the tau_t sessions only: trade_cost_dollars_s / pretrade_nav_s
    # is bit-for-bit the trade_cost_return MARK books into row s + 1 (the fill costs of t-1 land in r_t), so the
    # numerator excludes the deployment session exactly as the denominator does (T31 Minor 3, T41 M2).
    cost_tau = float((daily["trade_cost_dollars"][keep] / daily["pretrade_nav"][keep]).sum())
    rows = int(daily["gross_leverage"].size)
    any_prev = prev.size > 0
    ramp = post_ramp_rows(daily)
    post_rows = int(daily["gross_leverage"][ramp].size)
    post_ramp = post_rows > 0
    return {
        "mean_gross_leverage_post_ramp": float(daily["gross_leverage"][ramp].mean()) if post_ramp else None,
        "mean_net_leverage_post_ramp": float(daily["net_leverage"][ramp].mean()) if post_ramp else None,
        "post_ramp_rows": post_rows,
        "mean_gross_leverage": float(daily["gross_leverage"][prev].mean()) if any_prev else None,
        "mean_net_leverage": float(daily["net_leverage"][prev].mean()) if any_prev else None,
        "mean_abs_net_leverage": float(np.abs(daily["net_leverage"][prev]).mean()) if any_prev else None,
        "mean_gross_leverage_all_rows": float(daily["gross_leverage"].mean()) if rows else None,
        "mean_net_leverage_all_rows": float(daily["net_leverage"].mean()) if rows else None,
        "csv_rows": rows,
        "leverage_gate_basis": LEVERAGE_GATE_BASIS,
        "mean_held_names": float(daily["held_names"][prev].mean()) if any_prev else None,
        "held_share": v5.get("mean_held_share"),
        "tau_gmv_mean": float(tau.mean()) if tau.size else None,
        "tau_gmv_p95": float(np.quantile(tau, 0.95)) if tau.size else None,
        "tau_gmv_sessions": int(tau.size),
        "cost_per_gmv_turnover": cost_tau / tau_sum if tau_sum > 0 else None,
        "cost_bps_traded": (1e4 * float(daily["trade_cost_dollars"][executed].sum()) / traded) if traded > 0 else None,
        "return_rows": int(ret.sum()),
    }


def net_series(daily: dict) -> dict:
    ret = return_mask(daily)
    return dict(zip(daily["session_ns"][ret].astype(np.int64).tolist(), daily["net_return"][ret].tolist()))


# ------------------------------------------------------------------ statistics
def sharpe(x: np.ndarray) -> float:
    x = np.asarray(x, dtype=np.float64)
    sd = x.std(ddof=1) if x.size > 1 else 0.0
    return float(x.mean() / sd * math.sqrt(ANNUAL)) if sd > 0 else float("nan")


def align(cell: dict, reference: dict) -> tuple[np.ndarray, np.ndarray]:
    common = sorted(set(cell) & set(reference))
    return np.array([cell[s] for s in common]), np.array([reference[s] for s in common])


# ------------------------------------------------------------------ weights
def load_weights(paths: list[str]) -> list[dict]:
    out = []
    for p in paths:
        path = Path(p)
        if path.is_dir():
            path = path / "composition_weights.json"
        data = path.read_bytes()
        doc = json.loads(data)
        out.append({"path": str(path), "sha256": hashlib.sha256(data).hexdigest(),
                    "weighted_standalone_turnover": doc["provenance"]["weighted_standalone_turnover"],
                    "rule": doc["provenance"].get("rule")})
    return out


def match_weights(weights: list[dict], summary: dict) -> tuple[dict | None, str]:
    want = summary.get("composition_weights_sha256")
    for w in weights:
        if w["sha256"] == want:
            return w, "matched"
    if len(weights) == 1:
        return weights[0], f"UNMATCHED (NAV composition_weights_sha256 {str(want)[:8]})"
    return None, "none"


def netting_ratio(tau_mean: float | None, weighted_standalone_turnover: float) -> float | None:
    if tau_mean is None or not weighted_standalone_turnover > 0:
        return None
    return tau_mean / weighted_standalone_turnover


# ------------------------------------------------------------------ listing checks and provenance (T41 M5, M7)
def same_series(a: dict, b: dict) -> bool:
    """Two net series (session_ns -> net) with the same sessions in the same order and equal values (NaN == NaN)."""
    if list(a) != list(b):
        return False
    va = np.fromiter(a.values(), dtype=np.float64, count=len(a))
    vb = np.fromiter(b.values(), dtype=np.float64, count=len(b))
    return bool(np.array_equal(va, vb, equal_nan=True))


def listing_warnings(dirs: list[str], results: list[dict], nets: list[dict], n: int) -> list[str]:
    """Warnings (never refusals) for a listing that silently moves V[SR_n] away from the declared N trials."""
    out = []
    defined = sum(1 for r in results if r["net_moments"]["sr_daily"] is not None)
    if defined != n:
        out.append(f"{defined} listed dir(s) have a defined SR but --dsr-n is {n}: V[SR_n] (hence SR0 and every "
                   "DSR) comes from the listed dirs, not from N")
    for i in range(len(nets)):
        for j in range(i + 1, len(nets)):
            if same_series(nets[i], nets[j]):
                out.append(f"identical daily net series: {dirs[i]} and {dirs[j]} (a duplicate cell shrinks V[SR_n])")
    return out


def git_head(where: Path) -> str | None:
    """``git rev-parse HEAD`` of the checkout holding ``where``; None when git or the repository is unavailable."""
    try:
        done = subprocess.run(["git", "rev-parse", "HEAD"], cwd=where, capture_output=True, text=True, timeout=30,
                              check=False)
    except (OSError, subprocess.SubprocessError):
        return None
    head = done.stdout.strip()
    return head if done.returncode == 0 and re.fullmatch(r"[0-9a-f]{40}(?:[0-9a-f]{24})?", head) else None


def run_provenance(argv: list[str], warnings: list[str], engine: "EngineStats | None" = None) -> dict:
    """nav_summ_run. With an engine (P9 ruling B2-R3) also the verb that computed the statistics: engine_exe (its
    path) and engine_exe_sha256 (the bytes it ran, hashed when the verb was opened)."""
    script = Path(__file__).resolve()
    out = {"argv": list(argv), "script": str(script),
           "script_sha256": hashlib.sha256(script.read_bytes()).hexdigest(), "git_head": git_head(script.parent),
           "warnings": list(warnings)}
    if engine is not None:
        out.update(engine_exe=str(engine.exe), engine_exe_sha256=engine.sha256)
    return out


# ------------------------------------------------------------------ validation kit (platform v8 V-1)
def finite_or_none(x):
    return float(x) if x is not None and math.isfinite(x) else None


def year_table(daily: dict) -> list[dict]:
    """Per calendar year (UTC date of session_ns) of the return rows: return rows, compounded net return, net Sharpe
    (mean / sd ddof 1 x sqrt 252), annualized volatility, mean daily GMV turnover over the tau_t sessions of the year
    (``turnover_rows``) and cost per traded dollar in bps over the year's executed sessions."""
    ret, keep = return_mask(daily), turnover_rows(daily)
    executed = daily["executed"] == 1
    years = np.array([BI.session_date(int(s)).year for s in daily["session_ns"]])
    rows = []
    for y in sorted(set(years[ret].tolist())):
        in_y = years == y
        x = daily["net_return"][ret & in_y]
        tau = daily["one_way_turnover_gmv"][keep & in_y]
        traded = float(daily["traded_dollars"][executed & in_y].sum())
        cost = float(daily["trade_cost_dollars"][executed & in_y].sum())
        sd = float(x.std(ddof=1)) if x.size > 1 else 0.0
        rows.append({"year": int(y), "return_rows": int(x.size), "net_return": float(np.prod(1.0 + x) - 1.0),
                     "net_sharpe": float(x.mean()) / sd * math.sqrt(ANNUAL) if sd > 0 else None,
                     "ann_vol": sd * math.sqrt(ANNUAL) if x.size > 1 else None,
                     "tau_gmv_mean": float(tau.mean()) if tau.size else None,
                     "cost_bps_traded": 1e4 * cost / traded if traded > 0 else None})
    return rows


def print_year_table(rows: list[dict], indent: str = "   ") -> None:
    print(f"{indent}year table (return rows, compounded net return, net Sharpe, volatility, tau_gmv mean, cost bps "
          "per traded dollar):")
    for y in rows:
        print(f"{indent}  {y['year']} rows {y['return_rows']:4d} net {fmt(y['net_return'], '+.4f')} sharpe "
              f"{fmt(y['net_sharpe'], '+.3f')} vol {fmt(y['ann_vol'], '.4f')} tau {fmt(y['tau_gmv_mean'], '.4f')} "
              f"cost_bps {fmt(y['cost_bps_traded'], '.2f')}")


def print_ledger_dsr(q: dict) -> None:
    print(f"   ledger DSR (N={q['n']}, {q['window_id']}): DSR {fmt(q['dsr'], '.4f')} vs SR0 "
          f"{fmt(q['sr0_annual'], '.3f')} ann | V[SR] from {q['cells']} cells ledgered on {q['window_id']} "
          f"({fmt(q['variance_sr'], '.3e')}) | beside: legacy variance ({q['legacy_cells']} cells) DSR "
          f"{fmt(q['legacy_dsr'], '.4f')} vs SR0 {fmt(q['legacy_sr0_annual'], '.3f')} ann")


def paired_verdict(paired: dict, alpha: float, rule: str) -> dict:
    """The verdict of a one-sided paired block (the engine verb's, ``--one-sided``): dSR > 0 and lw.p_one_sided <
    alpha. A comparison of the verb's numbers, no statistic: --bundle and --pool-one-sided (OD-3 T2) share it."""
    p1 = paired["lw"].get("p_one_sided")
    positive = bool(math.isfinite(paired["dsr"]) and paired["dsr"] > 0)
    return {"dsr_positive": positive, "p_one_sided": p1, "alpha": alpha,
            "pass": bool(positive and p1 is not None and p1 < alpha), "rule": rule}


def bundle(base: Path, final: Path, args) -> dict:
    """The cumulative paired test FINAL vs BASE (v8-prereg items 4 and 9): paired dSR with the one-sided studentized
    bootstrap p, a per-year dSR table, both cells' year tables and the verdict dSR > 0 and p_one_sided < alpha."""
    nets, tables, scenarios = {}, {}, {}
    for tag, d in (("base", base), ("final", final)):
        scen = scenario_of(load_summary(d), args.scenario)["scenario"]
        daily = load_daily(d, scen)
        nets[tag], tables[tag], scenarios[tag] = net_series(daily), year_table(daily), scen
    paired = engine_of(args).paired_of(nets["final"], nets["base"], args, one_sided=True)
    if paired is None:
        raise SystemExit("nav_summ: --bundle: need >= 3 sessions common to BASE and FINAL")
    common = sorted(set(nets["final"]) & set(nets["base"]))
    years = []
    for y in sorted({BI.session_date(s).year for s in common}):
        sessions = [s for s in common if BI.session_date(s).year == y]
        sr_f = finite_or_none(sharpe(np.array([nets["final"][s] for s in sessions])))
        sr_b = finite_or_none(sharpe(np.array([nets["base"][s] for s in sessions])))
        years.append({"year": y, "sessions": len(sessions), "sr_final": sr_f, "sr_base": sr_b,
                      "dsr": sr_f - sr_b if sr_f is not None and sr_b is not None else None})
    verdict = paired_verdict(paired, args.bundle_alpha,
                             "cumulative paired S2 net dSR(FINAL - BASE) > 0 and studentized circular-block-bootstrap "
                             "one-sided p < alpha (v8-prereg item 9: this part of the freeze gate only)")
    return {"base": str(base), "final": str(final), "scenarios": scenarios, "paired": paired, "years": years,
            "year_table": tables, "verdict": verdict}


def print_degenerate(p: dict) -> None:
    """The engine's degenerate-pair line (ruling B2-R1 (c): 1 - rho < 1e-6, SE / t / p refused); nothing otherwise."""
    if p.get("degenerate"):
        print(f"   paired: degenerate pair (1 - rho {fmt(p.get('one_minus_rho'), '.1e')} < 1e-06): Memmel SE, t and the "
              "LW CI and p are refused (ruling B2-R1)")


def print_bundle(res: dict) -> None:
    p, lw, v = res["paired"], res["paired"]["lw"], res["verdict"]
    print(f"== bundle {res['final']} vs {res['base']} [{res['scenarios']['final']}]: dSR(net) {p['dsr']:+.3f} = SR "
          f"{p['sr']:+.3f} - SR {p['sr_reference']:+.3f} | rho {fmt(p['rho'], '.3f')} | T {p['sessions']} | Memmel SE "
          f"{fmt(p['memmel_se'], '.3f')} | CBB 95% [{p['cbb_ci95'][0]:+.3f}, {p['cbb_ci95'][1]:+.3f}] | LW studentized "
          f"95% [{fmt(lw['ci95'][0], '+.3f')}, {fmt(lw['ci95'][1], '+.3f')}] p two-sided {fmt(lw['p_value'], '.4f')} "
          f"one-sided {fmt(lw['p_one_sided'], '.4f')} ({p['draws']} draws, block {p['block']}, seed {p['seed']})")
    print_degenerate(p)
    print(f"   verdict {'PASS' if v['pass'] else 'FAIL'}: dSR > 0 {v['dsr_positive']}; one-sided p "
          f"{fmt(v['p_one_sided'], '.4f')} < {v['alpha']}")
    for y in res["years"]:
        print(f"   {y['year']}: dSR {fmt(y['dsr'], '+.3f')} (SR final {fmt(y['sr_final'], '+.3f')}, SR base "
              f"{fmt(y['sr_base'], '+.3f')}, {y['sessions']} sessions)")


# ------------------------------------------------------------------ driver
def analyse(d: Path, args, weights: list[dict], ref_nets: dict | None) -> tuple[dict, dict]:
    """The dir's result row and its net series (session_ns -> net return over the return rows)."""
    summary = load_summary(d)
    scen = scenario_of(summary, args.scenario)
    daily = load_daily(d, scen["scenario"])
    stats = construction_stats(daily, scen)
    out = {"dir": str(d), "scenario": scen["scenario"], "rule": summary.get("rule"),
           "net_sharpe": scen.get("net_sharpe"), "gross_sharpe": scen.get("gross_sharpe"), "hac_t": scen.get("hac_t"),
           **stats}
    summ_tau = (scen.get("daily_turnover_gmv") or {}).get("mean")
    if summ_tau is not None and stats["tau_gmv_mean"] is not None and abs(summ_tau - stats["tau_gmv_mean"]) > 1e-9:
        out["warning_tau"] = f"CSV tau mean {stats['tau_gmv_mean']:.6f} != summary {summ_tau:.6f}"
    tau_book = summ_tau if summ_tau is not None else stats["tau_gmv_mean"]
    if weights:
        w, how = match_weights(weights, summary)
        out["weights_match"] = how
        if w is not None:
            out.update(netting_ratio=netting_ratio(tau_book, w["weighted_standalone_turnover"]),
                       weighted_standalone_turnover=w["weighted_standalone_turnover"], tau_book=tau_book,
                       weights_sha256=w["sha256"])
    nets = net_series(daily)
    engine = engine_of(args)
    out["net_moments"] = engine.moments_of(nets)
    if ref_nets is not None:
        out["paired"] = engine.paired_of(nets, ref_nets, args)
        if out["paired"] is None:
            raise SystemExit(f"nav_summ: {d}: need >= 3 sessions in common with the reference (paired dSR)")
    if getattr(args, "year_table", False):
        out["year_table"] = year_table(daily)
    return out, nets


# ------------------------------------------------------------------ era pools (platform v8 H-1)
def load_era(d: Path, args) -> tuple[dict, dict, dict]:
    """(summary, scenario, daily) of one era's NAV dir (its primary scenario, or --scenario)."""
    summary = load_summary(d)
    scen = scenario_of(summary, args.scenario)
    return summary, scen, load_daily(d, scen["scenario"])


def pooled_daily(dirs: list[str], ids: list[str], args) -> tuple[dict, list[tuple[dict, dict, dict]]]:
    """The pooled daily dict of era NAV dirs (date order; era_pool.pool_rows) and each era's (summary, scenario,
    daily). Refuses eras of different scenarios or rules and era windows that era_pool.check_windows refuses."""
    loaded = [load_era(Path(d), args) for d in dirs]
    if len({scen["scenario"] for _, scen, _ in loaded}) != 1 or len({s.get("rule") for s, _, _ in loaded}) != 1:
        raise SystemExit("nav_summ: --pool eras must share one scenario and one rule")
    rw = BI.research_window()
    try:
        windows = [(i, int(daily["session_ns"][0]), int(daily["session_ns"][-1]) + BI.DAY_NS)
                   for i, (_, _, daily) in zip(ids, loaded)]
        EP.check_windows(windows, rw.TRAIN_BEGIN_NS, rw.TRAIN_END_NS, rw.SEAL_NS)
        return EP.pool_rows([(i, daily) for i, (_, _, daily) in zip(ids, loaded)]), loaded
    except (EP.PoolError, IndexError) as exc:
        raise SystemExit(f"nav_summ: --pool: {exc}") from exc


def pool_weights(weights: list[dict], summaries: list[dict]) -> tuple[dict | None, str]:
    """The weights of a pooled row: every era's summary matches a --weights file and the matched files carry one
    weighted standalone turnover (the per-era files of one pooled fit); else none."""
    matched = [match_weights(weights, s) for s in summaries]
    if all(how == "matched" for _, how in matched) and \
            len({w["weighted_standalone_turnover"] for w, _ in matched}) == 1:
        return matched[-1][0], "matched (every era)"
    return None, "none"


def paired_or_note(nets: dict, ref_nets: dict, args) -> dict:
    """A pooled row's paired block, skipped with a note below 3 sessions in common with the reference."""
    a, b = align(nets, ref_nets)
    if a.size < 3:
        return {"paired_note": f"paired skipped: {a.size} session(s) in common with the reference (3 needed)"}
    if not getattr(args, "pool_one_sided", False):
        return {"paired": engine_of(args).paired_of(nets, ref_nets, args)}
    # OD-3 T2 (--pool-one-sided; PRE-21: on the engine verb's --one-sided, no Python statistic): the one-sided p of the
    # same resamples and the bundle's verdict rule on the pooled pair; without the flag the block is unchanged
    p = engine_of(args).paired_of(nets, ref_nets, args, one_sided=True)
    p["verdict"] = paired_verdict(p, args.bundle_alpha,
                                  "pooled paired S2 net dSR > 0 and studentized circular-block-bootstrap one-sided p "
                                  "< alpha (bundle rule, not G-B8: the bundle's rule on the pooled pair; OD-3 T2, "
                                  "ruling OD3P-T2V)")
    return {"paired": p}


def analyse_pool(dirs: list[str], ids: list[str], args, weights: list[dict], ref_nets: dict | None) -> tuple[dict, dict]:
    """The pooled row of era NAV dirs (date order) and its net series.

    One era: that era's row (``analyse``) with the pool's dir and block, so pooled == single. Several: the statistics
    of the pooled daily dict (each era's first row, deployment and ramp are per era: ``segments``), net and gross Sharpe
    recomputed with ``sharpe`` over the pooled return rows, hac_t and held_share null. The eras' own rows (no
    reference) sit in row["pool"]["eras"]."""
    label = EP.pool_label(dirs)
    daily, loaded = pooled_daily(dirs, ids, args)
    eras = [analyse(Path(d), args, weights, None) for d in dirs]
    if len(dirs) == 1:
        row, nets = dict(eras[0][0]), eras[0][1]
    else:
        ret = return_mask(daily)
        scen = loaded[0][1]
        row = {"scenario": scen["scenario"], "rule": loaded[0][0].get("rule"),
               "net_sharpe": finite_or_none(sharpe(daily["net_return"][ret])),
               "gross_sharpe": finite_or_none(sharpe(daily["gross_return"][ret])) if "gross_return" in daily else None,
               "hac_t": None, **construction_stats(daily, {})}
        if weights:
            w, how = pool_weights(weights, [s for s, _, _ in loaded])
            row["weights_match"] = how
            if w is not None:
                row.update(netting_ratio=netting_ratio(row["tau_gmv_mean"], w["weighted_standalone_turnover"]),
                           weighted_standalone_turnover=w["weighted_standalone_turnover"],
                           tau_book=row["tau_gmv_mean"], weights_sha256=w["sha256"])
        nets = net_series(daily)
        row["net_moments"] = engine_of(args).moments_of(nets)
        if getattr(args, "year_table", False):
            row["year_table"] = year_table(daily)
    if ref_nets is not None:
        row.update(paired_or_note(nets, ref_nets, args))
    row["dir"] = label
    row["pool"] = {"schema": EP.SCHEMA, "ids": list(ids), "dirs": [str(d) for d in dirs],
                   "segments": list(daily[EP.SEGMENTS]),
                   "eras": [dict(r, id=i, role_sha256=s.get("role_sha256"))
                            for i, (r, _), (s, _, _) in zip(ids, eras, loaded)]}
    return row, nets


def pool_ids(args) -> list[str]:
    ids = args.pool_ids.split(",") if args.pool_ids else [f"E{k}" for k in range(1, len(args.pool) + 1)]
    try:
        ids = [EP.check_id(i) for i in ids]
    except EP.PoolError as exc:
        raise SystemExit(f"nav_summ: --pool-ids: {exc}") from exc
    if len(ids) != len(args.pool) or len(set(ids)) != len(ids):
        raise SystemExit("nav_summ: --pool-ids names each --pool dir once (distinct ids)")
    return ids


def print_pool(r: dict) -> None:
    p = r["pool"]
    print(f"   pool {','.join(p['ids'])} (segments {p['segments']}): {len(p['ids'])} era(s) in date order")
    for e in p["eras"]:
        print(f"   era {e['id']} {e['dir']}: net {fmt(e['net_sharpe'], '+.3f')} gross {fmt(e['gross_sharpe'], '+.3f')} "
              f"hac {fmt(e['hac_t'], '+.2f')} return rows {e['return_rows']} tau_gmv mean "
              f"{fmt(e['tau_gmv_mean'], '.4f')} gross_lev_post_ramp {fmt(e['mean_gross_leverage_post_ramp'], '.4f')} "
              f"({e['post_ramp_rows']} rows)")
    v = (r.get("paired") or {}).get("verdict")
    if v is not None:   # OD-3 T2: --pool-one-sided only
        print(f"   pooled paired one-sided p {fmt(v['p_one_sided'], '.4f')} (engine verb --one-sided, same resamples); "
              f"verdict (bundle rule, not G-B8) dSR > 0 and p < {v['alpha']}: {'PASS' if v['pass'] else 'FAIL'}")
    if "paired_note" in r:
        print(f"   {r['paired_note']}")


def print_analysis(r: dict, reference: str | None) -> None:
    print(f"== {r['dir']} [{r['scenario']}] rule={r['rule']}")
    print(f"   construction: gross_lev {fmt(r['mean_gross_leverage'], '.4f')} net_lev {fmt(r['mean_net_leverage'], '+.4f')} "
          f"|net| {fmt(r['mean_abs_net_leverage'], '.4f')} held_share {fmt(r['held_share'], '.4f')} "
          f"held_names {fmt(r['mean_held_names'], '.1f')} tau_gmv mean {fmt(r['tau_gmv_mean'], '.4f')} "
          f"p95 {fmt(r['tau_gmv_p95'], '.4f')} ({r['tau_gmv_sessions']} sessions) "
          f"cost/GMV-tau {fmt(r['cost_per_gmv_turnover'], '.5f')} cost_bps_traded {fmt(r['cost_bps_traded'], '.2f')}")
    print(f"   leverage over all {r['csv_rows']} CSV rows [R6' mechanics gate]: "
          f"gross_lev_all_rows {fmt(r['mean_gross_leverage_all_rows'], '.4f')} "
          f"net_lev_all_rows {fmt(r['mean_net_leverage_all_rows'], '+.4f')} "
          "(construction gross_lev/net_lev: previous close over return rows)")
    print(f"   leverage post-ramp over the {r['post_ramp_rows']} CSV rows after the first {RAMP_ROWS} "
          f"[v6 C4 L calibration; gate stays all rows]: gross_lev_post_ramp "
          f"{fmt(r['mean_gross_leverage_post_ramp'], '.4f')} net_lev_post_ramp "
          f"{fmt(r['mean_net_leverage_post_ramp'], '+.4f')}")
    if "warning_tau" in r:
        print(f"   WARNING {r['warning_tau']}")
    if "weights_match" in r:
        if "netting_ratio" not in r:
            print("   netting_ratio: na (no --weights file matches this NAV's composition_weights_sha256)")
        else:
            print(f"   netting_ratio = tau_book / weighted_standalone_turnover = {fmt(r['tau_book'], '.4f')} / "
                  f"{fmt(r['weighted_standalone_turnover'], '.4f')} = {fmt(r['netting_ratio'], '.3f')} "
                  f"(weights {r['weights_sha256'][:8]} {r['weights_match']})")
    p = r.get("paired")
    if p:
        lw = p["lw"]
        print(f"   paired vs {reference}: dSR(net) {p['dsr']:+.3f} = SR {p['sr']:+.3f} - SR {p['sr_reference']:+.3f} | "
              f"rho {fmt(p['rho'], '.3f')} | T {p['sessions']} | Memmel SE {fmt(p['memmel_se'], '.3f')} t "
              f"{fmt(p['t'], '+.2f')} | CBB 95% [{p['cbb_ci95'][0]:+.3f}, {p['cbb_ci95'][1]:+.3f}] | LW studentized 95% "
              f"[{fmt(lw['ci95'][0], '+.3f')}, {fmt(lw['ci95'][1], '+.3f')}] p {fmt(lw['p_value'], '.3f')} "
              f"(dSR_pop {lw['dsr_population']:+.3f}; {p['draws']} draws, block {p['block']}, seed {p['seed']})")
        print_degenerate(p)
    q = r.get("deflated")
    if q:
        print(f"   deflated SR (N={q['n']}): DSR {fmt(q['dsr'], '.4f')} | SR {fmt(q['sr_daily'], '+.5f')}/session "
              f"({fmt(q['sr_annual'], '+.3f')} ann) vs SR0 {fmt(q['sr0_daily'], '.5f')}/session "
              f"({fmt(q['sr0_annual'], '.3f')} ann) | skew {fmt(q['skew'], '+.3f')} kurtosis {fmt(q['kurtosis'], '.3f')} | "
              f"T {q['sessions']} | V[SR_n] {fmt(q['variance_sr'], '.3e')} ({q['variance_source']})")


def print_integrity(r: dict) -> None:
    """The v7 integrity lines of one dir (only when their options were given; legacy output is untouched)."""
    e = r.get("deflated_effective_n")
    if e:
        q = r.get("deflated") or {}
        lo = r.get("deflated_lo_null") or {}
        print(f"   effective-N DSR (ONC N_eff={e['n_eff']} of {e['n_trials']} trial series): DSR {fmt(e['dsr'], '.4f')} "
              f"vs SR0 {fmt(e['sr0_daily'], '.5f')}/session ({fmt(e['sr0_annual'], '.3f')} ann) | V[SR_k] "
              f"{fmt(e['variance_sr'], '.3e')} | beside: cell-count DSR (N={q.get('n')}) {fmt(q.get('dsr'), '.4f')}, "
              f"Lo null (N={lo.get('n')}) DSR {fmt(lo.get('dsr'), '.4f')} (SR0 {fmt(lo.get('sr0_annual'), '.3f')} ann)")
    p = r.get("psr")
    if p:
        for b in p["benchmarks"]:
            print(f"   PSR vs SR* {b['sr_star_annual']:+.2f} ann: {fmt(b['psr'], '.4f')} | MinTRL at {1 - p['alpha']:.0%}: "
                  f"{fmt(b['min_trl_sessions'], '.0f')} sessions ({fmt(b['min_trl_years'], '.2f')} y) | SR "
                  f"{fmt(p['sr_annual'], '+.3f')} ann, skew {fmt(p['skew'], '+.3f')} kurtosis {fmt(p['kurtosis'], '.3f')}, "
                  f"T {p['sessions']}")


def print_pbo(res: dict, cells: list[str], common: list[int]) -> None:
    q = res["logit_quantiles"]
    how = "exhaustive" if res["exhaustive"] else f"SEEDED SUBSAMPLE (seed {res['seed']})"
    span = (f"{BI.session_date(common[0]).isoformat()}..{BI.session_date(common[-1]).isoformat()}" if common else "none")
    print(f"== CSCV PBO (Bailey-Borwein-Lopez de Prado-Zhu 2017) over {res['n_candidates']} cells: PBO {res['pbo']:.4f} | "
          f"{res['splits']} of {res['splits_total']} splits ({how}) | {res['blocks']} blocks x {res['block_width']} "
          f"sessions of {res['sessions']} common ({span}; {res['dropped_tail_sessions']} trailing dropped)")
    print(f"   logit mean {res['logit_mean']:+.3f} p5 {q['p5']:+.3f} p25 {q['p25']:+.3f} p50 {q['p50']:+.3f} "
          f"p75 {q['p75']:+.3f} p95 {q['p95']:+.3f} | IS winner SR {res['winner_is_sr_annual_mean']:+.3f} ann -> OOS "
          f"{res['winner_oos_sr_annual_mean']:+.3f} ann, P[OOS SR < 0] {res['prob_winner_oos_loss']:.3f}, degradation "
          f"slope {fmt(res['degradation_slope'], '+.3f')}")
    dist = res["logit_distribution"]
    print(f"   logit distribution (IS winner's OOS rank 0 = worst .. {res['n_candidates'] - 1} = best; logit "
          f"{dist['logit'][0]:+.3f} .. {dist['logit'][-1]:+.3f}): splits {dist['splits']}")
    for c, w in zip(cells, res["winner_counts"]):
        print(f"   IS winner {w:6d}x  {c}")


def print_effective(eff: dict, source: str) -> None:
    print(f"== effective N (ONC, Lopez de Prado-Lewis 2019) from {source}: {eff['n_trials']} trial series, "
          f"{eff['sessions']} common sessions, mean off-diagonal rho {eff['mean_offdiag_corr']:.3f} -> N_eff "
          f"{eff['n_eff']} (n_init {eff['n_init']}, seed {eff['seed']}); V[SR_k] {fmt(eff['variance_sr'], '.3e')}")
    for k, (cl, sr) in enumerate(zip(eff["clusters"], eff["cluster_sr_daily"])):
        ann = sr * math.sqrt(ANNUAL) if sr is not None else None
        print(f"   cluster {k}: {len(cl)} series, IVP SR {fmt(ann, '+.3f')} ann: {', '.join(cl)}")


def ledger_fields(args) -> dict:
    """The v8 ledger-line fields the options ask for (empty without them: the line keeps the v7 layout)."""
    out = {}
    if getattr(args, "origin", None):
        out["origin"] = args.origin
    if getattr(args, "protocol", "v7") == "v8":
        out["research_window_id"] = BI.window_id()
    if getattr(args, "rerun_of", None):
        out.update(rerun_of=args.rerun_of, rerun_basis=args.rerun_basis)
    if getattr(args, "ledger_defect", None):
        out["defect"] = args.ledger_defect
    return out


def integrity(args, argv, results, analysed) -> None:
    """--ledger / --psr / --effective-n / --pbo / --ledger-n, after the legacy per-dir analysis (v8: the v8 ledger
    fields and chain, --dsr-ledger and the v8 Appendix A block). Every ledger reader skips protocol lines."""
    pool = getattr(args, "pool", None)
    labels = list(args.dirs) + ([results[-1]["dir"]] if pool else [])  # the pooled row is the last result
    nets_by = {d: nets for d, (_, nets) in zip(labels, analysed)}
    v8 = getattr(args, "protocol", "v7") == "v8"
    if args.ledger and (args.dirs or pool):
        run = {k: v for k, v in run_provenance(argv, []).items() if k in ("script_sha256", "git_head")}
        recs = []
        for d, r in zip(args.dirs, results):
            daily = Path(d) / f"daily_{r['scenario']}.csv"
            try:
                recs.append(BI.ledger_record(args.ledger_kind, d, Path(d) / "summary.json", daily, r["scenario"],
                                             nets_by[d], r["net_sharpe"], count=args.ledger_count,
                                             note=args.ledger_note, run=run, **ledger_fields(args)))
            except ValueError as exc:
                raise SystemExit(f"nav_summ: {exc}") from exc
        pool_recs = []
        if pool:
            pooled = results[-1]
            eras = [{"id": e["id"], "role_sha256": e.get("role_sha256"), "cell": e["dir"],
                     "summary_path": Path(e["dir"]) / "summary.json",
                     "daily_path": Path(e["dir"]) / f"daily_{e['scenario']}.csv", "scenario": e["scenario"],
                     "nets": net_series(load_era(Path(e["dir"]), args)[2]), "net_sharpe": e["net_sharpe"]}
                    for e in pooled["pool"]["eras"]]
            try:
                pool_recs = BI.ledger_pool_records(args.ledger_kind, eras, nets_by[pooled["dir"]],
                                                   pooled["net_sharpe"], count=args.ledger_count,
                                                   note=args.ledger_note, run=run,
                                                   era_of=getattr(args, "era_of", None), **ledger_fields(args))
            except ValueError as exc:
                raise SystemExit(f"nav_summ: {exc}") from exc
            recs += pool_recs
        try:
            added, skipped = BI.ledger_append(Path(args.ledger), recs, chain=v8)
        except ValueError as exc:
            raise SystemExit(f"nav_summ: {exc}") from exc
        added_ids = {a["trial_id"] for a in added}
        owners = recs[:len(args.dirs)] + pool_recs[-1:]   # the pooled row owns its trial line (the last)
        for rec, r in zip(owners, results):
            r["ledger"] = {"path": args.ledger, "trial_id": rec["trial_id"], "kind": rec["kind"],
                           "appended": rec["trial_id"] in added_ids}
        print(f"== ledger {args.ledger}: appended {len(added)}, skipped {len(skipped)} already ledgered "
              f"(kind {args.ledger_kind})")
    if args.psr:      # the verb is opened only by a block that computes a statistic (review B2 S8)
        for d, r in zip(labels, results):
            r["psr"] = engine_of(args).psr_of(nets_by[d], args.psr_benchmarks, args.psr_alpha)
    eff = None
    if args.effective_n:
        if args.effective_n == "dirs":
            names, series = list(labels), [nets_by[d] for d in labels]
        else:
            records = BI.ledger_read(Path(args.effective_n))
            names, series = BI.ledger_net_series(
                records, lambda rec: net_series(load_daily_csv(Path(rec["series"]["path"]))))
        eff, rows = engine_of(args).effective([nets_by[d] for d in labels], names, series, args)
        for r, (e, lo) in zip(results, rows):
            r["deflated_effective_n"] = dict(e, clusters=eff["clusters"], source=args.effective_n)
            r["deflated_lo_null"] = lo
    for d, r in zip(labels, results):
        if "deflated_effective_n" in r or "psr" in r:
            print(f"== integrity {d}")
            print_integrity(r)
    if eff is not None:
        print_effective(eff, args.effective_n)
    if args.pbo is not None:
        cells = list(args.pbo) or list(args.dirs)
        if len(cells) < BI.PBO_MIN_CELLS:
            raise SystemExit(f"nav_summ: --pbo needs a grid of >= {BI.PBO_MIN_CELLS} cells, got {len(cells)}")
        series = [nets_by[c] if c in nets_by else
                  net_series(load_daily(Path(c), scenario_of(load_summary(Path(c)), args.scenario)["scenario"]))
                  for c in cells]
        res, common = engine_of(args).cscv(series, args.pbo_blocks)
        print_pbo(res, cells, common)
        if args.pbo_json:
            doc = dict(res, logits=res["logits"].tolist(), cells=cells,
                       common_sessions=[BI.session_date(common[0]).isoformat(), BI.session_date(common[-1]).isoformat()],
                       nav_summ_run=run_provenance(argv, [], args.engine))
            Path(args.pbo_json).write_text(json.dumps(doc, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if getattr(args, "dsr_ledger", None):
        wid = BI.window_id()
        tids, histories = [], []
        for d, r in zip(labels, results):
            history = False
            if "pool" in r and d not in args.dirs:   # the pooled row: its pooled trial_id (task H-1)
                tid = BI.pooled_trial_id("construction", [BI.sha256_file(Path(e["dir"]) / f"daily_{e['scenario']}.csv")
                                                          for e in r["pool"]["eras"]])
                history = min(nets_by[d]) < BI.research_window().TRAIN_BEGIN_NS   # a history read (Ruling E-41)
            else:
                tid = BI.trial_id("construction", BI.sha256_file(Path(d) / f"daily_{r['scenario']}.csv"))
            tids.append(tid)
            histories.append(history)
        doc = engine_of(args).ledger_rows_of([nets_by[d] for d in labels], args.dsr_ledger, wid, tids, histories)
        rows = [c["dsr"]["house_v1"] for c in doc["cells"]]
        for d, r, row in zip(labels, results, rows):
            r["deflated_ledger"] = row
            print(f"== ledger DSR {d}")
            print_ledger_dsr(r["deflated_ledger"])
    if getattr(args, "dsr_total", None):
        dsr_total_block(args, results, nets_by)
    if args.ledger_n:
        records = BI.ledger_read(Path(args.ledger_n))
        for line in BI.appendix_a(records, args.ledger_n):
            print(line)
        if v8:
            print(BI.appendix_a_v8(records))


def ledgered_book_series(d: Path, scenario: str | None) -> tuple[str, dict]:
    """(construction trial_id, net series) of a --dsr-hand NAV dir. A dir without summary.json or the scenario's
    daily CSV is refused before anything is computed."""
    if not (d / "summary.json").is_file():
        raise SystemExit(f"nav_summ: --dsr-hand {d}: no summary.json")
    scen = scenario_of(load_summary(d), scenario)["scenario"]
    daily = d / f"daily_{scen}.csv"
    if not daily.is_file():
        raise SystemExit(f"nav_summ: --dsr-hand {d}: no {daily.name}")
    return BI.trial_id("construction", BI.sha256_file(daily)), net_series(load_daily(d, scen))


def dsr_total_block(args, results: list[dict], nets_by: dict) -> None:
    """--dsr-total / --dsr-hand (v8x-prereg.md section 4). Prints the total-count DSR and the v8-count value of every
    listed dir, then DSR_hand of each --dsr-hand book; adds r["deflated_total"] to every listed row (the --dsr-hand
    rows under "hand"). The rows are the engine verb's (dsr.tot and hand of its document): one call for the listed
    dirs, then one per --dsr-hand book, so the printed lines and refusals keep their order."""
    engine, wid, path = engine_of(args), BI.window_id(), str(Path(args.dsr_total))
    tids = [BI.trial_id("construction", BI.sha256_file(Path(d) / f"daily_{r['scenario']}.csv"))
            for d, r in zip(args.dirs, results)]
    doc = engine.ledger_rows_of([nets_by[d] for d in args.dirs], path, wid, tids, [False] * len(tids), "refuse")
    led = doc["ledger"]
    ledger = {"path": led["path"], "records": range(led["lines"]), "chain_head": led["chain_head"]}
    head = {"ledger": led["path"], "lines": led["lines"], "chain_head": led["chain_head"]}
    for d, r, tid, cell in zip(args.dirs, results, tids, doc["cells"]):
        r["deflated_total"] = dict(head, trial_id=tid, **cell["dsr"]["tot"])
        for line in DT.format_book(d, r["net_moments"], ledger, r["deflated_total"]):
            print(line)
    hands = []
    for h in args.dsr_hand:
        tid, nets = ledgered_book_series(Path(h), args.scenario)
        row = engine.ledger_rows_of([nets], path, wid, [tid], [False], "refuse", [(tid, nets)])["hand"][0]
        row.pop("path", None)
        moments = row.pop("net_moments")
        hands.append(dict(row, dir=h, net_moments=moments))
        for line in DT.format_hand(h, moments, row):
            print(line)
    for r in results[:len(args.dirs)]:
        r["deflated_total"]["hand"] = hands


def float_list(text: str) -> list[float]:
    return [float(x) for x in text.split(",") if x.strip()]


def main(argv=None) -> int:
    # ruling D2R-2 (b): no flag abbreviation (a prefix like --proto is a usage error, not --protocol): the same defect
    # class as the fitter's (D2R-1) waits for the next prefix-sharing flag
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0], allow_abbrev=False)
    ap.add_argument("dirs", nargs="*", help="NAV output directories (summary.json + daily_<scenario>.csv)")
    ap.add_argument("--weights", action="append", default=[], help="composition_weights.json (or its dir); repeatable")
    ap.add_argument("--reference", default=None, help="reference NAV dir for the paired dSR(net)")
    ap.add_argument("--scenario", default=None, help="scenario name (default: each summary's primary)")
    ap.add_argument("--draws", type=int, default=None, help=f"bootstrap resamples ({DEFAULT_DRAWS}; v8: {V8_DRAWS})")
    ap.add_argument("--block", type=int, default=DEFAULT_BLOCK)
    ap.add_argument("--seed", type=int, default=None, help=f"bootstrap seed ({DEFAULT_SEED}; v8: {V8_SEED})")
    ap.add_argument("--dsr-n", type=int, default=DEFAULT_DSR_N,
                    help="trials N of the deflated Sharpe ratio (R6': 10; >= 2); V[SR_n] comes from the dirs given")
    ap.add_argument("--json", default=None, help="also write the per-dir results as JSON")
    ap.add_argument("--ledger", default=None, help="append the listed dirs to this atx.trial-ledger/v1 JSONL")
    ap.add_argument("--ledger-kind", default="construction", choices=BI.LEDGER_KINDS)
    ap.add_argument("--ledger-count", type=int, default=1, help="trials each appended line stands for (default 1)")
    ap.add_argument("--ledger-note", default=None)
    ap.add_argument("--ledger-n", default=None, help="print the Appendix A trial counts (kind x window) of a ledger")
    ap.add_argument("--effective-n", default=None, help='effective-N DSR from "dirs" or a ledger path')
    ap.add_argument("--onc-init", type=int, default=10, help="ONC k-means restarts (default 10)")
    ap.add_argument("--onc-seed", type=int, default=DEFAULT_SEED)
    ap.add_argument("--pbo", nargs="*", default=None, help="CSCV PBO over these cells (default: the listed dirs)")
    ap.add_argument("--pbo-blocks", type=int, default=BI.PBO_BLOCKS)
    ap.add_argument("--pbo-max-splits", type=int, default=0,
                    help="0 = every split (the engine CSCV); K > 0 is refused")
    ap.add_argument("--pbo-json", default=None, help="write the PBO result (with every split logit) as JSON")
    ap.add_argument("--psr", action="store_true", help="PSR and MinTRL of every listed dir")
    ap.add_argument("--psr-benchmarks", type=float_list, default=[0.0, 0.5], help="annualized SR* list (0,0.5)")
    ap.add_argument("--psr-alpha", type=float, default=0.05)
    ap.add_argument("--protocol", choices=PROTOCOLS, default="v7",
                    help="v8: pre-registered bootstrap (seed 20260929, 4,999 draws), year tables, origin + window_id + "
                         "hash chain in ledger lines, v8 Appendix A block")
    ap.add_argument("--origin", choices=BI.ORIGINS, default=None, help="origin class of the ledgered cells (K5)")
    ap.add_argument("--rerun-of", default=None, help="the appended line re-runs this trial_id")
    ap.add_argument("--rerun-basis", choices=BI.RERUN_BASES, default=None,
                    help="window: longer window, no new trial; blind / returns: the defect rule (v8-prereg item 7)")
    ap.add_argument("--ledger-defect", default=None, help="REASON: the appended line is an invalid cell (N excludes it)")
    ap.add_argument("--year-table", action="store_true", help="per-year table of every listed dir")
    ap.add_argument("--dsr-ledger", default=None, help="DSR with N and V[SR] from this ledger (OD-4)")
    ap.add_argument("--dsr-total", default=None, metavar="LEDGER",
                    help="total-count DSR: N_tot = every trial of LEDGER + campaign registry counts (v8x-prereg)")
    ap.add_argument("--dsr-hand", action="append", default=[], metavar="DIR",
                    help="with --dsr-total: DSR_hand of a ledgered book by the prefix rule (the ledger prefix ending "
                         "at its line)")
    ap.add_argument("--bundle", nargs=2, default=None, metavar=("BASE", "FINAL"),
                    help="cumulative paired test FINAL vs BASE with its verdict")
    ap.add_argument("--bundle-alpha", type=float, default=BUNDLE_ALPHA)
    ap.add_argument("--bundle-json", default=None, help="write the --bundle result as JSON")
    ap.add_argument("--pool", nargs="+", default=None, metavar="DIR",
                    help="era NAV dirs in date order: one pooled row after the listed dirs (task H-1)")
    ap.add_argument("--pool-ids", default=None, help="ID,... of the --pool eras (default E1,E2,...)")
    ap.add_argument("--pool-reference", nargs="+", default=None, metavar="DIR",
                    help="the reference's era NAV dirs, pooled, for the pooled row's paired dSR (default --reference)")
    ap.add_argument("--pool-one-sided", action="store_true",
                    help="the pooled row's paired block adds the verb's one-sided p and the bundle verdict (OD-3 T2)")
    ap.add_argument("--era-of", default=None, metavar="TRIAL_ID",
                    help="a one-era history --pool with --ledger: the trial_id of the TRAIN cell it re-reads (P-1)")
    ap.add_argument("--engine-stats", nargs="?", const=None, default=None, metavar="EXE",
                    help="the engine verb atx-research-eval that computes the statistics of record (K-P9-5; default "
                         "$ATX_EQUITY_BIN when set, refused there without it, else build-equity/bin of this checkout)")
    argv = list(sys.argv[1:] if argv is None else argv)
    args = ap.parse_args(argv)
    v8 = args.protocol == "v8"
    args.draws = (V8_DRAWS if v8 else DEFAULT_DRAWS) if args.draws is None else args.draws
    args.seed = (V8_SEED if v8 else DEFAULT_SEED) if args.seed is None else args.seed
    args.year_table = args.year_table or v8
    if args.dsr_n < 2:
        ap.error("--dsr-n must be >= 2")
    if not args.dirs and not args.ledger_n and not args.pbo and not args.bundle and not args.pool:
        ap.error("give NAV dirs (or --ledger-n LEDGER / --pbo CELL ...)")
    if args.ledger_count < 1 or args.onc_init < 1 or not 0 < args.psr_alpha < 0.5:
        ap.error("--ledger-count and --onc-init must be >= 1; --psr-alpha in (0, 0.5)")
    if (args.ledger or args.psr or args.effective_n == "dirs") and not (args.dirs or args.pool):
        ap.error("--ledger / --psr / --effective-n dirs need NAV dirs")
    if (args.pool_ids or args.pool_reference) and not args.pool:
        ap.error("--pool-ids / --pool-reference need --pool")
    if args.pool_one_sided and not (args.pool and (args.pool_reference or args.reference)):
        ap.error("--pool-one-sided needs --pool and a reference (--pool-reference or --reference)")
    if args.era_of is not None and not (args.pool and args.ledger):
        ap.error("--era-of needs --pool and --ledger (the ledger line of a one-era history read)")
    if (args.rerun_of is None) != (args.rerun_basis is None):
        ap.error("--rerun-of and --rerun-basis go together")
    if (args.origin or args.rerun_of or args.ledger_defect) and not args.ledger:
        ap.error("--origin / --rerun-of / --ledger-defect need --ledger")
    if v8 and args.ledger and (args.dirs or args.pool) and not args.origin:
        ap.error("--protocol v8 records the origin class in every ledger line: give --origin prior|grid|mined (K5)")
    if args.dsr_ledger and not (args.dirs or args.pool):
        ap.error("--dsr-ledger needs NAV dirs")
    if args.dsr_total and (not args.dirs or args.pool):
        ap.error("--dsr-total needs NAV dirs and reads TRAIN dirs only (no --pool)")
    if args.dsr_hand and not args.dsr_total:
        ap.error("--dsr-hand needs --dsr-total LEDGER")
    if args.bundle_json and not args.bundle:
        ap.error("--bundle-json needs --bundle BASE FINAL")
    if not 0 < args.bundle_alpha < 0.5:
        ap.error("--bundle-alpha in (0, 0.5)")
    if args.pbo_max_splits:
        ap.error("--pbo-max-splits: the engine CSCV is exhaustive (no seeded subsample)")
    args.engine = None          # the verb, opened on first use by engine_of (review B2 S8)
    weights = load_weights(args.weights)
    ref_nets = None
    if args.reference:
        ref_summary = load_summary(Path(args.reference))
        ref_nets = net_series(load_daily(Path(args.reference), scenario_of(ref_summary, args.scenario)["scenario"]))
    # every dir first: V[SR_n] of the deflated Sharpe ratio spans all the NAV dirs on the command line
    analysed = [analyse(Path(d), args, weights, ref_nets) for d in args.dirs]
    if args.pool:  # task H-1: the pooled row of the era dirs, after the listed dirs (one trial in V[SR_n] and N)
        pool_ref = ref_nets
        if args.pool_reference:
            ids = [f"R{k}" for k in range(1, len(args.pool_reference) + 1)]
            pool_ref = net_series(pooled_daily(args.pool_reference, ids, args)[0])
        analysed.append(analyse_pool(args.pool, pool_ids(args), args, weights, pool_ref))
    results = [r for r, _ in analysed]
    names = list(args.dirs) + ([results[-1]["dir"]] if args.pool else [])
    warnings = []
    if results:
        rows = engine_of(args).deflated_rows([nets for _, nets in analysed], args.dsr_n)
        for r, q in zip(results, rows):
            r["deflated"] = q
        warnings = listing_warnings(names, results, [nets for _, nets in analysed], args.dsr_n)
    for w in warnings:
        print(f"nav_summ: WARNING {w}", file=sys.stderr)
    for d, r in zip(names, results):
        if d in args.dirs:
            print_scenarios(load_summary(Path(d)))
        print_analysis(r, " + ".join(args.pool_reference) if args.pool_reference and "pool" in r else args.reference)
        if "pool" in r and d not in args.dirs:
            print_pool(r)
        if "year_table" in r:
            print_year_table(r["year_table"])
    integrity(args, argv, results, analysed)
    if args.json:
        run = run_provenance(argv, warnings, args.engine)
        for r in results:
            r["nav_summ_run"] = run
        Path(args.json).write_text(json.dumps(results, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if args.bundle:
        res = bundle(Path(args.bundle[0]), Path(args.bundle[1]), args)
        print_bundle(res)
        if args.bundle_json:
            doc = dict(res, protocol=args.protocol, nav_summ_run=run_provenance(argv, [], args.engine))
            Path(args.bundle_json).write_text(json.dumps(doc, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
