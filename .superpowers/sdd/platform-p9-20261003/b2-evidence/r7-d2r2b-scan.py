"""D2R-2 (b) scan, before nav_summ's parser gets allow_abbrev=False: does any spec, template, fixture, driver or argv that
reaches nav_summ use an abbreviated nav_summ flag (a `--x` that is not one of its long options but a strict prefix of
one or more, as argparse with allow_abbrev on would read it)?

Run from the checkout root: python r7-d2r2b-scan.py
Scope: `git ls-files` (tracked files only), minus atx-db/ (another session's; never touched) and .superpowers/ (sprint
logs and reports: not specs, templates, fixtures or drivers). Prints, per hit, path:line and the token only (never the
line's text: docs may carry statistics; the scan is blind).

1. JSON: every list that is a spec's summ.extra (a cell spec, or a template's change.set "summ.extra" / change.flags
   "summ"), and every JSON list holding a "nav_summ.py" token (a pinned argv).
2. Text (py, ps1, sh, bat, cmd, json-as-text, md, txt): tokens `--word[=...]` in files that name nav_summ (nav_summ.py,
   "import nav_summ", NS.main, NAV_SUMM): a token that is a strict prefix of a nav_summ option and not itself an option
   of nav_summ is a CANDIDATE; candidates are listed for a hand check (another tool's flag in the same file is not a
   nav_summ argv).
"""
from __future__ import annotations

import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path.cwd()
SRC = (ROOT / "atx-impl/tools/nav_summ.py").read_text(encoding="utf-8")
OPTIONS = sorted(set(re.findall(r'add_argument\(\s*"(--[a-z0-9][a-z0-9-]*)"', SRC)))
TOKEN = re.compile(r'(?<![\w-])(--[a-z][a-z0-9-]*)(?:=|\b)')
NAMES_NAV_SUMM = re.compile(r"nav_summ\.py|import nav_summ|NS\.main|NAV_SUMM\b|nav_summ\.main")
TEXT_EXT = {".py", ".ps1", ".sh", ".bat", ".cmd", ".md", ".txt", ".json"}


def abbreviation(tok: str) -> list[str]:
    """The nav_summ options ``tok`` abbreviates ([] when it is an option, or a prefix of none)."""
    if tok in OPTIONS:
        return []
    return [o for o in OPTIONS if o.startswith(tok) and o != tok]


def files() -> list[str]:
    out = subprocess.run(["git", "ls-files"], capture_output=True, text=True, check=True).stdout.splitlines()
    return [f for f in out if not f.startswith(("atx-db/", ".superpowers/")) and Path(f).suffix.lower() in TEXT_EXT]


def json_lists(doc, path=()):
    if isinstance(doc, dict):
        for k, v in doc.items():
            yield from json_lists(v, path + (k,))
    elif isinstance(doc, list):
        yield path, doc
        for i, v in enumerate(doc):
            yield from json_lists(v, path + (i,))


def main() -> int:
    print(f"nav_summ long options ({len(OPTIONS)}): {' '.join(OPTIONS)}")
    spec_hits, argv_hits, cands, scanned_json, summ_lists = [], [], [], 0, 0
    for f in files():
        p = ROOT / f
        try:
            text = p.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        if p.suffix.lower() == ".json":
            try:
                doc = json.loads(text)
            except ValueError:
                doc = None
            if doc is not None:
                scanned_json += 1
                for path, lst in json_lists(doc):
                    is_summ = (path[-2:] == ("summ", "extra") or path[-1:] == ("summ.extra",)
                               or path[-3:-1] == ("flags", "summ"))
                    is_argv = any(isinstance(x, str) and x.endswith("nav_summ.py") for x in lst)
                    if not (is_summ or is_argv):
                        continue
                    summ_lists += 1
                    for x in lst:
                        if isinstance(x, str) and x.startswith("--"):
                            tok = x.split("=", 1)[0]
                            if abbreviation(tok):
                                (spec_hits if is_summ else argv_hits).append((f, "/".join(map(str, path)), tok))
                if isinstance(doc, dict) and isinstance((doc.get("change") or {}).get("flags"), dict):
                    summ_flags = doc["change"]["flags"].get("summ")
                    if isinstance(summ_flags, dict):
                        summ_lists += 1
                        for k in summ_flags:
                            if abbreviation(str(k).split("=", 1)[0]):
                                spec_hits.append((f, "change/flags/summ", k))
        if not NAMES_NAV_SUMM.search(text):
            continue
        for n, line in enumerate(text.splitlines(), 1):
            for tok in TOKEN.findall(line):
                if abbreviation(tok):
                    cands.append((f, n, tok, abbreviation(tok)))
    print(f"JSON files parsed: {scanned_json}; summ.extra / summ-flag / nav_summ argv lists checked: {summ_lists}")
    print(f"1. spec / template summ flags abbreviating a nav_summ option: {len(spec_hits)}")
    for h in spec_hits:
        print("   ", *h)
    print(f"1. pinned JSON argvs of nav_summ with an abbreviation: {len(argv_hits)}")
    for h in argv_hits:
        print("   ", *h)
    print(f"2. text candidates in files naming nav_summ (hand check): {len(cands)}")
    for f, n, tok, opts in cands:
        print(f"    {f}:{n} {tok} (prefix of {', '.join(opts)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
