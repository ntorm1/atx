"""The named rules a research wave applies by code (platform v8 lane YINFRA): what root applied by hand per wave.

Every rule is looked up by the name a wave manifest gives; a name not listed here is refused when the manifest loads,
so a new ruling is a new entry here (reviewed code), never prose.

  sign rules   (screen) the wave composition from the gate's admission rows, decided by the IC exe's sign verb (P9 lane
               SIGN, ruling C3-11: atx-equity-strategy-ic sign, the engine's research/admission pm7_35; the Python
               copy sign_pm7_35 is retired). Here only the ids the verb knows (SIGN_RULES), its input rows (sign_rows)
               and the reading of its receipt (verb_decision)
    pm7-35     additions: status admitted and runner sign = prior -> keep; admitted and runner sign 0 -> keep (R-2
               precedent); admitted and runner sign opposite to the prior -> drop; not admitted -> keep (weight 0 in
               the fit, R-2 / R-7 precedent). Replacements (a refinement, or a re-screen: add-alpha --replaces): keep
               only when admitted with runner sign = prior; otherwise the replaced member keeps its string (0 trials)
  acceptance   (judge) the verdict of the cell from the paired test and the mechanics
    pm7-34     accepted iff paired S2 net dSR > 0 AND mechanics PASS; the printed criteria are computed and decide nothing
    v8-prereg-5  accepted iff dSR > 0 AND mechanics AND every listed criterion is met (the pre-PM7-34 rule)
  criteria     printed (or deciding, under v8-prereg-5) comparisons of the cell's book with its parent's
  gross match  (match) Ruling PM6-6: a calibration run at the parent's L; |G - G_parent| <= .005 stands, else ONE
               correction L' = L x G_parent / G (4 decimals) and the matched run must be within .005 (else a stop)
    pm6-6-engine  the rule inside the nav exe (P9 C2, P15: nav --calibrate-gross G_parent): the run stage stops
               after the w phase and the match stage runs ONE NAV, which calibrates and publishes at the final L
    none       no matching: the run stage's NAV is the cell's
    pm6-6      RETIRED (P9 ruling C2-11): the Python match (two NAVs, L' computed here). A ledgered v1 manifest that
               names it still validates (plan and status of its state dir work); preflight, run and match refuse it,
               naming pm6-6-engine (retired_gross_match). There is no default: a manifest names gross_match (C2-9)
  mechanics    (verify) the v8 cell limits on the S2 daily CSV: all-rows gross in [.90, 1.05], |all-rows net| <= .02,
               tau mean <= .20, tau p95 <= .30, the NAV's accounting errors within its own tolerance
"""
from __future__ import annotations

import math

KEEP, DROP = "keep", "drop"
ADD, REPLACE = "add", "replace"


class RuleError(ValueError):
    pass


# ------------------------------------------------------------------ sign rules (screen)
# P9 lane SIGN (ruling C3-11): the screen stage's sign rule is the IC exe's `sign` verb (atx-equity-strategy-ic sign:
# the engine's research/admission pm7_35; atx-impl/src/strategy_sign_verb.hpp). Here only the rule ids the verb knows
# (a manifest naming another is refused at load), its input rows and the reading of its receipt; nothing decides.
SIGN_RULES = ("pm7-35",)
SIGN_RECEIPT = "atx.sign-receipt/v1"
SIGN_ECHO = ("id", "kind", "prior_sign", "status", "runner_sign")


def sign_rows(candidates: list[dict], rows: list[dict]) -> list[dict]:
    """The sign verb's rows, one per candidate in wave order: the manifest's {id, kind, prior_sign} with the status and
    runner sign of its row of the fit's admission.json. A candidate without an admission row is no decision: RuleError
    (the fit's admission.json lists every candidate; a missing row is a broken screen, never a drop)."""
    by_id = {r.get("id"): r for r in rows if isinstance(r, dict)}
    missing = [c["id"] for c in candidates if c["id"] not in by_id]
    if missing:
        raise RuleError(f"no admission row for {missing}: the screen did not score these strings (a stop, not a drop)")
    return [{"id": c["id"], "kind": c.get("kind", ADD), "prior_sign": int(c["prior_sign"]),
             "status": by_id[c["id"]].get("status"), "runner_sign": by_id[c["id"]].get("runner_sign")}
            for c in candidates]


def verb_decision(rule: str, rows: list[dict], receipt, rows_sha256: str) -> dict:
    """{rule, rows: [{id, kind, prior_sign, status, runner_sign, decision, reason}], kept, dropped} in wave order (the
    layout of the screen receipt and wave-result.json) from the sign verb's receipt for ``rows``, the rows file of
    SHA-256 ``rows_sha256``. RuleError when the receipt is not the verb's answer to exactly these rows under ``rule``:
    its schema, rule or rows SHA-256 differ, a row is missing, extra, out of order or echoed with another value, a
    verdict is not keep | drop, a reason is not text, or kept / dropped do not list the verdicts in wave order."""
    if not isinstance(receipt, dict) or receipt.get("schema") != SIGN_RECEIPT:
        raise RuleError(f"not a {SIGN_RECEIPT} receipt of the sign verb")
    if receipt.get("rule") != rule or receipt.get("rows_sha256") != rows_sha256:
        raise RuleError(f"the receipt is for rule {receipt.get('rule')!r} on rows sha256 "
                        f"{str(receipt.get('rows_sha256'))[:12]}, not {rule!r} on {rows_sha256[:12]}")
    got = receipt.get("rows")
    if not isinstance(got, list) or len(got) != len(rows):
        raise RuleError(f"the receipt lists {len(got) if isinstance(got, list) else 'no'} rows for {len(rows)}")
    out = []
    for want, row in zip(rows, got):
        if not isinstance(row, dict) or any(row.get(k) != want.get(k) for k in SIGN_ECHO):
            raise RuleError(f"the receipt's row {row!r:.200} is not the screen's {want!r:.200}")
        if row.get("verdict") not in (KEEP, DROP) or not isinstance(row.get("reason"), str):
            raise RuleError(f"the receipt's row {want['id']} has verdict {row.get('verdict')!r}, not keep | drop")
        out.append(dict({k: want[k] for k in SIGN_ECHO}, decision=row["verdict"], reason=row["reason"]))
    kept = [r["id"] for r in out if r["decision"] == KEEP]
    dropped = [r["id"] for r in out if r["decision"] == DROP]
    if receipt.get("kept") != kept or receipt.get("dropped") != dropped:
        raise RuleError("the receipt's kept / dropped do not list its verdicts in wave order")
    return {"rule": rule, "rows": out, "kept": kept, "dropped": dropped}


# ------------------------------------------------------------------ mechanics (verify)
MECHANICS = {"v8-mech": {"gross_all_rows": (0.90, 1.05), "abs_net_all_rows_max": 0.02, "tau_mean_max": 0.20,
                         "tau_p95_max": 0.30}}


def mechanics_check(m: dict, rule: str = "v8-mech") -> dict:
    """{rule, pass, rows: [{check, value, limit, pass}]} of the mechanics keys ``m`` (wave_readers mechanics)."""
    lim = MECHANICS[rule]
    acc = m.get("accounting") or {}
    tol = acc.get("tolerance")
    lo, hi = lim["gross_all_rows"]
    g, net = m.get("mean_gross_leverage_all_rows"), m.get("mean_net_leverage_all_rows")
    rows = [("gross_all_rows", g, f"[{lo}, {hi}]", _num(g) and lo <= g <= hi),
            ("abs_net_all_rows", None if net is None else abs(net), f"<= {lim['abs_net_all_rows_max']}",
             _num(net) and abs(net) <= lim["abs_net_all_rows_max"]),
            ("tau_mean", m.get("tau_gmv_mean"), f"<= {lim['tau_mean_max']}",
             _num(m.get("tau_gmv_mean")) and m["tau_gmv_mean"] <= lim["tau_mean_max"]),
            ("tau_p95", m.get("tau_gmv_p95"), f"<= {lim['tau_p95_max']}",
             _num(m.get("tau_gmv_p95")) and m["tau_gmv_p95"] <= lim["tau_p95_max"])]
    for key in ("max_return_identity_error", "max_cash_book_relative_error"):
        v = acc.get(key)
        rows.append((key, v, f"<= {tol}", _num(v) and _num(tol) and v <= tol))
    out = [{"check": c, "value": v, "limit": lm, "pass": bool(ok)} for c, v, lm, ok in rows]
    return {"rule": rule, "pass": all(r["pass"] for r in out), "rows": out}


def _num(x) -> bool:
    return isinstance(x, (int, float)) and not isinstance(x, bool) and math.isfinite(x)


# ------------------------------------------------------------------ gross matching (match)
GROSS_MATCH = {"none": None,
               "pm6-6-engine": {"tolerance": 0.005, "corrections": 1, "engine": True}}  # P9 C2 (P15)
# P9 ruling C2-11: a retired mode -> the mode that replaces it. Accepted by validate_names (a ledgered v1 manifest keeps
# its validation), refused by every stage that would run it (retired_gross_match)
RETIRED_GROSS_MATCH = {"pm6-6": "pm6-6-engine"}


def retired_gross_match(mode) -> str | None:
    """The refusal of a retired gross_match name (P9 ruling C2-11), naming the mode that replaces it; None for a live
    or an unknown name (validate_names refuses an unknown one at load)."""
    new = RETIRED_GROSS_MATCH.get(mode) if isinstance(mode, str) else None
    if new is None:
        return None
    return (f'gross_match "{mode}" is retired (ruling C2-11: the Python gross match is gone): the manifest names '
            f'"{new}" (PM6-6 inside the NAV exe); a ledgered v1 manifest is replayed from a copy that names "{new}" '
            "(ruling C2-9)")


# ------------------------------------------------------------------ criteria and acceptance (judge)
def _per_gross(b: dict):
    t, g = b.get("tau_gmv_mean"), b.get("mean_gross_leverage_all_rows")
    return t / g if _num(t) and _num(g) and g > 0 else None


def _cmp(a, b, op) -> bool | None:
    return None if not (_num(a) and _num(b)) else op(a, b)


CRITERIA = {
    "turnover-per-gross-not-higher": ("turnover per unit gross (tau_gmv_mean / all-rows gross) not higher than the "
                                      "parent's", lambda c, p: _cmp(_per_gross(c), _per_gross(p), lambda x, y: x <= y)),
    "capacity-4x-higher": ("net Sharpe at 4x NAV (capacity_curve.csv) higher than the parent's",
                           lambda c, p: _cmp(c.get("x4_net_sharpe"), p.get("x4_net_sharpe"), lambda x, y: x > y)),
    "cost-bps-lower": ("S2 cost_bps_traded lower than the parent's",
                       lambda c, p: _cmp(c.get("cost_bps_traded"), p.get("cost_bps_traded"), lambda x, y: x < y)),
}
ACCEPTANCE = {"pm7-34": {"decides": ("dsr_positive", "mechanics"),
                         "text": "paired S2 net dSR > 0 AND mechanics (PM7-34); criteria printed, decide nothing"},
              "v8-prereg-5": {"decides": ("dsr_positive", "mechanics", "criteria"),
                              "text": "paired S2 net dSR > 0 AND mechanics AND every listed criterion (prereg rule 5)"}}


def criteria_rows(names: list[str], cell: dict, parent: dict) -> list[dict]:
    out = []
    for n in names:
        text, fn = CRITERIA[n]
        met = fn(cell, parent)
        out.append({"criterion": n, "text": text, "met": met, "printed": "met" if met else
                    ("unmet" if met is False else "not computable")})
    return out


def judge(rule: str, dsr, mechanics_pass: bool, criteria: list[dict]) -> dict:
    """{rule, text, accepted, checks{dsr_positive, mechanics, criteria}, decided_by} by the named acceptance rule."""
    spec = ACCEPTANCE[rule]
    checks = {"dsr_positive": bool(_num(dsr) and dsr > 0), "mechanics": bool(mechanics_pass),
              "criteria": all(c["met"] is True for c in criteria)}
    accepted = all(checks[k] for k in spec["decides"])
    return {"rule": rule, "text": spec["text"], "accepted": accepted, "checks": checks,
            "decided_by": list(spec["decides"]), "dsr": dsr}


def validate_names(sign_rule: str | None, acceptance: str, criteria: list[str], gross_match: str | None) -> list[str]:
    """Problems with the rule names a manifest gives (empty: every name is known). A retired gross_match name is known
    (RETIRED_GROSS_MATCH: refused when a stage would run it, not here); gross_match None (absent) is the caller's
    problem to state (wave_manifest: ruling C2-9)."""
    out = []
    if sign_rule is not None and sign_rule not in SIGN_RULES:
        out.append(f"sign_rule {sign_rule!r} is not one of {', '.join(SIGN_RULES)}")
    if acceptance not in ACCEPTANCE:
        out.append(f"acceptance.rule {acceptance!r} is not one of {', '.join(ACCEPTANCE)}")
    out += [f"acceptance.printed {c!r} is not one of {', '.join(CRITERIA)}" for c in criteria if c not in CRITERIA]
    if gross_match is not None and gross_match not in GROSS_MATCH and gross_match not in RETIRED_GROSS_MATCH:
        out.append(f"gross_match {gross_match!r} is not one of {', '.join(GROSS_MATCH)}")
    return out
