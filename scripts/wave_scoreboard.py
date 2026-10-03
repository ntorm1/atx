"""research_cycle.py scoreboard: the lineage of accepted books, one command (platform v8 lane YINFRA).

  research_cycle.py scoreboard [--results GLOB ...] [--ledger PATH] [--markdown OUT] [--json OUT] [--timings]
                               [--root R]

Reads ONLY the wave results (wave-result.json, written by the wave's record stage from what the summariser and the
readers printed into its receipts; default glob build-equity/waves/*/wave-result.json) and the trial ledger (its chain
verified); never a NAV file. Prints, as markdown (and JSON with --json):

  lineage   the accepted books in order: the first wave's parent, then each accepted cell whose parent is the
            previous book (a parent with two accepted children is reported as a branch)
  waves     every wave: verdict, cell, S2 net Sharpe, net and gross annual return, 4x net Sharpe, turnover, max
            drawdown, N after the wave, paired dSR, bundle p (one-sided), ledger DSR
  checks    each cell's ledger line is present (trial_id) and its s2_net_sr equals the result's net Sharpe (1e-6);
            the ledger's N now; two results of one wave id that differ are refused
  timings   (--timings) wall seconds by phase per wave; a result written under driver.timings (P9 OR section 5) adds
            the screen's, readers' and bundle's rows, register and git, and a table by stage
"""
from __future__ import annotations

import argparse
import glob
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import research_ledger  # noqa: E402
import research_tree  # noqa: E402
import wave_result  # noqa: E402

SCHEMA = "atx.book-scoreboard/v1"
DEFAULT_RESULTS = "build-equity/waves/*/" + wave_result.RESULT
SR_TOLERANCE = 1e-6
COLUMNS = (("net_sharpe", "S2 net SR", "+.4f"), ("net_annual", "net ann", ".2%"), ("gross_annual", "gross ann", ".2%"),
           ("x4_net_sharpe", "4x net SR", "+.4f"), ("tau_gmv_mean", "tau", ".5f"), ("max_drawdown", "max DD", ".2%"))


class ScoreboardError(ValueError):
    pass


def load_results(root: Path, patterns: list[str]) -> list[dict]:
    """The wave results matching the globs (root-relative), one per wave id (identical copies merge)."""
    by_wave: dict[str, tuple[str, dict]] = {}
    for pattern in patterns:
        for p in sorted(glob.glob(str(root / pattern))):
            doc = json.loads(Path(p).read_text(encoding="utf-8"))
            if doc.get("schema") != wave_result.SCHEMA:
                raise ScoreboardError(f"{p}: not an {wave_result.SCHEMA} file")
            seen = by_wave.get(doc["wave"])
            if seen is not None and seen[1] != doc:
                raise ScoreboardError(f"wave {doc['wave']}: {seen[0]} and {p} differ")
            by_wave.setdefault(doc["wave"], (p, doc))
    return [doc for _, doc in by_wave.values()]


def ledger_view(root: Path, rel: str) -> tuple[dict, dict]:
    """({trial_id: line}, {path, lines, head, n}) of the ledger, its chain verified."""
    bi = research_ledger.backtest_integrity()
    p = root / rel
    try:
        records = bi.ledger_read(p)
        head = bi.ledger_head(p)
    except (OSError, ValueError) as exc:
        raise ScoreboardError(f"ledger {rel}: {exc}") from exc
    return {r.get("trial_id"): r for r in records}, {"path": rel, "lines": len(records), "head": head,
                                                     "n": bi.ledger_n(records, True)}


def row(doc: dict) -> dict:
    cell = doc.get("cell") or {}
    stats = ((doc.get("stats") or {}).get("cell")) or {}
    return {"wave": doc["wave"], "verdict": wave_result.verdict_word(doc), "rule": (doc.get("verdict") or {}).get("rule"),
            "parent": doc["parent"]["spec"], "cell": cell.get("spec"), "library": cell.get("library"),
            **{k: stats.get(k) for k, _, _ in COLUMNS}, "n": doc["ledger"]["n_after"],
            "dsr": (doc.get("paired") or {}).get("dsr"), "p_one_sided": (doc.get("bundle") or {}).get("p_one_sided"),
            "ledger_dsr": (doc.get("dsr") or {}).get("cell_count"), "trial_id": doc["ledger"].get("trial_id")}


def lineage(docs: list[dict]) -> tuple[list[dict], list[str]]:
    """The accepted books in order, from the parent no accepted wave produced; and the branches found."""
    accepted = [d for d in docs if d.get("cell") and (d.get("verdict") or {}).get("accepted")]
    children: dict[str, list[dict]] = {}
    for d in accepted:
        children.setdefault(d["parent"]["spec"], []).append(d)
    made = {d["cell"]["spec"] for d in accepted}
    roots = [d for d in docs if d["parent"]["spec"] not in made]
    if not roots:
        return [], []
    first = min(roots, key=lambda d: d["ledger"]["n_before"])
    stats = (first.get("stats") or {}).get("parent") or {}
    out = [{"wave": "(parent)", "verdict": "BASE", "cell": first["parent"]["spec"], "library": first["parent"]["library"],
            **{k: stats.get(k) for k, _, _ in COLUMNS}, "n": first["ledger"]["n_before"], "dsr": None,
            "p_one_sided": None, "ledger_dsr": None, "trial_id": None}]
    branches, spec, seen = [], first["parent"]["spec"], set()
    while spec in children and spec not in seen:
        seen.add(spec)
        kids = sorted(children[spec], key=lambda d: d["ledger"]["n_after"])
        if len(kids) > 1:
            branches.append(f"{spec} has {len(kids)} accepted children ({', '.join(k['wave'] for k in kids)}): the "
                            "lineage follows the first ledgered")
        out.append(row(kids[0]))
        spec = kids[0]["cell"]["spec"]
    return out, branches


def checks(docs: list[dict], lines: dict) -> list[dict]:
    out = []
    for d in docs:
        tid = d["ledger"].get("trial_id")
        if not d.get("cell"):
            continue
        line = lines.get(tid)
        sr = ((d.get("stats") or {}).get("cell") or {}).get("net_sharpe")
        ok_line = line is not None
        ok_sr = ok_line and isinstance(sr, (int, float)) and isinstance(line.get("s2_net_sr"), (int, float)) and \
            abs(line["s2_net_sr"] - sr) <= SR_TOLERANCE
        out.append({"wave": d["wave"], "trial_id": tid, "ledgered": ok_line, "s2_net_sr_equal": bool(ok_sr),
                    "ledger_s2_net_sr": (line or {}).get("s2_net_sr"), "result_net_sharpe": sr})
    return out


def board(root: Path, patterns: list[str], ledger: str | None = None) -> dict:
    docs = load_results(root, patterns)
    paths = {d["ledger"]["path"] for d in docs}
    rel = ledger or (paths.pop() if len(paths) == 1 else None)
    if rel is None:
        raise ScoreboardError(f"the results name {len(paths) or 'no'} ledgers: pass --ledger")
    lines, led = ledger_view(root, rel)
    lin, branches = lineage(docs)
    return {"schema": SCHEMA, "ledger": led, "lineage": lin, "branches": branches,
            "waves": sorted((row(d) for d in docs), key=lambda r: (r["n"], r["wave"])), "checks": checks(docs, lines),
            "timings": sorted((timing(d) for d in docs), key=lambda t: t["wave"])}


def process_phase(p: dict) -> str | None:
    """The timings phase of a process row a wave recorded (driver.timings, P9 OR section 5): "register" (add-alpha),
    "git" (commits and git queries); None for a process whose time is in a runner row already (research_cycle runs, the
    readers, the bundle) or that is no phase (lock)."""
    what = str(p.get("what", ""))
    if what.startswith("add-alpha"):
        return "register"
    if what == "commit" or what.startswith("git"):
        return "git"
    return None


def timing(doc: dict) -> dict:
    """Wall seconds and peak MiB by phase of one wave (its bounded-runner receipts, every attempt; under driver.timings
    also the screen's, the readers' and the bundle's rows and the register and git processes): the speed record a
    later wave is compared with. A result with stage_seconds adds them ("stages") and totals them."""
    by: dict[str, dict] = {}
    for r in doc.get("timings") or []:
        t = by.setdefault(r["phase"], {"runs": 0, "seconds": 0.0, "peak_mib": 0})
        t["runs"] += 1
        t["seconds"] += r.get("seconds") or 0.0
        t["peak_mib"] = max(t["peak_mib"], r.get("peak_mib") or 0)
    for p in doc.get("processes") or []:
        phase = process_phase(p)
        if phase:
            t = by.setdefault(phase, {"runs": 0, "seconds": 0.0, "peak_mib": 0})
            t["runs"] += p.get("calls") or 0
            t["seconds"] += p.get("seconds") or 0.0
    out = {"wave": doc["wave"], "phases": by, "seconds": round(sum(t["seconds"] for t in by.values()), 1)}
    if doc.get("stage_seconds"):
        out["stages"] = dict(doc["stage_seconds"])
        out["seconds"] = round(sum(v or 0.0 for v in doc["stage_seconds"].values()), 1)
    return out


def timings_markdown(b: dict) -> list[str]:
    phases = sorted({p for t in b["timings"] for p in t["phases"]})
    out = ["### Wall-clock by phase (s; runs)", "", "| wave | " + " | ".join(phases) + " | total |",
           "|" + "---|" * (len(phases) + 2)]
    for t in b["timings"]:
        cells = [f"{t['phases'][p]['seconds']:.1f} ({t['phases'][p]['runs']})" if p in t["phases"] else "-"
                 for p in phases]
        out.append(f"| {t['wave']} | " + " | ".join(cells) + f" | {t['seconds']:.1f} |")
    staged = [t for t in b["timings"] if t.get("stages")]
    if staged:                              # P9 OR section 5: the stage receipts' seconds
        names = list(dict.fromkeys(s for t in staged for s in t["stages"]))
        out += ["", "### Wall-clock by stage (s)", "", "| wave | " + " | ".join(names) + " |",
                "|" + "---|" * (len(names) + 1)]
        out += [f"| {t['wave']} | " + " | ".join(_f(t["stages"].get(s), ".1f") for s in names) + " |"
                for t in staged]
    return out + [""]


def _f(x, spec: str) -> str:
    return format(x, spec) if isinstance(x, (int, float)) and not isinstance(x, bool) else "-"


def markdown(b: dict, with_timings: bool = False) -> str:
    led = b["ledger"]
    head = ["wave", "verdict", "cell"] + [title for _, title, _ in COLUMNS] + ["N", "dSR", "p (1-sided)", "DSR"]

    def table(rows: list[dict]) -> list[str]:
        out = ["| " + " | ".join(head) + " |", "|" + "---|" * len(head)]
        for r in rows:
            cells = [r["wave"], r["verdict"], f"`{r['cell']}`" if r.get("cell") else "-"]
            cells += [_f(r.get(k), spec) for k, _, spec in COLUMNS]
            cells += [str(r["n"]), _f(r.get("dsr"), "+.4f"), _f(r.get("p_one_sided"), ".4f"), _f(r.get("ledger_dsr"), ".4f")]
            out.append("| " + " | ".join(cells) + " |")
        return out
    out = [f"## Book scoreboard (ledger `{led['path']}`: {led['lines']} lines, head `{led['head'][:16]}`, N {led['n']})",
           "", "### Accepted lineage", ""] + table(b["lineage"]) + [""]
    out += [f"- branch: {x}" for x in b["branches"]]
    out += ["### Every wave", ""] + table(b["waves"]) + ["", "### Ledger checks", ""]
    out += [f"- {c['wave']}: trial `{c['trial_id']}` " + ("ledgered" if c["ledgered"] else "NOT IN THE LEDGER") +
            ("; s2_net_sr equal" if c["s2_net_sr_equal"] else f"; s2_net_sr {c['ledger_s2_net_sr']} vs result "
             f"{c['result_net_sharpe']}: MISMATCH") for c in b["checks"]]
    if with_timings:
        out += [""] + timings_markdown(b)
    return "\n".join(out) + "\n"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="research_cycle.py scoreboard", description=__doc__.split("\n", 1)[0],
                                 epilog=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--results", action="append", default=None, metavar="GLOB",
                    help=f"root-relative glob of wave-result.json files (default {DEFAULT_RESULTS})")
    ap.add_argument("--ledger", default=None)
    ap.add_argument("--markdown", type=Path, default=None, help="also write the markdown to this file")
    ap.add_argument("--json", type=Path, default=None, help="also write the scoreboard as JSON")
    ap.add_argument("--timings", action="store_true", help="also print the wall seconds by phase of every wave")
    ap.add_argument("--root", type=Path, default=research_tree.REPO)
    a = ap.parse_args(argv)
    try:
        b = board(a.root, a.results or [DEFAULT_RESULTS], a.ledger)
    except (ScoreboardError, OSError, ValueError, KeyError) as exc:
        print(f"research_cycle scoreboard: {exc}", file=sys.stderr)
        return 2
    text = markdown(b, a.timings)
    print(text, end="")
    if a.markdown:
        a.markdown.write_text(text, encoding="utf-8", newline="\n")
    if a.json:
        a.json.write_text(json.dumps(b, indent=2) + "\n", encoding="utf-8", newline="\n")
    bad = [c for c in b["checks"] if not (c["ledgered"] and c["s2_net_sr_equal"])]
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
