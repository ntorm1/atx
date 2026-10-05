"""Root M2h: E2's positive live-table check (re-review 4, "What root must check at merge" item 1).

Reads the merged exes' rule envelopes (IC ``--list-rules --json``, NAV ``nav --list-rules --json``) through E2's own
argv / parser (wave_kinds.listing_argv / parse_listing) and checks:
  1. the live IC table equals the committed composition_rules_list.json (bytes and JSON) that E2's tests read;
  2. D1_SCHEMAS (E2's frozen D1 rows) vs the live rows (reported, not asserted: D2 re-pinned them, D1-PSCHEMA);
  3. unmapped_ic_rules(live) == []; ew-theme-std-v1 derives --composition; MR-4's two ids derive their flags;
  4. the live NAV table: fixed-v1 has an aim_leverage range [1, 2] (E2-10 / E2-13 (a)); new-nav-rule.json derives
     --aim-leverage 1.5; the test file's synthetic NAV_TABLE vs the live rows (reported);
  5. in E2's fake world served the LIVE tables: new-combination-rule.json runs to the spec stage; leverage-change.json
     (2.0) and new-nav-rule.json pass preflight.
Synthetic worlds only (wave_fixture); no data is opened. Usage: python e2-live-table-check.py BIN_DIR [BIN_DIR...]
"""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(REPO / "scripts"))
sys.path.insert(0, str(REPO / "scripts" / "tests"))
import wave_kinds as WK  # noqa: E402
import test_wave_kinds as T  # noqa: E402

FIXTURE = REPO / "atx-impl" / "tests" / "fixtures" / "composition_rules_list.json"
fails: list[str] = []


def check(label: str, ok: bool, detail: str = "") -> None:
    print(f"{'PASS' if ok else 'FAIL'}  {label}{(': ' + detail) if detail else ''}")
    if not ok:
        fails.append(label)


def listing(exe: Path, nav: bool) -> tuple[str, dict | None]:
    done = subprocess.run(WK.listing_argv(str(exe), nav), capture_output=True, text=True, timeout=60)
    assert done.returncode == 0, done.stderr
    return done.stdout, WK.parse_listing(done.stdout, WK.NAV_RULES if nav else WK.IC_RULES)


def row(table: dict, rid: str) -> dict:
    return next(r for r in table["rules"] if r["id"] == rid)


def fake_world(name: str, until: str, ic: dict, nav: dict) -> tuple[int, str]:
    with tempfile.TemporaryDirectory() as tmp:
        root = T.world(Path(tmp), T.load_fixture(name))
        code, log = T.wave(root, T.KindsFake(root, ic=ic, nav=nav), "run", "--until", until)
        return code, (log[-1] if log else "")


for bin_dir in map(Path, sys.argv[1:]):
    print(f"== {bin_dir}")
    ic_text, ic = listing(bin_dir / "atx-equity-strategy-ic.exe", False)
    nav_text, nav = listing(bin_dir / "atx-equity-strategy-targets.exe", True)
    check("IC envelope parses (atx.composition-rules/v1)", ic is not None)
    check("NAV envelope parses (atx.nav-rules/v1)", nav is not None)
    fixture_text = FIXTURE.read_text(encoding="utf-8")
    check("1. live IC listing == committed composition_rules_list.json (bytes, LF-normalised)",
          ic_text.replace("\r\n", "\n") == fixture_text.replace("\r\n", "\n"))
    check("1. live IC listing == committed composition_rules_list.json (JSON)", ic == json.loads(fixture_text))
    check("1. E2's test table ic_table(False) == live", T.ic_table(False) == ic)
    for rid, frozen in T.D1_SCHEMAS.items():
        live = row(ic, rid)["params_schema"]
        print(f"INFO  2. D1_SCHEMAS[{rid}] == live params_schema: {frozen == live}"
              + ("" if frozen == live else f" (live {json.dumps(live, sort_keys=True)})"))
    tables = {"ic": ic, "nav": nav}
    check("3. unmapped_ic_rules(live) == []", WK.unmapped_ic_rules(tables) == [], str(WK.unmapped_ic_rules(tables)))
    got = WK.rule_flags([{"name": "ew-theme-std-v1"}], {"ic": ic})
    check("3. rule_flags(ew-theme-std-v1, live IC)", got == ({"fit": {"--composition": "ew-theme-std-v1"}}, []), str(got))
    for rid, flag in (("theme-cov-hedge-v1", "--theme-hedge"), ("mom-volman-v1", "--mom-volman")):
        got = WK.rule_flags([{"name": rid}], {"ic": ic})
        check(f"3. MR-4 rule_flags({rid}, live IC)", got == ({"fit": {flag: rid}}, []), str(got))
    lev = row(nav, "fixed-v1")["params_schema"]["properties"]["aim_leverage"]
    check("4. live fixed-v1 aim_leverage range [1, 2]", (lev.get("minimum"), lev.get("maximum")) == (1, 2), str(lev))
    got = WK.rule_flags(T.load_fixture("new-nav-rule.json")["rules"], tables)
    check("4. new-nav-rule.json rule_flags on the live tables", got == ({"nav": {"--aim-leverage": "1.5"}}, []), str(got))
    for r in T.NAV_TABLE["rules"]:
        live = row(nav, r["id"])
        diff = [k for k in ("x-flag", "incompatible", "kind") if r.get(k) != live.get(k)]
        ls, ss = live["params_schema"], r["params_schema"]
        if ls.get("required") != ss.get("required"):
            diff.append(f"required live {ls.get('required')} vs test {ss.get('required')}")
        extra = sorted(set(ls.get("properties", {})) - set(ss.get("properties", {})))
        if extra:
            diff.append(f"live-only params {extra}")
        print(f"INFO  4. test NAV_TABLE[{r['id']}] vs live: {'equal' if not diff else '; '.join(diff)}")
    print(f"INFO  4. live NAV ids {[r['id'] for r in nav['rules']]}; capabilities {nav.get('capabilities')}")
    for name, until in (("new-combination-rule.json", "spec"), ("leverage-change.json", "preflight"),
                        ("new-nav-rule.json", "preflight")):
        code, last = fake_world(name, until, ic, nav)
        check(f"5. {name} --until {until} in the fake world on the live tables: exit {code}", code == 0, last[:200])

print(f"\nRESULT {'PASS' if not fails else 'FAIL ' + str(fails)}")
sys.exit(1 if fails else 0)
