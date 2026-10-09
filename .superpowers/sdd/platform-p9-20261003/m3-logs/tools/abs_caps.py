"""Root M3 (RESUME 10), ABS root item 4 and the exe half of item 6: the built IC exe's `--list-rules --json`
envelope against the committed fixture, and `scripts/cycle/phases.exe_capabilities` on the same exe.
usage (cwd = pool root): abs_caps.py EXE. Prints booleans, counts and capability names only."""
import hashlib
import json
import subprocess
import sys
from pathlib import Path

root = Path.cwd()
exe = Path(sys.argv[1])
fixture = root / "atx-impl/tests/fixtures/composition_rules_list.json"
done = subprocess.run([str(exe), "--list-rules", "--json"], cwd=root, capture_output=True)
print("exe", exe.name, "sha256", hashlib.sha256(exe.read_bytes()).hexdigest())
print("exit", done.returncode, "stderr bytes", len(done.stderr))
out = done.stdout.replace(b"\r\n", b"\n")
fix = fixture.read_bytes().replace(b"\r\n", b"\n")
print("stdout == fixture (LF-normalised bytes):", out == fix, "| as JSON:", json.loads(out) == json.loads(fix))
doc = json.loads(out)
caps = doc.get("capabilities")
print("schema", doc.get("schema"))
print("capabilities", caps)
print("sorted:", caps == sorted(caps), "| absent-members-v1 first:", bool(caps) and caps[0] == "absent-members-v1")
sys.path.insert(0, str(root / "scripts"))
from cycle import phases  # noqa: E402

got = phases.exe_capabilities(str(exe), root)
print("exe_capabilities holds absent-members-v1:", "absent-members-v1" in got, "| count", len(got))
