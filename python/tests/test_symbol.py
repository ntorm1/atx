import pytest

from atxpy._core import Symbol, SymbolTable


def test_intern_is_idempotent():
    t = SymbolTable()
    a1 = t.intern("AAPL")
    a2 = t.intern("AAPL")
    msft = t.intern("MSFT")
    assert a1 == a2
    assert a1 != msft
    assert t.size() == 2


def test_name_roundtrip():
    t = SymbolTable()
    s = t.intern("SPY")
    assert t.name(s) == "SPY"


def test_symbol_id_and_hash():
    t = SymbolTable()
    s = t.intern("QQQ")
    assert isinstance(s.id, int)
    assert hash(s) == hash(t.intern("QQQ"))  # equal symbols hash equal


def test_name_out_of_range_raises():
    # A Symbol not from this table (id >= size) would ABORT in C++; binding guards it.
    t = SymbolTable()
    t.intern("ONE")
    bogus = Symbol(999)
    with pytest.raises((ValueError, IndexError)):
        t.name(bogus)
