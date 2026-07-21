"""Readers for the deterministic TSVs the C++ tools emit.

`write_backtest_tsv` and `write_backtest_pnl_tsv` write doubles with `%.17g`, so
the values round-trip through `float()` bit-exactly. Reading one back into a
`BacktestResult` means the library's own `tearsheet` does the fold — the report
layer never reimplements a metric.
"""

from __future__ import annotations

from typing import Iterable

import atxvol as _av

__all__ = ["read_backtest_tsv", "read_kv_tsv"]

# Columns `write_backtest_tsv` emits, in its order. Anything else in the file
# (extra signal columns) is returned in the side dict rather than dropped.
_SERIES = (
    "pnl_total", "pnl_delta", "pnl_gamma", "pnl_vega", "pnl_vanna", "pnl_volga",
    "pnl_theta", "pnl_rho", "pnl_charm", "pnl_unexplained", "pnl_settlement",
    "pnl_shares", "financing", "cost", "nav", "cash", "gross_delta", "gross_gamma",
    "gross_vega", "gross_theta", "turnover_notional", "turnover_vega",
    "n_open_lots", "n_unpriced_lots", "n_unpriced_greeks", "step_pnl_total",
)


def read_backtest_tsv(path: str) -> tuple["_av.BacktestResult", dict[str, str], dict[str, list[float]]]:
    """Load a backtest TSV.

    Returns `(result, meta, extra)` — `meta` is the `# key=value` header block
    (empty for `write_backtest_tsv` output, populated for the PnL-track variant)
    and `extra` holds any columns outside the standard schema.
    """
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
