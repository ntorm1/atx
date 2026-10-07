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
  SR0, DSR the house DSR at (N, V) (per-session units)

Rows:
- ``tot``: N_tot of the whole ledger. This is DSR_tot, the value the X gate reads.
- ``v8``: N = ledger_n(records, ...), the construction count of v8 rule 3, with the same V. This is the value
  ``nav_summ --dsr-ledger`` prints.
- ``tot_minus_m`` (P9 ruling PRE-15): DSR_hand at N = N_tot - M, the whole ledger's trials by the defect rule (+ 1 when
  not yet ledgered) without the mining campaigns' registry counts M, with the same V: v8y's YP-2 reading ("what
  dropping the campaigns gives"), printed for every --dsr-total dir.
- ``hand`` (the prefix rule, v8x PM7-2): N_tot and V of the ledger prefix that ends at a named book's own construction
  line. Applied to the last hand-written book H-F this was v8x's DSR_hand. The same rule gives any ledgered book's
  value at its own ledger state, for example V8-F's. Printed labelled "prefix rule" beside the PRE-15 row.

Platform P9 B2 (K-P9-5): the rows are computed by the engine verb atx-research-eval (dsr.tot and hand of its document;
research/ledger trial_breakdown, dsr_variance, ledger_n and eval house_dsr), which nav_summ.py calls. What stays here
is the ledger read for --dsr-total, the N_tot breakdown and prefix rules the readers share, and the printed lines.
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


def hand_parts_text(rows: dict) -> str:
    """N_tot - M's parts: the trials by the defect rule, without the campaigns' registry counts (PRE-15)."""
    p = rows["parts"]
    kinds = " + ".join(f"{kind} {n}" for kind, n in p["by_kind"].items()) or "no ledger trial"
    own = "" if rows["in_ledger"] else " + 1 (this cell, not yet ledgered)"
    return f"{kinds}{own}; campaign registry {p['campaign_registry']} left out"


def format_book(label: str, moments: dict, ledger: dict, rows: dict) -> list[str]:
    """The printed lines of one book's ``tot``, ``v8`` and ``tot_minus_m`` rows (one block per --dsr-total dir)."""
    t, v8, h = rows["tot"], rows["v8"], rows["tot_minus_m"]
    return [f"== total-count DSR {label} (ledger {ledger['path']}, {len(ledger['records'])} lines, chain head "
            f"{ledger['chain_head'][:16]})",
            f"   DSR_tot (N_tot={t['n']}): DSR {fmt(t['dsr'], '.4f')} vs SR0 {fmt(t['sr0_annual'], '.3f')} ann | "
            f"N_tot = {parts_text(rows)} | {variance_text(rows)} | {moments_text(moments)}",
            f"   DSR_v8 (N_c={v8['n']}, v8 rule 3 as --dsr-ledger): DSR {fmt(v8['dsr'], '.4f')} vs SR0 "
            f"{fmt(v8['sr0_annual'], '.3f')} ann | same V",
            f"   DSR_hand (N_tot - M={h['n']}, ruling PRE-15): DSR {fmt(h['dsr'], '.4f')} vs SR0 "
            f"{fmt(h['sr0_annual'], '.3f')} ann | N_tot - M = {hand_parts_text(rows)} | same V"]


def format_hand(label: str, moments: dict, row: dict) -> list[str]:
    """The printed lines of one --dsr-hand book: the prefix rule (v8x PM7-2), labelled as such beside PRE-15's row."""
    t = row["tot"]
    return [f"== DSR_hand (prefix rule) {label}: ledger prefix of {row['prefix_lines']} lines ending at its "
            f"construction line {row['trial_id']} (chain head {row['prefix_chain_head'][:16]})",
            f"   DSR_hand (prefix rule, N_tot={t['n']}): DSR {fmt(t['dsr'], '.4f')} vs SR0 "
            f"{fmt(t['sr0_annual'], '.3f')} ann | N_tot = {parts_text(row)} | {variance_text(row)} | "
            f"{moments_text(moments)}"]
