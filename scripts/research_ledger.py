"""The trial ledger as research_cycle.py reads and extends it (platform v8 lane A, task A-3).

The ledger (``build-equity/trials.jsonl``, schema ``atx.trial-ledger/v1``) is append-only JSONL, one compact sorted-key
JSON object per line, written by nav_summ (backtest_integrity.ledger_append: one line per NAV cell, deduplicated by
``trial_id``). A **protocol** line (kind ``protocol``: a research-window change under an owner ruling) is not a trial:
it has no ``cell`` and ``count`` 0, so it never raises the cross-cell N (nav_summ's Appendix A sums ``count``; its
series readers skip lines without a ``series``). It stays in the file like every line and is skipped when listing the
ledger's cells.

One N, one append (PM ruling 2026-09-29, lane G task 1): both go through the validation kit's ledger module,
atx-impl/tools/backtest_integrity.py (``backtest_integrity()`` below, loaded as the nav_summ shim loads the moved
tools: that directory on sys.path, imported by name, on first use only).
  N       ``summ.dsr_n: "ledger+1"`` resolves to ``backtest_integrity.ledger_n``: the construction trials by the defect
          rule (``trial_counts``: protocol lines, window re-runs, invalid and blind-replaced cells add 0) plus 1 when
          the scored cell has no line yet -- the N nav_summ --dsr-ledger prints. The scored cell is matched as
          nav_summ matches it (the trial_id of its primary daily CSV) once its NAV output exists, by its cell name
          before that (plan time).
  append  the protocol line is written by ``backtest_integrity.ledger_append(..., chain=True)``, nav_summ's append:
          the ledger's hash chain is verified first and the line carries ``prev_sha256`` (ledger-chain-v1), so it is
          protected the moment it is written.

  research_cycle.py ledger-protocol --ledger PATH --owner-ruling TEXT --date YYYY-MM-DD
                                    [--window-id ID] [--research-window FILE] [--root R]

appends one protocol line {schema, kind, count 0, window_id, owner_ruling, date, research_window_sha256, trial_id,
prev_sha256}; the window id defaults to the W0-1 window's, the window file to atx-impl/strategies/research_window.json.
The same line is never appended twice (same trial_id).
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
NON_TRIAL_KINDS = (PROTOCOL,)           # ledger lines that are no trial: skipped by cells()
N_KIND = "construction"                 # the kind whose trials make N (nav_summ --dsr-ledger)
SHA_RE = re.compile(r"[0-9a-f]{64}")
TOOLS = research_tree.REPO / "atx-impl" / "tools"


class LedgerError(ValueError):
    pass


def backtest_integrity():
    """atx-impl/tools/backtest_integrity.py, the validation kit's ledger module (one N, one append), imported as the
    nav_summ shim imports the moved tools: that directory on sys.path, the module by name. On first use only (numpy)."""
    tools = str(TOOLS)
    if tools not in sys.path:
        sys.path.insert(0, tools)
    import backtest_integrity as BI  # noqa: PLC0415
    return BI


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


def scored_trial_id(nav_dir: Path | None) -> str | None:
    """nav_summ's identity of a NAV cell: trial_id(construction, SHA-256 of its primary daily CSV); None while the NAV
    output (summary.json and that CSV) does not exist."""
    if nav_dir is None:
        return None
    try:
        summary = json.loads((Path(nav_dir) / "summary.json").read_text(encoding="utf-8"))
        daily = Path(nav_dir) / f"daily_{summary['primary_scenario']}.csv"
    except (OSError, ValueError, KeyError, TypeError):
        return None
    if not daily.is_file():
        return None
    bi = backtest_integrity()
    return bi.trial_id(N_KIND, bi.sha256_file(daily))


def ledger_n(path: Path, cell: str, nav_dir: Path | None = None) -> int:
    """N for summ.dsr_n "ledger+1": backtest_integrity.ledger_n over every line (the defect rule), the scored cell
    matched by its daily series' trial_id once ``nav_dir`` holds the NAV output (nav_summ's rule), else by its name."""
    records = [rec for _, rec in read_lines(path)]
    tid = scored_trial_id(nav_dir)

    def scored(rec: dict) -> bool:
        if rec.get("kind", N_KIND) != N_KIND:
            return False
        return rec["trial_id"] == tid if tid is not None and "trial_id" in rec else rec.get("cell") == cell
    return backtest_integrity().ledger_n(records, any(scored(rec) for rec in records), N_KIND)


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


def append(path: Path, rec: dict) -> dict | None:
    """Append one line through nav_summ's append (backtest_integrity.ledger_append, chained: the chain is verified
    first and the line carries prev_sha256) unless a line with its trial_id is present; never rewrites. Returns the
    line as written, or None when it was already present."""
    appended, _ = backtest_integrity().ledger_append(Path(path), [rec], chain=True)
    return appended[0] if appended else None


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
        written = append(ledger, rec)
    except (ValueError, LookupError) as exc:  # LedgerError, a schema or hash-chain refusal of the ledger
        print(f"research_cycle ledger-protocol: {exc}", file=sys.stderr)
        return 2
    print(("appended" if written else "already present (not appended)") +
          f": {json.dumps(written or rec, sort_keys=True)}")
    return 0
