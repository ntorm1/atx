"""D3W-PB's D2-3 deletion as a reproducible edit (ruling D2-3; TEETH-RESTORE form for the trial run).

    python pb-deletion.py apply    -- save the three files' bytes, then move composition_resid.py's numpy reference of
                                      the runner's rule into test_composition_resid.py and retire its G-P5 row
    python pb-deletion.py restore  -- write the saved bytes back, then assert `git status --porcelain` is empty and
                                      each file's content equals its HEAD blob (git hash-object)

The trial run (apply, the lane's tests, restore) records the evidence before the records commit; the deletion commit
is the same `apply` on the records commit (identical bytes), committed last.
"""
import pathlib
import subprocess
import sys

REPO = pathlib.Path("C:/atx-wt/pool-23")
SAVE = pathlib.Path("C:/Users/natha/AppData/Local/Temp/claude/c--atx/a3de34c1-d7ad-4b5c-9079-b9c90532d424/"
                    "scratchpad/d3wpb/deletion-saved")
FILES = ("atx-impl/tools/composition_resid.py", "atx-impl/tools/test_composition_resid.py",
         "scripts/tests/test_no_python_mirror.py")
SECTION = "# ---------------------------------------------------------------- reference of the runner's rule (numpy)\n"
REFS = ("centred_tied_ranks", "residual", "tie_block_means", "standardise", "add_session", "kernel", "blend")


def text_of(path: pathlib.Path) -> tuple[str, bool]:
    raw = path.read_bytes()
    return raw.decode("utf-8").replace("\r\n", "\n"), b"\r\n" in raw


def write(path: pathlib.Path, text: str, crlf: bool) -> None:
    path.write_bytes((text.replace("\n", "\r\n") if crlf else text).encode("utf-8"))


def replace_once(text: str, old: str, new: str) -> str:
    assert text.count(old) == 1, old[:100]
    return text.replace(old, new)


def apply() -> None:
    SAVE.mkdir(parents=True, exist_ok=True)
    for rel in FILES:
        (SAVE / rel.replace("/", "__")).write_bytes((REPO / rel).read_bytes())
    resid_path = REPO / FILES[0]
    resid, resid_crlf = text_of(resid_path)
    at = resid.index(SECTION)
    reference = resid[at + len(SECTION):]
    resid = resid[:at].rstrip("\n") + "\n"
    resid = replace_once(resid, "import json\n\nimport numpy as np\n\nimport composition_engine_fit",
                         "import json\n\nimport composition_engine_fit")
    resid = replace_once(resid, """per session. ``blend`` below is
the independent numpy reference of the runner's rule (least squares by ``numpy.linalg.lstsq``) and ``kernel`` its
plane-level form (the arguments of add_theme_residualised), the Python side of the "Python equals C++" check
(strategy_ic_theme_resid_test.cpp pins the same fixture values). No TRAIN statistic""", """per session; the C++ is the rule's
only producer (P9 lane D3W-PB, ruling D2-3: the independent numpy reference of the runner's rule, ``blend`` and its
plane-level form ``kernel``, moved to test_composition_resid.py, where both sides meet the same exact values). No TRAIN
statistic""")
    write(resid_path, resid, resid_crlf)

    test_path = REPO / FILES[1]
    test, test_crlf = text_of(test_path)
    body = reference
    for name in REFS:  # the reference's own calls go to the moved definitions
        body = body.replace(f"def {name}(", f"def _{name}(")
        for call in (f" {name}(", f"({name}(", f"[{name}("):
            body = body.replace(call, call.replace(name, f"_{name}"))
    body = body.replace("tolerance: float = SPAN_TOLERANCE", "tolerance: float = cres.SPAN_TOLERANCE")
    for name in REFS:
        assert f"def _{name}(" in body, name
    block = ("\n\n# ---------------------------------------------------------------- the runner's rule: numpy reference\n"
             "# P9 lane D3W-PB (ruling D2-3): the independent numpy reference of theme-resid-v1 that composition_resid.py\n"
             "# held until the C++ (atx-engine composition/theme_resid.hpp add_theme_residualised, the IC runner's\n"
             "# residualise stage) became the rule's only producer. A test may hold an independent oracle (G-P5 scans no\n"
             "# test): both sides meet EXPECTED_BLEND / TIE_EXPECTED, the exact values of the rule.\n"
             + body.rstrip("\n") + "\n\n\n"
             "ref = types.SimpleNamespace(**{name: globals()[f\"_{name}\"] for name in (\n"
             "    \"centred_tied_ranks\", \"residual\", \"tie_block_means\", \"standardise\", \"add_session\", \"kernel\", \"blend\")})\n")
    anchor = "\n\nclass DeclaredRule(unittest.TestCase):"
    test = replace_once(test, anchor, block + anchor)
    test = replace_once(test, "import tempfile\nimport unittest\n", "import tempfile\nimport types\nimport unittest\n")
    test = replace_once(test, """built
here identically; both sides compare against the same exact values of the rule (EXPECTED_BLEND), which is the "Python
equals C++" check of the brief.""", """built
here identically; both sides compare against the same exact values of the rule (EXPECTED_BLEND), which is the "Python
equals C++" check of the brief. The numpy side (``ref``) lives in this file since P9 lane D3W-PB (ruling D2-3).""")
    for name in REFS:
        test = test.replace(f"cres.{name}(", f"ref.{name}(")
    # the wrong tie steps are injected where the moved kernel looks its tie step up
    test = replace_once(test, 'unittest.mock.patch.object(cres, "tie_block_means", ',
                        'unittest.mock.patch.object(sys.modules[__name__], "_tie_block_means", ')
    write(test_path, test, test_crlf)

    guard_path = REPO / FILES[2]
    guard, guard_crlf = text_of(guard_path)
    guard = replace_once(guard, """    ("atx-impl/tools/composition_resid.py", ("centred_tied_ranks", "tie_block_means"), COMPOSITION,
     "atx-impl/src/strategy_ic_theme_resid.cpp over engine combine/group_residualise.hpp", "D3"),
""", """    # D3 part B (lane D3W-PB) retired composition_resid.py's row: its numpy reference moved to its test. The other
    # D3 rows wait for a fitter that builds every document from composition_fit.json (P10).
""")
    write(guard_path, guard, guard_crlf)
    print("applied")


def restore() -> None:
    for rel in FILES:
        (REPO / rel).write_bytes((SAVE / rel.replace("/", "__")).read_bytes())
    status = subprocess.run(["git", "-C", str(REPO), "status", "--porcelain"], capture_output=True, text=True,
                            check=True).stdout
    assert status.strip() == "", status
    for rel in FILES:
        work = subprocess.run(["git", "-C", str(REPO), "hash-object", rel], capture_output=True, text=True,
                              check=True).stdout.strip()
        head = subprocess.run(["git", "-C", str(REPO), "rev-parse", f"HEAD:{rel}"], capture_output=True, text=True,
                              check=True).stdout.strip()
        assert work == head, (rel, work, head)
    print("restored; git status --porcelain empty; blobs equal HEAD")


if __name__ == "__main__":
    {"apply": apply, "restore": restore}[sys.argv[1]]()
