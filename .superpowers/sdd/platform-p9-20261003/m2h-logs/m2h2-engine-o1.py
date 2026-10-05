"""Root M2h finish: E2's match stage on REAL exes on the tiny world (re-review 4 "What root must check" item 2) and O1
(ruling E2-16). Pre-run list: m2h2-prerun.md (same dir). Synthetic only: tiny_world seed 7, its own empty ledger
trials.jsonl inside the world root; every wave stops at --until verify (zero trials, no record stage).

  python m2h2-engine-o1.py base            build the base world, git init, run the parent (--stop-after nav)
  python m2h2-engine-o1.py wave LABEL      copy the base to build-equity/p9-m2h-tw-LABEL and run wave LABEL
  python m2h2-engine-o1.py report LABEL    print the wave's receipts / specs / argvs as the evidence lines
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

REPO = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(REPO / "scripts"))
sys.path.insert(0, str(REPO / "scripts" / "tests" / "fixtures"))
import tiny_world as TW  # noqa: E402

BIN = REPO / "build-equity-rel" / "bin"
BASE = REPO / "build-equity" / "p9-m2h-tw"
PY = Path(sys.executable).as_posix()
CYCLE = (REPO / "scripts" / "research_cycle.py").as_posix()
KINDS = REPO / "scripts" / "specs" / "p9" / "kinds"
TDIR = "scripts/specs/p9/kinds/templates"
FIELDS_PIN = "395f82f7b34dd96e50403ff8b47faa58a3d7a94af433ddaaf906f596d039a1ac"
REF_TEMPLATE = "m2h-rule-ref.json"


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
# the gross mismatch: no IC rule moves the tiny world's weights (within-theme-v1 is a no-op with one admitted member
# per theme; every other rule needs a standardised composition, which the tiny world refuses by the member cap), and
# the live NAV table's rules set leverage (fixed-v1) or need a risk store; so the named rule is within-theme-v1 and the
# gross moves through a FREE NAV flag constant (a trading-rate change, as P9-C's trade-rate row would make)
TRADE = {"flags": {"nav": {"--trade-fraction": ".10"}}}
SEAL_RULING = "SEAL-ALLOW"


def base() -> None:
    """A repository-shaped tiny world: in git mode the bounded runner runs in ITS worktree (run_bounded_research.py
    --root defaults to the script's parents[1]), so the world root carries a byte copy of HEAD's code dirs (git
    archive) and every script runs from that copy; tiny.json's script paths are made relative to the world root."""
    head = git(REPO, "rev-parse", "HEAD").strip()
    if not BASE.exists():
        print(json.dumps(TW.build(BASE, TW.DEFAULT_SEED, bin_dir=BIN, python=PY, build_type="Release"), indent=2))
        tar = BASE.parent / "p9-m2h-code.tar"
        subprocess.run(["git", "archive", "-o", str(tar), head, *CODE_DIRS], cwd=REPO, check=True)
        subprocess.run(["tar", "-xf", tar.as_posix(), "-C", BASE.as_posix()], check=True)
        tar.unlink()
    spec_p = BASE / TW.SPEC
    spec = json.loads(spec_p.read_text(encoding="utf-8"))
    prefix = REPO.as_posix() + "/"
    for section in TW.SCRIPT_SECTIONS:
        if section in spec and str(spec[section].get("script", "")).startswith(prefix):
            spec[section]["script"] = spec[section]["script"][len(prefix):]
    spec["summ"] = {"script": "atx-impl/tools/nav_summ.py", "dsr_n": "ledger+1", "ledger": "trials.jsonl",
                    "origin": "prior"}
    # attempt 1: at L 1.247 the parent's all-rows S2 gross is 1.0972511252536685, outside verify's mechanics band
    # [0.9, 1.05]; at L 1.12 it sits inside (gross is about linear in L), so verify can pass on a matched cell
    spec["nav"]["leverage"] = PARENT_L
    write_json(spec_p, spec)
    (BASE / "trials.jsonl").write_bytes(b"")
    if not (BASE / ".git").exists():
        git(BASE, "init", "-q")
        git(BASE, "config", "user.name", "root-m2h-tinyworld")
        git(BASE, "config", "user.email", "root-m2h@localhost")
        git(BASE, "config", "core.autocrlf", "false")
    git(BASE, "add", "--", "tiny.json", "trials.jsonl", *CODE_DIRS)
    if git(BASE, "status", "--porcelain", "--", "tiny.json", "trials.jsonl", *CODE_DIRS).strip():
        git(BASE, "commit", "-q", "-m", f"tiny world parent on a git archive of {head} (M2h finish)")
    done = subprocess.run([PY, "scripts/research_cycle.py", "run", "tiny.json", "--root", str(BASE), "--stop-after",
                           "nav"], cwd=BASE, capture_output=True, text=True)
    print(done.stdout[-4000:], done.stderr[-3000:], f"parent run exit {done.returncode}", sep="\n")


def manifest(label: str, mode: str, template: str, rules: list[dict]) -> dict:
    return {"schema": "atx.research-wave/v2", "wave": f"m2h-{label}", "kind": "rule",
            "description": f"Root M2h finish (synthetic, tiny world seed 7): wave {label}, gross_match {mode}, "
                           f"template {template}, rules {[r['name'] for r in rules]}; stops at verify (zero trials).",
            "parent": {"spec": "tiny.json", "library": "tiny_world_v1"},
            "fields": {"dir": "fields", "manifest_sha256": FIELDS_PIN},
            "acceptance": {"rule": "pm7-34", "printed": []}, "gross_match": mode,
            "budget": {"id": "p9-kinds-fixture", "construction_cap": 1000}, "ledger": "trials.jsonl",
            "expect": {"n_before": 0}, "out_dir": f"waves/m2h-{label}",
            "rule_cell": {"template": f"{TDIR}/{template}", "template_sha256": None}, "rules": rules}


def ref_template(root: Path) -> dict:
    comb = "tiny-w-1/train_combined.json"
    return {"schema": "atx.research-cycle-template/v1", "name": "m2h-rule-ref",
            "description": "Root M2h O1 probe (synthetic): the rule-composition fixture plus the template's OWN whole "
                           "ref block (no leverage, no flags) on the parent's w combined signal.",
            "parent": None, "nominal_parent": "../../../../../tiny.json",
            "change": {"set": {"fit.output": "tiny-weights-m2h", "card.output": "tiny-cards-m2h",
                               "ic.w_output": "tiny-w-m2h", "nav.output": "tiny-nav-m2h",
                               "ref": {"output": "tiny-ref-m2h", "combined": "reference_combined"}},
                       "inputs": {"reference_combined": {"path": comb,
                                                         "sha256": sha_bytes((root / comb).read_bytes())}}}}


WITHIN = [{"name": "within-theme-v1", "params": {}}]
WAVES = {   # label: (gross_match, template, rules, rule_cell.constants)
    "eng0": ("pm6-6-engine", "rule-composition.json", WITHIN, TRADE),
    "eng": ("pm6-6-engine", REF_TEMPLATE, WITHIN, TRADE),
    "py": ("pm6-6", REF_TEMPLATE, WITHIN, TRADE),
    "pin": ("pm6-6-engine", "rule-nav.json", [{"name": "fixed-v1", "params": {"aim_leverage": 1.5}}], None),
}
SEAL_FORMS = (re.compile(r"(?<![0-9])((?:19|20)[0-9]{2})-([01][0-9])-([0-3][0-9])(?![0-9])"),   # wave_seal FORMS
              re.compile(r"(?<![A-Za-z0-9.])((?:19|20)[0-9]{2})([01][0-9])([0-3][0-9])(?![A-Za-z0-9.])"))


def seal_hits(root: Path, stderr: str) -> tuple[dict, list[str]]:
    """SEAL-ALLOW (b) per-hit source check: every 2024+ date token in the files the verify refusal names must sit on
    a run's own wall-clock stamp line (``"started_utc"``); returns ({token: ruling}, problems)."""
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
    mode, template, rules, constants = WAVES[label]
    root = REPO / "build-equity" / f"p9-m2h-tw-{label}"
    if not root.exists():
        shutil.copytree(BASE, root)
    if template == REF_TEMPLATE:
        write_json(root / TDIR / template, ref_template(root))
    m = manifest(label, mode, template, rules)
    if constants:
        m["rule_cell"]["constants"] = constants
    m["rule_cell"]["template_sha256"] = sha_bytes((root / TDIR / template).read_bytes())
    mrel = f"scripts/specs/p9/kinds/m2h-{label}.json"
    write_json(root / mrel, m)
    git(root, "add", "--", "scripts")
    if git(root, "status", "--porcelain", "--", "scripts").strip():   # only this wave's manifest / template
        git(root, "commit", "-q", "-m", f"M2h wave {label} manifest")
    argv = [PY, "scripts/research_cycle.py", "wave", "run", mrel, "--root", str(root), "--until", "verify"]
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


if __name__ == "__main__":
    verb = sys.argv[1]
    if verb == "base":
        base()
    elif verb == "wave":
        sys.exit(wave(sys.argv[2]))
