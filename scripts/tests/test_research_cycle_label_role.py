"""research_cycle.py inputs.label_role (platform v8 Ruling E-25, cell B0c): the nav phase's --label-role pair.

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_research_cycle_label_role.py

Hash-only (no file is read beyond the pins): the spec key is accepted, the nav phase appends
--label-role PATH --label-role-sha256 PIN and binds the manifest in the runner line, the ref phase never gets them, a
spec without the key plans the same argv minus exactly those tokens, and a wrong pin stops before any phase.
"""
from __future__ import annotations

import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import research_cycle as RC  # noqa: E402

PINS = {"lib.json": "a" * 64, "role/manifest.json": "b" * 64, "label/manifest.json": "c" * 64,
        "ref/train_combined.json": "d" * 64}
LABEL = {"dir": "label", "path": "label/manifest.json", "sha256": "c" * 64}


def spec(label: bool, **over) -> dict:
    inputs = {"library": {"path": "lib.json", "sha256": "a" * 64},
              "role": {"dir": "role", "path": "role/manifest.json", "sha256": "b" * 64},
              "reference_combined": {"path": "ref/train_combined.json", "sha256": "d" * 64}}
    if label:
        inputs["label_role"] = dict(LABEL)
    s = {"schema": RC.SCHEMA, "name": "b0c", "python": sys.executable,
         "runner": {"script": "r.py", "seconds": 180, "max_rss_mib": 1536, "min_free_mib": 512},
         "exes": {"ic": "bin/ic.exe", "nav": "bin/nav.exe"}, "inputs": inputs,
         "fields": {"output": "F", "manifest_sha256": "f" * 64},
         "ic": {"u_output": "U", "w_output": "WT", "flags": []},
         "nav": {"output": "N", "rule": "aim-partial-v5", "flags": ["--warm-start-sessions", "252"]},
         "ref": {"output": "R", "combined": "reference_combined"}}
    s.update(over)
    return s


def nav_argv(cycle: RC.Cycle, phase: str = "nav") -> list[str]:
    nav = cycle.spec["nav"]
    return cycle.nav_step(phase, "N" if phase == "nav" else "R", "WT-1/train_combined.json", "e" * 64, nav,
                          "F/manifest.json", "role/manifest.json", "b" * 64).argv


def test_label_role_spec_key_reaches_the_nav_phase_only(tmp_path):
    RC.validate_spec(spec(True))                                   # the key is known
    on = RC.Cycle(spec(True), RC.Resolver(tmp_path, PINS))
    off = RC.Cycle(spec(False), RC.Resolver(tmp_path, PINS))
    assert on.pins["label_role"][:2] == ("label/manifest.json", "c" * 64)
    a, b = nav_argv(on), nav_argv(off)
    assert a[-4:] == ["--label-role", "label/manifest.json", "--label-role-sha256", "c" * 64]
    k = a.index("--")
    assert a[k - 2:k] == ["--bind", "label/manifest.json"]         # the manifest is bound in the receipt
    # flag off: the same argv minus exactly the bind pair and the two flags
    assert a[:k - 2] + a[k:-4] == b and "--label-role" not in b and "label/manifest.json" not in b
    # the ref phase (an identity against the unlabelled parent) never takes the label role
    assert nav_argv(on, "ref") == nav_argv(off, "ref")
    assert "--label-role" not in nav_argv(on, "ref")
    # the plan header lists the pin like every input
    assert any(line.startswith("# pin label_role: label/manifest.json " + "c" * 64) for line in RC.header(on))


def test_label_role_pin_and_shape_are_checked(tmp_path):
    bad = spec(True)
    bad["inputs"]["label_role"]["sha256"] = "0" * 64
    with pytest.raises(RC.CycleError) as e:
        RC.Cycle(bad, RC.Resolver(tmp_path, PINS))
    assert e.value.code == RC.EXIT_PIN and "PIN MISMATCH label_role" in str(e.value)
    no_path = spec(True)
    del no_path["inputs"]["label_role"]["path"]
    with pytest.raises(RC.CycleError) as e:
        RC.validate_spec(no_path)
    assert e.value.code == RC.EXIT_USAGE and "inputs.label_role" in str(e.value)
    typo = spec(False)
    typo["inputs"]["label_roles"] = dict(LABEL)
    with pytest.raises(RC.CycleError) as e:
        RC.validate_spec(typo)
    assert e.value.code == RC.EXIT_USAGE and "label_roles" in str(e.value)
