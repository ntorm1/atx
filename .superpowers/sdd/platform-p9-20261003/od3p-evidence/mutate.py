"""OD3P teeth: remove one check at a time, run the test that should catch it, restore the file byte for byte."""
import hashlib
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path("C:/atx-wt/pool-21")
PY = "C:/Program Files/Python312/python.exe"
BT = Path(sys.argv[1])
TOOLS_T = "atx-impl/tools/test_od3_tooling.py"
CYCLE_T = "scripts/tests/test_research_cycle_od3.py"
CW = "atx-impl/tools/carry_weights.py"
RR = "scripts/research_roles.py"
CLI = "scripts/cycle/cli.py"

MUTATIONS = [
    ("M1 unknown member id", CW, "    if unknown:\n        raise CarryError(f\"drop list ids",
     "    if False:\n        raise CarryError(f\"drop list ids", TOOLS_T, "test_drop_refusals_publish_nothing and unknown_id"),
    ("M2 pending rules", CW, "    if pending:\n", "    if False:\n", TOOLS_T, "test_drop_refusals_publish_nothing and pending"),
    ("M3 flagged rule in the base", CW, "    if flagged:\n", "    if False:\n", TOOLS_T,
     "test_drop_refusals_publish_nothing and flagged_rule"),
    ("M4 verified theme_standardise rule", CW, "    if std is not None and rule not in ZERO_SAFE_STANDARDISE:",
     "    if False:", TOOLS_T, "test_drop_refusals_publish_nothing and theme_erc"),
    ("M5 v1/v2 weights only", CW, "    if doc.get(\"schema\") not in WEIGHTS_SCHEMAS or",
     "    if False and doc.get(\"schema\") not in WEIGHTS_SCHEMAS or", TOOLS_T,
     "test_drop_refusals_publish_nothing and v3"),
    ("M6 repeated id", CW, "    if len(set(value)) != len(value):", "    if False:", TOOLS_T,
     "test_drop_refusals_publish_nothing and repeated_id"),
    ("M7 drop zeroes (identity of kept / zero of dropped)", CW, "(0.0 if k in dropped else v)", "v", TOOLS_T,
     "test_drop_pins_the_listed_members_to_zero_on_the_history_eras_only"),
    ("M8 drop also at the anchor", CW, "files = {OUTPUT_WEIGHTS: weights, OUTPUT_ADMISSION: admission}",
     "files = {OUTPUT_WEIGHTS: canonical_bytes(era_doc) if drop else weights, OUTPUT_ADMISSION: admission}", TOOLS_T,
     "test_drop_pins_the_listed_members_to_zero_on_the_history_eras_only"),
    ("M9 risk-model refusal on history", RR, "    check_history_risk_model(spec, roles)", "    pass", CYCLE_T,
     "test_a_book_reading_the_risk_model_is_refused_on_history"),
    ("M10 risk-model refusal reads ref.flags", RR, "for section in (\"nav\", \"ref\")\n            if any",
     "for section in (\"nav\",)\n            if any", CYCLE_T, "ref_flags"),
    ("M11 drop rule refusal in the cycle", RR, "        if named:\n", "        if False:\n", CYCLE_T,
     "test_drop_refuses_a_spec_that_applies_a_flagged_rule"),
    ("M12 drop list reaches the carry argv", RR, "\"--output\", w_dir, *drop]", "\"--output\", w_dir]", CYCLE_T,
     "test_drop_list_reaches_the_carry"),
    ("M13 T1 refusal in make_cycle", CLI, "    if isinstance(raw, dict) and isinstance(raw.get(\"fit\"), dict) and \"carry\" in raw[\"fit\"]:",
     "    if False:", CYCLE_T, "test_carry_without_roles_is_refused"),
    ("M14 per-era label pins (T4)", RR, "        out[\"inputs\"][\"label_role\"] = {\"dir\": role[\"label_dir\"],",
     "        _unused = {\"dir\": role[\"label_dir\"],", CYCLE_T, "test_each_era_marks_its_nav_with_its_own_label_role"),
]


def run(test: str, expr: str) -> subprocess.CompletedProcess:
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
    return subprocess.run([PY, "-m", "pytest", "-q", "-p", "no:cacheprovider", "--basetemp", str(BT), test, "-k", expr],
                          cwd=ROOT, capture_output=True, text=True, env=env)


def main() -> int:
    bad = 0
    for name, rel, old, new, test, expr in MUTATIONS:
        path = ROOT / rel
        original = path.read_bytes()
        before = hashlib.sha256(original).hexdigest()
        text = original.decode("utf-8")
        nl = "\r\n" if "\r\n" in text else "\n"
        old_n, new_n = old.replace("\n", nl), new.replace("\n", nl)
        if text.count(old_n) != 1:
            print(f"{name}: SITE NOT UNIQUE ({text.count(old_n)})")
            bad += 1
            continue
        try:
            path.write_bytes(text.replace(old_n, new_n).encode("utf-8"))
            done = run(test, expr)
        finally:
            path.write_bytes(original)
        after = hashlib.sha256(path.read_bytes()).hexdigest()
        summary = (done.stdout.strip().splitlines() or [""])[-1]
        caught = done.returncode != 0
        print(f"{name}: {'CAUGHT' if caught else 'NOT CAUGHT'} (exit {done.returncode}; {summary}); restored "
              f"{'byte-identical' if after == before else 'DIFFERS'}")
        bad += (not caught) + (after != before)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
