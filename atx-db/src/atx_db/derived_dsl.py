"""A restricted arithmetic DSL for derived-metric definitions.

The DSL is parsed by a hand-written tokenizer and recursive-descent parser into
a frozen AST and lowered to DuckDB SQL. There is no ``eval``, no ``exec`` and no
``DataFrame.eval``: the only characters the tokenizer accepts are digits, the
decimal point, ASCII letters, the underscore, the five operators ``+ - * /``,
parentheses and the comma. Anything else is a ``DslError`` naming its position.

Every lowering produces three things: the SQL value expression, a parallel SQL
availability expression built from the same tree, and the deepest row offset the
expression reaches back on its grid. The availability expression is what makes
"a derived value's available_at is the max over its inputs" true by
construction rather than by convention.
"""

from __future__ import annotations

from dataclasses import dataclass

__all__ = [
    "DAILY_FUNCTIONS",
    "FUNCTION_ARITY",
    "QUARTER_FUNCTIONS",
    "SCALAR_FUNCTIONS",
    "BinOp",
    "Call",
    "DslError",
    "LowerContext",
    "Lowered",
    "Neg",
    "Node",
    "Number",
    "Ref",
    "Token",
    "compile_expression",
    "expression_names",
    "lower",
    "parse_expression",
    "tokenize",
]

_NEG_INFINITY = "TIMESTAMP '-infinity'"


class DslError(ValueError):
    """A malformed or unsupported derived-metric expression."""


@dataclass(frozen=True)
class Token:
    kind: str
    text: str
    position: int


@dataclass(frozen=True)
class Number:
    value: float


@dataclass(frozen=True)
class Ref:
    name: str


@dataclass(frozen=True)
class Neg:
    operand: Node


@dataclass(frozen=True)
class BinOp:
    op: str
    left: Node
    right: Node


@dataclass(frozen=True)
class Call:
    name: str
    args: tuple[Node, ...]


Node = Number | Ref | Neg | BinOp | Call

SCALAR_FUNCTIONS = frozenset(
    {"safe_div", "coalesce", "abs", "min", "max", "ln", "indicator_gt", "indicator_lt"}
)
QUARTER_FUNCTIONS = frozenset({"ttm", "avg2", "lag", "yoy", "qoq", "cagr", "stdev_q"})
DAILY_FUNCTIONS = frozenset({"lag_d", "avg_d", "tret", "rvol"})
_WINDOW_FUNCTIONS = QUARTER_FUNCTIONS | DAILY_FUNCTIONS

FUNCTION_ARITY: dict[str, tuple[int, int]] = {
    "safe_div": (2, 2),
    "coalesce": (1, 8),
    "abs": (1, 1),
    "min": (2, 2),
    "max": (2, 2),
    "ln": (1, 1),
    "indicator_gt": (2, 2),
    "indicator_lt": (2, 2),
    "ttm": (1, 1),
    "avg2": (1, 1),
    "lag": (2, 2),
    "yoy": (1, 1),
    "qoq": (1, 1),
    "cagr": (2, 2),
    "stdev_q": (2, 2),
    "lag_d": (2, 2),
    "avg_d": (2, 2),
    "tret": (2, 2),
    "rvol": (2, 2),
}

_OPERATORS = frozenset({"+", "-", "*", "/", "(", ")", ","})
_NO_DOUBLE = frozenset({"*", "/"})
_NAME_START = frozenset("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_")
_NAME_BODY = _NAME_START | frozenset("0123456789")
_DIGITS = frozenset("0123456789")


def tokenize(expression: str) -> tuple[Token, ...]:
    tokens: list[Token] = []
    index = 0
    length = len(expression)
    while index < length:
        char = expression[index]
        if char in " \t":
            index += 1
            continue
        if char in _OPERATORS:
            if (
                char in _NO_DOUBLE
                and index + 1 < length
                and expression[index + 1] == char
            ):
                raise DslError(
                    f"unsupported operator {expression[index:index + 2]!r} "
                    f"at position {index} in {expression!r}"
                )
            tokens.append(Token("op", char, index))
            index += 1
            continue
        if char in _DIGITS or (char == "." and index + 1 < length and expression[index + 1] in _DIGITS):
            start = index
            seen_dot = False
            while index < length and (expression[index] in _DIGITS or (expression[index] == "." and not seen_dot)):
                seen_dot = seen_dot or expression[index] == "."
                index += 1
            tokens.append(Token("number", expression[start:index], start))
            continue
        if char in _NAME_START:
            start = index
            while index < length and expression[index] in _NAME_BODY:
                index += 1
            tokens.append(Token("name", expression[start:index], start))
            continue
        raise DslError(f"unsupported character {char!r} at position {index} in {expression!r}")
    tokens.append(Token("end", "", length))
    return tuple(tokens)


def _is_reserved_identifier(name: str) -> bool:
    return name.startswith("__") and name.endswith("__") and len(name) > 4


class _Parser:
    def __init__(self, tokens: tuple[Token, ...], expression: str) -> None:
        self._tokens = tokens
        self._expression = expression
        self._index = 0

    @property
    def _current(self) -> Token:
        return self._tokens[self._index]

    def _advance(self) -> Token:
        token = self._tokens[self._index]
        self._index += 1
        return token

    def _expect_op(self, text: str) -> None:
        token = self._current
        if token.kind != "op" or token.text != text:
            raise DslError(
                f"expected {text!r} at position {token.position} in {self._expression!r}, got {token.text!r}"
            )
        self._advance()

    def parse(self) -> Node:
        node = self._expr()
        if self._current.kind != "end":
            raise DslError(
                f"unexpected trailing input at position {self._current.position} in {self._expression!r}"
            )
        return node

    def _expr(self) -> Node:
        node = self._term()
        while self._current.kind == "op" and self._current.text in ("+", "-"):
            op = self._advance().text
            node = BinOp(op, node, self._term())
        return node

    def _term(self) -> Node:
        node = self._factor()
        while self._current.kind == "op" and self._current.text in ("*", "/"):
            op = self._advance().text
            node = BinOp(op, node, self._factor())
        return node

    def _factor(self) -> Node:
        if self._current.kind == "op" and self._current.text == "-":
            self._advance()
            return Neg(self._factor())
        return self._primary()

    def _primary(self) -> Node:
        token = self._current
        if token.kind == "number":
            self._advance()
            return Number(float(token.text))
        if token.kind == "op" and token.text == "(":
            self._advance()
            node = self._expr()
            self._expect_op(")")
            return node
        if token.kind == "name":
            self._advance()
            if self._current.kind == "op" and self._current.text == "(":
                return self._call(token)
            if _is_reserved_identifier(token.text):
                raise DslError(
                    f"reserved identifier {token.text!r} at position {token.position} in {self._expression!r}"
                )
            return Ref(token.text)
        raise DslError(f"unexpected token {token.text!r} at position {token.position} in {self._expression!r}")

    def _call(self, name_token: Token) -> Node:
        name = name_token.text
        if name not in FUNCTION_ARITY:
            raise DslError(
                f"unknown function {name!r} at position {name_token.position} in {self._expression!r}"
            )
        self._expect_op("(")
        args: list[Node] = []
        if not (self._current.kind == "op" and self._current.text == ")"):
            args.append(self._expr())
            while self._current.kind == "op" and self._current.text == ",":
                self._advance()
                args.append(self._expr())
        self._expect_op(")")
        minimum, maximum = FUNCTION_ARITY[name]
        if not minimum <= len(args) <= maximum:
            raise DslError(
                f"{name!r} takes {minimum}..{maximum} arguments, got {len(args)} in {self._expression!r}"
            )
        return Call(name, tuple(args))


def parse_expression(expression: str) -> Node:
    text = expression.strip()
    if not text:
        raise DslError("empty expression")
    return _Parser(tokenize(text), text).parse()


def expression_names(node: Node) -> tuple[str, ...]:
    found: set[str] = set()

    def walk(current: Node) -> None:
        if isinstance(current, Ref):
            found.add(current.name)
        elif isinstance(current, Neg):
            walk(current.operand)
        elif isinstance(current, BinOp):
            walk(current.left)
            walk(current.right)
        elif isinstance(current, Call):
            for argument in current.args:
                walk(argument)

    walk(node)
    return tuple(sorted(found))


@dataclass(frozen=True)
class LowerContext:
    grid: str
    columns: dict[str, str]
    availability: dict[str, str]
    partition_sql: str
    order_sql: str


@dataclass(frozen=True)
class Lowered:
    value_sql: str
    availability_sql: str
    max_lag: int


def _frame(context: LowerContext, preceding: int) -> str:
    return (
        f"PARTITION BY {context.partition_sql} ORDER BY {context.order_sql} "
        f"ROWS BETWEEN {preceding} PRECEDING AND CURRENT ROW"
    )


def _unbounded(context: LowerContext) -> str:
    return f"PARTITION BY {context.partition_sql} ORDER BY {context.order_sql}"


def _greatest(parts: list[str]) -> str:
    live = [part for part in parts if part != "NULL"]
    if not live:
        return "NULL"
    if len(live) == 1:
        return live[0]
    wrapped = ", ".join(f"coalesce({part}, {_NEG_INFINITY})" for part in live)
    return f"nullif(greatest({wrapped}), {_NEG_INFINITY})"


def _find_nested_window_call(node: Node) -> Call | None:
    """Return the first ``Call`` to a window function reachable from ``node``.

    DuckDB rejects a window function used as the argument of another window
    function within one SQL expression (no implicit subquery). The search
    descends through scalar ``Call`` wrappers, ``Neg`` and ``BinOp`` so that a
    window function buried under scalar functions or arithmetic is still
    found, while independent window calls combined at the top level (e.g. two
    ``ttm`` calls passed to ``safe_div``) are left alone.
    """
    if isinstance(node, Call):
        if node.name in _WINDOW_FUNCTIONS:
            return node
        for argument in node.args:
            found = _find_nested_window_call(argument)
            if found is not None:
                return found
        return None
    if isinstance(node, Neg):
        return _find_nested_window_call(node.operand)
    if isinstance(node, BinOp):
        return _find_nested_window_call(node.left) or _find_nested_window_call(node.right)
    return None


def _integer_literal(node: Node, *, function: str, expression_hint: str) -> int:
    if not isinstance(node, Number) or node.value != int(node.value) or node.value < 0:
        raise DslError(
            f"{function!r} requires a non-negative integer literal argument in {expression_hint}"
        )
    return int(node.value)


def lower(node: Node, context: LowerContext) -> Lowered:
    if context.grid not in ("quarter", "day"):
        raise DslError(f"unknown grid {context.grid!r}")
    if isinstance(node, Number):
        return Lowered(repr(float(node.value)), "NULL", 0)
    if isinstance(node, Ref):
        if node.name not in context.columns:
            raise DslError(f"unresolved reference {node.name!r} on the {context.grid} grid")
        return Lowered(context.columns[node.name], context.availability[node.name], 0)
    if isinstance(node, Neg):
        inner = lower(node.operand, context)
        return Lowered(f"(-({inner.value_sql}))", inner.availability_sql, inner.max_lag)
    if isinstance(node, BinOp):
        left = lower(node.left, context)
        right = lower(node.right, context)
        if node.op == "/":
            value = (
                f"(CASE WHEN ({right.value_sql}) IS NULL OR ({right.value_sql}) = 0 "
                f"THEN NULL ELSE ({left.value_sql}) / ({right.value_sql}) END)"
            )
        else:
            value = f"(({left.value_sql}) {node.op} ({right.value_sql}))"
        return Lowered(
            value,
            _greatest([left.availability_sql, right.availability_sql]),
            max(left.max_lag, right.max_lag),
        )
    if isinstance(node, Call):
        return _lower_call(node, context)
    raise DslError(f"unsupported node {node!r}")


def _lower_call(node: Call, context: LowerContext) -> Lowered:
    name = node.name
    if name in QUARTER_FUNCTIONS and context.grid != "quarter":
        raise DslError(f"{name!r} is only valid on the quarter grid, not {context.grid!r}")
    if name in DAILY_FUNCTIONS and context.grid != "day":
        raise DslError(f"{name!r} is only valid on the day grid, not {context.grid!r}")

    if name in _WINDOW_FUNCTIONS:
        for argument in node.args:
            nested = _find_nested_window_call(argument)
            if nested is not None:
                raise DslError(
                    f"{name!r} cannot be composed with {nested.name!r}: DuckDB does not support "
                    f"nesting one window function inside another within a single SQL expression. "
                    f"Define {nested.name!r}(...) as its own derived metric and reference it by "
                    f"name inside {name!r} instead; the registry supports metric-to-metric "
                    f"dependencies with topological ordering."
                )

    inner = lower(node.args[0], context)

    if name in ("tret", "rvol"):
        periods = _integer_literal(node.args[1], function=name, expression_hint="a daily window length")
        if name == "tret":
            previous = f"lag({inner.value_sql}, {periods}) OVER ({_unbounded(context)})"
            value = (
                f"(CASE WHEN {previous} IS NULL OR {previous} = 0 THEN NULL "
                f"ELSE ({inner.value_sql}) / {previous} - 1.0 END)"
            )
            availability = _greatest(
                [
                    inner.availability_sql,
                    f"lag({inner.availability_sql}, {periods}) OVER ({_unbounded(context)})",
                ]
            )
            return Lowered(value, availability, inner.max_lag + periods)
        if periods < 2:
            raise DslError("'rvol' requires a non-negative integer literal window of at least 2")
        frame = _frame(context, periods - 1)
        value = (
            f"(CASE WHEN count({inner.value_sql}) OVER ({frame}) = {periods} "
            f"THEN sqrt(252.0) * stddev_samp({inner.value_sql}) OVER ({frame}) END)"
        )
        availability = f"max({inner.availability_sql}) OVER ({frame})"
        return Lowered(value, availability, inner.max_lag + periods - 1)

    if name == "safe_div":
        right = lower(node.args[1], context)
        value = (
            f"(CASE WHEN ({right.value_sql}) IS NULL OR ({right.value_sql}) = 0 THEN NULL "
            f"ELSE ({inner.value_sql}) / ({right.value_sql}) END)"
        )
        return Lowered(
            value,
            _greatest([inner.availability_sql, right.availability_sql]),
            max(inner.max_lag, right.max_lag),
        )
    if name == "coalesce":
        branches = [inner] + [lower(argument, context) for argument in node.args[1:]]
        value = "coalesce(" + ", ".join(f"({branch.value_sql})" for branch in branches) + ")"
        availability = "NULL"
        for branch in reversed(branches):
            availability = (
                f"(CASE WHEN ({branch.value_sql}) IS NOT NULL THEN {branch.availability_sql} "
                f"ELSE {availability} END)"
            )
        return Lowered(value, availability, max(branch.max_lag for branch in branches))
    if name == "abs":
        return Lowered(f"abs({inner.value_sql})", inner.availability_sql, inner.max_lag)
    if name == "ln":
        value = (
            f"(CASE WHEN ({inner.value_sql}) IS NULL OR ({inner.value_sql}) <= 0 "
            f"THEN NULL ELSE ln({inner.value_sql}) END)"
        )
        return Lowered(value, inner.availability_sql, inner.max_lag)
    if name in ("min", "max"):
        right = lower(node.args[1], context)
        sql_function = "least" if name == "min" else "greatest"
        return Lowered(
            f"{sql_function}(({inner.value_sql}), ({right.value_sql}))",
            _greatest([inner.availability_sql, right.availability_sql]),
            max(inner.max_lag, right.max_lag),
        )
    if name in ("indicator_gt", "indicator_lt"):
        right = lower(node.args[1], context)
        comparison = ">" if name == "indicator_gt" else "<"
        value = (
            f"(CASE WHEN ({inner.value_sql}) IS NULL OR ({right.value_sql}) IS NULL THEN NULL "
            f"WHEN ({inner.value_sql}) {comparison} ({right.value_sql}) THEN 1.0 ELSE 0.0 END)"
        )
        return Lowered(
            value,
            _greatest([inner.availability_sql, right.availability_sql]),
            max(inner.max_lag, right.max_lag),
        )

    if name == "ttm":
        frame = _frame(context, 3)
        value = (
            f"(CASE WHEN count({inner.value_sql}) OVER ({frame}) = 4 "
            f"THEN sum({inner.value_sql}) OVER ({frame}) END)"
        )
        availability = f"max({inner.availability_sql}) OVER ({frame})"
        return Lowered(value, availability, inner.max_lag + 3)
    if name == "avg2":
        previous = f"lag({inner.value_sql}, 4) OVER ({_unbounded(context)})"
        value = f"(CASE WHEN {previous} IS NULL THEN NULL ELSE (({inner.value_sql}) + {previous}) / 2.0 END)"
        availability = _greatest(
            [inner.availability_sql, f"lag({inner.availability_sql}, 4) OVER ({_unbounded(context)})"]
        )
        return Lowered(value, availability, inner.max_lag + 4)
    if name in ("lag", "lag_d"):
        periods = _integer_literal(node.args[1], function=name, expression_hint="a period count")
        value = f"lag({inner.value_sql}, {periods}) OVER ({_unbounded(context)})"
        availability = f"lag({inner.availability_sql}, {periods}) OVER ({_unbounded(context)})"
        return Lowered(value, availability, inner.max_lag + periods)
    if name in ("yoy", "qoq"):
        periods = 4 if name == "yoy" else 1
        previous = f"lag({inner.value_sql}, {periods}) OVER ({_unbounded(context)})"
        value = (
            f"(CASE WHEN {previous} IS NULL OR {previous} = 0 THEN NULL "
            f"ELSE (({inner.value_sql}) - {previous}) / abs({previous}) END)"
        )
        availability = _greatest(
            [
                inner.availability_sql,
                f"lag({inner.availability_sql}, {periods}) OVER ({_unbounded(context)})",
            ]
        )
        return Lowered(value, availability, inner.max_lag + periods)
    if name == "cagr":
        years = _integer_literal(node.args[1], function="cagr", expression_hint="a year count")
        if years < 1:
            raise DslError("'cagr' requires a non-negative integer literal year count of at least 1")
        periods = 4 * years
        previous = f"lag({inner.value_sql}, {periods}) OVER ({_unbounded(context)})"
        value = (
            f"(CASE WHEN ({inner.value_sql}) > 0 AND {previous} > 0 "
            f"THEN power(({inner.value_sql}) / {previous}, 1.0 / {years}.0) - 1.0 END)"
        )
        availability = _greatest(
            [
                inner.availability_sql,
                f"lag({inner.availability_sql}, {periods}) OVER ({_unbounded(context)})",
            ]
        )
        return Lowered(value, availability, inner.max_lag + periods)
    if name in ("stdev_q", "avg_d"):
        count = _integer_literal(node.args[1], function=name, expression_hint="a window length")
        if count < 2:
            raise DslError(f"{name!r} requires a non-negative integer literal window of at least 2")
        frame = _frame(context, count - 1)
        aggregate = "stddev_samp" if name == "stdev_q" else "avg"
        value = (
            f"(CASE WHEN count({inner.value_sql}) OVER ({frame}) = {count} "
            f"THEN {aggregate}({inner.value_sql}) OVER ({frame}) END)"
        )
        availability = f"max({inner.availability_sql}) OVER ({frame})"
        return Lowered(value, availability, inner.max_lag + count - 1)
    raise DslError(f"unhandled function {name!r}")


def compile_expression(expression: str, context: LowerContext) -> Lowered:
    return lower(parse_expression(expression), context)
