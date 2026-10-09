"""COV-MV evidence: the registration and template resolve through the repo's own validators.

Run from the pool root with PYTHONDONTWRITEBYTECODE=1:
  python .superpowers/sdd/platform-p9-20261003/covmv-logs/validate_specs.py [NAV_RULES_JSON]
NAV_RULES_JSON: the output of the built NAV exe's `nav --list-rules --json` (optional): the fragment's rules
block is then derived through wave_kinds.rule_flags against it.
"""
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path.cwd()
sys.path.insert(0, str(ROOT / "scripts"))

import wave_manifest as WM  # noqa: E402
from cycle.spec import load_spec  # noqa: E402

reg_path = ROOT / "scripts/specs/p9/rules/mv-aim-v1.json"
tpl_path = ROOT / "scripts/specs/p9/templates/mv-aim.json"
reg = json.loads(reg_path.read_text(encoding="utf-8"))
tpl_bytes = tpl_path.read_bytes()
frag = reg["manifest_fragment"]
cell = frag["rule_cell"]
print("template CR bytes:", tpl_bytes.count(b"\r"))
print("template sha256:", hashlib.sha256(tpl_bytes).hexdigest())
print("registration sha256:", hashlib.sha256(reg_path.read_bytes()).hexdigest())
print("rule_cell.template_sha256 equals the file:", cell["template_sha256"] == hashlib.sha256(tpl_bytes).hexdigest())
print("rule_cell_problems:", WM.rule_cell_problems(cell))
print("registration_problems:", WM.registration_problems(reg, "registration"))
print("K-P9-11 keys equal the registration's:",
      all(cell[k] == reg[k] for k in ("source_sample_end", "predicted_mechanism", "data_class")))
print("gross_match accepted:", frag["gross_match"], frag["gross_match"] in ("pm6-6", "pm6-6-engine", "none"))
spec = load_spec(tpl_path)
print("load_spec:", spec.get("name"), "| nav.output", spec["nav"]["output"],
      "| monitor.output", spec.get("monitor", {}).get("output"))
print("no rules / change.flags in the template:",
      "rules" not in json.loads(tpl_bytes) and "flags" not in json.loads(tpl_bytes)["change"])
if len(sys.argv) > 1:
    import wave_kinds as WK  # noqa: E402
    nav_table = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
    flags, problems = WK.rule_flags(frag["rules"], {"nav": nav_table, "ic": {"rules": []}})
    print("rule_flags problems:", problems)
    print("derived flags:", json.dumps(flags, sort_keys=True))
