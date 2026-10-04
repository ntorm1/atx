"""Exact loaded reader/runtime and timezone identities for native admission."""
import hashlib
import importlib
import importlib.resources
import json
import types
import inspect
import sys
from pathlib import Path


def _code_sha(code):
    """Fingerprint loaded semantics without marshal's mutable intern references."""
    def constant(value):
        if isinstance(value, types.CodeType):
            return structure(value)
        if isinstance(value, (tuple, frozenset)):
            values = [constant(item) for item in value]
            if isinstance(value, frozenset):
                values.sort(key=lambda item: json.dumps(item, sort_keys=True))
            return [type(value).__name__, values]
        if isinstance(value, bytes):
            return ["bytes", value.hex()]
        return [type(value).__name__, repr(value)]
    def structure(value):
        return {"bytecode": value.co_code.hex(), "constants": [constant(item) for item in value.co_consts],
                "names": value.co_names, "varnames": value.co_varnames,
                "freevars": value.co_freevars, "cellvars": value.co_cellvars,
                "flags": value.co_flags, "argcount": value.co_argcount,
                "posonlyargcount": value.co_posonlyargcount, "kwonlyargcount": value.co_kwonlyargcount,
                "exceptiontable": value.co_exceptiontable.hex()}
    return hashlib.sha256(json.dumps(structure(code), sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def runtime_identity() -> dict:
    import pyarrow as pa
    import pyarrow.parquet as pq
    import duckdb
    from . import common as C
    names = ("pyarrow", "pyarrow.parquet", "pyarrow.parquet.core", "pyarrow.lib", "pyarrow._parquet",
             "pyarrow._dataset", "pyarrow._dataset_parquet", "duckdb", "_duckdb", "_datetime", "_zoneinfo")
    modules = [importlib.import_module(name) for name in names]
    python_dll = Path(sys.base_prefix) / f"python{sys.version_info.major}{sys.version_info.minor}.dll"
    files = {}
    for module in modules:
        path = Path(module.__file__) if getattr(module, "__file__", None) else python_dll
        if not path.is_file():
            raise ValueError("actual builtin Python runtime binary identity unavailable")
        files[module.__name__] = {"origin": module.__spec__.origin,
                                 "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
    entries = {}
    for path in sorted(Path(pa.__file__).parent.glob("*.dll")):
        files["pyarrow-shared/" + path.name] = {"origin": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
    for name, value in (("parquet.read_table", pq.read_table), ("parquet.read_schema", pq.read_schema),
                        ("parquet.ParquetFile", pq.ParquetFile), ("parquet.ParquetFile.__init__", pq.ParquetFile.__init__),
                        ("parquet.ParquetFile.iter_batches", pq.ParquetFile.iter_batches),
                        ("arrow.BufferReader", pa.BufferReader), ("duckdb.connect", duckdb.connect),
                        ("common.connect", C.connect), ("common.copy_to_parquet", C.copy_to_parquet),
                        ("common.write_json_atomic", C.write_json_atomic)):
        entries[name] = {"module": value.__module__, "qualname": value.__qualname__}
        code = getattr(value, "__code__", None)
        if code is not None:
            entries[name]["loaded_code_sha256"] = _code_sha(code)
    # Explicit tzdata bytes are used by the clock reader; do not depend on a
    # mutable ZoneInfo cache or platform search-path preference.
    tz = importlib.resources.files("tzdata.zoneinfo").joinpath("America/New_York").read_bytes()
    loaded = {}
    for name in ("mna_native_v2", "mna_vintages_v2", "terminal_mna_v2", "mna_financial_v2",
                 "mna_price_native_v2", "mna_exact_v2", "mna_publish_v2"):
        module = importlib.import_module("atx_db.alpha_panel." + name)
        for field, value in vars(module).items():
            if inspect.isfunction(value) and value.__module__ == module.__name__:
                loaded[name + "." + field] = _code_sha(value.__code__)
    return {"python": sys.version, "executable_sha256": hashlib.sha256(Path(sys.executable).read_bytes()).hexdigest(),
            "pyarrow_version": pa.__version__, "duckdb_version": duckdb.__version__, "files": files,
            "entrypoints": entries, "loaded_python_readers": loaded,
            "timezone": {"source": "tzdata.zoneinfo/America/New_York",
                                                     "sha256": hashlib.sha256(tz).hexdigest()}}


def new_york_zone():
    import io
    from zoneinfo import ZoneInfo
    data = importlib.resources.files("tzdata.zoneinfo").joinpath("America/New_York").read_bytes()
    return ZoneInfo.from_file(io.BytesIO(data), key="America/New_York")
