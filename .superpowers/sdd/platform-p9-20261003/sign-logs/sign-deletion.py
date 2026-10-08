"""SIGN's LAST commit: the Python PM7-35 sign rule (wave_rules.sign_pm7_35, SIGN_RULES' function, screen_decision)
deleted with its G-P5 row; the tests that called it move to the verb (its fake and the pinned column).
Run from the pool root."""
from pathlib import Path



def sub(path, pairs):
    p = Path(path)
    raw = p.read_bytes().decode("utf-8")
    crlf = "\r\n" in raw
    text = raw.replace("\r\n", "\n")
    for old, new in pairs:
        n = text.count(old)
        assert n == 1, (path, n, old[:120])
        text = text.replace(old, new)
    p.write_bytes((text.replace("\n", "\r\n") if crlf else text).encode("utf-8"))


def append(path, extra):
    p = Path(path)
    raw = p.read_bytes().decode("utf-8")
    crlf = "\r\n" in raw
    text = raw.replace("\r\n", "\n") + extra
    p.write_bytes((text.replace("\n", "\r\n") if crlf else text).encode("utf-8"))
sub("scripts/wave_rules.py", [
('''  sign rules   (screen) the wave composition from the gate's admission rows
    pm7-35     additions:''',
'''  sign rules   (screen) the wave composition from the gate's admission rows, decided by the IC exe's sign verb (P9 lane
               SIGN, ruling C3-11: atx-equity-strategy-ic sign, the engine's research/admission pm7_35; the Python
               copy sign_pm7_35 is retired). Here only the ids the verb knows (SIGN_RULES), its input rows (sign_rows)
               and the reading of its receipt (verb_decision)
    pm7-35     additions:'''),
('''# ------------------------------------------------------------------ sign rules (screen)
def sign_pm7_35(row: dict | None, kind: str, prior: int) -> tuple[str, str]:
    """(keep | drop, reason) of one screened candidate under PM7-35 (see the module doc). A candidate without an
    admission row is no decision: RuleError (the fit's admission.json lists every candidate; a missing row is a
    broken screen, never a drop)."""
    if row is None:
        raise RuleError("no admission row: the screen did not score this string (a stop, not a drop)")
    status, sign = row.get("status"), row.get("runner_sign")
    admitted = status == "admitted"
    if kind == REPLACE:
        if admitted and sign == prior:
            return KEEP, "replacement admitted with the prior sign"
        return DROP, (f"replacement not kept (status {status}, runner sign {sign}, prior {prior:+d}): the replaced "
                      "member keeps its string at 0 trials")
    if not admitted:
        return KEEP, f"addition not admitted (status {status}): stays at weight 0"
    if sign == prior:
        return KEEP, "addition admitted with the prior sign"
    if sign == 0:
        return KEEP, "addition admitted, runner sign 0 (R-2 precedent)"
    return DROP, f"addition admitted with runner sign {sign} against prior {prior:+d}: dropped from the wave"


SIGN_RULES = {"pm7-35": sign_pm7_35}


def screen_decision(rule: str, candidates: list[dict], rows: list[dict]) -> dict:
    """{rule, rows: [{id, kind, prior_sign, status, runner_sign, decision, reason}], kept, dropped} in wave order."""
    fn = SIGN_RULES[rule]
    by_id = {r.get("id"): r for r in rows if isinstance(r, dict)}
    out = []
    missing = [c["id"] for c in candidates if c["id"] not in by_id]
    if missing:
        raise RuleError(f"no admission row for {missing}: the screen did not score these strings (a stop, not a drop)")
    for c in candidates:
        row = by_id.get(c["id"])
        decision, reason = fn(row, c.get("kind", ADD), int(c["prior_sign"]))
        out.append({"id": c["id"], "kind": c.get("kind", ADD), "prior_sign": int(c["prior_sign"]),
                    "status": (row or {}).get("status"), "runner_sign": (row or {}).get("runner_sign"),
                    "decision": decision, "reason": reason})
    return {"rule": rule, "rows": out, "kept": [r["id"] for r in out if r["decision"] == KEEP],
            "dropped": [r["id"] for r in out if r["decision"] == DROP]}


# P9 lane SIGN (ruling C3-11): the screen stage's sign rule is the IC exe's `sign` verb (atx-equity-strategy-ic sign:
# the engine's research/admission pm7_35; atx-impl/src/strategy_sign_verb.hpp). Here only its input rows and the
# reading of its receipt; nothing below decides.
SIGN_RECEIPT''',
'''# ------------------------------------------------------------------ sign rules (screen)
# P9 lane SIGN (ruling C3-11): the screen stage's sign rule is the IC exe's `sign` verb (atx-equity-strategy-ic sign:
# the engine's research/admission pm7_35; atx-impl/src/strategy_sign_verb.hpp). Here only the rule ids the verb knows
# (a manifest naming another is refused at load), its input rows and the reading of its receipt; nothing decides.
SIGN_RULES = ("pm7-35",)
SIGN_RECEIPT'''),
])

sub("scripts/tests/test_no_python_mirror.py", [
('''    # P9 ruling C3-11 / C3B-S: lane SIGN retires it (the screen stage runs the IC exe's sign verb on B1's predicate).
    ("scripts/wave_rules.py", ("sign_pm7_35",), "PM7-35 sign rule (P9 ruling P9: kept in wave 1)",
     "atx-impl/src/strategy_sign_verb.cpp over engine research/admission/sign_rule.hpp pm7_35", "SIGN"),
''',
'''    # P9 lane SIGN (ruling C3-11) retired the PM7-35 sign rule row (sign_pm7_35): the screen stage runs the IC exe's
    # sign verb (atx-impl/src/strategy_sign_verb.cpp over engine research/admission/sign_rule.hpp pm7_35); the name
    # stays detected through FROZEN_PAIRS.
'''),
])

sub("scripts/tests/test_research_wave.py", [
('''def test_sign_rule_pm7_35_as_ruled():
    cases = [("add", ("admitted", 1), 1, "keep"), ("add", ("admitted", 0), 1, "keep"),
             ("add", ("admitted", -1), 1, "drop"), ("add", ("reject_redundant", -1), 1, "keep"),
             ("add", ("admitted", 1), -1, "drop"), ("replace", ("admitted", 1), 1, "keep"),
             ("replace", ("admitted", 0), 1, "drop"), ("replace", ("reject_redundant", 1), 1, "drop")]
    for kind, (status, sign), prior, want in cases:
        got, _ = WR.sign_pm7_35({"status": status, "runner_sign": sign}, kind, prior)
        assert got == want, (kind, status, sign, prior)
    with pytest.raises(WR.RuleError, match="no admission row"):                    # a stop, never a drop
        WR.sign_pm7_35(None, "add", 1)
    dec = WR.screen_decision("pm7-35", [F.candidate(c) for c in ONE_DROPPED],
                             [{"id": k, "status": s, "runner_sign": g} for k, (s, g) in ONE_DROPPED.items()])
    assert dec["kept"] == ["alpha_a", "alpha_c"] and dec["dropped"] == ["alpha_b"]
''',
'''def test_sign_rule_pm7_35_as_ruled(tmp_path):
    """PM7-35 as ruled, decided by the IC exe's sign verb (P9 lane SIGN: the engine's pm7_35; here its fake, which
    test_wave_sign_verb.py holds equal to the real exe row for row): the eight ruled cases, a stop on a string without
    an admission row, and the screen decision read from the verb's receipt."""
    cases = [("add", ("admitted", 1), 1, "keep"), ("add", ("admitted", 0), 1, "keep"),
             ("add", ("admitted", -1), 1, "drop"), ("add", ("reject_redundant", -1), 1, "keep"),
             ("add", ("admitted", 1), -1, "drop"), ("replace", ("admitted", 1), 1, "keep"),
             ("replace", ("admitted", 0), 1, "drop"), ("replace", ("reject_redundant", 1), 1, "drop")]
    for kind, (status, sign), prior, want in cases:
        got, _ = F.sign_oracle({"kind": kind, "status": status, "runner_sign": sign, "prior_sign": prior})
        assert got == want, (kind, status, sign, prior)
    with pytest.raises(WR.RuleError, match="no admission row"):                    # a stop, never a drop
        WR.sign_rows([F.candidate("alpha_a")], [])
    rows = WR.sign_rows([F.candidate(c) for c in ONE_DROPPED],
                        [{"id": k, "status": s, "runner_sign": g} for k, (s, g) in ONE_DROPPED.items()])
    F.write_json(tmp_path, "rows.json", {"rows": rows})
    argv = ["bin/ic.exe", "sign", "--rule", "pm7-35", "--rows", "rows.json", "--output", "receipt.json"]
    assert F.FakeCycle(tmp_path, {})(argv, tmp_path).returncode == 0
    dec = WR.verb_decision("pm7-35", rows, json.loads((tmp_path / "receipt.json").read_text(encoding="utf-8")),
                           F.sha((tmp_path / "rows.json").read_bytes()))
    assert dec["kept"] == ["alpha_a", "alpha_c"] and dec["dropped"] == ["alpha_b"]
'''),
])

sub("scripts/tests/test_wave_sign_verb.py", [
('''  a superset of its eight cases and of the decision table of research_admission_sign_test.cpp) the verb's verdict and
  reason equal the Python rule's (wave_rules.sign_pm7_35, before its deletion), the fake verb's
  (wave_fixture.sign_oracle) and the pinned verdict column PINNED; the receipt pins the rows file.''',
'''  a superset of its eight cases and of the decision table of research_admission_sign_test.cpp) the verb's verdict and
  reason equal the fake verb's (wave_fixture.sign_oracle) and the pinned verdict column PINNED; the receipt pins the
  rows file. Until SIGN's deletion commit the test also held both to wave_rules.sign_pm7_35, the Python rule (the
  identity of record: the pre-deletion head, task-SIGN-report.md); root's identity on the ledgered screen rows is
  sign-identity-ledgered.py in the lane's sprint dir.'''),
('''def test_the_pinned_column_and_the_fake_verb_are_the_python_rule():
    for (s, g, k, p), want in zip(COMBOS, PINNED):
        row = {"id": "x", "kind": k, "prior_sign": p, "status": s, "runner_sign": g}
        py = WR.sign_pm7_35({"status": s, "runner_sign": g}, k, p)
        assert py == F.sign_oracle(row), row
        assert py[0] == {"k": WR.KEEP, "d": WR.DROP}[want], row
''',
'''def test_the_fake_verb_is_the_pinned_column():
    for (s, g, k, p), want in zip(COMBOS, PINNED):
        row = {"id": "x", "kind": k, "prior_sign": p, "status": s, "runner_sign": g}
        assert F.sign_oracle(row)[0] == {"k": WR.KEEP, "d": WR.DROP}[want], row
    assert not hasattr(WR, "sign_pm7_35") and not hasattr(WR, "screen_decision")    # the Python rule is gone
'''),
('''def test_the_verb_equals_the_python_rule_on_every_enumerated_combination(tmp_path):''',
'''def test_the_verb_equals_the_pinned_rule_on_every_enumerated_combination(tmp_path):'''),
('''    for want, got, pin in zip(rows, receipt["rows"], PINNED):
        py = WR.sign_pm7_35({"status": want["status"], "runner_sign": want["runner_sign"]}, want["kind"],
                            want["prior_sign"])
        assert (got["verdict"], got["reason"]) == py, want
        assert (got["verdict"], got["reason"]) == F.sign_oracle(want), want''',
'''    for want, got, pin in zip(rows, receipt["rows"], PINNED):
        assert (got["verdict"], got["reason"]) == F.sign_oracle(want), want'''),
('''def test_the_screen_stage_runs_the_sign_verb_once_and_records_its_receipt(tmp_path, monkeypatch):
    def python_rule(*_a, **_k):
        raise AssertionError("the Python sign rule was called")
    monkeypatch.setattr(WR, "sign_pm7_35", python_rule)            # the stage never calls it
    monkeypatch.setattr(WR, "screen_decision", python_rule)
    root''',
'''def test_the_screen_stage_runs_the_sign_verb_once_and_records_its_receipt(tmp_path):
    root'''),
])
print("deletion edits applied")
