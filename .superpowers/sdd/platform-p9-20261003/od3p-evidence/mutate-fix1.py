"""OD3P teeth, fix round 1 (review R-1, S-1, S-2, S-3): remove one check at a time, run the test that should catch it,
restore the file byte for byte (sha256 compared). Same harness as mutate-r7.py.

Usage: "C:/Program Files/Python312/python.exe" mutate-fix1.py <own basetemp dir>"""
import hashlib
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path("C:/atx-wt/pool-21")
PY = "C:/Program Files/Python312/python.exe"
BT = Path(sys.argv[1])
CYCLE_T = "scripts/tests/test_research_cycle_od3.py"
RR = "scripts/research_roles.py"
HISTORY = "history = [c for c, r in zip(self.eras, self.roles) if is_history(r)]"

MUTATIONS = [
    ("F1 R-1: fit.output not this carry's output refused", RR, "        self.check_carry_output(res)\n",
     "        pass\n", CYCLE_T, "test_a_fit_output_that_is_not_this_carrys_is_refused_at_plan"),
    ("F2 R-1: the guard compares the drop list's pin", RR, "        if not got or got != want:",
     "        if not got or {k: v for k, v in got.items() if k != 'drop'} != "
     "{k: v for k, v in want.items() if k != 'drop'}:", CYCLE_T,
     "test_a_fit_output_that_is_not_this_carrys_is_refused_at_plan"),
    ("F3 R-1: a fresh fit.output per book plans pending and runs the carry (guard refusing its own output)", RR,
     '"eras": [[c.role_key, c.pin("role")] for c in self.eras[:-1]],',
     '"eras": [[c.role_key, c.pin("role")] for c in self.eras],', CYCLE_T,
     "test_each_history_book_gets_its_own_fresh_fit_output_and_the_carry_runs"),
    ("F4 S-1: the carry tool's list checks at plan time", RR, "            self.check_drop_list(res, doc)\n",
     "            pass\n", CYCLE_T, "test_the_carry_tools_list_checks_run_at_plan_time"),
    ("F5 S-2: probe P1, history = the non-anchor eras", RR, HISTORY, "history = list(self.eras[:-1])", CYCLE_T,
     "test_the_absent_flags_reach_only_the_history_roles_ic_passes"),
    ("F5b S-2: probe P1 against the whole lane file (review: NOT CAUGHT by the 12 drop tests)", RR, HISTORY,
     "history = list(self.eras[:-1])", CYCLE_T, "drop or absent"),
    ("F6 S-2: history = the first history era only", RR, HISTORY, HISTORY + "[:1]", CYCLE_T,
     "test_the_absent_flags_reach_only_the_history_roles_ic_passes"),
    ("F7 S-3: a history IC pass without the runner silently skipped", RR,
     "                if st.phase in ABSENT_PHASES:\n                    if not st.argv or \"--\" not in st.argv:",
     "                if st.phase in ABSENT_PHASES and st.argv and \"--\" in st.argv:\n                    if False:",
     CYCLE_T, "test_a_history_ic_pass_without_the_runner_is_refused_not_skipped"),
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
              f"{'byte-identical' if after == before else 'DIFFERS'} (sha256 {before[:12]})", flush=True)
        bad += (not caught) + (after != before)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
