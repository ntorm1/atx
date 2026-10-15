"""AL-CLOCK fix round 1, review S-4: writes atx-impl/tests/fixtures/nav_rule_info_clock_rate_v1.json, the K-P9-7 row
info-clock-rate-v1 as `nav --list-rules --json` prints it (atx-impl/src/book/nav_rules.cpp row_json: id, kind, x-flag,
params_schema {type, additionalProperties, required, properties}, incompatible, description). The description and the
incompatible list are read from nav_rules.cpp itself (the adjacent string literals of the row, the info_clock_refuses
array), so the fixture is the source's text, not a transcription. InfoClockRate.RegistryRowAndParser checks the exe's
listed row equals the fixture; scripts/tests/test_info_clock_rule.py reads it as its NAV table's last row.

usage: python mk_row_fixture.py [--check]   (run from the pool root; --check compares, writes nothing)
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path.cwd()
SRC = ROOT / "atx-impl" / "src" / "book" / "nav_rules.cpp"
OUT = ROOT / "atx-impl" / "tests" / "fixtures" / "nav_rule_info_clock_rate_v1.json"


def literals(text: str) -> list[str]:
    return [m.group(1).encode("ascii").decode("unicode_escape") for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', text)]


def main() -> int:
    src = SRC.read_text(encoding="utf-8")
    refuses = re.search(r"info_clock_refuses\{(.*?)\};", src, re.S)
    row = re.search(r"\{nav_info_clock_rate_id, Kind::TradeRate, (.*?)\},\s*\};", src, re.S)
    if not refuses or not row:
        print("nav_rules.cpp: the row or its refusals not found")
        return 2
    lits = literals(row.group(1))
    x_flag, description = lits[0], "".join(lits[1:])
    doc = {"id": "info-clock-rate-v1", "kind": "trade-rate", "x-flag": x_flag,
           "params_schema": {"type": "object", "additionalProperties": False, "required": [], "properties": {}},
           "incompatible": literals(refuses.group(1)), "description": description}
    text = json.dumps(doc, indent=2, sort_keys=True) + "\n"
    if "--check" in sys.argv[1:]:
        got = json.loads(OUT.read_text(encoding="utf-8"))
        print("fixture equals the source's row:", got == doc)
        return 0 if got == doc else 1
    OUT.write_bytes(text.replace("\n", "\r\n").encode("ascii"))
    print(OUT, len(text), "bytes;", "x-flag", x_flag, "| incompatible", doc["incompatible"])
    print("description:", description)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
