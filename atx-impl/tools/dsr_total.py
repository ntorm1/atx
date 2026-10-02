"""Total-count deflated Sharpe ratio (platform v8 expansion X: v8x-prereg.md section 4, precondition P5).

Rulings PM7-1, PM7-2 and PM7-7. The deflated Sharpe ratio of an X book is computed on the TOTAL trial count of the
ledger of record. A trial that is not counted could raise the Sharpe ratio without lowering its deflated value.

  N_tot = sum(trial_counts(records))            every line the defect rule counts, of any kind (construction,
                                                admission and the legacy kinds); event, mining-campaign, era and
                                                history lines add 0 there
        + campaign_registry_count(records)      every mining campaign's evaluations (Ruling E-33a registry.count)
        + 1 when the scored book has no construction line in the ledger yet (ledger_n's convention)
  V     = dsr_variance(records, window_id)["variance_sr"]   the ddof-1 variance of s2_net_sr / sqrt(252) over the
                                                construction lines scored on the research window (book cells only;
                                                defect-rule exclusions and history lines are left out)
  SR0   = expected_max_sr(V, N)
  DSR   = deflated_sharpe(SR, T, skew, kurtosis, SR0)   (per-session units; backtest_integrity)

Rows:
- ``tot``: N_tot of the whole ledger. This is DSR_tot, the value the X gate reads.
- ``v8``: N = ledger_n(records, ...), the construction count of v8 rule 3, with the same V. This is the value
  ``nav_summ --dsr-ledger`` prints.
- ``hand``: N_tot and V of the ledger prefix that ends at a named book's own construction line. Applied to the last
  hand-written book H-F this is DSR_hand (PM7-2). The same rule gives any ledgered book's value at its own ledger
  state, for example V8-F's.

The functions are pure over ledger records and net moments, except ``read_ledger``. nav_summ.py reads the NAV
directories (flags --dsr-total and --dsr-hand). Without those flags nothing here runs.
"""
from __future__ import annotations

import math
from pathlib import Path

import backtest_integrity as BI

CONSTRUCTION = "construction"


def fmt(v, spec: str) -> str:
    return format(v, spec) if isinstance(v, (int, float)) else "na"


def read_ledger(path: Path) -> dict:
    """Read the ledger of record and verify its hash chain.

    Returns {"path", "records", "lines" (the stored non-blank line texts), "chain_head"}. A missing file is refused
    (ValueError): backtest_integrity.ledger_read reads an absent ledger as empty, and here that would print a DSR at
    N 1."""
    p = Path(path)
    if not p.is_file():
        raise ValueError(f"--dsr-total: ledger {p} does not exist (N_tot and V are read from it; an absent ledger "
                         "would read as N 0)")
    records = BI.ledger_read(p)                    # schema-checked, hash chain verified (ValueError on a break)
    lines = [line for line in p.read_text(encoding="utf-8").splitlines() if line.strip()]
    if len(lines) != len(records):
        raise ValueError(f"--dsr-total: ledger {p}: {len(lines)} non-blank lines but {len(records)} records")
    return {"path": str(p), "records": records, "lines": lines, "chain_head": BI.ledger_head(p)}


def trial_breakdown(records: list[dict]) -> dict:
    """N_tot of the lines and its parts: the trials of each kind by the defect rule (``trial_counts``; a line
    without a kind is a construction line, as in ``ledger_n``), and the campaigns' registry counts."""
    counts = BI.trial_counts(records)
    by_kind: dict[str, int] = {}
    for rec, c in zip(records, counts):
        if c:
            kind = rec.get("kind", CONSTRUCTION)
            by_kind[kind] = by_kind.get(kind, 0) + c
    m = BI.campaign_registry_count(records)
    return {"by_kind": dict(sorted(by_kind.items())), "ledger_trials": sum(counts), "campaign_registry": m,
            "n_tot": sum(counts) + m}


def dsr_at(moments: dict, n: int, variance: float | None) -> dict:
    """SR0 and DSR of one book at N trials and cross-trial variance V (per-session units). Values are None when V is
    undefined (fewer than two cells on the window), when the series has no defined moments, or when N < 2."""
    row = {"n": n, "sr0_daily": None, "sr0_annual": None, "dsr": None}
    sr, t, skew, kurt = moments["sr_daily"], moments["sessions"], moments["skew"], moments["kurtosis"]
    if variance is None or sr is None or skew is None or kurt is None or t < 2 or n < 2:
        return row
    sr0 = BI.expected_max_sr(variance, n)
    row.update(sr0_daily=sr0, sr0_annual=sr0 * math.sqrt(BI.ANNUAL),
               dsr=BI.deflated_sharpe(sr, t, skew, kurt, sr0))
    return row


def ledger_rows(moments: dict, records: list[dict], in_ledger: bool, window_id: str) -> dict:
    """The ``tot`` and ``v8`` rows of one book over these ledger lines, with N_tot's parts and V.

    ``in_ledger`` says whether the book has a construction line among ``records``. When it has none, both counts add
    1 for the book itself, as ledger_n does."""
    v = BI.dsr_variance(records, window_id)
    parts = trial_breakdown(records)
    n_tot = parts["n_tot"] + (0 if in_ledger else 1)
    n_c = BI.ledger_n(records, in_ledger)
    return {"window_id": window_id, "in_ledger": in_ledger, "variance_sr": v["variance_sr"], "cells_in_v": v["cells"],
            "variance_source": v["variance_source"], "parts": parts,
            "tot": dsr_at(moments, n_tot, v["variance_sr"]), "v8": dsr_at(moments, n_c, v["variance_sr"])}


def construction_ids(records: list[dict]) -> set:
    return {r.get("trial_id") for r in records if r.get("kind") == CONSTRUCTION}


def prefix_length(records: list[dict], trial_id: str) -> int:
    """The number of ledger lines up to and including the first construction line with this trial_id. A book
    without a construction line cannot have a ledger state, so that case is refused (ValueError)."""
    for k, rec in enumerate(records):
        if rec.get("kind") == CONSTRUCTION and rec.get("trial_id") == trial_id:
            return k + 1
    raise ValueError(f"--dsr-hand: trial {trial_id} has no construction line in the ledger (DSR_hand is read on the "
                     "ledger prefix that ends at the book's own line; ledger the book first)")


def hand_row(moments: dict, ledger: dict, trial_id: str, window_id: str) -> dict:
    """DSR_hand: the book's total-count DSR on the ledger prefix that ends at its own construction line (PM7-2).

    N_tot, V and the chain head are all taken from that prefix. Every line after it is left out, the campaign line
    of an X campaign that ran after H-F included."""
    k = prefix_length(ledger["records"], trial_id)
    rows = ledger_rows(moments, ledger["records"][:k], True, window_id)
    return dict(rows, trial_id=trial_id, prefix_lines=k, prefix_chain_head=BI.chain_head(ledger["lines"][:k]))


def parts_text(rows: dict) -> str:
    p = rows["parts"]
    kinds = " + ".join(f"{kind} {n}" for kind, n in p["by_kind"].items()) or "no ledger trial"
    own = "" if rows["in_ledger"] else " + 1 (this cell, not yet ledgered)"
    return f"{kinds} + campaign registry {p['campaign_registry']}{own}"


def moments_text(m: dict) -> str:
    sr = m["sr_daily"] * math.sqrt(BI.ANNUAL) if m["sr_daily"] is not None else None
    return (f"SR {fmt(sr, '+.3f')} ann, T {m['sessions']}, skew {fmt(m['skew'], '+.3f')}, "
            f"kurtosis {fmt(m['kurtosis'], '.3f')}")


def variance_text(rows: dict) -> str:
    return f"V[SR] {fmt(rows['variance_sr'], '.3e')} from {rows['cells_in_v']} cells ledgered on {rows['window_id']}"


def format_book(label: str, moments: dict, ledger: dict, rows: dict) -> list[str]:
    """The printed lines of one book's ``tot`` and ``v8`` rows (one block per --dsr-total dir)."""
    t, v8 = rows["tot"], rows["v8"]
    return [f"== total-count DSR {label} (ledger {ledger['path']}, {len(ledger['records'])} lines, chain head "
            f"{ledger['chain_head'][:16]})",
            f"   DSR_tot (N_tot={t['n']}): DSR {fmt(t['dsr'], '.4f')} vs SR0 {fmt(t['sr0_annual'], '.3f')} ann | "
            f"N_tot = {parts_text(rows)} | {variance_text(rows)} | {moments_text(moments)}",
            f"   DSR_v8 (N_c={v8['n']}, v8 rule 3 as --dsr-ledger): DSR {fmt(v8['dsr'], '.4f')} vs SR0 "
            f"{fmt(v8['sr0_annual'], '.3f')} ann | same V"]


def format_hand(label: str, moments: dict, row: dict) -> list[str]:
    """The printed lines of one --dsr-hand book."""
    t = row["tot"]
    return [f"== DSR_hand {label}: ledger prefix of {row['prefix_lines']} lines ending at its construction line "
            f"{row['trial_id']} (chain head {row['prefix_chain_head'][:16]})",
            f"   DSR_hand (N_tot={t['n']}): DSR {fmt(t['dsr'], '.4f')} vs SR0 {fmt(t['sr0_annual'], '.3f')} ann | "
            f"N_tot = {parts_text(row)} | {variance_text(row)} | {moments_text(moments)}"]
