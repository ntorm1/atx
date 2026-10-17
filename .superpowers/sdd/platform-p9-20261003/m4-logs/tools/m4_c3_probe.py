"""Root M4 (RESUME 10), rulings C3-5 / C3-13: the M2j tiny-world probe (m2j-logs/tools/m2j_c3_probe.py, a copy of
m2h-logs/m2h2-engine-o1.py; copied here, the changed lines marked M4) re-run on root's REAL Debug exes of the build
named on the command line (EXE-CUR: the p9-2x receipt) after D3's C3-13 kind derivation and AL-CLOCK's row merged:

  (a)  wave d3a: parent nav.rule aim-partial-v5 (--trade-fraction .05), the wave names aim-partial-v5 with
       trade_fraction .1  -> the cell's nav.rule is aim-partial-v5, ONE --rule on every NAV argv, the exe accepts;
  (b)  wave d3b: parent nav.rule aim-partial-v6 (another aim rule; the live table has one --rule row, aim-partial-v5),
       the wave names aim-partial-v5 with trade_fraction .1 -> the cell's nav.rule is aim-partial-v5, ONE --rule
       (aim-partial-v5) on the cell's NAV argv, the exe accepts;
  (clk) wave clk: parent as (a), the wave names info-clock-rate-v1 (P9-C's shape: a trade-rate row with the engine
       gross match), --until spec (the tiny world has no ea_days_since, so a NAV run of the cell is refused before
       output, AL-CLOCK review item 2) -> the cell's planned NAV argv carries --rule aim-partial-v5 once (nav.rule)
       and --rate info-clock-rate-v1 once (the append route).

Synthetic only: tiny_world seed 7, its own empty ledger trials.jsonl inside the world root; every wave stops at
--until verify (clk: --until spec): zero trials, no record stage. Argvs and exit codes only: no statistic is printed.

  python m4_c3_probe.py base BASE           build base world BASE (v5 | v6 | v5e | v6e: e = eval verb pinned), git init, run the parent (--stop-after nav)
  python m4_c3_probe.py wave LABEL          copy the wave's base to build-equity/p9-m4-tw-LABEL and run wave LABEL
  python m4_c3_probe.py report LABEL        every NAV argv the wave's bounded runs and its cell specs' plans carry
"""
from __future__ import annotations

import glob
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

REPO = Path(__file__).resolve().parents[5]   # m4-logs/tools
sys.path.insert(0, str(REPO / "scripts"))
sys.path.insert(0, str(REPO / "scripts" / "tests" / "fixtures"))
import tiny_world as TW  # noqa: E402

BIN = REPO / "build-equity" / "bin"          # M4: Debug, the p9-2x build (EXE-CUR; Release is the gate's, TEST-1)
BUILD_TYPE = "Debug"                         # M4
PY = Path(sys.executable).as_posix()
TDIR = "scripts/specs/p9/kinds/templates"
FIELDS_PIN = "395f82f7b34dd96e50403ff8b47faa58a3d7a94af433ddaaf906f596d039a1ac"


def base_dir(base: str) -> Path:
    return REPO / "build-equity" / f"p9-m4-tw-{base}"   # M4


def sha_bytes(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def git(root: Path, *args: str) -> str:
    return subprocess.run(["git", *args], cwd=root, check=True, capture_output=True, text=True).stdout


def write_json(p: Path, doc) -> bytes:
    p.parent.mkdir(parents=True, exist_ok=True)
    data = (json.dumps(doc, indent=2) + "\n").encode("utf-8")
    p.write_bytes(data)
    return data


CODE_DIRS = ("scripts", "atx-engine/tools", "atx-impl/tools", "atx-impl/strategies")   # git archive of HEAD
PARENT_L = "1.12"
SEAL_RULING = "SEAL-ALLOW"                   # ruling SEAL-ALLOW-3: the run's own UTC date(s) after the per-hit check
PARENT_RULE = {"v5": "aim-partial-v5", "v6": "aim-partial-v6",   # M4: (b)'s parent runs another aim rule
               "v5e": "aim-partial-v5", "v6e": "aim-partial-v6",
               "v5p": "aim-partial-v5", "v6p": "aim-partial-v6"}   # p: eval named, then `lock --exes` pins all
EVAL_EXE = BIN / "atx-research-eval.exe"     # M4: bases v5e / v6e name and pin the engine verb (B2-10 (b) / (c), B2-11)


def base(which: str) -> None:
    """A repository-shaped tiny world (as M2j): the world root carries a byte copy of HEAD's code dirs (git archive)
    and every script runs from that copy; tiny.json's script paths are made relative to the world root."""
    head = git(REPO, "rev-parse", "HEAD").strip()
    root = base_dir(which)
    if not root.exists():
        print(json.dumps(TW.build(root, TW.DEFAULT_SEED, bin_dir=BIN, python=PY, build_type=BUILD_TYPE), indent=2))
        tar = root.parent / f"p9-m4-code-{which}.tar"
        subprocess.run(["git", "archive", "-o", str(tar), head, *CODE_DIRS], cwd=REPO, check=True)
        subprocess.run(["tar", "-xf", tar.as_posix(), "-C", root.as_posix()], check=True)
        tar.unlink()
    spec_p = root / TW.SPEC
    spec = json.loads(spec_p.read_text(encoding="utf-8"))
    prefix = REPO.as_posix() + "/"
    for section in TW.SCRIPT_SECTIONS:
        if section in spec and str(spec[section].get("script", "")).startswith(prefix):
            spec[section]["script"] = spec[section]["script"][len(prefix):]
    spec["summ"] = {"script": "atx-impl/tools/nav_summ.py", "dsr_n": "ledger+1", "ledger": "trials.jsonl",
                    "origin": "prior"}
    spec["nav"]["leverage"] = PARENT_L       # M2j attempt 1: inside verify's mechanics band
    spec["nav"]["rule"] = PARENT_RULE[which]   # M4
    if which[-1] in "ep":                      # M4: as root's re-pin of the P9 lineage parents (wave_fixture.pin_eval_verb)
        spec["exes"]["eval"] = EVAL_EXE.as_posix()
        spec["exes_sha256"] = dict(spec.get("exes_sha256") or {}, eval=sha_bytes(EVAL_EXE.read_bytes()))
    write_json(spec_p, spec)
    (root / "trials.jsonl").write_bytes(b"")
    if not (root / ".git").exists():
        git(root, "init", "-q")
        git(root, "config", "user.name", "root-m4-tinyworld")   # M4
        git(root, "config", "user.email", "root-m4@localhost")
        git(root, "config", "core.autocrlf", "false")
    if which.endswith("p"):                    # M4: W1-I2 (b) / B2-10 (b): the parent pins ic, nav and eval (lock --exes)
        done = subprocess.run([PY, "scripts/research_cycle.py", "lock", "tiny.json", "--root", str(root), "--exes",
                               "--write"], cwd=root, capture_output=True, text=True)
        print(done.stdout[-2000:], done.stderr[-2000:], f"lock --exes exit {done.returncode}", sep=chr(10))
    git(root, "add", "--", "tiny.json", "trials.jsonl", *CODE_DIRS)
    if git(root, "status", "--porcelain", "--", "tiny.json", "trials.jsonl", *CODE_DIRS).strip():
        git(root, "commit", "-q", "-m", f"tiny world parent ({which}) on a git archive of {head} (M4 C3-5)")
    done = subprocess.run([PY, "scripts/research_cycle.py", "run", "tiny.json", "--root", str(root), "--stop-after",
                           "nav"], cwd=root, capture_output=True, text=True)
    print(done.stdout[-4000:], done.stderr[-3000:], f"parent ({which}) run exit {done.returncode}", sep="\n")


def manifest(label: str, mode: str, template: str, rules: list[dict]) -> dict:
    return {"schema": "atx.research-wave/v2", "wave": f"m4-{label}", "kind": "rule",   # M4
            "description": f"Root M4 C3-5 probe (synthetic, tiny world seed 7): wave {label}, gross_match {mode}, "
                           f"template {template}, rules {[r['name'] for r in rules]}; stops before judge (zero "
                           "trials).",
            "parent": {"spec": "tiny.json", "library": "tiny_world_v1"},
            "fields": {"dir": "fields", "manifest_sha256": FIELDS_PIN},
            "acceptance": {"rule": "pm7-34", "printed": []}, "gross_match": mode,
            "budget": {"id": "p9-kinds-fixture", "construction_cap": 1000}, "ledger": "trials.jsonl",
            "expect": {"n_before": 0}, "out_dir": f"waves/m4-{label}",   # M4
            "rule_cell": {"template": f"{TDIR}/{template}", "template_sha256": None}, "rules": rules}


AIM_V5 = [{"name": "aim-partial-v5", "params": {"trade_fraction": 0.1}}]
CLOCK = [{"name": "info-clock-rate-v1", "params": {}}]
WAVES = {   # M4: label: (base, gross_match, template, rules, --until)
    "d3a": ("v5", "pm6-6-engine", "rule-nav.json", AIM_V5, "verify"),    # attempt 1: the base pins no eval verb
    "d3a2": ("v5e", "pm6-6-engine", "rule-nav.json", AIM_V5, "verify"),  # (a)
    "d3b2": ("v6e", "pm6-6-engine", "rule-nav.json", AIM_V5, "verify"),  # (b)
    "clk": ("v5e", "pm6-6-engine", "rule-nav.json", CLOCK, "spec"),      # AL-CLOCK's repeat
    "d3a3": ("v5p", "pm6-6-engine", "rule-nav.json", AIM_V5, "verify"),  # (a), parent exes pinned (W1-I2 (b))
    "d3b3": ("v6p", "pm6-6-engine", "rule-nav.json", AIM_V5, "verify"),  # (b), parent exes pinned
    "clk3": ("v5p", "pm6-6-engine", "rule-nav.json", CLOCK, "spec"),     # AL-CLOCK's repeat, parent exes pinned
}
SEAL_FORMS = (re.compile(r"(?<![0-9])((?:19|20)[0-9]{2})-([01][0-9])-([0-3][0-9])(?![0-9])"),   # wave_seal FORMS
              re.compile(r"(?<![A-Za-z0-9.])((?:19|20)[0-9]{2})([01][0-9])([0-3][0-9])(?![A-Za-z0-9.])"))


def seal_hits(root: Path, stderr: str) -> tuple[dict, list[str]]:
    """SEAL-ALLOW-3 per-hit source check (as M2j): every 2024+ date token in the files the verify refusal names must
    sit on a run's own wall-clock stamp line (``"started_utc"``); returns ({token: ruling}, problems)."""
    found = re.search(r"date token\(s\) at or after the seal \S+ in \[(.*?)\]", stderr)
    allow, problems = {}, []
    if not found:
        return {}, ["no seal-token refusal to check"]
    for rel in re.findall(r"'([^']+)'", found.group(1)):
        for n, line in enumerate((root / rel).read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            for form in SEAL_FORMS:
                for mt in form.finditer(line):
                    if int(mt.group(1)) < 2024:
                        continue
                    tok = mt.group(0)
                    print(f"SEAL-HIT {rel}:{n}: {tok}: {line.strip()[:160]}")
                    if '"started_utc"' not in line:
                        problems.append(f"{rel}:{n}: {tok} is not on a started_utc stamp line")
                    allow[tok.split("T")[0]] = SEAL_RULING
    return allow, problems


def wave(label: str) -> int:
    which, mode, template, rules, until = WAVES[label]
    root = REPO / "build-equity" / f"p9-m4-tw-{label}"   # M4
    if not root.exists():
        shutil.copytree(base_dir(which), root)
    m = manifest(label, mode, template, rules)
    m["rule_cell"]["template_sha256"] = sha_bytes((root / TDIR / template).read_bytes())
    mrel = f"scripts/specs/p9/kinds/m4-{label}.json"   # M4
    write_json(root / mrel, m)
    git(root, "add", "--", "scripts")
    if git(root, "status", "--porcelain", "--", "scripts").strip():   # only this wave's manifest
        git(root, "commit", "-q", "-m", f"M4 wave {label} manifest")
    argv = [PY, "scripts/research_cycle.py", "wave", "run", mrel, "--root", str(root), "--until", until]
    done = subprocess.run(argv, cwd=root, capture_output=True, text=True)
    print(done.stdout, done.stderr, sep="\n")
    print(f"WAVE {label} exit {done.returncode}")
    if done.returncode == 4 and "date token(s) at or after the seal" in done.stderr and \
            "mechanics FAIL" not in done.stderr:
        allow, problems = seal_hits(root, done.stderr)
        if problems:
            print("SEAL CHECK FAILED (no --seal-allow):", *problems, sep="\n  ")
            return done.returncode
        extra = [x for tok, rule in sorted(allow.items()) for x in ("--seal-allow", f"{tok}={rule}")]
        print(f"SEAL CHECK OK: every hit is a started_utc stamp; resuming with {extra}")
        done = subprocess.run(argv + extra, cwd=root, capture_output=True, text=True)
        print(done.stdout, done.stderr, sep="\n")
        print(f"WAVE {label} (resumed) exit {done.returncode}")
    return done.returncode


def tokens(argv: list[str]) -> str:
    rules = [argv[i + 1] for i, t in enumerate(argv) if t == "--rule" and i + 1 < len(argv)]
    rates = [argv[i + 1] for i, t in enumerate(argv) if t == "--rate" and i + 1 < len(argv)]
    tf = [argv[i + 1] for i, t in enumerate(argv) if t == "--trade-fraction" and i + 1 < len(argv)]
    return f"--rule x{len(rules)} {rules}; --rate x{len(rates)} {rates}; --trade-fraction {tf}"


def nav_argvs(label: str) -> int:
    """Every NAV command the wave carries (as M2j): each bounded-run receipt under the world root whose command runs
    the NAV exe's nav verb, and the planned NAV argv of every cell spec the wave wrote (research_cycle.py plan)."""
    root = REPO / "build-equity" / f"p9-m4-tw-{label}"
    found = 0
    for rec in sorted(root.rglob("receipt.json")):
        try:
            doc = json.loads(rec.read_text(encoding="utf-8"))
        except ValueError:
            continue
        cmd = doc.get("command") or []
        if len(cmd) < 2 or not str(cmd[0]).endswith("atx-equity-strategy-targets.exe") or cmd[1] != "nav":
            continue
        found += 1
        print(f"NAV {rec.parent.relative_to(root).as_posix()}: outcome {doc.get('outcome')} exit "
              f"{doc.get('exit_code')}; {tokens(cmd)}")
        print("   argv:", " ".join(cmd[1:]))
    print(f"NAV receipts: {found}")
    wdir = root / "waves" / f"m4-{label}" / "receipts"
    print("wave receipts:", sorted(p.name for p in wdir.glob("*.json")) if wdir.is_dir() else "none")
    for p in sorted(wdir.glob("*.failed-*.json")) if wdir.is_dir() else []:
        f = json.loads(p.read_text(encoding="utf-8"))
        print(f"failed receipt {p.name}: {str(f.get('error') or f.get('outputs') or '')[:600]}")
    specs = sorted({Path(x).relative_to(root).as_posix() for x in glob.glob(str(root / TDIR / f"*m4-{label}*.json"))})
    m = wdir / "06-match.json"
    rel = json.loads(m.read_text(encoding="utf-8"))["outputs"].get("record_spec") if m.is_file() else None
    if rel and rel not in specs:
        specs.append(rel)
    for rel in specs:
        cell = json.loads((root / rel).read_text(encoding="utf-8"))
        print(f"CELL {rel}: change.set nav.rule = {(cell.get('change') or {}).get('set', {}).get('nav.rule')}")
        done = subprocess.run([PY, "scripts/research_cycle.py", "plan", rel, "--root", str(root)], cwd=root,
                              capture_output=True, text=True)
        for line in done.stdout.splitlines():
            if " nav " in line and "--combined" in line:
                argv = [t.strip('"') for t in line.split()]
                print(f"PLANNED {Path(rel).name}: {tokens(argv)}")
                print("   argv:", line.strip()[:2000])
        print(f"plan {Path(rel).name} exit {done.returncode}")
        if done.returncode:
            print(done.stderr[-1500:])
    return 0


if __name__ == "__main__":
    verb = sys.argv[1]
    if verb == "base":
        base(sys.argv[2])
    elif verb == "wave":
        sys.exit(wave(sys.argv[2]))
    elif verb == "report":
        sys.exit(nav_argvs(sys.argv[2]))
