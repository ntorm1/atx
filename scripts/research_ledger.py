"""The trial ledger as research_cycle.py reads and extends it (platform v8 lane A, task A-3).

The ledger (``build-equity/trials.jsonl``, schema ``atx.trial-ledger/v1``) is append-only JSONL, one compact sorted-key
JSON object per line, written by nav_summ (backtest_integrity.ledger_append: one line per NAV cell, deduplicated by
``trial_id``). A **protocol** line (kind ``protocol``: a research-window change under an owner ruling) is not a trial:
it has no ``cell`` and ``count`` 0, so it never raises the cross-cell N (nav_summ's Appendix A sums ``count``; its
series readers skip lines without a ``series``). It stays in the file like every line and is skipped when listing the
ledger's cells and when resolving ``summ.dsr_n: "ledger+1"``.

  research_cycle.py ledger-protocol --ledger PATH --owner-ruling TEXT --date YYYY-MM-DD
                                    [--window-id ID] [--research-window FILE] [--root R]

appends one protocol line {schema, kind, count 0, window_id, owner_ruling, date, research_window_sha256, trial_id};
the window id defaults to the W0-1 window's, the window file to atx-impl/strategies/research_window.json. The same
line is never appended twice (same trial_id).
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import research_tree  # noqa: E402

LEDGER_SCHEMA = "atx.trial-ledger/v1"
PROTOCOL = "protocol"
NON_TRIAL_KINDS = (PROTOCOL,)           # ledger lines that are no trial: skipped by cells() and dsr_n "ledger+1"
SHA_RE = re.compile(r"[0-9a-f]{64}")


class LedgerError(ValueError):
    pass


def read_lines(path: Path) -> list[tuple[int, dict]]:
    """(line number, object) of every non-empty line."""
    out = []
    for k, line in enumerate(Path(path).read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        try:
            rec = json.loads(line)
        except ValueError as exc:
            raise LedgerError(f"{path} line {k} is not a JSON object") from exc
        if not isinstance(rec, dict):
            raise LedgerError(f"{path} line {k} is not a JSON object")
        out.append((k, rec))
    return out


def cells(path: Path) -> list[str]:
    """The ledgered cells in ledger order (protocol lines skipped); a trial line without a cell is an error."""
    out = []
    for k, rec in read_lines(path):
        if rec.get("kind") in NON_TRIAL_KINDS:
            continue
        cell = rec.get("cell")
        if not isinstance(cell, str) or not cell:
            raise LedgerError(f"{path} line {k} has no cell")
        out.append(cell)
    return out


def protocol_line(window_id: str, owner_ruling: str, date: str, research_window_sha256: str) -> dict:
    if not all(isinstance(x, str) and x.strip() for x in (window_id, owner_ruling)):
        raise LedgerError("protocol line: window_id and owner_ruling must be non-empty")
    try:
        dt.date.fromisoformat(date)
    except (TypeError, ValueError) as exc:
        raise LedgerError(f"protocol line: date must be YYYY-MM-DD, got {date!r}") from exc
    if not isinstance(research_window_sha256, str) or not SHA_RE.fullmatch(research_window_sha256):
        raise LedgerError("protocol line: research_window_sha256 must be a SHA-256 hex digest")
    rec = {"schema": LEDGER_SCHEMA, "kind": PROTOCOL, "count": 0, "window_id": window_id,
           "owner_ruling": owner_ruling, "date": date, "research_window_sha256": research_window_sha256}
    ident = json.dumps([PROTOCOL, window_id, research_window_sha256, date], separators=(",", ":"))
    rec["trial_id"] = hashlib.sha256(ident.encode()).hexdigest()[:16]
    return rec


def append(path: Path, rec: dict) -> bool:
    """Append one line (backtest_integrity's encoding) unless a line with its trial_id is present; never rewrites."""
    path = Path(path)
    if path.exists() and any(r.get("trial_id") == rec["trial_id"] for _, r in read_lines(path)):
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(rec, sort_keys=True, separators=(",", ":")) + "\n")
    return True


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="research_cycle.py ledger-protocol", description=__doc__.split("\n", 1)[0])
    ap.add_argument("--ledger", required=True, help="the trial ledger (root-relative or absolute)")
    ap.add_argument("--owner-ruling", required=True)
    ap.add_argument("--date", required=True, help="YYYY-MM-DD of the ruling")
    ap.add_argument("--window-id", default=None, help="default: the W0-1 research window's id")
    ap.add_argument("--research-window", type=Path, default=research_tree.REPO / research_tree.WINDOW_JSON)
    ap.add_argument("--root", type=Path, default=research_tree.REPO)
    a = ap.parse_args(argv)
    try:
        window = a.research_window if a.research_window.is_absolute() else a.root / a.research_window
        if not window.is_file():
            raise LedgerError(f"no research window file {window} (task W0-1)")
        wid = a.window_id or research_tree.window_id()
        rec = protocol_line(wid, a.owner_ruling, a.date, hashlib.sha256(window.read_bytes()).hexdigest())
        ledger = Path(a.ledger) if Path(a.ledger).is_absolute() else a.root / a.ledger
        added = append(ledger, rec)
    except (LedgerError, LookupError) as exc:
        print(f"research_cycle ledger-protocol: {exc}", file=sys.stderr)
        return 2
    print(("appended" if added else "already present (not appended)") + f": {json.dumps(rec, sort_keys=True)}")
    return 0
