"""Hidden-block gate (platform v8 V-2): two bits about the frozen book on the hidden blocks, and nothing else.

Usage:
  holdout_gate.py --deploy MANIFEST --thresholds FILE --owner-ruling FILE --ledger TRIALS

Output (stdout, one line, exit 0): {"pass_2024": <bool>, "pass_2025_onward": <bool>}. No statistic, session count or
file is ever printed or written, except the one trial ledger line below. A refusal prints one reason on stderr (never
a statistic) and exits 2 before any daily NAV series is read (the owner ruling is checked before anything else); an
evaluation that fails after the series is opened prints a fixed message and exits 3.

The ruling in the trial ledger (review C-11): after every check and before any NAV series is opened, the gate appends
to the trial ledger (--ledger, build-equity/trials.jsonl) one chained ``validation`` line citing the ruling
{kind validation, count 0, book, owner_ruling {path (relative to the ledger's directory), sha256 of the file's
bytes}, owner, date, blocks, thresholds_sha256, deploy_manifest_sha256}; a second read under the same ruling bytes
appends nothing (same trial_id). The gate refuses a ruling whose bytes differ from the SHA-256 that a validation line
citing the same file records (the file was edited after a read under it). So every ruling -- and every thresholds
file, which a ruling pins -- the gate reads under leaves its line: a bisection of the thresholds is in the ledger.
A validation line is an event line (backtest_integrity.EVENT_KINDS): no cell, no series, it adds no trial.

OD-1 makes this tool the only reader of sessions at or after the research seal, and only under an owner ruling. It is
built and tested on synthetic NAV files; it is not run on real data in the v8 sprint.

The blocks come from research_window.json (task W0-1) ``hidden``: ``read_twice_at_book_level`` [begin, end) is the
2024 block, ``never_read`` [begin, open) the 2025-onward block.

Inputs (JSON; every key is checked, an unknown key refuses):
  deploy manifest   {"schema": "atx.holdout-deploy/v1", "book": NAME, "nav_dir": DIR, "scenario": NAME | null,
                     "composition_weights_sha256": SHA | null}
                    nav_dir holds a NAV replay of the frozen book over the hidden sessions (summary.json +
                    daily_<scenario>.csv as strategy_nav_replay.cpp writes them); scenario null = the summary's primary;
                    a weights SHA must equal the summary's composition_weights_sha256. A relative nav_dir is taken
                    relative to the manifest's directory.
  thresholds        {"schema": "atx.holdout-thresholds/v1", "min_sessions": INT >= 2, "net_sharpe_min": FLOAT,
                     "max_drawdown_max": FLOAT in (0, 1] (optional), "net_return_min": FLOAT (optional)}
                    A block passes when it has at least min_sessions return rows, its S2 net Sharpe (mean / sd ddof 1 x
                    sqrt 252 of the daily net returns, as nav_summ) is >= net_sharpe_min, and the optional drawdown and
                    compounded-return bounds hold. Anything short of that, a missing block included, is a fail.
  owner ruling      {"schema": "atx.holdout-owner-ruling/v1", "date": "YYYY-MM-DD", "owner": NAME, "text": TEXT,
                     "deploy_manifest_sha256": SHA, "thresholds_sha256": SHA, "blocks": ["2024", "2025_onward"]}
                    The ruling pins the bytes of both other files (SHA-256) and names both blocks; any mismatch refuses.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import math
import os
from pathlib import Path
import sys

import numpy as np

import backtest_integrity as BI
import nav_summ as NS

DEPLOY_SCHEMA = "atx.holdout-deploy/v1"
THRESHOLDS_SCHEMA = "atx.holdout-thresholds/v1"
RULING_SCHEMA = "atx.holdout-owner-ruling/v1"
BITS = ("pass_2024", "pass_2025_onward")
BLOCK_KEYS = {"2024": "read_twice_at_book_level", "2025_onward": "never_read"}   # ruling block -> window hidden key
EXIT_REFUSED, EXIT_FAILED = 2, 3
VALIDATION = BI.VALIDATION          # review C-11: the ledger line of a hidden-block read
_SHA_CHARS = set("0123456789abcdef")


class Refusal(Exception):
    """A reason to refuse; its text names inputs only, never a statistic."""


def _sha(value) -> bool:
    return isinstance(value, str) and len(value) == 64 and set(value) <= _SHA_CHARS


def _read(path: Path, what: str) -> tuple[dict, str]:
    try:
        data = Path(path).read_bytes()
    except OSError:
        raise Refusal(f"cannot read the {what} {path}") from None
    try:
        doc = json.loads(data)
    except ValueError:
        raise Refusal(f"the {what} {path} is not JSON") from None
    if not isinstance(doc, dict):
        raise Refusal(f"the {what} {path} is not a JSON object")
    return doc, hashlib.sha256(data).hexdigest()


def _keys(doc: dict, what: str, required: set, optional: set = frozenset()) -> None:
    missing, unknown = required - set(doc), set(doc) - required - set(optional)
    if missing or unknown:
        raise Refusal(f"the {what} has missing keys {sorted(missing)} or unknown keys {sorted(unknown)}")


def check_ruling(ruling: dict, deploy_sha: str, thresholds_sha: str) -> None:
    _keys(ruling, "owner ruling", {"schema", "date", "owner", "text", "deploy_manifest_sha256", "thresholds_sha256",
                                   "blocks"})
    if ruling["schema"] != RULING_SCHEMA:
        raise Refusal(f"the owner ruling's schema is not {RULING_SCHEMA}")
    try:
        dt.date.fromisoformat(ruling["date"])
    except (TypeError, ValueError):
        raise Refusal("the owner ruling's date is not YYYY-MM-DD") from None
    for key in ("owner", "text"):
        if not (isinstance(ruling[key], str) and ruling[key].strip()):
            raise Refusal(f"the owner ruling has no {key}")
    if not (_sha(ruling["deploy_manifest_sha256"]) and _sha(ruling["thresholds_sha256"])):
        raise Refusal("the owner ruling's SHA-256 pins are not lowercase hex digests")
    if ruling["deploy_manifest_sha256"] != deploy_sha:
        raise Refusal("the deploy manifest is not the one the owner ruling names (SHA-256 differs)")
    if ruling["thresholds_sha256"] != thresholds_sha:
        raise Refusal("the thresholds file is not the one the owner ruling names (SHA-256 differs)")
    if not isinstance(ruling["blocks"], list) or sorted(ruling["blocks"]) != sorted(BLOCK_KEYS):
        raise Refusal(f"the owner ruling must name exactly the blocks {sorted(BLOCK_KEYS)}")


def check_thresholds(doc: dict) -> dict:
    _keys(doc, "thresholds file", {"schema", "min_sessions", "net_sharpe_min"}, {"max_drawdown_max", "net_return_min"})
    if doc["schema"] != THRESHOLDS_SCHEMA:
        raise Refusal(f"the thresholds file's schema is not {THRESHOLDS_SCHEMA}")
    if not (isinstance(doc["min_sessions"], int) and not isinstance(doc["min_sessions"], bool)
            and doc["min_sessions"] >= 2):
        raise Refusal("thresholds min_sessions must be an integer >= 2")
    for key in ("net_sharpe_min", "max_drawdown_max", "net_return_min"):
        if key in doc and not (isinstance(doc[key], (int, float)) and not isinstance(doc[key], bool)
                               and math.isfinite(doc[key])):
            raise Refusal(f"thresholds {key} must be a finite number")
    if "max_drawdown_max" in doc and not 0 < doc["max_drawdown_max"] <= 1:
        raise Refusal("thresholds max_drawdown_max must be in (0, 1]")
    return doc


def check_deploy(doc: dict, base: Path) -> dict:
    _keys(doc, "deploy manifest", {"schema", "book", "nav_dir"}, {"scenario", "composition_weights_sha256"})
    if doc["schema"] != DEPLOY_SCHEMA:
        raise Refusal(f"the deploy manifest's schema is not {DEPLOY_SCHEMA}")
    if not (isinstance(doc["book"], str) and doc["book"].strip()):
        raise Refusal("the deploy manifest has no book name")
    if not (isinstance(doc["nav_dir"], str) and doc["nav_dir"].strip()):
        raise Refusal("the deploy manifest has no nav_dir")
    if doc.get("scenario") is not None and not isinstance(doc["scenario"], str):
        raise Refusal("the deploy manifest's scenario must be a name or null")
    pin = doc.get("composition_weights_sha256")
    if pin is not None and not _sha(pin):
        raise Refusal("the deploy manifest's composition_weights_sha256 is not a SHA-256 hex digest")
    nav = Path(doc["nav_dir"])
    return dict(doc, nav_dir=nav if nav.is_absolute() else base / nav)


def hidden_blocks() -> dict:
    """{ruling block: (begin_ns, end_ns or None)} from research_window.json ``hidden`` (W0-1)."""
    hidden = BI.research_window().load().get("hidden") or {}
    out = {}
    for block, key in BLOCK_KEYS.items():
        span = hidden.get(key)
        if not (isinstance(span, list) and len(span) == 2 and isinstance(span[0], str)
                and (span[1] is None or isinstance(span[1], str))):
            raise Refusal(f"research_window.json has no hidden.{key} block")
        out[block] = (_date_ns(span[0]), None if span[1] is None else _date_ns(span[1]))
    if out["2024"][1] is None or out["2024"][1] > out["2025_onward"][0]:
        raise Refusal("research_window.json hidden blocks overlap")
    return out


def _date_ns(text: str) -> int:
    """Session label (ns, 00:00 UTC) of a YYYY-MM-DD date read from research_window.json."""
    return (dt.date.fromisoformat(text) - dt.date(1970, 1, 1)).days * BI.DAY_NS


def block_passes(x: np.ndarray, thresholds: dict) -> bool:
    """The thresholds on one block's daily net returns (return rows in the block, in session order)."""
    if x.size < thresholds["min_sessions"] or not np.all(np.isfinite(x)):
        return False
    sd = float(x.std(ddof=1))
    if not sd > 0 or float(np.ptp(x)) == 0.0:      # a constant series has no Sharpe ratio (rounding leaves sd ~1e-19)
        return False
    if float(x.mean()) / sd * math.sqrt(NS.ANNUAL) < thresholds["net_sharpe_min"]:
        return False
    wealth = np.cumprod(1.0 + x)
    if "max_drawdown_max" in thresholds:
        peak = np.maximum.accumulate(np.r_[1.0, wealth])[1:]
        if float((1.0 - wealth / peak).max()) > thresholds["max_drawdown_max"]:
            return False
    if "net_return_min" in thresholds and float(wealth[-1] - 1.0) < thresholds["net_return_min"]:
        return False
    return True


def evaluate(deploy: dict, thresholds: dict, blocks: dict) -> dict:
    """The two bits. Opens the NAV output (the only step that reads sealed sessions)."""
    nav = Path(deploy["nav_dir"])
    summary = json.loads((nav / "summary.json").read_text(encoding="utf-8"))
    pin = deploy.get("composition_weights_sha256")
    if pin is not None and summary.get("composition_weights_sha256") != pin:
        raise Refusal("the NAV output is not the frozen book (composition_weights_sha256 differs from the manifest)")
    wanted = deploy.get("scenario") or summary.get("primary_scenario")
    if wanted not in {s.get("scenario") for s in summary.get("scenarios", [])}:
        raise Refusal("the deploy manifest's scenario is not in the NAV summary")
    daily = NS.load_daily_csv(nav / f"daily_{wanted}.csv", allow_sealed=True)
    nets = NS.net_series(daily)
    sessions = np.array(list(nets), dtype=np.int64)
    values = np.array(list(nets.values()), dtype=np.float64)
    bits = {}
    for bit, (begin, end) in zip(BITS, (blocks["2024"], blocks["2025_onward"])):
        inside = (sessions >= begin) & (sessions < end if end is not None else True)
        bits[bit] = bool(block_passes(values[inside], thresholds))
    return bits


def cited_path(ruling_path, ledger_path) -> str:
    """The ruling's path as a validation line records it: relative to the ledger's directory, '/'-separated (absolute
    when it is on another drive)."""
    ruling, base = Path(ruling_path).resolve(), Path(ledger_path).resolve().parent
    try:
        return Path(os.path.relpath(ruling, base)).as_posix()
    except ValueError:
        return ruling.as_posix()


def _path_key(cited: str, ledger_path) -> str:
    """One spelling of a cited ruling path (case, separators and '..' normalised) for comparing two citations."""
    return os.path.normcase(os.path.normpath(os.path.join(Path(ledger_path).resolve().parent, cited)))


def _ledger(ledger_path) -> list[dict]:
    if ledger_path is None:
        raise Refusal("no trial ledger (--ledger FILE): a hidden-block read is recorded there (review C-11)")
    try:
        return BI.ledger_read(Path(ledger_path))
    except (OSError, ValueError):
        raise Refusal("the trial ledger cannot be read or its hash chain is broken") from None


def validation_line(ruling: dict, ruling_sha: str, cited: str, deploy: dict) -> dict:
    """The ledger line citing the ruling (kind validation, count 0: the record of a hidden-block read)."""
    return {"schema": BI.LEDGER_SCHEMA, "kind": VALIDATION, "count": 0, "book": deploy["book"],
            "owner_ruling": {"path": cited, "sha256": ruling_sha},
            "owner": ruling["owner"], "date": ruling["date"], "blocks": sorted(ruling["blocks"]),
            "thresholds_sha256": ruling["thresholds_sha256"], "deploy_manifest_sha256": ruling["deploy_manifest_sha256"],
            "trial_id": BI.trial_id(VALIDATION, ruling_sha)}


def check_cited(records: list[dict], ledger_path, line: dict) -> None:
    """Refuse when a validation line cites the same ruling file with another SHA-256 (review C-11: the file was edited
    after a read under it)."""
    key, sha = _path_key(line["owner_ruling"]["path"], ledger_path), line["owner_ruling"]["sha256"]
    for rec in records:
        cite = rec.get("owner_ruling") if rec.get("kind") == VALIDATION else None
        if isinstance(cite, dict) and _path_key(str(cite.get("path", "")), ledger_path) == key \
                and cite.get("sha256") != sha:
            raise Refusal(f"the owner ruling's bytes differ from the SHA-256 the trial ledger line citing it records "
                          f"({str(cite.get('sha256'))[:16]}, now {sha[:16]}): an edited ruling is a new ruling file")


def prepare(deploy_path, thresholds_path, ruling_path, ledger_path=None):
    """Every check, the owner ruling first; no NAV data is opened here. Returns (deploy, thresholds, blocks, the
    validation line to append before ``evaluate``)."""
    if ruling_path is None:
        raise Refusal("no owner ruling (--owner-ruling FILE); the hidden blocks are opened only under one (OD-1)")
    ruling, ruling_sha = _read(Path(ruling_path), "owner ruling")
    if deploy_path is None or thresholds_path is None:
        raise Refusal("--deploy MANIFEST and --thresholds FILE are both required")
    deploy_doc, deploy_sha = _read(Path(deploy_path), "deploy manifest")
    thresholds_doc, thresholds_sha = _read(Path(thresholds_path), "thresholds file")
    check_ruling(ruling, deploy_sha, thresholds_sha)
    records = _ledger(ledger_path)
    thresholds = check_thresholds(thresholds_doc)
    deploy = check_deploy(deploy_doc, Path(deploy_path).resolve().parent)
    line = validation_line(ruling, ruling_sha, cited_path(ruling_path, ledger_path), deploy)
    check_cited(records, ledger_path, line)
    blocks = hidden_blocks()
    if not (Path(deploy["nav_dir"]) / "summary.json").is_file():
        raise Refusal("the deploy manifest's nav_dir has no summary.json")
    return deploy, thresholds, blocks, line


def record(ledger_path, line: dict) -> dict | None:
    """Append the chained validation line (before any NAV series is opened); the line as written, or None when the
    ledger cites these ruling bytes already."""
    try:
        appended, _ = BI.ledger_append(Path(ledger_path), [line], chain=True)
    except (OSError, ValueError):
        raise Refusal("the validation line could not be appended to the trial ledger") from None
    return appended[0] if appended else None


def gate(deploy_path, thresholds_path, ruling_path, ledger_path) -> dict:
    """``prepare``, ``record`` then ``evaluate``: {"pass_2024": bool, "pass_2025_onward": bool}."""
    deploy, thresholds, blocks, line = prepare(deploy_path, thresholds_path, ruling_path, ledger_path)
    record(ledger_path, line)
    return evaluate(deploy, thresholds, blocks)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="holdout_gate.py", description=__doc__.split("\n", 1)[0])
    ap.add_argument("--deploy", default=None, metavar="MANIFEST", help=f"deploy manifest ({DEPLOY_SCHEMA})")
    ap.add_argument("--thresholds", default=None, metavar="FILE", help=f"thresholds ({THRESHOLDS_SCHEMA})")
    ap.add_argument("--owner-ruling", default=None, metavar="FILE", help=f"owner ruling ({RULING_SCHEMA})")
    ap.add_argument("--ledger", default=None, metavar="TRIALS",
                    help="trial ledger (atx.trial-ledger/v1): the validation line citing the ruling is appended there")
    args = ap.parse_args(argv)
    try:
        *ready, line = prepare(args.deploy, args.thresholds, args.owner_ruling, args.ledger)
        record(args.ledger, line)
    except Refusal as exc:
        print(f"holdout_gate: refusing: {exc}", file=sys.stderr)
        return EXIT_REFUSED
    try:
        bits = evaluate(*ready)
    except Refusal as exc:           # raised before the daily CSV is read; names inputs only
        print(f"holdout_gate: refusing: {exc}", file=sys.stderr)
        return EXIT_REFUSED
    except (Exception, SystemExit):  # noqa: BLE001  never echo a message that might carry a hidden-block value
        print("holdout_gate: the NAV output could not be evaluated (no result)", file=sys.stderr)
        return EXIT_FAILED
    print(json.dumps({bit: bits[bit] for bit in BITS}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
