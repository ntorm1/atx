"""Lane XSIG (expansion X): static and semantic checks of the X screen set's frozen DSL strings. Python only.

Run from the repository root (no arguments; synthetic data only, reads no data payload):
    "C:/Program Files/Python312/python.exe" .superpowers/sdd/platform-v8-20260929/xsig_check.py

1. Offline mirror of the alpha compiler (rules read from atx-engine/src/alpha): operator table from registry.cpp
   (name, arity, opcode, dtype, peeled hparams); typing (compare F64 -> Mask, && / || Mask, Select(Mask, F64, F64),
   Group fields by the grp_ prefix, group ops need (F64, Group)); lookback (delay / delta d + child, rolling ts
   (w - 1) + child, cross-section 0); DAG nodes in parse order with structural and literal CSE (peeled hparams are not
   nodes); peak slots with the output slot taken before the inputs retire (bytecode.cpp linearize).
2. Calibration: the mirror must reproduce the 16 recorded (bars, slots, nodes) rows of library-v8-draft section 4,
   task-LIB2 section 1 and the v9 draft section 1, parse all 52 v8.0 roster strings and give qmj_safety its recorded
   8 slots.
3. The X candidates: each frozen string's SHA-256, bars, slots, nodes, bytes and fields, against the house budget
   (bars <= 314, slots <= 7, extra fields <= 5, DSL <= 4,096 B) and the field lists v10 (lib-v80.json) plus the named
   v11 / v12 / draft v13 additions.
4. Semantics (synthetic panels): a small numpy interpreter of the ops the two new strings use evaluates them and
   compares every cell (and the NaN pattern) with a direct implementation of the registered definition:
   inst_persist = rank(decay_linear(-DPV persistence, 21)) and k8_intensity = rank(decay_linear(-(8-K count over 252
   sessions), 21)).
K1 (`--plan-only` through add-alpha) remains the checker of record; this file only states what K1 should print.
"""
from __future__ import annotations

import hashlib
import json
import re
import sys
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[3]
REG_CPP = (REPO / "atx-engine/src/alpha/registry.cpp").read_text(encoding="utf-8")
OPS = {}
for _m in re.finditer(r'\{"(\w+)",\s*(\d+),\s*(\d+),\s*OpCode::(\w+),\s*DType::(\w+),\s*\w+,\s*\{[^}]*\},\s*&(\w+)'
                      r'(?:,\s*(\d+))?', REG_CPP):
    OPS[_m[1]] = dict(lo=int(_m[2]), hi=int(_m[3]), opcode=_m[4], dtype=_m[5], shape=_m[6], nh=int(_m[7] or 0))
SHIFT = {"TsDelay", "TsDelta"}
NO_WINDOW_PANEL = {"TradeWhen", "Hump", "KalmanLevel", "OuFilter"}
GROUP_ARG = {"CsDemeanG", "CsNeutG", "CsRankG", "CsZscoreG", "CsCountG", "CsMeanG", "CsScaleG", "CsResidualize"}
TOKEN = re.compile(r"\s*(?:(\d+\.\d*|\d*\.\d+|\d+)|([A-Za-z_]\w*)|(==|!=|>=|<=|&&|\|\||[()+\-*/<>?:,]))")
HOUSE = {"bars": 314, "slots": 7, "extra_fields": 5, "bytes": 4096}
BASE_FIELDS = {"close", "raw_close", "volume"}

V10 = json.loads((REPO / "scripts/specs/v8/lib-v80.json").read_text(encoding="utf-8"))["fields"]["list"]
V11_ADD = ["k8_item402_63", "gscore7_lowbm", "eps_consist_4y"]       # task-F-3 report
V12_ADD = ["exch_up_365d"]                                             # task-LIB2 report section 5
V13_DRAFT_ADD = ["nt_first_126", "earn_season_rank"]                   # FIELDS-V9 (834d5a05), draft list fields v13
FIELDS = set(V10) | set(V11_ADD) | set(V12_ADD) | set(V13_DRAFT_ADD) | BASE_FIELDS


def tokenize(s):
    out, i = [], 0
    while i < len(s):
        m = TOKEN.match(s, i)
        if not m or m.end() == i:
            if not s[i:].strip():
                break
            raise ValueError(f"bad character at {i}: {s[i:i + 10]!r}")
        out.append(m[1] or m[2] or m[3])
        i = m.end()
    return out


def node(key, kids, dt, lb, fields=None):
    if fields is None:
        fields = set().union(*(k["fields"] for k in kids)) if kids else set()
    return {"key": key, "kids": kids, "dt": dt, "lb": lb, "fields": fields}


def literal(v):
    n = node(("num", v), [], "f64", 0, set())
    n["num"] = v
    return n


class Parser:
    def __init__(self, text):
        self.t, self.i = tokenize(text), 0

    def peek(self):
        return self.t[self.i] if self.i < len(self.t) else None

    def take(self, want=None):
        tok = self.peek()
        if tok is None or (want is not None and tok != want):
            raise ValueError(f"expected {want!r}, got {tok!r} at token {self.i}")
        self.i += 1
        return tok

    def expr(self):
        c = self.chain(("||",), self.conj, "logic", "mask", "mask")
        if self.peek() != "?":
            return c
        self.take("?")
        a = self.expr()
        self.take(":")
        b = self.expr()
        if (c["dt"], a["dt"], b["dt"]) != ("mask", "f64", "f64"):
            raise ValueError("select needs Mask ? F64 : F64")
        return node(("sel",), [c, a, b], "f64", max(c["lb"], a["lb"], b["lb"]))

    def chain(self, ops, sub, head, want, out):
        a = sub()
        while self.peek() in ops:
            op = self.take()
            b = sub()
            if a["dt"] != want or b["dt"] != want:
                raise ValueError(f"{op} operand types ({a['dt']}, {b['dt']})")
            a = node((head, op), [a, b], out, max(a["lb"], b["lb"]))
        return a

    def conj(self):
        return self.chain(("&&",), self.compare, "logic", "mask", "mask")

    def compare(self):
        a = self.chain(("+", "-"), self.term, "bin", "f64", "f64")
        if self.peek() in ("==", "!=", ">", "<", ">=", "<="):
            op = self.take()
            b = self.chain(("+", "-"), self.term, "bin", "f64", "f64")
            if a["dt"] != "f64" or b["dt"] != "f64":
                raise ValueError("compare needs F64 operands")
            a = node(("cmp", op), [a, b], "mask", max(a["lb"], b["lb"]))
        return a

    def term(self):
        return self.chain(("*", "/"), self.unary, "bin", "f64", "f64")

    def unary(self):
        if self.peek() == "-":
            self.take()
            tok = self.take()
            if not re.match(r"[\d.]", tok):
                raise ValueError("unary minus only on a literal")
            return literal(-float(tok))
        return self.primary()

    def primary(self):
        tok = self.take()
        if tok == "(":
            e = self.expr()
            self.take(")")
            return e
        if re.match(r"[\d.]", tok):
            return literal(float(tok))
        if self.peek() != "(":
            if tok not in FIELDS:
                raise ValueError(f"field {tok!r} is in no listed fields build")
            return node(("field", tok), [], "group" if tok.startswith("grp_") else "f64", 0, {tok})
        self.take("(")
        args = [self.expr()]
        while self.peek() == ",":
            self.take(",")
            args.append(self.expr())
        self.take(")")
        if tok not in OPS:
            raise ValueError(f"operator {tok!r} is not in registry.cpp")
        row = OPS[tok]
        if not row["lo"] <= len(args) <= row["hi"]:
            raise ValueError(f"arity {len(args)} for {tok}")
        nh = row["nh"]
        hp, xs = (args[len(args) - nh:], args[:len(args) - nh]) if nh else ([], args)
        if any("num" not in h for h in hp):
            raise ValueError(f"{tok}: a peeled hparam must be a literal")
        op, lb = row["opcode"], max(a["lb"] for a in xs)
        if row["shape"] == "shape_panel" and op not in NO_WINDOW_PANEL:
            w = xs[-1]
            if "num" not in w or w["num"] < 1 or w["num"] != int(w["num"]):
                raise ValueError(f"{tok}: the window must be a positive integer literal")
            if any(a["dt"] != "f64" for a in xs[:-1]):
                raise ValueError(f"{tok}: operand type")
            lb = max(a["lb"] for a in xs[:-1]) + (int(w["num"]) if op in SHIFT else int(w["num"]) - 1)
        elif op in GROUP_ARG:
            if xs[0]["dt"] != "f64" or xs[1]["dt"] != "group":
                raise ValueError(f"{tok} needs (F64, Group)")
        elif op == "GroupCross":
            if any(a["dt"] != "group" for a in xs):
                raise ValueError("group_cross needs two Groups")
        elif any(a["dt"] != "f64" for a in xs):
            raise ValueError(f"{tok}: operand type")
        return node(("call", tok, tuple(h["num"] for h in hp)), xs, "group" if row["dtype"] == "Group" else "f64", lb)


def parse(text):
    p = Parser(text)
    root = p.expr()
    if p.peek() is not None:
        raise ValueError(f"trailing tokens at {p.i}")
    if root["dt"] != "f64":
        raise ValueError("the root is not F64")
    return root


def static(text):
    root = parse(text)
    keys, kids, refs = {}, [], []

    def intern(n):
        ch = tuple(intern(k) for k in n["kids"])
        key = (n["key"], ch)
        if key not in keys:
            keys[key] = len(kids)
            kids.append(ch)
            refs.append(0)
            for c in ch:
                refs[c] += 1
        return keys[key]

    refs[intern(root)] += 1
    live = peak = 0
    for ch in kids:
        live += 1
        peak = max(peak, live)
        for c in ch:
            refs[c] -= 1
            live -= refs[c] == 0
    fields = sorted(root["fields"])
    return {"bars": root["lb"], "slots": peak, "nodes": len(kids), "bytes": len(text.encode()),
            "fields": fields, "extra_fields": [f for f in fields if f not in BASE_FIELDS],
            "sha256": hashlib.sha256(text.encode()).hexdigest()}


# Recorded rows (bars, slots, nodes or None): library-v8-draft section 4, task-LIB2 section 1, v9 draft section 1.
RECORDED = [
    ("rank(ts_mean_mp((((ea_days_since == 2) ? (((close / delay(close, 5)) - 1) - ts_sum(mkt_ret, 5)) : 0)), 252, 126))", 256, 5, 17),
    ("rank(decay_linear((((rank(sue) + rank(((((ni_q / be_lag1q) - (ni_q_lag4 / be_lag1q_lag4)) + (0 * log(be_lag1q))) + (0 * log(be_lag1q_lag4))))) + rank((((txt_q - txt_q_lag4) / at_lag4) + (0 * log(at_lag4))))) / 3), 21))", 20, 6, 33),
    ("group_rank(decay_linear((((oi_ttm + (power(xrd_ttm, (1 - ts_count_nans(xrd_ttm, 1))) - ts_count_nans(xrd_ttm, 1))) / at) + (0 * log(at))), 21), grp_ff12)", 20, 5, 18),
    ("rank(decay_linear((-1 * (si_shares / ts_mean(volume, 126))), 21))", 145, 5, None),
    ("rank(decay_linear(((-1 * ((ni_ttm - cfo_ttm) / abs(ni_ttm))) + (0 * log(abs(ni_ttm)))), 21))", 20, 4, 14),
    ("rank(decay_linear(delay(ts_mean(sign(((close / delay(close, 1)) - 1)), 231), 21), 21))", 272, 4, None),
    ("rank(decay_linear((-1 * ((si_shares / shares_out) / inst_own_share)), 21))", 20, 4, 10),
    ("group_rank(decay_linear((-1 * ceq_iss_5y), 21), grp_ff12)", 20, 3, 7),
    ("rank(decay_linear((-1 * coskew_60m), 21))", 20, 3, 6),
    ("group_rank(decay_linear(gscore7_lowbm, 21), grp_ff12)", 20, 3, 5),
    ("rank((-1 * k8_item402_63))", 0, 3, 4),
    ("rank(decay_linear(eps_consist_4y, 21))", 20, 3, None),
    ("rank(decay_linear(((-1 * (ts_std_mp(iv_atm_21d, 21, 12) / ts_mean_mp(iv_atm_21d, 21, 12))) + (0 * log(ts_std_mp(iv_atm_21d, 21, 12)))), 21))", 40, 5, 13),
    ("rank(decay_linear(ts_mean_mp((((ret_overnight > 0) && (ret_intraday < 0)) ? 1 : 0), 21, 15), 21))", 40, 4, 12),
    ("rank((-1 * exch_up_365d))", 0, 3, 4),
    ("rank(((group_mean((((close / delay(close, 21)) - 1) * (((group_rank(me_company, grp_ff49) > 0.7) ? 1 : 0) + (0 * ((close / delay(close, 21)) - 1)))), grp_ff49) / group_mean((((group_rank(me_company, grp_ff49) > 0.7) ? 1 : 0) + (0 * ((close / delay(close, 21)) - 1))), grp_ff49)) + (0 * log((1 - (((group_rank(me_company, grp_ff49) > 0.7) ? 1 : 0) + (0 * ((close / delay(close, 21)) - 1))))))))", 21, 7, 24),
]

# The X screen set, in rank order (frozen strings; the three carried from the v9 draft are byte-equal to it).
CANDIDATES = {
    "stmom": "rank(decay_linear(((group_rank((ts_sum(volume, 21) / shares_out), bucket(((close / delay(close, 21)) - 1), 10)) > 0.9) ? (rank(((close / delay(close, 21)) - 1)) - 0.5) : 0), 21))",
    "earn_season": "rank((((ea_days_to_expected >= 0) && (ea_days_to_expected <= 21)) ? (earn_season_rank - 10.5) : 0))",
    "k8_intensity": "rank(decay_linear((-1 * (((k8_count_63 + delay(k8_count_63, 63)) + delay(k8_count_63, 126)) + delay(k8_count_63, 189))), 21))",
    "inst_persist": "rank(decay_linear((((((((max((sign((0.5 - rank(inst_own_chg_q))) * delay(sign((0.5 - rank(inst_own_chg_q))), 252)), 0) + 1) * max((sign((0.5 - rank(inst_own_chg_q))) * delay(sign((0.5 - rank(inst_own_chg_q))), 189)), 0)) + 1) * max((sign((0.5 - rank(inst_own_chg_q))) * delay(sign((0.5 - rank(inst_own_chg_q))), 126)), 0)) + 2) * max((sign((0.5 - rank(inst_own_chg_q))) * delay(sign((0.5 - rank(inst_own_chg_q))), 63)), 0)) * sign((0.5 - rank(inst_own_chg_q)))), 21))",
    "nt_late": "rank((-1 * nt_first_126))",
}
V9_DRAFT_SHA16 = {"stmom": "06dc6238d7e94089", "earn_season": "64a0be8f2c71efd1", "nt_late": "bafc4e3a93204e59"}
EXPECTED = {  # (bars, slots, nodes, extra fields): what K1 should print
    "stmom": (41, 5, 21, ["shares_out"]),
    "earn_season": (0, 5, 11, ["ea_days_to_expected", "earn_season_rank"]),
    "k8_intensity": (209, 5, 15, ["k8_count_63"]),
    "inst_persist": (272, 6, 34, ["inst_own_chg_q"]),
    "nt_late": (0, 3, 4, ["nt_first_126"]),
}


# ------------------------------------------------------------------ semantics on synthetic panels (numpy interpreter)
def avg_rank_pct(v):
    order = np.argsort(v, kind="mergesort")
    ranks = np.empty(len(v))
    sv = v[order]
    i = 0
    while i < len(v):
        j = i
        while j + 1 < len(v) and sv[j + 1] == sv[i]:
            j += 1
        ranks[order[i:j + 1]] = (i + j) / 2.0
        i = j + 1
    return ranks / (len(v) - 1) if len(v) > 1 else np.full(len(v), 0.5)


def cs_rank(x):
    out = np.full_like(x, np.nan)
    for t in range(x.shape[0]):
        ok = np.isfinite(x[t])
        if ok.any():
            out[t, ok] = avg_rank_pct(x[t, ok])
    return out


def ts_delay(x, d):
    out = np.full_like(x, np.nan)
    out[d:] = x[:-d]
    return out


def ts_decay_linear(x, w):
    out = np.full_like(x, np.nan)
    wts = np.arange(1, w + 1, dtype=float)
    wts /= wts.sum()
    for t in range(w - 1, x.shape[0]):
        blk = x[t - w + 1:t + 1]
        ok = np.isfinite(blk).all(axis=0)
        out[t, ok] = (wts[:, None] * blk[:, ok]).sum(axis=0)
    return out


def evaluate(n, env):
    k = n["key"]
    if k[0] == "num":
        return k[1]
    if k[0] == "field":
        return env[k[1]]
    a = [evaluate(c, env) for c in n["kids"]]
    if k[0] == "bin":
        return {"+": np.add, "-": np.subtract, "*": np.multiply, "/": np.divide}[k[1]](a[0], a[1])
    if k == ("call", "rank", ()):
        return cs_rank(a[0])
    if k == ("call", "sign", ()):
        return np.sign(a[0])
    if k == ("call", "max", ()):
        return np.where(np.isnan(a[0]) | np.isnan(a[1]), np.nan, np.maximum(a[0], a[1]))
    if k == ("call", "delay", ()):
        return ts_delay(a[0], int(a[1]))
    if k == ("call", "decay_linear", ()):
        return ts_decay_linear(a[0], int(a[1]))
    raise NotImplementedError(k)


def check_inst_persist(rng):
    """DPV (2011): net buy / sell = above / below the cross-sectional median of the quarter's IO change; persistence =
    consecutive same-side quarters including the current one, capped at 5, +-1 consolidated to 0; signal -persistence."""
    quarter, n_names = 63, 41
    q = rng.normal(size=(11, n_names))
    for i in range(12):                       # planted runs on both sides
        q[:, i] = (1.0 if i % 2 == 0 else -1.0) * (1.0 + rng.random(q.shape[0]))
    q[rng.random(q.shape) < 0.03] = np.nan     # missing quarters
    x = np.repeat(q, quarter, axis=0)[:quarter * 10 + 30]
    got = evaluate(parse(CANDIDATES["inst_persist"]), {"inst_own_chg_q": x})
    side = np.full_like(x, np.nan)
    for t in range(x.shape[0]):
        ok = np.isfinite(x[t])
        side[t, ok] = np.sign(x[t, ok] - np.median(x[t, ok]))
    pers = np.full_like(x, np.nan)
    for t in range(4 * quarter, x.shape[0]):
        for i in range(n_names):
            s = [side[t - quarter * k, i] for k in range(5)]
            if np.isnan(s).any():
                continue
            run = 1
            while run < 5 and s[0] != 0 and s[run] == s[0]:
                run += 1
            pers[t, i] = 0.0 if s[0] == 0 or run < 2 else s[0] * run
    ref = cs_rank(ts_decay_linear(-pers, 21))
    assert np.array_equal(np.isfinite(got), np.isfinite(ref)), "inst_persist: NaN pattern differs"
    ok = np.isfinite(ref)
    assert np.allclose(got[ok], ref[ok], rtol=0, atol=1e-12), "inst_persist: values differ"
    seen = sorted(set(np.unique(pers[np.isfinite(pers)]).tolist()))
    assert seen == [-5.0, -4.0, -3.0, -2.0, 0.0, 2.0, 3.0, 4.0, 5.0], seen
    return f"inst_persist == rank(decay_linear(-DPV persistence, 21)) on {int(ok.sum())} cells; persistence {seen}"


def check_k8(rng):
    """k8_count_63 at t counts filings usable at e with e <= t < e + 63; four disjoint 63-session blocks = 252."""
    n_t, n_names = 420, 25
    usable = (rng.random((n_t, n_names)) < 0.04).astype(float)
    c63 = np.full((n_t, n_names), np.nan)
    for t in range(62, n_t):
        c63[t] = usable[t - 62:t + 1].sum(axis=0)
    got = evaluate(parse(CANDIDATES["k8_intensity"]), {"k8_count_63": c63})
    c252 = np.full((n_t, n_names), np.nan)
    for t in range(62 + 189, n_t):
        c252[t] = usable[t - 251:t + 1].sum(axis=0)
    ref = cs_rank(ts_decay_linear(-c252, 21))
    assert np.array_equal(np.isfinite(got), np.isfinite(ref)), "k8_intensity: NaN pattern differs"
    ok = np.isfinite(ref)
    assert np.allclose(got[ok], ref[ok], rtol=0, atol=1e-12), "k8_intensity: values differ"
    return f"k8_intensity == rank(decay_linear(-(8-K count over 252 sessions), 21)) on {int(ok.sum())} cells"


def main():
    bad = [r for r in RECORDED if (lambda s: (s["bars"], s["slots"]) != (r[1], r[2])
                                   or (r[3] is not None and s["nodes"] != r[3]))(static(r[0]))]
    reg = json.loads((REPO / "atx-impl/strategies/alphas/registry.json").read_text(encoding="utf-8"))
    lib = json.loads((REPO / "atx-impl/strategies/libraries/v80.json").read_text(encoding="utf-8"))
    by_id = {a["id"]: a for a in reg["alphas"]}
    roster = {m: static(by_id[m]["dsl"]) for m in lib["members"]}
    assert not bad, f"mirror calibration failed on {len(bad)} recorded rows"
    assert len(roster) == 52 and roster["qmj_safety"]["slots"] == 8, "roster calibration"
    print(f"calibration: {len(RECORDED)} recorded rows reproduced; 52 v8.0 strings parsed; qmj_safety 8 slots")
    for cid, text in CANDIDATES.items():
        s = static(text)
        exp = EXPECTED[cid]
        assert (s["bars"], s["slots"], s["nodes"], s["extra_fields"]) == exp, (cid, s)
        assert s["bars"] <= HOUSE["bars"] and s["slots"] <= HOUSE["slots"], cid
        assert len(s["extra_fields"]) <= HOUSE["extra_fields"] and s["bytes"] <= HOUSE["bytes"], cid
        if cid in V9_DRAFT_SHA16:
            assert s["sha256"][:16] == V9_DRAFT_SHA16[cid], f"{cid}: not byte-equal to the v9 draft"
        rows = [f for f in s["extra_fields"] if f not in reg["fields"]]
        print(f"{cid:13s} bars {s['bars']:3d} slots {s['slots']} nodes {s['nodes']:2d} bytes {s['bytes']:3d} "
              f"extra {s['extra_fields']} registry field rows needed {rows} sha256 {s['sha256']}")
    rng = np.random.default_rng(20261002)
    print(check_inst_persist(rng))
    print(check_k8(rng))
    print("xsig_check: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
