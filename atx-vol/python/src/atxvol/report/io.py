"""Readers for the deterministic TSVs the C++ tools emit.

`write_backtest_tsv` and `write_backtest_pnl_tsv` write doubles with `%.17g`, so
the values round-trip through `float()` bit-exactly. Reading one back into a
`BacktestResult` means the library's own `tearsheet` does the fold — the report
layer never reimplements a metric.
"""

from __future__ import annotations

import numpy as np

from .runarchive import BacktestSection, RunArchive, read_backtest_section

__all__ = ["read_backtest_tsv", "read_backtest_archive", "read_kv_tsv"]

# Columns `write_backtest_tsv` emits, in its order. Anything else in the file
# (extra signal columns) is returned in the side dict rather than dropped. There
# is deliberately no `step_pnl_total`: it was a phantom the reader looked for but
# the C++ writer never emitted (append_backtest_series_tsv has no such column).
_SERIES = (
    "pnl_total", "pnl_delta", "pnl_gamma", "pnl_vega", "pnl_vanna", "pnl_volga",
    "pnl_theta", "pnl_rho", "pnl_charm", "pnl_unexplained", "pnl_settlement",
    "pnl_shares", "financing", "cost", "nav", "cash", "gross_delta", "gross_gamma",
    "gross_vega", "gross_theta", "turnover_notional", "turnover_vega",
    "n_open_lots", "n_unpriced_lots", "n_unpriced_greeks",
)


def read_backtest_tsv(path: str) -> tuple["_av.BacktestResult", dict[str, str], dict[str, list[float]]]:
    """Load a backtest TSV.

    Returns `(result, meta, extra)` — `meta` is the `# key=value` header block
    (empty for `write_backtest_tsv` output, populated for the PnL-track variant)
    and `extra` holds any columns outside the standard schema.
    """
    import atxvol as _av  # lazy: only the loose-TSV reader needs the binding.

    meta: dict[str, str] = {}
    header: list[str] = []
    rows: list[list[str]] = []

    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.rstrip("\n").rstrip("\r")
            if not line:
                continue
            if line.startswith("#"):
                key, _, value = line[1:].strip().partition("=")
                if value:
                    meta[key.strip()] = value.strip()
                continue
            fields = line.split("\t")
            if not header:
                header = fields
            else:
                rows.append(fields)

    if not header:
        raise ValueError(f"{path}: no column header row found")

    index = {name: i for i, name in enumerate(header)}

    def column(name: str) -> list[float]:
        i = index[name]
        return [float(r[i]) if i < len(r) and r[i] != "" else float("nan") for r in rows]

    result = _av.BacktestResult()
    # Size every column first: the library's consumers index all columns by the
    # row count, so any column the file omits must still be present (zeroed)
    # rather than left empty.
    result.resize(len(rows))
    if "date" in index:
        di = index["date"]
        result.date = [r[di] for r in rows]
    if "ts_ns" in index:
        ti = index["ts_ns"]
        result.ts_ns = [int(float(r[ti])) for r in rows]

    for name in _SERIES:
        if name in index:
            setattr(result, name, column(name))
    result.validate()

    known = {"date", "ts_ns", *_SERIES}
    extra = {name: column(name) for name in header if name not in known}
    return result, meta, extra


def read_backtest_archive(
    path: str, section: str = "backtest"
) -> tuple[BacktestSection, dict[str, str], dict[str, np.ndarray]]:
    """Load a backtest section from a ``run.atxrun`` RunArchive.

    The binary-container counterpart of :func:`read_backtest_tsv`, returning the
    same ``(result, meta, extra)`` shape via the binding-free
    :func:`runarchive.read_backtest_section`. ``result`` is a
    :class:`runarchive.BacktestSection` (``date`` / ``ts_ns`` plus every registry
    F64 series as an attribute), ``meta`` is the archive's ``meta`` key/value
    section, and ``extra`` holds any dynamically-appended per-signal columns.

    The archive's mapped column views are materialized into owned numpy arrays
    before the mapping is released, so the returned data outlives this call (no
    dangling view over a closed mmap).
    """
    archive = RunArchive.open(path)
    try:
        result, meta, extra = read_backtest_section(archive, section)
        owned_series = {name: np.array(arr) for name, arr in result.series.items()}
        owned_extra = {name: np.array(arr) for name, arr in extra.items()}
        result = BacktestSection(
            date=list(result.date), ts_ns=list(result.ts_ns), series=owned_series
        )
        meta = dict(meta)
    finally:
        # Copies above are owned; if numpy still exports the original views close()
        # no-ops (BufferError) and leaves the small mapping open for the process.
        archive.close()
    return result, meta, owned_extra


def read_kv_tsv(path: str) -> dict[str, str]:
    """Read a two-column `key<TAB>value` TSV (the run_spec / counters files)."""
    out: dict[str, str] = {}
    with open(path, "r", encoding="utf-8") as fh:
        for n, line in enumerate(fh):
            line = line.rstrip("\n").rstrip("\r")
            if not line or line.startswith("#"):
                continue
            key, _, value = line.partition("\t")
            if n == 0 and key.strip().lower() == "key":
                continue  # column-name header
            out[key.strip()] = value.strip()
    return out
