"""OD3P teeth, RESUME 7 (Ruling OD3P-1): remove one check at a time, run the test that should catch it, restore the
file byte for byte. R* are the OD3P-1 checks; M* are the earlier mutations (mutate.py) whose site still exists, re-run
on the OD3P-1 code (M4, M7, M8 tested code OD3P-1 removed: the theme_standardise refusal and the pinned zero)."""
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
    # ---- OD3P-1 (this round)
    ("R1 capability absent-members-v1 refused", RR,
     "        if history and ABSENT_CAPABILITY not in self.anchor.capabilities():", "        if False:", CYCLE_T,
     "test_drop_is_refused_when_the_ic_exe_lacks_the_capability"),
    ("R2 a drop with no history role refused", RR, "            if not any(is_history(r) for r in self.roles):",
     "            if False:", CYCLE_T, "test_drop_with_no_history_role_is_refused"),
    ("R3 history roles only", RR, "history = [c for c, r in zip(self.eras, self.roles) if is_history(r)]",
     "history = list(self.eras)", CYCLE_T, "test_drop_passes_the_absent_list_to_the_history_ic_passes_only"),
    ("R4 the flags reach the steps", RR, "            self.absent_members(per)  ", "            pass  # ", CYCLE_T,
     "test_drop_passes_the_absent_list_to_the_history_ic_passes_only"),
    ("R5 the runner binds the list", RR, "[\"--bind\", path] + st.argv[k:]", "st.argv[k:]", CYCLE_T,
     "test_drop_passes_the_absent_list_to_the_history_ic_passes_only"),
    ("R6 the u pass gets the flags too", RR, "if st.phase in ABSENT_PHASES and", "if st.phase == \"w\" and", CYCLE_T,
     "test_drop_passes_the_absent_list_to_the_history_ic_passes_only"),
    ("R7 the carry zeroes no weight", CW,
     "files[name] = canonical_bytes(dict(doc, train_manifest_sha256=sha, carried_from=block))",
     "files[name] = canonical_bytes(dict(doc, weights={k: (0.0 if drop and k in drop['members'] else v) for k, v in "
     "doc['weights'].items()}, train_manifest_sha256=sha, carried_from=block))", TOOLS_T,
     "test_drop_carries_train_bytes_and_only_records_the_list"),
    ("R7b the carry zeroes no weight (cycle run)", CW,
     "files[name] = canonical_bytes(dict(doc, train_manifest_sha256=sha, carried_from=block))",
     "files[name] = canonical_bytes(dict(doc, weights={k: (0.0 if drop and k in drop['members'] else v) for k, v in "
     "doc['weights'].items()}, train_manifest_sha256=sha, carried_from=block))", CYCLE_T,
     "test_drop_runs_the_real_carry_and_the_carried_weights_are_train_bytes"),
    ("R8 carry.json records the list", CW, "        record[\"drop\"] = {", "        record[\"_unused\"] = {", TOOLS_T,
     "test_drop_carries_train_bytes_and_only_records_the_list"),
    ("R9 no --era refused", CW, "        if not eras:\n            raise CarryError(\"a drop list applies",
     "        if False:\n            raise CarryError(\"a drop list applies", TOOLS_T,
     "test_drop_refusals_publish_nothing and no_era"),
    ("R10 theme-erc / ic-shrink bases accepted", CW, "    flagged = sorted(applied_rules(doc) & set(drop[\"rules\"]))",
     "    if (doc.get(\"theme_standardise\") or {}).get(\"rule\") not in (None, \"ew-theme-std-v1\"):\n"
     "        raise CarryError(\"re-derives and verifies\")\n"
     "    flagged = sorted(applied_rules(doc) & set(drop[\"rules\"]))", TOOLS_T,
     "test_drop_carries_train_bytes_and_only_records_the_list"),
    # ---- earlier checks, re-run on the OD3P-1 code
    ("M1 unknown member id", CW, "    if unknown:\n        raise CarryError(f\"drop list ids",
     "    if False:\n        raise CarryError(f\"drop list ids", TOOLS_T, "test_drop_refusals_publish_nothing and unknown_id"),
    ("M2 pending rules", CW, "    if pending:\n", "    if False:\n", TOOLS_T,
     "test_drop_refusals_publish_nothing and pending or test_the_committed_list"),
    ("M3 flagged rule in the base", CW, "    if flagged:\n", "    if False:\n", TOOLS_T,
     "test_drop_refusals_publish_nothing and flagged_rule"),
    ("M5 v1/v2 weights only", CW, "    if doc.get(\"schema\") not in WEIGHTS_SCHEMAS or",
     "    if False and doc.get(\"schema\") not in WEIGHTS_SCHEMAS or", TOOLS_T,
     "test_drop_refusals_publish_nothing and v3"),
    ("M6 repeated id", CW, "    if len(set(value)) != len(value):", "    if False:", TOOLS_T,
     "test_drop_refusals_publish_nothing and repeated_id"),
    ("M9 risk-model refusal on history", RR, "    check_history_risk_model(spec, roles)", "    pass", CYCLE_T,
     "test_a_book_reading_the_risk_model_is_refused_on_history"),
    ("M10 risk-model refusal reads ref.flags", RR, "for section in (\"nav\", \"ref\")\n            if any",
     "for section in (\"nav\",)\n            if any", CYCLE_T, "ref_flags"),
    ("M11 drop rule refusal in the cycle", RR, "        if named:\n", "        if False:\n", CYCLE_T,
     "test_drop_refuses_a_spec_that_applies_a_flagged_rule"),
    ("M12 drop list reaches the carry argv", RR, "\"--output\", w_dir, *drop]", "\"--output\", w_dir]", CYCLE_T,
     "test_drop_passes_the_absent_list or test_drop_runs_the_real_carry"),
    ("M13 T1 refusal in make_cycle", CLI,
     "    if isinstance(raw, dict) and isinstance(raw.get(\"fit\"), dict) and \"carry\" in raw[\"fit\"]:",
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
