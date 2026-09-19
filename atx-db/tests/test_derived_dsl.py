"""Tier1-S3 T2: the derived-metric expression DSL parses and lowers to DuckDB SQL."""

from __future__ import annotations

import pytest

from atx_db.derived_dsl import (
    BinOp,
    DslError,
    LowerContext,
    Neg,
    Number,
    Ref,
    compile_expression,
    expression_names,
    parse_expression,
    tokenize,
)

_QUARTER = LowerContext(
    grid="quarter",
    columns={"revenue": "b.revenue", "total_assets": "b.total_assets"},
    availability={"revenue": "b.revenue_at", "total_assets": "b.total_assets_at"},
    partition_sql="b.security_id",
    order_sql="b.period_end",
)

_DAY = LowerContext(
    grid="day",
    columns={"adj_close": "d.adj_close", "log_return": "d.log_return", "volume": "d.volume"},
    availability={"adj_close": "d.bar_at", "log_return": "d.bar_at", "volume": "d.bar_at"},
    partition_sql="d.security_id",
    order_sql="d.trade_date",
)


def test_tokenize_splits_names_numbers_and_operators():
    kinds = [token.kind for token in tokenize("safe_div(a, 2.5) - -b")]
    assert kinds == [
        "name", "op", "name", "op", "number", "op", "op", "op", "name", "end",
    ]


def test_tokenize_rejects_a_character_outside_the_grammar():
    with pytest.raises(DslError) as excinfo:
        tokenize("revenue ** 2")
    assert "position" in str(excinfo.value)


def test_parse_builds_left_associative_subtraction():
    node = parse_expression("a - b - c")
    assert node == BinOp("-", BinOp("-", Ref("a"), Ref("b")), Ref("c"))


def test_parse_gives_multiplication_higher_precedence_than_addition():
    assert parse_expression("a + b * c") == BinOp("+", Ref("a"), BinOp("*", Ref("b"), Ref("c")))


def test_parse_honours_parentheses_and_unary_minus():
    assert parse_expression("-(a + 1)") == Neg(BinOp("+", Ref("a"), Number(1.0)))


def test_parse_rejects_an_unknown_function():
    with pytest.raises(DslError) as excinfo:
        parse_expression("__import__(a)")
    assert "unknown function" in str(excinfo.value)


def test_parse_rejects_wrong_arity():
    with pytest.raises(DslError) as excinfo:
        parse_expression("safe_div(a)")
    assert "safe_div" in str(excinfo.value)


def test_parse_rejects_trailing_input():
    with pytest.raises(DslError):
        parse_expression("a b")


def test_expression_names_are_sorted_and_deduplicated():
    assert expression_names(parse_expression("b + a + b")) == ("a", "b")


def test_ttm_lowers_to_a_four_row_window_with_a_completeness_guard():
    lowered = compile_expression("ttm(revenue)", _QUARTER)
    assert "ROWS BETWEEN 3 PRECEDING AND CURRENT ROW" in lowered.value_sql
    assert "count(b.revenue)" in lowered.value_sql
    assert lowered.max_lag == 3


def test_window_availability_uses_the_same_frame():
    lowered = compile_expression("ttm(revenue)", _QUARTER)
    assert "max(b.revenue_at)" in lowered.availability_sql
    assert lowered.availability_sql.count("ROWS BETWEEN 3 PRECEDING AND CURRENT ROW") == 1


def test_binary_availability_is_the_greatest_of_both_sides():
    lowered = compile_expression("revenue - total_assets", _QUARTER)
    assert "greatest(" in lowered.availability_sql
    assert "b.revenue_at" in lowered.availability_sql
    assert "b.total_assets_at" in lowered.availability_sql


def test_safe_div_guards_a_zero_denominator():
    lowered = compile_expression("safe_div(revenue, total_assets)", _QUARTER)
    assert "= 0" in lowered.value_sql
    assert "ELSE" in lowered.value_sql


def test_cagr_requires_an_integer_year_literal():
    with pytest.raises(DslError) as excinfo:
        compile_expression("cagr(revenue, total_assets)", _QUARTER)
    assert "integer literal" in str(excinfo.value)
    assert compile_expression("cagr(revenue, 3)", _QUARTER).max_lag == 12


def test_number_has_no_availability():
    lowered = compile_expression("revenue * 2", _QUARTER)
    assert lowered.availability_sql.count("b.revenue_at") == 1


def test_unknown_reference_is_rejected_by_the_lowering():
    with pytest.raises(DslError) as excinfo:
        compile_expression("ebitda", _QUARTER)
    assert "ebitda" in str(excinfo.value)


def test_quarter_function_is_rejected_on_the_daily_grid():
    with pytest.raises(DslError) as excinfo:
        compile_expression("ttm(adj_close)", _DAY)
    assert "quarter" in str(excinfo.value)


def test_daily_function_is_rejected_on_the_quarterly_grid():
    with pytest.raises(DslError) as excinfo:
        compile_expression("tret(revenue, 21)", _QUARTER)
    assert "day" in str(excinfo.value)


def test_tret_divides_by_the_lagged_price():
    lowered = compile_expression("tret(adj_close, 21)", _DAY)
    assert "lag(d.adj_close, 21)" in lowered.value_sql
    assert lowered.max_lag == 21


def test_rvol_annualizes_with_sqrt_252():
    lowered = compile_expression("rvol(log_return, 60)", _DAY)
    assert "sqrt(252" in lowered.value_sql
    assert "stddev_samp(d.log_return)" in lowered.value_sql
    assert lowered.max_lag == 59


def test_no_expression_reaches_python_builtins():
    for hostile in ("__class__", "open('x')", "eval(a)", "a; DROP TABLE t", "a--b\nc"):
        with pytest.raises(DslError):
            parse_expression(hostile)


def _pointwise_availability(availability_sql: str, periods: int, context: LowerContext) -> str:
    """The GREATEST(current, lag(current, periods)) form a lag-of-N window should use.

    Anything wider (e.g. a MAX(...) OVER a multi-row frame) would pull in
    availabilities of rows the value expression never actually reads.
    """
    lagged = (
        f"lag({availability_sql}, {periods}) OVER "
        f"(PARTITION BY {context.partition_sql} ORDER BY {context.order_sql})"
    )
    return (
        f"nullif(greatest(coalesce({availability_sql}, TIMESTAMP '-infinity'), "
        f"coalesce({lagged}, TIMESTAMP '-infinity')), TIMESTAMP '-infinity')"
    )


def test_avg2_availability_is_pointwise_not_frame_wide():
    lowered = compile_expression("avg2(revenue)", _QUARTER)
    assert lowered.availability_sql == _pointwise_availability("b.revenue_at", 4, _QUARTER)


def test_yoy_availability_is_pointwise_not_frame_wide():
    lowered = compile_expression("yoy(revenue)", _QUARTER)
    assert lowered.availability_sql == _pointwise_availability("b.revenue_at", 4, _QUARTER)


def test_qoq_availability_is_pointwise_not_frame_wide():
    lowered = compile_expression("qoq(revenue)", _QUARTER)
    assert lowered.availability_sql == _pointwise_availability("b.revenue_at", 1, _QUARTER)


def test_cagr_availability_is_pointwise_not_frame_wide():
    lowered = compile_expression("cagr(revenue, 3)", _QUARTER)
    assert lowered.availability_sql == _pointwise_availability("b.revenue_at", 12, _QUARTER)


def test_tret_availability_is_pointwise_not_frame_wide():
    lowered = compile_expression("tret(adj_close, 21)", _DAY)
    assert lowered.availability_sql == _pointwise_availability("d.bar_at", 21, _DAY)


def test_nested_window_composition_is_rejected():
    with pytest.raises(DslError) as excinfo:
        compile_expression("stdev_q(qoq(revenue), 8)", _QUARTER)
    message = str(excinfo.value)
    assert "stdev_q" in message
    assert "qoq" in message


def test_window_nested_under_a_scalar_function_is_still_rejected():
    with pytest.raises(DslError) as excinfo:
        compile_expression("stdev_q(abs(yoy(revenue)), 8)", _QUARTER)
    message = str(excinfo.value)
    assert "stdev_q" in message
    assert "yoy" in message


def test_scalar_function_wrapping_a_window_function_is_allowed():
    lowered = compile_expression("abs(yoy(revenue))", _QUARTER)
    assert lowered.value_sql.startswith("abs(")


def test_scalar_function_combining_two_independent_window_functions_is_allowed():
    lowered = compile_expression("safe_div(ttm(revenue), ttm(total_assets))", _QUARTER)
    assert "sum(b.revenue)" in lowered.value_sql
    assert "sum(b.total_assets)" in lowered.value_sql
