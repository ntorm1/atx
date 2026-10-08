"""SIGN C++ teeth (TEST-1: ONE teeth build, every refusal of the sign verb disabled at once; TEETH-RESTORE).

Keeps the pre-mutation bytes of atx-impl/src/strategy_sign_verb.cpp itself, applies every mutation (each needle found
exactly once), builds the IC test target through scripts/research-build.ps1 (one tag), runs the named filter
SignVerb*, restores the bytes in a finally block, then asserts: the bytes equal the pre-mutation bytes, `git
hash-object` of the file equals HEAD's blob, `git status --porcelain` is empty. Never git checkout / restore / stash.
A mutation is caught when every marker it lists appears in the gtest output's failure lines.

  python sign-teeth-cpp.py --root C:/atx-wt/pool-29 --tag p9-sign-b --log <sprint>/sign-logs/sign-teeth-b.log
"""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import subprocess
import sys

FILE = "atx-impl/src/strategy_sign_verb.cpp"
TARGET = "atx-impl-strategy-ic-tests"
M = "SignVerb.RefusesMalformedRowsBeforeAnyOutput"
U = "SignVerb.UsageErrorsWriteNothing"
O = "SignVerb.RefusesAMissingRowsFileAndAnExistingOutput"
# (id, needle, replacement, markers that must show in the failure output)
MUTATIONS = [
    ("C1 unknown rule id accepted",
     "    if (rule.id == id) return rule;\n  }\n  return std::nullopt;\n",
     "    if (rule.id == id) return rule;\n  }\n  return kWaveSignRules.front();\n",
     [U, "unknown --rule pm7-36", "unknown --rule wave-zero-kept-v1"]),
    ("C2 unknown kind accepted",
     "    if (kind.name == name) return kind;\n  }\n  return std::nullopt;\n",
     "    if (kind.name == name) return kind;\n  }\n  return kKindNames.front();\n",
     [M, "unknown kind: ", "second row: "]),
    ("C3 integer type check off",
     "  if (!row.contains(key) || !row.at(key).is_number_integer()) return std::nullopt;\n",
     "  if (!row.contains(key)) return std::nullopt;\n",
     [M, "prior text: ", "prior bool: ", "prior float: ", "runner bool: "]),
    ("C4 integer range check off",
     "  if (v < lowest || v > highest) return std::nullopt;\n", "",
     [M, "prior 2: ", "runner 2: ", "runner -2: "]),
    ("C5 unsigned range check off",
     "  if (value.is_number_unsigned() && value.get<u64>() > static_cast<u64>(highest)) {\n",
     "  if (false) {\n",
     [M, "runner huge: "]),
    ("C6 prior 0 accepted",
     "  if (!prior || *prior == 0) {\n", "  if (!prior) {\n",
     [M, "prior 0: "]),
    ("C7 no admission row accepted",
     '  if (!row.contains("status") || !row.contains("runner_sign")) {\n', "  if (false) {\n",
     [M, "no status: ", "no runner sign: "]),
    ("C8 status check off",
     '  if (!non_empty_string(row, "status")) {\n', "  if (false) {\n",
     [M, "null status: ", "empty status: "]),
    ("C9 kind type check off",
     '  if (!row.contains("kind") || !row.at("kind").is_string()) {\n', "  if (false) {\n",
     [M, "no kind: "]),
    ("C10 id check off",
     '  if (!non_empty_string(row, "id")) return co::Err(refused(where + " needs a non-empty string id"));\n',
     '  if (false) return co::Err(refused(where + " needs a non-empty string id"));\n',
     [M, "no id: ", "empty id: "]),
    ("C11 row object check off",
     '  if (!row.is_object()) return co::Err(refused(where + " is not an object"));\n',
     '  if (false) return co::Err(refused(where + " is not an object"));\n',
     [M, "row not an object: "]),
    ("C12 document check off",
     "  if (doc.is_discarded() || !doc.is_object()) {\n", "  if (false) {\n",
     [M, "not JSON: ", "array root: "]),
    ("C13 rows array check off",
     '  if (!doc.contains("rows") || !doc.at("rows").is_array() || doc.at("rows").empty()) {\n',
     "  if (false) {\n",
     [M, "no rows: ", "rows an object: ", "empty rows: "]),
    ("C14 row count limit off",
     "  if (rows.size() > kMaxRows) {\n", "  if (false) {\n",
     [M, "too many rows: "]),
    ("C15 duplicate id accepted",
     '    if (!seen.insert(row.id).second) return co::Err(refused("duplicate id " + row.id));\n',
     '    if (false) return co::Err(refused("duplicate id " + row.id));\n',
     [M, "duplicate id: "]),
    ("C16 size limit off",
     "  if (size < 0 || static_cast<u64>(size) > kRowsLimit) {\n", "  if (size < 0) {\n",
     [M, "over 16 MiB: "]),
    ("C17 missing rows file not refused at open",
     "  if (!in) {\n    return co::Err(co::ErrorCode::IoError, \"sign: cannot open the rows file \"",
     "  if (false) {\n    return co::Err(co::ErrorCode::IoError, \"sign: cannot open the rows file \"",
     [O, 'find("cannot open the rows file")']),
    ("C18 existing output overwritten",
     "  if (fs::exists(output, ec) || ec) {\n", "  if (false) {\n",
     [O, 'find("the output must not exist (never overwritten)")']),
    ("C19 unknown flag ignored",
     '      p.problem = "unknown flag " + std::string(key);\n      return p;\n', "      continue;\n",
     [U, "unknown flag --row"]),
    ("C20 repeated flag accepted",
     "    if (i + 1 >= argc || !seen.insert(key).second) {\n", "    if (i + 1 >= argc) {\n",
     [U, "missing value or repeated flag --rule"]),
    ("C21 required flags not checked",
     "  if (p.cfg.rule.id.empty() || p.cfg.rows_path.empty() || p.cfg.output_path.empty()) {\n",
     "  if (false) {\n",
     [U, "--rule, --rows and --output are required"]),
]


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def git(root: Path, *args: str) -> str:
    return subprocess.run(["git", *args], cwd=root, capture_output=True, text=True, check=True).stdout.strip()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", type=Path, required=True)
    ap.add_argument("--tag", required=True)
    ap.add_argument("--log", type=Path, required=True)
    ap.add_argument("--check", action="store_true", help="only verify that every needle is found exactly once")
    a = ap.parse_args()
    if a.check:
        text = (a.root / FILE).read_bytes().decode("utf-8")
        nl = "\r\n" if "\r\n" in text else "\n"
        bad = [(mid, text.count(n.replace("\n", nl))) for mid, n, _r, _m in MUTATIONS
               if text.count(n.replace("\n", nl)) != 1]
        print(f"{len(MUTATIONS)} mutations; needles not found exactly once: {bad}")
        return 0 if not bad else 1
    assert git(a.root, "status", "--porcelain") == "", "the tree must be clean before a teeth run"
    path = a.root / FILE
    orig = path.read_bytes()
    text = orig.decode("utf-8")
    nl = "\r\n" if "\r\n" in text else "\n"
    for mid, needle, repl, _ in MUTATIONS:
        needle, repl = needle.replace("\n", nl), repl.replace("\n", nl)
        assert text.count(needle) == 1, (mid, text.count(needle))
        text = text.replace(needle, repl)
    lines: list[str] = []
    try:
        path.write_bytes(text.encode("utf-8"))
        build = subprocess.run(["powershell", "-NoProfile", "-File", str(a.root / "scripts/research-build.ps1"),
                                "-Tag", a.tag, "-Targets", TARGET], cwd=a.root, capture_output=True, text=True)
        lines.append(f"== build {a.tag} ({TARGET}, every refusal of {FILE} disabled): exit {build.returncode}")
        lines += [f"   {x}" for x in (build.stdout + build.stderr).splitlines()[-12:]]
        if build.returncode == 0:
            exe = a.root / "build-equity" / "bin" / f"{TARGET}.exe"
            run = subprocess.run([str(exe), "--gtest_filter=SignVerb*"], cwd=a.root, capture_output=True,
                                 text=True, timeout=900)
            out = run.stdout + run.stderr
            lines.append(f"== gtest {exe.name} --gtest_filter=SignVerb*: exit {run.returncode}")
            lines += out.splitlines()
    finally:
        path.write_bytes(orig)
    restored = path.read_bytes() == orig and sha(path.read_bytes()) == sha(orig)
    blob = git(a.root, "hash-object", FILE) == git(a.root, "rev-parse", f"HEAD:{FILE}")
    clean = git(a.root, "status", "--porcelain") == ""
    gtest = "\n".join(lines)
    rows = []
    for mid, _n, _r, markers in MUTATIONS:
        seen = all(m in gtest for m in markers)
        rows.append((mid, seen, [m for m in markers if m not in gtest]))
    lines.append("\n| mutation | caught | markers missing |\n|---|---|---|")
    lines += [f"| {mid} | {'yes' if ok else 'NO'} | {miss or ''} |" for mid, ok, miss in rows]
    caught = all(ok for _m, ok, _x in rows)
    lines.append(f"RESTORE bytes={restored} hash-object==HEAD blob={blob} git status --porcelain empty={clean}")
    lines.append(f"RESULT {'PASS: every refusal caught' if caught else 'FAIL'} ({sum(ok for _m, ok, _x in rows)} / "
                 f"{len(rows)})")
    a.log.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\n".join(lines[-(len(rows) + 5):]))
    return 0 if caught and restored and blob and clean else 1


if __name__ == "__main__":
    sys.exit(main())
