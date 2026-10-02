"""The named rules a research wave applies by code (platform v8 lane YINFRA): what root applied by hand per wave.

Every rule is looked up by the name a wave manifest gives; a name not listed here is refused when the manifest loads,
so a new ruling is a new entry here (reviewed code), never prose.

  sign rules   (screen) the wave composition from the gate's admission rows
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
GROSS_MATCH = {"pm6-6": {"tolerance": 0.005, "corrections": 1}, "none": None}


def matched_leverage(leverage: str | float, g_parent: float, g_cal: float) -> str:
    """Ruling PM6-6: L' = L x G_parent / G_cal, written with 4 decimals (as nav.leverage is)."""
    if not (_num(g_parent) and _num(g_cal) and g_cal > 0):
        raise RuleError(f"gross match: G_parent {g_parent!r} and G {g_cal!r} must be finite, G > 0")
    return f"{float(leverage) * g_parent / g_cal:.4f}"


def gross_matches(mode: str, g_parent: float, g: float) -> bool:
    rule = GROSS_MATCH[mode]
    return True if rule is None else abs(g - g_parent) <= rule["tolerance"]


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
    """Problems with the rule names a manifest gives (empty: every name is known)."""
    out = []
    if sign_rule is not None and sign_rule not in SIGN_RULES:
        out.append(f"sign_rule {sign_rule!r} is not one of {', '.join(SIGN_RULES)}")
    if acceptance not in ACCEPTANCE:
        out.append(f"acceptance.rule {acceptance!r} is not one of {', '.join(ACCEPTANCE)}")
    out += [f"acceptance.printed {c!r} is not one of {', '.join(CRITERIA)}" for c in criteria if c not in CRITERIA]
    if gross_match not in GROSS_MATCH:
        out.append(f"gross_match {gross_match!r} is not one of {', '.join(GROSS_MATCH)}")
    return out
