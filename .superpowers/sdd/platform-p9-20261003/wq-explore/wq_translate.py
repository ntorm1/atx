"""WQ-explore (exploratory, off-ledger): the 101 formulaic alphas of Kakushadze (2016) as IC-runner DSL strings.

46 formulas: the house transcription of lane XWQ (platform-v8 xwq_check.py dsl(n); classes S / L, exact).
55 formulas (43 need vwap, 12 nest a price level in a time-series op): NOT exact. They are translated here from the
engine fixture atx-impl/tests/fixtures/alpha101.txt with
  vwap -> ((high_adj + low_adj + close) / 3)   (a PROXY, the typical price; the house has no vwap)
  a price-degree analysis: wherever a quantity of price degree m != 0 enters a cross-sectional op, a comparison or a
  sum with a quantity of another degree, it is multiplied by (raw_close / close)^m, i.e. rebased to the prices of the
  row being evaluated (XWQ reading R1, class L). For class N this evaluates each past cross-section on that past
  day's own prices (point in time, but not the paper's as-of-evaluation-day adjustment).
Other readings as XWQ R2-R8: volume = raw share volume; returns = close / delay(close, 1) - 1; adv{d} =
ts_mean(raw_close * volume, d); cap = me_company; sector / industry / subindustry = grp_ff12 / grp_ff49 / grp_sic2.
"""
from __future__ import annotations
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / ".superpowers/sdd/platform-v8-20260929"))
import xwq_check as xwq  # noqa: E402

K = "(raw_close / close)"
LEAF = {"open": ("open_adj", 1), "high": ("high_adj", 1), "low": ("low_adj", 1), "close": ("close", 1),
        "vwap": ("(((high_adj + low_adj) + close) / 3)", 1), "volume": ("volume", 0),
        "returns": ("((close / delay(close, 1)) - 1)", 0), "cap": ("me_company", 0),
        "IndClass.sector": ("grp_ff12", 0), "IndClass.industry": ("grp_ff49", 0),
        "IndClass.subindustry": ("grp_sic2", 0)}
TOK = re.compile(r"\s*(?:(\d+\.\d*|\.\d+|\d+)|([A-Za-z_][A-Za-z_0-9]*(?:\.[A-Za-z_]+)?)|(\|\||&&|<=|>=|==|!=|[-+*/()<>?:,!]))")
PRESERVE = {"delay", "delta", "ts_sum", "ts_min", "ts_max", "decay_linear", "ts_mean", "stddev", "abs"}
ZERO = {"correlation", "ts_rank", "ts_argmax", "ts_argmin", "sign"}
CS = {"rank", "scale", "indneutralize", "zscore"}
WINDOWED = {"delay", "delta", "ts_sum", "ts_min", "ts_max", "decay_linear", "ts_mean", "stddev", "correlation",
            "covariance", "ts_rank", "ts_argmax", "ts_argmin", "product"}


def tokenize(s):
    out, i = [], 0
    while i < len(s):
        m = TOK.match(s, i)
        if not m:
            if s[i:].strip() == "":
                break
            raise ValueError(f"token at {s[i:i+20]!r}")
        out.append(("num", m.group(1)) if m.group(1) else ("id", m.group(2)) if m.group(2) else ("op", m.group(3)))
        i = m.end()
    return out


class P:
    """Recursive-descent parser; every node is (text, price degree, is numeric literal)."""

    def __init__(self, text):
        self.t, self.i, self.windows = tokenize(text), 0, []

    def peek(self):
        return self.t[self.i] if self.i < len(self.t) else (None, None)

    def eat(self, v=None):
        k = self.t[self.i]
        self.i += 1
        if v is not None and k[1] != v:
            raise ValueError(f"expected {v} got {k}")
        return k

    def fix(self, n):
        txt, d, _ = n
        if d == 0:
            return n
        if d == 1:
            return (f"({txt} * {K})", 0, False)
        if d == -1:
            return (f"({txt} / {K})", 0, False)
        return (f"({txt} * power({K}, {d}))", 0, False)

    def unify(self, a, b):
        if a[1] == b[1]:
            return a, b
        return self.fix(a), self.fix(b)

    def expr(self):
        c = self.orx()
        if self.peek()[1] == "?":
            self.eat()
            a = self.expr()
            self.eat(":")
            b = self.expr()
            a, b = self.unify(a, b)
            return (f"({c[0]} ? {a[0]} : {b[0]})", a[1], False)
        return c

    def orx(self):
        a = self.andx()
        while self.peek()[1] == "||":
            self.eat()
            b = self.andx()
            a = (f"({a[0]} || {b[0]})", 0, False)
        return a

    def andx(self):
        a = self.cmp()
        while self.peek()[1] == "&&":
            self.eat()
            b = self.cmp()
            a = (f"({a[0]} && {b[0]})", 0, False)
        return a

    def cmp(self):
        a = self.add()
        if self.peek()[1] in ("<", ">", "<=", ">=", "==", "!="):
            op = self.eat()[1]
            b = self.add()
            a, b = self.unify(a, b)
            return (f"({a[0]} {op} {b[0]})", 0, False)
        return a

    def add(self):
        a = self.mul()
        while self.peek()[1] in ("+", "-"):
            op = self.eat()[1]
            b = self.mul()
            a, b = self.unify(a, b)
            a = (f"({a[0]} {op} {b[0]})", a[1], False)
        return a

    def mul(self):
        a = self.unary()
        while self.peek()[1] in ("*", "/"):
            op = self.eat()[1]
            b = self.unary()
            a = (f"({a[0]} {op} {b[0]})", a[1] + (b[1] if op == "*" else -b[1]), False)
        return a

    def unary(self):
        if self.peek()[1] == "-":
            self.eat()
            a = self.unary()
            if a[2]:
                return (f"(0 - {a[0]})", 0, False)
            return (f"(-1 * {a[0]})", a[1], False)
        return self.primary()

    def primary(self):
        kind, v = self.eat()
        if kind == "num":
            return (v, 0, True)
        if kind == "op" and v == "(":
            a = self.expr()
            self.eat(")")
            return a
        if kind != "id":
            raise ValueError(f"unexpected {v}")
        if self.peek()[1] != "(":
            if v in LEAF:
                return (LEAF[v][0], LEAF[v][1], False)
            m = re.fullmatch(r"adv(\d+)", v)
            if m:
                self.windows.append(int(m.group(1)))
                return (f"ts_mean((raw_close * volume), {m.group(1)})", 0, False)
            raise ValueError(f"unknown field {v}")
        self.eat("(")
        args = [self.expr()]
        while self.peek()[1] == ",":
            self.eat()
            args.append(self.expr())
        self.eat(")")
        if v in WINDOWED and args[-1][2]:
            self.windows.append(int(float(args[-1][0])))
        j = ", ".join(a[0] for a in args)
        if v in CS:
            x = self.fix(args[0])
            if v == "scale":
                return (f"scale({x[0]})", 0, False)
            if v == "indneutralize":
                return (f"indneutralize({x[0]}, {args[1][0]})", 0, False)
            return (f"{v}({x[0]})", 0, False)
        if v in PRESERVE:
            return (f"{v}({j})", args[0][1], False)
        if v in ZERO:
            return (f"{v}({j})", 0, False)
        if v == "covariance":
            return (f"covariance({j})", args[0][1] + args[1][1], False)
        if v == "product":
            x = self.fix(args[0])
            return (f"product({x[0]}, {args[1][0]})", 0, False)
        if v == "log":
            x = self.fix(args[0])
            return (f"log({x[0]})", 0, False)
        if v in ("power", "signedpower"):
            if args[1][2]:
                return (f"{v}({j})", args[0][1] * float(args[1][0]), False)
            x, y = self.fix(args[0]), self.fix(args[1])
            return (f"{v}({x[0]}, {y[0]})", 0, False)
        if v in ("min", "max"):
            a, b = self.unify(args[0], args[1])
            return (f"{v}({a[0]}, {b[0]})", a[1], False)
        raise ValueError(f"unknown function {v}")


def translate(text):
    p = P(text)
    n = p.expr()
    if p.i != len(p.t):
        raise ValueError("trailing tokens")
    n = p.fix(n)
    return n[0], (max(p.windows) if p.windows else 0)


def fixture():
    out = {}
    for line in (ROOT / "atx-impl/tests/fixtures/alpha101.txt").read_text().splitlines():
        m = re.match(r"(\d+):\s*(.*)$", line)
        if m:
            out[int(m.group(1))] = m.group(2)
    assert set(out) == set(range(1, 102))
    return out


# Fixture strings re-read where the literal text does not fit the degree rule or the paper:
#   #29 the paper's delta((close - 1), 5) = delta(close, 5); its min(x, 5) is ts_min(x, 5); product(x, 1) = x.
OVERRIDE = {
    29: "(ts_min(rank(rank(scale(log(ts_sum(ts_min(rank(rank((-1 * rank(delta(close, 5))))), 2), 1)), 1))), 5)"
        " + ts_rank(delay(-1 * returns, 6), 5))",
}


def all_alphas():
    """n -> dict(dsl, window, cls, exact, flags, cluster, note)."""
    fx, out = fixture(), {}
    for n in range(1, 102):
        cls, cluster, _tpl, flags, note = xwq.TABLE[n]
        if cls in ("S", "L"):
            text = xwq.dsl(n)
            out[n] = dict(dsl=text, window=xwq.longest_window(text), cls=cls, exact=True, flags=sorted(flags),
                          cluster=cluster, note=note)
        else:
            text, win = translate(OVERRIDE.get(n, fx[n]))
            out[n] = dict(dsl=text, window=win, cls=cls, exact=False, flags=sorted(flags), cluster=None,
                          note=("vwap replaced by the proxy (high + low + close) / 3" if cls == "V" else
                                "price level nested in a time-series op: each past cross-section on that day's own "
                                "prices, not re-adjusted as of the evaluation day"))
    return out


def house_form(text, window):
    d = 5 if window < 10 else 21
    return f"rank(decay_linear({text}, {d}))", d


if __name__ == "__main__":
    a = all_alphas()
    for n in (1, 5, 29, 41, 42, 57, 62, 83, 84, 100):
        print(n, a[n]["cls"], a[n]["window"], a[n]["dsl"])
    fx = fixture()
    for n in (101, 8, 28, 18):  # eyeball: my translator against the house strings on exact rows
        print("mine ", n, translate(fx[n])[0])
        print("house", n, xwq.dsl(n))
