"""C3 fix round 1: flag-absent NAV byte identity before / after the round, and the --ann-mean-gross opt-in diff.

Synthetic tiny world only (scripts/tests/fixtures/tiny_world.py; 656 sessions ending 2022-12-30, simulated). Prints
counts and verdicts, never a statistic.

  old --bin B --work W   build the world in W/root with the exes in B (B's targets exe = the pre-round source), run the
                         cycle (research_cycle.py run --no-git, as the canary), record every nav command of the
                         cycle's bounded-run receipts in W/nav-commands.json, re-run each with --output W/old-<k>
                         (flag absent) and once with --ann-mean-gross (the pre-round exe must refuse it, exit 2)
  new --bin B --work W   re-run the same commands with B's exe: --output W/new-<k> (flag absent) and W/gross-<k>
                         (--ann-mean-gross); compare old-<k> vs new-<k> (every file byte-identical; summary.json after
                         substituting the producer's engine_git_sha value only) and new-<k> vs gross-<k> (every file
                         byte-identical but summary.json, which equals the absent file once each scenario's
                         "ann_mean_gross" line is deleted; one such line per scenario, nothing else differs)
Exit 0 only when every check holds.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys

sys.dont_write_bytecode = True
REPO = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(REPO / "scripts" / "tests" / "fixtures"))
import tiny_world as TW  # noqa: E402

CYCLE = REPO / "scripts" / "research_cycle.py"
NAV_EXE = "atx-equity-strategy-targets.exe"
GROSS_LINE = re.compile(rb'^[ ]*"ann_mean_gross": [^\n]*\n', re.MULTILINE)


def env() -> dict:
    e = dict(os.environ)
    e["PYTHONDONTWRITEBYTECODE"] = "1"
    return e


def files(root: Path) -> list[str]:
    return sorted(p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_file())


def with_output(command: list[str], exe: Path, output: Path, extra: list[str]) -> list[str]:
    args = [str(exe)] + command[1:]
    k = args.index("--output")
    args[k + 1] = str(output)
    return args + extra


def run(args: list[str], log: Path) -> int:
    """One NAV run from the world root (the cycle's argv names its inputs relative to it)."""
    done = subprocess.run(args, capture_output=True, env=env(), cwd=log.parent / "root")
    log.write_bytes(b"argv: " + " ".join(args).encode() + b"\nexit: " + str(done.returncode).encode() +
                    b"\n--- stdout\n" + done.stdout + b"--- stderr\n" + done.stderr)
    return done.returncode


def phase_old(bin_dir: Path, work: Path) -> int:
    root = work / "root"
    TW.build(root, bin_dir=bin_dir, build_type="Debug")
    done = subprocess.run([sys.executable, str(CYCLE), "run", str(root / TW.SPEC), "--root", str(root), "--no-git"],
                          cwd=root, capture_output=True, env=env())
    (work / "cycle-old.log").write_bytes(done.stdout + b"\n--- stderr\n" + done.stderr)
    print(f"cycle (pre-round targets exe): exit {done.returncode}")
    if done.returncode != 0:
        return 1
    commands = []
    for receipt in sorted(root.rglob("receipt.json")):
        doc = json.loads(receipt.read_bytes())
        command = doc.get("command") or []
        if doc.get("schema") == "atx.bounded-research-run/v1" and len(command) > 1 and \
                Path(command[0]).name == NAV_EXE and command[1] == "nav" and doc.get("exit_code") == 0:
            commands.append({"receipt": receipt.relative_to(root).as_posix(), "command": command})
    (work / "nav-commands.json").write_text(json.dumps(commands, indent=2) + "\n", encoding="utf-8")
    print(f"nav commands in the cycle's receipts: {len(commands)}")
    ok = len(commands) > 0
    exe = bin_dir / NAV_EXE
    for k, entry in enumerate(commands):
        code = run(with_output(entry["command"], exe, work / f"old-{k}", []), work / f"old-{k}.log")
        print(f"old-{k}: exit {code}, files {len(files(work / f'old-{k}')) if code == 0 else 0}")
        ok &= code == 0
        code = run(with_output(entry["command"], exe, work / f"oldgross-{k}", ["--ann-mean-gross"]),
                   work / f"oldgross-{k}.log")
        refused = code == 2 and not (work / f"oldgross-{k}").exists()
        print(f"old exe with --ann-mean-gross: exit {code}, no output dir {refused} (pre-round: unknown flag)")
        ok &= refused
    return 0 if ok else 1


def phase_new(bin_dir: Path, work: Path) -> int:
    commands = json.loads((work / "nav-commands.json").read_bytes())
    exe = bin_dir / NAV_EXE
    ok = len(commands) > 0
    for k, entry in enumerate(commands):
        old, new, gross = work / f"old-{k}", work / f"new-{k}", work / f"gross-{k}"
        code_new = run(with_output(entry["command"], exe, new, []), work / f"new-{k}.log")
        code_gross = run(with_output(entry["command"], exe, gross, ["--ann-mean-gross"]), work / f"gross-{k}.log")
        print(f"command {k} ({entry['receipt']}): new exit {code_new}, opt-in exit {code_gross}")
        if code_new != 0 or code_gross != 0:
            return 1
        # (1) before / after, flag absent.
        names = files(old)
        same_names = names == files(new)
        differing = [n for n in names if (old / n).read_bytes() != (new / n).read_bytes()] if same_names else names
        print(f"  old vs new: {len(names)} files, same file list {same_names}, byte-different {differing}")
        ok &= same_names and set(differing) <= {"summary.json"}
        if "summary.json" in differing:
            a, b = (old / "summary.json").read_bytes(), (new / "summary.json").read_bytes()
            sha_a = json.loads(a)["producer"]["engine_git_sha"]
            sha_b = json.loads(b)["producer"]["engine_git_sha"]
            needle = b'"engine_git_sha": "' + sha_a.encode() + b'"'
            once = a.count(needle) == 1
            substituted = a.replace(needle, b'"engine_git_sha": "' + sha_b.encode() + b'"')
            equal = once and substituted == b
            print(f"  summary.json: producer.engine_git_sha {sha_a} -> {sha_b}; the only difference: {equal}")
            ok &= equal
        stdout_old = (work / f"old-{k}.log").read_bytes().split(b"--- stdout\n", 1)[1].replace(
            str(old).encode(), b"<OUT>")
        stdout_new = (work / f"new-{k}.log").read_bytes().split(b"--- stdout\n", 1)[1].replace(
            str(new).encode(), b"<OUT>")
        print(f"  console (stdout + stderr, output path normalised) identical: {stdout_old == stdout_new}")
        ok &= stdout_old == stdout_new
        # (2) the opt-in changes only summary.json, by one ann_mean_gross line per scenario.
        same_names = files(new) == files(gross)
        differing = [n for n in files(new) if (new / n).read_bytes() != (gross / n).read_bytes()] \
            if same_names else files(new)
        print(f"  absent vs opt-in: same file list {same_names}, byte-different {differing}")
        ok &= same_names and differing == ["summary.json"]
        a, b = (new / "summary.json").read_bytes(), (gross / "summary.json").read_bytes()
        scenarios = len(json.loads(a)["scenarios"])
        lines = GROSS_LINE.findall(b)
        stripped = GROSS_LINE.sub(b"", b)
        keyed = all("ann_mean_gross" in s for s in json.loads(b)["scenarios"])
        print(f"  opt-in summary.json: {len(lines)} ann_mean_gross lines for {scenarios} scenarios, every scenario "
              f"keyed {keyed}, absent file has the key {b'ann_mean_gross' in a}, "
              f"opt-in minus those lines == absent bytes {stripped == a}")
        ok &= len(lines) == scenarios and keyed and b"ann_mean_gross" not in a and stripped == a
    print("RESULT", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("phase", choices=("old", "new"))
    parser.add_argument("--bin", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=args.phase == "new")
    return phase_old(args.bin, args.work) if args.phase == "old" else phase_new(args.bin, args.work)


if __name__ == "__main__":
    sys.exit(main())
