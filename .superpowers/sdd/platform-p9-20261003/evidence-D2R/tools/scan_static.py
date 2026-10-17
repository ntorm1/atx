"""D2R static scan (scratch): every committed argv source that can reach the fitter's parser.

1. specs / templates under scripts/specs/**: resolve (RC.load_spec; raw fallback), build the fit step's argv the way
   research_cycle.fit_step builds it (placeholders for paths / pins), parse it with the fitter's parser as it is
   (allow_abbrev as built) and with allow_abbrev=False; list non-exact option tokens.
2. every committed text file in scope: every `--name` token that is a strict prefix of a fitter long option and not an
   exact fitter option (an abbreviation candidate), with file:line, for triage.
Usage: python scan_static.py <repo> <out.json>"""
import argparse
import contextlib
import io
import json
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(sys.argv[1]).resolve()
OUT = Path(sys.argv[2])
sys.path.insert(0, str(REPO / "atx-impl" / "tools"))
sys.path.insert(0, str(REPO / "scripts"))
import fit_composition_weights as fcw  # noqa: E402
import research_cycle as RC  # noqa: E402
import research_spec as RS  # noqa: E402
import research_tree  # noqa: E402


def fitter_parser() -> argparse.ArgumentParser:
    class Caught(Exception):
        pass
    seen = []
    orig = argparse.ArgumentParser.parse_args

    def catch(self, args=None, namespace=None):
        seen.append(self)
        raise Caught
    argparse.ArgumentParser.parse_args = catch
    try:
        fcw.parse_args([])
    except Caught:
        pass
    finally:
        argparse.ArgumentParser.parse_args = orig
    return seen[0]


P = fitter_parser()
OPTS = set(P._option_string_actions)
LONGS = sorted(o for o in OPTS if o.startswith("--"))
BUILT_ABBREV = P.allow_abbrev


def parse(argv, abbrev: bool):
    P.allow_abbrev = abbrev
    err = io.StringIO()
    try:
        with contextlib.redirect_stderr(err):
            ns = P.parse_args(argv)
        return {"ok": True, "ns": {k: (str(v) if not isinstance(v, (int, float, bool, type(None), list)) else v)
                                   for k, v in sorted(vars(ns).items())}}
    except SystemExit as e:
        return {"ok": False, "code": e.code, "err": err.getvalue().strip().splitlines()[-1:]}
    finally:
        P.allow_abbrev = BUILT_ABBREV


def nonexact(argv):
    out = []
    for tok in argv:
        if isinstance(tok, str) and tok.startswith("--") and tok != "--":
            head = tok.split("=", 1)[0]
            if head not in OPTS:
                out.append({"token": tok, "matches": [o for o in LONGS if o.startswith(head)]})
    return out


def raw_load(path: Path, seen=()):
    doc = json.loads(Path(path).read_text(encoding="utf-8"))
    if RS.is_template(doc):
        here = Path(path).resolve()
        if here in seen:
            raise RuntimeError("cycle")
        doc = RS.resolve(doc, Path(path), lambda p: raw_load(p, seen + (here,)), research_tree.REPO)
    return doc


def fit_argv(spec: dict) -> list[str]:
    """research_cycle.fit_step's fitter argv (after the runner prefix), with placeholder paths / pins."""
    fit, inputs = spec["fit"], spec.get("inputs", {})
    argv = ["--library", "L", "--library-sha256", "0" * 64, "--train", "R", "--train-sha256", "0" * 64,
            "--orientations", "O", "--orientations-sha256", "0" * 64, "--runner-summary", "S",
            "--runner-summary-sha256", "0" * 64, *[str(f) for f in fit.get("flags", [])]]
    if "reference_resid_parent" in inputs:
        argv += ["--theme-resid-parent", "P", "--theme-resid-parent-sha256", "0" * 64]
    if "recipe" in inputs:
        argv += ["--recipe", "RC", "--recipe-sha256", "0" * 64]
    argv += ["--work-dir", "W"]
    if "max_seconds" in fit:
        argv += ["--max-seconds", str(fit["max_seconds"])]
    argv += ["--output", "OUT"]
    return argv


def git_files(*pathspecs):
    out = subprocess.run(["git", "-C", str(REPO), "ls-files", "-z", *pathspecs], capture_output=True, check=True)
    return [p for p in out.stdout.decode().split("\0") if p]


specs = []
for rel in sorted(git_files("scripts/specs")):
    if not rel.endswith(".json"):
        continue
    path = REPO / rel
    row = {"file": rel, "template": False}
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
        row["template"] = RS.is_template(raw)
        row["schema"] = raw.get("schema") if isinstance(raw, dict) else None
    except Exception as e:  # noqa: BLE001
        row["error"] = f"json: {e}"
        specs.append(row)
        continue
    spec, how = None, None
    try:
        spec, how = RC.load_spec(path), "load_spec"
    except Exception as e:  # noqa: BLE001
        row["load_spec_error"] = str(e)[:300]
        try:
            spec, how = raw_load(path), "raw-resolve"
        except Exception as e2:  # noqa: BLE001
            row["raw_error"] = str(e2)[:300]
    row["how"] = how
    if isinstance(spec, dict) and isinstance(spec.get("fit"), dict):
        argv = fit_argv(spec)
        row["fit_flags"] = [str(f) for f in spec["fit"].get("flags", [])]
        row["nonexact"] = nonexact(argv)
        a, b = parse(argv, True), parse(argv, False)
        row["parse_abbrev_on"], row["parse_abbrev_off"] = a["ok"], b["ok"]
        row["same_namespace"] = a.get("ns") == b.get("ns") if a["ok"] and b["ok"] else None
        if not a["ok"]:
            row["err_on"] = a["err"]
        if not b["ok"]:
            row["err_off"] = b["err"]
    else:
        row["fit"] = "none"
    specs.append(row)

# text scan: every --token in scope that is a strict prefix of a fitter long option and not exact
TOKEN = re.compile(r"(?<![\w-])(--[A-Za-z0-9][A-Za-z0-9_-]*)")
scope = git_files("scripts", "atx-impl/tools", "atx-impl/strategies", "atx-engine/tools", ".superpowers")
candidates, counts = [], {"files": 0, "tokens": 0, "exact": 0, "prefix": 0}
for rel in scope:
    if rel.startswith("atx-db/") or not rel.endswith((".py", ".json", ".sh", ".txt", ".env.example", ".ps1", ".toml")):
        continue
    try:
        text = (REPO / rel).read_text(encoding="utf-8")
    except Exception:  # noqa: BLE001
        continue
    counts["files"] += 1
    for n, line in enumerate(text.splitlines(), 1):
        for m in TOKEN.finditer(line):
            tok = m.group(1)
            counts["tokens"] += 1
            if tok in OPTS:
                counts["exact"] += 1
                continue
            matches = [o for o in LONGS if o.startswith(tok)]
            if matches:
                counts["prefix"] += 1
                candidates.append({"file": rel, "line": n, "token": tok, "matches": matches, "text": line.strip()[:200]})

OUT.write_text(json.dumps({"built_allow_abbrev": BUILT_ABBREV, "longs": LONGS, "specs": specs, "text_counts": counts,
                           "text_candidates": candidates}, indent=1), encoding="utf-8")
fits = [s for s in specs if "fit_flags" in s]
print(f"allow_abbrev as built: {BUILT_ABBREV}; {len(LONGS)} long options")
print(f"specs: {len(specs)} files, {len(fits)} with a fit section; "
      f"non-exact tokens in {sum(1 for s in fits if s['nonexact'])}; "
      f"parse on ok {sum(s['parse_abbrev_on'] for s in fits)}, off ok {sum(s['parse_abbrev_off'] for s in fits)}, "
      f"same ns {sum(1 for s in fits if s['same_namespace'])}")
for s in specs:
    if "fit_flags" not in s:
        print("  no fit:", s["file"], s.get("how"), s.get("load_spec_error", "")[:120], s.get("raw_error", "")[:120])
    elif s["nonexact"] or not s["parse_abbrev_on"] or not s["parse_abbrev_off"] or not s["same_namespace"]:
        print("  FLAG:", s["file"], s["nonexact"], s.get("err_on"), s.get("err_off"))
print(f"text: {counts}")
by_tok = {}
for c in candidates:
    by_tok.setdefault(c["token"], []).append(f"{c['file']}:{c['line']}")
for tok, where in sorted(by_tok.items()):
    print(f"  {tok}: {len(where)} -> {where[:6]}{' ...' if len(where) > 6 else ''}")
