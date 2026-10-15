"""A4 teeth run (ruling TEETH-RESTORE): plant every mutation below, build the fields test target once
(scripts/research-build.ps1, tag argv[1]), run the A4 gtests, then restore the exact pre-mutation bytes
this tool kept, assert each file's SHA-256 equals its HEAD blob and `git status --porcelain` is empty.
Each mutation removes one rule; its named test must fail. Never git checkout / restore / stash.

  "C:/Program Files/Python312/python.exe" teeth-a4.py <tag>      (from anywhere; the repo is found from here)
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
LOG = Path(__file__).resolve().parent
FILTER = ("ResearchFieldsA4*:ResearchFieldsHoldings*")
MUTATIONS = [  # (id, file, exact old text, new text, the test that must fail)
    ("M1-rule6", "atx-engine/src/research/fields/reuse.cpp",
     "        why = history_differs(prior.manifest, plan, history);\n",
     "        (void)history_differs(prior.manifest, plan, history); // TEETH M1\n",
     "ResearchFieldsA4Reuse.PriceFieldReusedOnlyWithTheBuildsPanelHistory"),
    ("M2-union", "atx-engine/src/research/fields/research_fields_cli.cpp",
     "build_entries(spec, role, to_build, plans)",
     "build_entries(spec, role, to_build, to_build) /* TEETH M2 */",
     "ResearchFieldsA4Reuse.RebuiltPriceFieldUsesTheWholeBuildsPanel"),
    ("M3-digest-once", "atx-engine/src/research/fields/reuse.cpp",
     "    auto it = digests_.find(path);\n",
     "    auto it = digests_.end(); // TEETH M3\n    digests_.erase(path);\n",
     "ResearchFieldsA4Reuse.EachInputFileIsDigestedOncePerDecision"),
    ("M4-late-per-file", "atx-engine/src/research/fields/holdings_regsho.cpp",
     "    held_.push_back(Held{*k, *column, market, available, available > list});\n",
     "    held_.push_back(Held{*k, *column, market, available, available > list});\n"
     "    panel_.list_visible_ns[market * dates + *k] = std::max(list, available); // TEETH M4\n",
     "ResearchFieldsHoldingsKernel.RegshoLateRuleIsPerFileAcrossBatches"),
    ("M5-ring-evict", "atx-engine/src/research/fields/holdings_ftd.cpp",
     "  if (lo + kFtdWindow + 1 < next_) {",
     "  if (false && lo + kFtdWindow + 1 < next_) { // TEETH M5",
     "ResearchFieldsHoldingsKernel.FtdRingIsTheFullMatrixInAnyOrder"),
]


def git(*args: str) -> str:
    return subprocess.run(["git", "-C", str(ROOT), *args], check=True, capture_output=True, text=True).stdout


def at_head(rel: str) -> bool:
    """The working file is HEAD's blob: its git object id (eol filters applied, as git status sees it) is HEAD's."""
    return git("hash-object", "--", rel).strip() == git("rev-parse", f"HEAD:{rel}").strip()


def main() -> int:
    tag = sys.argv[1]
    if git("status", "--porcelain").strip():
        print("refused: the tree is not clean before the teeth run")
        return 2
    files = sorted({m[1] for m in MUTATIONS})
    kept = {rel: (ROOT / rel).read_bytes() for rel in files}
    out = []
    try:
        text = {rel: kept[rel].decode("utf-8") for rel in files}
        for mid, rel, old, new, _ in MUTATIONS:
            if text[rel].count(old) != 1:
                raise RuntimeError(f"{mid}: the old text is not unique in {rel}")
            text[rel] = text[rel].replace(old, new)
        for rel in files:
            (ROOT / rel).write_bytes(text[rel].encode("utf-8"))
        build = subprocess.run(["powershell", "-NoProfile", "-File", str(ROOT / "scripts" / "research-build.ps1"),
                                "-Tag", tag, "-Targets", "atx-engine-research-fields-tests"],
                               capture_output=True, text=True, errors="replace")
        out.append(f"build {tag}: exit {build.returncode}")
        if build.returncode == 0:
            exe = ROOT / "build-equity" / "bin" / "atx-engine-research-fields-tests.exe"
            run = subprocess.run([str(exe), f"--gtest_filter={FILTER}"], capture_output=True, text=True,
                                 errors="replace", cwd=str(exe.parent))
            (LOG / f"teeth-{tag}-gtest.log").write_text(run.stdout + run.stderr, encoding="utf-8")
            names = (line[len("[  FAILED  ] "):].split(" (")[0].strip()
                     for line in run.stdout.splitlines() if line.startswith("[  FAILED  ] "))
            failed = {x for x in names if "." in x and not x[0].isdigit()}
            out.append(f"gtest exit {run.returncode}; failed: {sorted(failed)}")
            for mid, _, _, _, test in MUTATIONS:
                out.append(f"{mid}: {test} {'CAUGHT' if test in failed else 'MISSED'}")
        else:
            out.append(build.stdout[-3000:] + build.stderr[-2000:])
    finally:
        for rel in files:
            (ROOT / rel).write_bytes(kept[rel])
        restored = all((ROOT / rel).read_bytes() == kept[rel] and at_head(rel) for rel in files)
        clean = not git("status", "--porcelain").strip()
        out.append(f"restored to HEAD blobs: {restored}; git status --porcelain empty: {clean}")
        (LOG / f"teeth-{tag}.log").write_text("\n".join(out) + "\n", encoding="utf-8")
        print("\n".join(out))
    return 0 if restored and clean else 3


if __name__ == "__main__":
    sys.exit(main())
