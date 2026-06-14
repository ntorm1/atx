def test_import_core():
    import atxpy
    from atxpy import _core

    assert hasattr(_core, "__doc__")


def test_version():
    import atxpy

    assert isinstance(atxpy.__version__, str)
    assert atxpy.__version__


def test_public_exports():
    import atxpy

    assert {"Decimal", "Symbol", "SymbolTable", "AtxError"} <= set(dir(atxpy))
