"""Run the dictionary's existing focused checks under the external host guard."""
from pathlib import Path
import runpy
import subprocess
import sys

project = Path(__file__).resolve().parents[3] / "atx-db"
module = runpy.run_path(str(project / "scripts/generate_data_dictionary.py"))
if "--refresh-only" in sys.argv[1:]:
    module["main"]([])
if module["main"](["--check"]) != 0:
    raise SystemExit(1)
subprocess.run(
    [sys.executable, "-m", "ruff", "check", "scripts/generate_data_dictionary.py"],
    cwd=project, check=True,
)
if "--refresh-only" in sys.argv[1:]:
    raise SystemExit(0)
subprocess.run(
    [sys.executable, "-m", "pytest", "tests/test_data_dictionary.py", "-n", "0", "-q"],
    cwd=project, check=True,
)
