import math

import pytest

from atxpy._core import AtxError, Decimal


def test_from_string_roundtrip():
    d = Decimal.from_string("123.456789")
    assert d.to_string() == "123.456789"
    assert math.isclose(float(d), 123.456789, rel_tol=1e-12)


def test_from_int_and_arithmetic():
    a = Decimal.from_int(3)
    b = Decimal.from_string("1.5")
    assert (a + b).to_string() == "4.5"
    assert (a - b).to_string() == "1.5"
    assert (a * b).to_string() == "4.5"
    assert (-b).to_string() == "-1.5"


def test_comparison_and_eq():
    assert Decimal.from_int(2) < Decimal.from_int(3)
    assert Decimal.from_string("1.5") == Decimal.from_string("1.5")
    assert Decimal.from_int(3) >= Decimal.from_string("3.0")


def test_from_string_bad_input_raises():
    with pytest.raises(AtxError):
        Decimal.from_string("not-a-number")


def test_from_int_out_of_range_raises():
    # |whole| > kMaxWhole (9_223_372_036) would ABORT in C++; the binding guards it.
    with pytest.raises((AtxError, ValueError, OverflowError)):
        Decimal.from_int(10_000_000_000)


def test_repr_and_raw():
    d = Decimal.from_string("2.5")
    assert d.raw() == 2_500_000_000
    assert "2.5" in repr(d)
