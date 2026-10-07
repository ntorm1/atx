"""Teeth of the D3W Python refusals: each mutation alone (one line disabled), its tests run, the file restored."""
import os
import subprocess
import sys
from pathlib import Path

REPO = Path(r"C:/atx-wt/pool-23")
CEF = "atx-impl/tools/composition_engine_fit.py"
TESTS = ["atx-impl/tools/test_fit_walk_forward.py", "atx-impl/tools/test_composition_engine_fit.py"]
BASETEMP = Path(sys.argv[1])
MUTATIONS = [
    ("M-P1 WALK_FORWARD_REFUSED", CEF,
     '    _require_wf(getattr(args, "engine_fit", False) and getattr(args, "theme_tsmom", None) is None and',
     '    _require_wf(True or getattr(args, "engine_fit", False) and getattr(args, "theme_tsmom", None) is None and',
     "test_check_walk_forward_before_any_compute"),
    ("M-P2 POST_ONLY", CEF,
     "    _require(not post_step(args) or getattr(args, POST_ARMED, None) is not None, POST_ONLY, error)",
     "    _require(True or not post_step(args) or getattr(args, POST_ARMED, None) is not None, POST_ONLY, error)",
     "test_check_walk_forward_before_any_compute or test_fit_alone_is_refused"),
    ("M-P3 walk_forward row shape", CEF,
     "                    any(w > 0 for w in row[\"weights\"]),",
     "                    True,",
     "test_a_malformed_block_is_refused"),
    ("M-P4 weights_v3 top-level theme", CEF,
     "            _require_wf(theme_of[cid] in top,",
     "            _require_wf(True or theme_of[cid] in top,",
     "WeightsV3"),
    ("M-P5 stale stage", CEF,
     "    _require(not stage.exists() and not hooks.pending(stage).exists(),",
     "    _require(True or not stage.exists() and not hooks.pending(stage).exists(),",
     "test_an_existing_output_or_a_stale_stage_is_refused_before_the_fit"),
    ("M-P6 engine_sleeve P12", CEF,
     "    _require(not differ, f\"the written sleeve differs from the C++ fit's (ruling P12): {differ[:20]}\", error)",
     "    _require(True or not differ, f\"the written sleeve differs from the C++ fit's (ruling P12): {differ[:20]}\", error)",
     "test_engine_sleeve_refusals"),
    ("M-P7 strict int/float", CEF,
     "        if strict and isinstance(want, int) != isinstance(got, int):",
     "        if False and strict and isinstance(want, int) != isinstance(got, int):",
     "test_strict_types_refuse or test_parent_check_engine_mode_is_strict"),
    ("M-P8 factor rows objects", CEF,
     "    _require(isinstance(rows, list) and all(isinstance(c, dict) for c in rows),",
     "    _require(True or isinstance(rows, list) and all(isinstance(c, dict) for c in rows),",
     "test_factor_series_reads_are_refusals"),
]
env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
for n, (name, rel, old, new, k) in enumerate(MUTATIONS):
    path = REPO / rel
    original = path.read_bytes()
    text = original.decode("utf-8")
    assert text.count(old) == 1, (name, text.count(old))
    try:
        path.write_bytes(text.replace(old, new).encode("utf-8"))
        done = subprocess.run([sys.executable, "-m", "pytest", "-q", "-p", "no:cacheprovider",
                               f"--basetemp={BASETEMP / f'm{n}'}", *TESTS, "-k", k],
                              cwd=REPO, env=env, capture_output=True, text=True, timeout=1800)
    finally:
        path.write_bytes(original)
    tail = [line for line in done.stdout.splitlines() if line.startswith(("FAILED", "SUBFAILED"))]
    summary = done.stdout.strip().splitlines()[-1] if done.stdout.strip() else done.stderr[-300:]
    print(f"{name}: exit {done.returncode}; {summary}")
    for line in tail[:6]:
        print(f"    {line[:180]}")
    assert path.read_bytes() == original
print("restored:", subprocess.run(["git", "status", "--short", "--", *[m[1] for m in MUTATIONS]], cwd=REPO,
                                  capture_output=True, text=True).stdout.strip() or "clean")
