"""Per-hit source check for PM ruling SEAL-ALLOW (P9 progress.md): every sealed date token in the logs the wave's seal
scan reads (wave_seal.wave_logs over the done receipts), classified by source. Prints file, line, token, form and
source only -- no log content.

  A  = the file name docs/plans/2026-10-02-x5-equity-curve.png (the untracked owner plot in the runner's dirty list)
  B  = a receipt "started_utc" wall-clock stamp ("started_utc": "<date>T...)
  C  = R15-SEAL-C: compact 20261003 in the exact Git exclusion argv token on a commit console's first header
  D  = R15-SEAL-D: exact additional owner-PNG paths in native dirty-list headers or parsed receipt arrays
  OTHER = anything else: a stop under the ruling

usage: python sealsrc.py [--root C:/atx-wt/pool-2] [--manifest scripts/specs/v8/waves/y-s.json]
--controls runs only the focused source-classification controls, no wave or data process.
exit 0 when every hit is A, B, C or D, 1 when any hit is OTHER.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
from collections import Counter
from pathlib import Path
import re
import sys

PNG = "docs/plans/2026-10-02-x5-equity-curve.png"
EXCLUDE = ":(exclude).superpowers/sdd/platform-p9-20261003/gate-logs/tools/.mypy_cache"
DIRTY_HEADER = "# dirty outside the code pathspec (listed, not a stop): "
OWNER_PNGS = (
    "docs/plans/2026-10-03-p9-b0-yf0-equity-curve.png",
    "docs/plans/2026-10-03-wq-explore-equity-curve.png",
    "docs/plans/2026-10-03-wq-explore-sharpe-vs-turnover.png",
    "docs/plans/2026-10-06-p9-f0-yf0-equity-curve.png",
)


def dirty_spans(rel: str, lines: list[str]) -> set[tuple[int, int, int]]:
    """Exact ISO hit positions in the two authorized provenance forms; never opens the named PNGs."""
    spans = set()
    if not re.fullmatch(r"build-equity/waves/[^/]+/consoles/[0-9]+-[^/]+\.log", rel.replace("\\", "/")):
        return spans
    for ln, line in enumerate(lines, 1):
        if line.startswith(DIRTY_HEADER):
            pos = len(DIRTY_HEADER)
            for entry in line[pos:].split(", "):
                if entry in OWNER_PNGS:
                    spans.add((ln, pos + 11, pos + 21))
                pos += len(entry) + 2
    # A native bounded-reader console renders one receipt object between its stdout/stderr markers.
    # Parse that object first; a quoted path in another field or arbitrary JSON-like text does not qualify.
    try:
        first = lines.index("# stdout") + 1
        last = lines.index("# stderr", first)
        body = "\n".join(lines[first:last])
        receipt = json.loads(body)
        entries = receipt.get("dirty_outside_pathspec") if isinstance(receipt, dict) else None
        arrays = list(re.finditer(r'^\s*"dirty_outside_pathspec":\s*(\[[\s\S]*?\])\s*,?\s*$', body, re.M))
        if not isinstance(entries, list) or not all(isinstance(x, str) for x in entries) or len(arrays) != 1:
            return spans
        array = arrays[0]
        if json.loads(array.group(1)) != entries:
            return spans
        for mt in re.finditer(r'"([^"\\]*)"', array.group(1)):
            if mt.group(1) not in OWNER_PNGS:
                continue
            absolute = array.start(1) + mt.start(1) + 11
            ln = first + body.count("\n", 0, absolute) + 1
            start = absolute - (body.rfind("\n", 0, absolute) + 1)
            spans.add((ln, start, start + 10))
    except (ValueError, TypeError):
        pass
    return spans


def source(rel: str, ln: int, line: str, form: str, s: int, e: int,
           dirty: set[tuple[int, int, int]] | None = None) -> str:
    """Classify this exact hit; C is an unquoted standalone negative pathspec after Git's -- separator."""
    if form == "iso" and line[max(0, s - 11):e + 20] == PNG:
        return "A"
    if form == "iso" and line[max(0, s - 16):s] == '"started_utc": "' and line[e:e + 1] == "T":
        return "B"
    if form == "iso" and (ln, s, e) in (dirty or set()):
        return "D"
    if form != "compact" or line[s:e] != "20261003" or ln != 1 or \
            not re.fullmatch(r"[0-9]+-commit\.log", Path(rel).name) or \
            not line.startswith(("$ git add ", "$ git commit ")):
        return "OTHER"
    # Wave.console uses fmt_argv: whitespace arguments double-quoted. Reject malformed quoting, and do not
    # mistake an occurrence inside a quoted commit message for a separate argv token.
    parts = list(re.finditer(r'"[^"]*"|[^\s"]+', line))
    end = 0
    for index, part in enumerate(parts):
        gap = line[end:part.start()]
        if gap.strip() or (index and not gap):
            return "OTHER"
        end = part.end()
    if line[end:].strip():
        return "OTHER"
    separator = next((i for i, p in enumerate(parts) if p.group() == "--"), len(parts))
    for part in parts[separator + 1:]:
        if part.group() == EXCLUDE and s == part.start() + EXCLUDE.index("20261003") and e == s + 8:
            return "C"
    return "OTHER"


def controls() -> None:
    rel = "build-equity/waves/p9h-cov-hedge/consoles/004-commit.log"
    add = "$ git add -- scripts/cell.json " + EXCLUDE
    commit = '$ git commit -q -m "cell registration" -m "literal trailer" -- scripts/cell.json ' + EXCLUDE
    cases = [
        ("add", rel, 1, add, "compact", "20261003", 0, "C"),
        ("commit", rel, 1, commit, "compact", "20261003", 0, "C"),
        ("other-file", rel + ".bak", 1, add, "compact", "20261003", 0, "OTHER"),
        ("unnumbered-file", "commit.log", 1, add, "compact", "20261003", 0, "OTHER"),
        ("later-line", rel, 2, add, "compact", "20261003", 0, "OTHER"),
        ("non-git", rel, 1, add.replace("$ git add", "$ python tool"), "compact", "20261003", 0, "OTHER"),
        ("data", rel, 1, "session=20261003 " + EXCLUDE, "compact", "20261003", 0, "OTHER"),
        ("token-prefix", rel, 1, add.replace(EXCLUDE, "x" + EXCLUDE), "compact", "20261003", 0, "OTHER"),
        ("token-suffix", rel, 1, add + "/extra", "compact", "20261003", 0, "OTHER"),
        ("quoted-token", rel, 1, add.replace(EXCLUDE, '"' + EXCLUDE + '"'), "compact", "20261003", 0, "OTHER"),
        ("adjacent-quoted-prefix", rel, 1, add.replace(EXCLUDE, '"prefix"' + EXCLUDE), "compact", "20261003", 0, "OTHER"),
        ("adjacent-quoted-suffix", rel, 1, add + '"suffix"', "compact", "20261003", 0, "OTHER"),
        ("message", rel, 1, '$ git commit -m "text ' + EXCLUDE + ' text" -- scripts/cell.json', "compact", "20261003", 0, "OTHER"),
        ("before-separator", rel, 1, "$ git commit -m " + EXCLUDE + " -- scripts/cell.json", "compact", "20261003", 0, "OTHER"),
        ("malformed-quote", rel, 1, '$ git commit -m "open -- scripts/cell.json ' + EXCLUDE, "compact", "20261003", 0, "OTHER"),
        ("other-hit-same-line", rel, 1, add + " 20261003", "compact", "20261003", 1, "OTHER"),
        ("exact-hit-same-line", rel, 1, add + " 20261003", "compact", "20261003", 0, "C"),
        ("wrong-form", rel, 1, add, "iso", "20261003", 0, "OTHER"),
        ("A-unchanged", "stdout.log", 7, "dirty: " + PNG, "iso", "2026-10-02", 0, "A"),
        ("B-unchanged", "receipt.log", 8, '"started_utc": "2026-10-17T14:00:00Z"', "iso", "2026-10-17", 0, "B"),
        ("other-date", rel, 1, add + " 20261017", "compact", "20261017", 0, "OTHER"),
    ]
    for name, path, ln, line, form, token, occurrence, expected in cases:
        hits = list(re.finditer(re.escape(token), line))
        hit = hits[occurrence]
        got = source(path, ln, line, form, hit.start(), hit.end())
        assert got == expected, f"{name}: {got} != {expected}"
        print(f"control {name}: {got} PASS")
    drel = "build-equity/waves/p9h-cov-hedge-attempt2/consoles/007-mechanics-reader.log"
    owner = OWNER_PNGS[0]
    def receipt(key, value):
        return ["$ python reader.py", "# exit 0", "# stdout", *json.dumps({key: value}, indent=2).splitlines(), "# stderr"]
    d_cases = [
        ("D-header-all", drel, [DIRTY_HEADER + ", ".join(OWNER_PNGS)], 4),
        ("D-array-all", drel, receipt("dirty_outside_pathspec", list(OWNER_PNGS)), 4),
        ("D-wrong-file", "stdout.log", [DIRTY_HEADER + owner], 0),
        ("D-unnumbered", drel.replace("007-", ""), [DIRTY_HEADER + owner], 0),
        ("D-other-dir", drel.replace("/consoles/", "/data/"), [DIRTY_HEADER + owner], 0),
        ("D-header-prefix", drel, ["text " + DIRTY_HEADER + owner], 0),
        ("D-path-prefix", drel, [DIRTY_HEADER + "x" + owner], 0),
        ("D-path-suffix", drel, [DIRTY_HEADER + owner + ".bak"], 0),
        ("D-unregistered", drel, [DIRTY_HEADER + owner.replace("p9-b0-yf0", "unknown")], 0),
        ("D-free-text", drel, [owner], 0),
        ("D-data", drel, ["session=2026-10-03 " + owner], 0),
        ("D-other-field", drel, receipt("input_data", [owner]), 0),
        ("D-array-suffix", drel, receipt("dirty_outside_pathspec", [owner + ".bak"]), 0),
        ("D-array-not-string", drel, receipt("dirty_outside_pathspec", [{"file": owner}]), 0),
        ("D-array-free-fragment", drel, ['"dirty_outside_pathspec": [', '  "' + owner + '"', ']'], 0),
        ("D-extra-date", drel, [DIRTY_HEADER + owner + ", 2026-10-03"], 1),
    ]
    for name, path, lines, expected in d_cases:
        spans = dirty_spans(path, lines)
        got = 0
        for ln, line in enumerate(lines, 1):
            for hit in re.finditer(r"2026-10-(?:03|06)", line):
                result = source(path, ln, line, "iso", hit.start(), hit.end(), spans)
                assert result in ("D", "OTHER"), f"{name}: unexpected {result}"
                got += result == "D"
        assert got == expected, f"{name}: {got} != {expected}"
        print(f"control {name}: D={got} PASS")
    print(f"controls: {len(cases) + len(d_cases)} PASS")


ap = argparse.ArgumentParser()
ap.add_argument("--root", default="C:/atx-wt/pool-2")
ap.add_argument("--manifest", default="scripts/specs/v8/waves/y-s.json")
ap.add_argument("--controls", action="store_true")
a = ap.parse_args()
if a.controls:
    controls()
    sys.exit(0)
root = Path(a.root)
sys.path.insert(0, str(root / "scripts"))
from wave_context import Wave  # noqa: E402
import wave_seal as WSL  # noqa: E402
import wave_stages  # noqa: E402

w = Wave(Path(a.manifest), root)
rdir = w.path(w.wave_path("receipts"))
done = {}
for i, st in enumerate(wave_stages.STAGES):
    p = rdir / f"{i + 1:02d}-{st.name}.json"
    if not p.is_file():
        break
    done[st.name] = json.loads(p.read_text(encoding="utf-8"))["outputs"]
files = WSL.wave_logs(w, done)
print(f"done stages: {list(done)}; files scanned: {len(files)}")
by = Counter()
other = 0
for rel in files:
    lines = w.path(rel).read_text(encoding="utf-8", errors="replace").splitlines()
    dirty = dirty_spans(rel, lines)
    for ln, line in enumerate(lines, 1):
        for form, rx in WSL.FORMS.items():
            for mt in rx.finditer(line):
                raw, g = mt.group(0), mt.groups()
                try:                                   # wave_seal.tokens' own date rule
                    if form in ("iso", "compact"):
                        iso = dt.date(int(g[0]), int(g[1]), int(g[2])).isoformat()
                    else:
                        iso = WSL.RW.period_begin(int(g[0]), int(g[1]) if form == "quarter" else None).isoformat()
                except ValueError:
                    continue
                if not WSL.sealed(iso) or raw in WSL.SEEDS:
                    continue
                if iso == WSL.RW.SEAL_DATE and "seal" in line.lower():
                    continue
                s, e = mt.start(), mt.end()
                src = source(rel, ln, line, form, s, e, dirty)
                if src == "OTHER":
                    other += 1
                by[(raw, src)] += 1
                print(f"  {rel}:{ln} token={raw} form={form} source={src}")
print("summary:", ", ".join(f"{t} {s} x{n}" for (t, s), n in sorted(by.items())) or "no hits")
print("OTHER hits:", other)
sys.exit(1 if other else 0)
