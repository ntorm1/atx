"""The research ledger rules fixture (platform P9 lane B2; migration slice 8, carry SQL-7).

A synthetic atx.trial-ledger/v1 file that exercises every trial-counting rule and the hash chain, with the values
today's Python readers of record compute on it, so the engine's research/ledger library (gtest
ResearchLedger.TrialCountsEqualPython, atx-engine-research-ledger-tests) is tied to them.

  python generate_research_ledger.py --write    regenerate ledger.jsonl and expected.json (a deliberate change)
  python generate_research_ledger.py --check    recompute expected.json from the committed ledger (exit 0 = tied)

Files (expected.json pins ledger.jsonl's SHA-256; .gitattributes keeps the bytes):
  ledger.jsonl    20 lines and one blank line: legacy (unchained) lines first, then chained ones (prev_sha256 =
                  backtest_integrity.chain_head of the lines before). It holds every kind the rules read: a line
                  without kind, admission, protocol, defect, validation, a mining campaign with a registry count, an
                  invalid cell, window / blind / returns re-runs, an era shard, an "era" line, two pool lines (one
                  a history read, one inside TRAIN), a cell on another window and a NaN Sharpe.
  expected.json   the values below, each from the Python called as a library (nothing here re-implements a rule):
                  backtest_integrity.chain_head of every prefix and ledger_head, trial_counts, invalid_ids,
                  history_read_lines and excluded_lines (as line indices), campaign_registry_count, ledger_n,
                  dsr_variance on two windows, dsr_total.trial_breakdown / construction_ids / prefix_length, and
                  the trial identities (trial_id, pooled_trial_id, era_pool.pooled_sha256 / pool_label).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[3]
TOOLS = REPO / "atx-impl" / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))
import backtest_integrity as BI  # noqa: E402
import dsr_total as DT  # noqa: E402

SCHEMA = "atx.research-ledger-fixture/v1"
LEDGER, EXPECTED = "ledger.jsonl", "expected.json"
WINDOW, OLD_WINDOW = "ledger-fx-v1", "ledger-fx-v0"
TRAIN = {"label": "TRAIN", "first_session": "2020-01-02", "last_session": "2023-12-29"}
SHA_A, SHA_B = "a" * 64, "b" * 64


def tid(k: int) -> str:
    return f"{k:016x}"


def records() -> list[dict]:
    """The lines in ledger order (the first five unchained, the rest chained by ``lines``)."""
    s = BI.LEDGER_SCHEMA
    return [
        {"schema": s, "kind": "construction", "trial_id": tid(1), "cell": "A", "count": 1, "s2_net_sr": 0.8,
         "window": TRAIN},
        {"schema": s, "kind": "construction", "trial_id": tid(2), "cell": "B", "s2_net_sr": 1.1, "window": TRAIN},
        {"schema": s, "kind": "admission", "trial_id": tid(3), "cell": "adm", "count": 5, "window": TRAIN},
        {"schema": s, "trial_id": tid(4), "cell": "C", "count": 2, "s2_net_sr": 0.5, "window": TRAIN},
        {"schema": s, "kind": "construction", "trial_id": tid(5), "cell": "C2", "s2_net_sr": -0.2, "window": TRAIN},
        {"schema": s, "kind": "protocol", "note": "window change", "window_id": WINDOW},
        {"schema": s, "kind": "construction", "trial_id": tid(6), "cell": "D", "s2_net_sr": 1.3,
         "window_id": WINDOW, "window": TRAIN},
        {"schema": s, "kind": "construction", "trial_id": tid(7), "cell": "E", "s2_net_sr": float("nan"),
         "window_id": WINDOW, "window": TRAIN},
        {"schema": s, "kind": "construction", "trial_id": tid(8), "cell": "F", "s2_net_sr": 0.9, "window_id": WINDOW,
         "window": TRAIN, "defect": {"invalid": True, "reason": "fixture"}},
        {"schema": s, "kind": "construction", "trial_id": tid(9), "cell": "G", "s2_net_sr": 1.4, "window_id": WINDOW,
         "window": TRAIN, "rerun_of": tid(6), "rerun_basis": "window"},
        {"schema": s, "kind": "construction", "trial_id": tid(10), "cell": "H", "s2_net_sr": 1.0,
         "window_id": WINDOW, "window": TRAIN},
        {"schema": s, "kind": "defect", "defect_of": tid(10), "reason": "fixture", "date": "2026-10-03",
         "ruling": "FX-1"},
        {"schema": s, "kind": "construction", "trial_id": tid(11), "cell": "I", "s2_net_sr": 0.6,
         "window_id": WINDOW, "window": TRAIN, "rerun_of": tid(10), "rerun_basis": "blind"},
        {"schema": s, "kind": "construction", "trial_id": tid(12), "cell": "J", "s2_net_sr": 0.75,
         "window_id": WINDOW, "window": TRAIN, "rerun_of": tid(8), "rerun_basis": "returns"},
        {"schema": s, "kind": "mining-campaign", "trial_id": tid(13), "campaign": "fx", "registry": {"count": 40}},
        {"schema": s, "kind": "construction", "trial_id": tid(14), "cell": "D@E1", "era_of": tid(6), "era": "E1",
         "s2_net_sr": 2.0, "window_id": WINDOW, "window": {"label": "ERA E1", "first_session": "2012-01-03",
                                                           "last_session": "2012-12-31"}},
        {"schema": s, "kind": "construction", "trial_id": tid(15), "cell": "pool:old", "s2_net_sr": 0.4,
         "window_id": WINDOW, "window": {"label": "POOL", "first_session": "2008-01-02",
                                         "last_session": "2019-12-31"}},
        {"schema": s, "kind": "construction", "trial_id": tid(16), "cell": "pool:train", "s2_net_sr": 0.95,
         "window_id": WINDOW, "window": {"label": "POOL", "first_session": "2020-01-02",
                                         "last_session": "2023-12-29"}},
        {"schema": s, "kind": "validation", "note": "hidden-block read", "ruling": "FX-2"},
        {"schema": s, "kind": "construction", "trial_id": tid(17), "cell": "K", "count": 3, "s2_net_sr": 0.7,
         "window_id": OLD_WINDOW, "window": TRAIN},
    ]


UNCHAINED = 5          # the first five lines are legacy (no prev_sha256)
BLANK_AFTER = 11       # a blank line follows the 11th line (the readers skip it)


def lines(recs: list[dict]) -> list[str]:
    out: list[str] = []
    for k, rec in enumerate(recs):
        rec = dict(rec)
        if k >= UNCHAINED:
            rec["prev_sha256"] = BI.chain_head(out)
        out.append(json.dumps(rec, sort_keys=True))
    return out


def ledger_text(text_lines: list[str]) -> str:
    rows = list(text_lines)
    rows.insert(BLANK_AFTER, "   ")
    return "\n".join(rows) + "\n"


def read_lines(directory: Path = HERE) -> list[str]:
    return [line for line in (directory / LEDGER).read_text(encoding="utf-8").splitlines() if line.strip()]


def indices(recs: list[dict], subset: list[dict]) -> list[int]:
    ids = {id(r) for r in subset}
    return [k for k, r in enumerate(recs) if id(r) in ids]


def finite(x):
    return x if x is None or math.isfinite(x) else None


def compute(directory: Path = HERE) -> dict:
    path = directory / LEDGER
    recs = BI.ledger_read(path)
    text = read_lines(directory)
    ids = [tid(k) for k in (1, 6, 10, 11, 12, 16, 17)] + ["ffffffffffffffff"]
    prefix = {}
    for t in ids:
        try:
            prefix[t] = DT.prefix_length(recs, t)
        except ValueError:
            prefix[t] = None
    variance = {}
    for w in (WINDOW, OLD_WINDOW):
        v = BI.dsr_variance(recs, w)
        variance[w] = {k: finite(v[k]) if isinstance(v[k], float) else v[k] for k in v}
    pooled = [SHA_A, SHA_B]
    return {
        "lines": len(text),
        "prefix_heads": [BI.chain_head(text[:k]) for k in range(1, len(text) + 1)],
        "ledger_head": BI.ledger_head(path),
        "trial_counts": BI.trial_counts(recs),
        "invalid_ids": sorted(str(x) for x in BI.invalid_ids(recs)),
        "history_read_lines": indices(recs, BI.history_read_lines(recs)),
        "excluded_lines": indices(recs, BI.excluded_lines(recs)),
        "campaign_registry_count": BI.campaign_registry_count(recs),
        "ledger_n": {"construction_in": BI.ledger_n(recs, True), "construction_out": BI.ledger_n(recs, False),
                     "admission_in": BI.ledger_n(recs, True, "admission")},
        "trial_breakdown": DT.trial_breakdown(recs),
        "dsr_variance": variance,
        "construction_ids": sorted(str(x) for x in DT.construction_ids(recs)),
        "prefix_length": prefix,
        "identities": {
            "trial_id_construction_a": BI.trial_id("construction", SHA_A),
            "trial_id_admission_b": BI.trial_id("admission", SHA_B),
            "pooled_sha256_one": BI.era_pool().pooled_sha256([SHA_A]),
            "pooled_sha256_two": BI.era_pool().pooled_sha256(pooled),
            "pooled_trial_id_two": BI.pooled_trial_id("construction", pooled),
            "pool_label": BI.era_pool().pool_label(["navs/e1", "navs\\e2"]),
        },
    }


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def document(directory: Path = HERE) -> dict:
    return {"schema": SCHEMA, "generator": "atx-engine/tests/fixtures/research_ledger/generate_research_ledger.py",
            "ledger": {"file": LEDGER, "sha256": sha256_bytes((directory / LEDGER).read_bytes()),
                       "window_id": WINDOW, "old_window_id": OLD_WINDOW, "unchained": UNCHAINED,
                       "blank_physical_line": BLANK_AFTER + 1},
            "values": compute(directory)}


def encode(doc) -> bytes:
    return (json.dumps(doc, indent=1, sort_keys=True, allow_nan=False) + "\n").encode("utf-8")


def write(directory: Path = HERE) -> dict:
    (directory / LEDGER).write_bytes(ledger_text(lines(records())).encode("utf-8"))
    doc = document(directory)
    (directory / EXPECTED).write_bytes(encode(doc))
    return doc


def check(directory: Path = HERE) -> list[str]:
    want = json.loads((directory / EXPECTED).read_bytes())
    got = json.loads(encode(document(directory)))
    return [] if want == got else ["expected.json differs from today's Python on the committed ledger"]


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="the research ledger rules fixture (P9 B2)")
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true")
    mode.add_argument("--check", action="store_true")
    a = ap.parse_args(argv)
    if a.write:
        write()
        return 0
    problems = check()
    for p in problems:
        print(f"research_ledger: {p}", file=sys.stderr)
    print("research_ledger: tied" if not problems else "research_ledger: differs")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
