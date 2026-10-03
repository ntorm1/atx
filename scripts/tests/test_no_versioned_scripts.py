"""Guard G-P6 (P9 lane T1): no new versioned script.

A versioned script is a Python file whose name carries a `_v<digits>` token (`foo_v2.py`, `foo_v8_quarters.py`) or a
`generate_*_v*` file of any extension: a copy of a script made for a new version instead of a spec entry or a flag
(PM8-12, plan DEC-5). Every such file in the repository (tracked, or untracked and not ignored, so a new file fails
before it is committed) must be a row of ALLOWLIST, which is frozen at the P9 wave-1 base d7c1c520 and only shrinks:

  - a file that is not a row fails (no new versioned script; pick an unversioned name, a flag or a spec entry);
  - a row whose file is gone fails (the commit that deletes a file deletes its row, so the list cannot go stale);
  - the list never grows past its frozen size, and every row names the lane that retires it (P10 = after P9).

Out of scope, with the reason: atx-db/ (a separate package with its own CI and owners; P9 lanes never touch it),
archive/ (archived code, never run), .superpowers/ (sprint working state: studies and review diffs), docs/, and the
build trees build*/ and deps/ (derived artifacts and run outputs).

  "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_no_versioned_scripts.py
"""
from __future__ import annotations

from pathlib import Path
import re
import subprocess

REPO = Path(__file__).resolve().parents[2]
FROZEN_AT = "d7c1c520"
OUT_OF_SCOPE = ("atx-db/", "archive/", ".superpowers/", "docs/", "deps/")
VERSION_TOKEN = re.compile(r"_v\d+[a-z]*(?:_|\.py$)")
GENERATOR = re.compile(r"^generate_.*_v\d")
LANES = ("T1", "A3", "A4", "B2", "C2", "C3", "D2", "D3", "E2", "P10")
CLASS_C = "class-C generator (audit section 4), deleted after root's slice-2 --check"

# path -> (retired_by, why it exists). Frozen at FROZEN_AT: delete rows, never add one.
ALLOWLIST = {
    "atx-engine/tools/research_fields_v8.py": ("A4", "Python field builders (v8 set); C++ builder kinds replace them"),
    "atx-engine/tools/research_fields_v9.py": ("A4", "Python field builders (v9 set); C++ builder kinds replace them"),
    "atx-engine/tools/test_research_fields_v8.py": ("A4", "test of research_fields_v8.py"),
    "atx-engine/tools/test_research_fields_v8_quarters.py": ("A4", "test of research_fields_v8.py"),
    "atx-engine/tools/test_research_fields_v9_earn.py": ("A4", "test of research_fields_v9.py"),
    "atx-engine/tools/test_research_fields_v9_nt.py": ("A4", "test of research_fields_v9.py"),
    "atx-engine/tools/test_linked_operating_v2.py": ("P10", "test of the linked-operating-v2 role rule (role builder "
                                                            "stays Python in P9; migration slice 9)"),
    "atx-engine/tools/test_linked_operating_v3.py": ("P10", "test of the linked-operating-v3 role rule"),
    "atx-impl/strategies/check_fund_ic_v6.py": ("T1", CLASS_C),
    "atx-impl/strategies/generate_fund_ic_v4.py": ("T1", CLASS_C),
    "atx-impl/strategies/generate_fund_ic_v42.py": ("T1", CLASS_C),
    "atx-impl/strategies/generate_fund_ic_v5.py": ("T1", CLASS_C),
    "atx-impl/strategies/generate_fund_ic_v6.py": ("T1", CLASS_C),
    "atx-impl/strategies/generate_fund_ic_v61.py": ("T1", CLASS_C),
    "atx-impl/strategies/generate_fund_ic_v70.py": ("T1", CLASS_C),
    "atx-impl/strategies/generate_fund_ic_v71.py": ("T1", CLASS_C),
    "atx-impl/strategies/generate_price_volume_ic96_v2.py": ("T1", CLASS_C),
    "atx-impl/strategies/generate_pv_fields_ic121_v3.py": ("T1", CLASS_C),
    "atx-impl/strategies/test_check_fund_ic_v6_ops.py": ("T1", "test of a class-C file"),
    "atx-impl/strategies/test_generate_fund_ic_v4.py": ("T1", "test of a class-C file"),
    "atx-impl/strategies/test_generate_fund_ic_v42.py": ("T1", "test of a class-C file"),
    "atx-impl/strategies/test_generate_fund_ic_v5.py": ("T1", "test of a class-C file"),
    "atx-impl/strategies/test_generate_fund_ic_v6.py": ("T1", "test of a class-C file"),
    "atx-impl/strategies/test_generate_fund_ic_v61.py": ("T1", "test of a class-C file"),
    "atx-impl/strategies/test_generate_fund_ic_v70.py": ("T1", "test of a class-C file"),
    "atx-impl/strategies/test_generate_fund_ic_v71.py": ("T1", "test of a class-C file"),
    "atx-impl/strategies/test_generate_pv_fields_ic121_v3.py": ("T1", "test of a class-C file"),
    "atx-impl/tools/test_mega_report_v8.py": ("P10", "tests of the v8 report protocol (rename after P9)"),
    "atx-impl/tools/test_mega_report_v8_render.py": ("P10", "tests of the v8 report protocol (rename after P9)"),
    "atx-impl/tools/test_nav_summ_v8.py": ("P10", "tests of nav_summ --protocol v8 (rename after P9)"),
}
FROZEN_SIZE = 30


def is_versioned(rel: str) -> bool:
    name = rel.rsplit("/", 1)[-1]
    return bool(GENERATOR.search(name)) or (name.endswith(".py") and bool(VERSION_TOKEN.search(name)))


def in_scope(rel: str) -> bool:
    return not rel.startswith(OUT_OF_SCOPE) and not rel.split("/", 1)[0].startswith("build")


def repository_files() -> list[str]:
    done = subprocess.run(["git", "ls-files", "--cached", "--others", "--exclude-standard"], cwd=REPO,
                          capture_output=True, text=True, check=True)
    return [line for line in done.stdout.splitlines() if line]


def versioned_files() -> list[str]:
    return sorted(rel for rel in repository_files() if in_scope(rel) and is_versioned(rel) and (REPO / rel).is_file())


def test_every_versioned_script_is_allowlisted():
    new = [rel for rel in versioned_files() if rel not in ALLOWLIST]
    assert not new, ("new versioned script(s) (G-P6, DEC-5): use an unversioned name, a flag or a spec entry; the "
                     f"allowlist is frozen at {FROZEN_AT}: {new}")


def test_every_allowlist_row_names_an_existing_file():
    stale = [rel for rel in ALLOWLIST if not (REPO / rel).is_file()]
    assert not stale, f"rows whose file is gone: delete them in the commit that deleted the file: {stale}"


def test_the_allowlist_only_shrinks_and_names_a_retiring_lane():
    assert len(ALLOWLIST) <= FROZEN_SIZE, f"the allowlist frozen at {FROZEN_AT} gained rows"
    assert all(lane in LANES and why for lane, why in ALLOWLIST.values())
    assert all(is_versioned(rel) and in_scope(rel) for rel in ALLOWLIST)


def test_the_name_rule():
    for rel in ("a/foo_v2.py", "a/foo_v8_quarters.py", "a/generate_fund_ic_v42.py", "a/generate_x_v1.json",
                "a/test_foo_v3.py", "a/foo_v2b.py"):
        assert is_versioned(rel), rel
    for rel in ("a/foo.py", "a/vol_v.py", "a/generate_library.py", "a/foo_v2.json", "a/fund_industry_ic_v71.json",
                "a/kv2.py", "a/foo_version.py"):
        assert not is_versioned(rel), rel
    assert not in_scope("atx-db/x_v2.py") and not in_scope("build-equity/audits/x_v2.py") and in_scope("scripts/x.py")
