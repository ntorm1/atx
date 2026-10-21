"""Independent shape check of served quote-slice prices (necessary conditions of the certificate).

usage: python -E -P -B -X utf8 check_served.py [--market-distance [BOUND]] <dataset_dir> <predictions.csv> [...]

Brought into the tree by lane L3q (2026-10-21) from the evidence copies
(slice-repair-20261017-bench-v2 -> audit-robustness-20261020 -> L3r1-20261021-resume,
sha256 b9790fe5...), unchanged except the opt-in `--market-distance` class: a served row is
status ok OR ok_price_only (a price-only row carries a certified price and a blank vol, so it
is part of the served price set the certificate covers).

Per served chain and side, over its served rows sorted by strike: calls non-increasing and puts
non-decreasing in strike, and discrete convexity (each interior price at most the chord of its
neighbours). Across expiries of one (underlier, root, snapshot): at a strike and side served by
both, the earlier expiry's price is at most the later one's. These hold for any subset of the
certified nodes, so a violation beyond `TOL` means a served price set the certificate should
have refused. TOL absorbs CSV rounding only.

--market-distance [BOUND] (default off, so every existing receipt is unchanged; BOUND defaults
to 1.0): adds the class `market_distance` = served rows whose price lies more than BOUND spreads
outside their own quote, in the gate's unit (american_price_slice.hpp market_distance_spreads,
the scorer's tools/quote_benchmark.py spread_error): 0 inside [bid, ask], (price - ask) / s
above, (bid - price) / s below, s = max(ask - bid, tick 0.01); rows without a valid quote
(ask <= bid, bid < 0, a missing edge) are not measured. The flagged row ids are listed.
"""
import json
import sys
from pathlib import Path

import polars as pl

TOL = 1.0e-6
TICK = 0.01
PRICE_SERVED = ("ok", "ok_price_only")


def market_distance(d: pl.DataFrame, bound: float) -> tuple[int, float, list[int]]:
    s = pl.max_horizontal(pl.col("ask") - pl.col("bid"), pl.lit(TICK))
    m = d.filter(pl.col("bid").is_finite() & pl.col("ask").is_finite() & (pl.col("bid") >= 0)
                 & (pl.col("ask") > pl.col("bid"))).with_columns(
        pl.max_horizontal((pl.col("predicted_price") - pl.col("ask")) / s,
                          (pl.col("bid") - pl.col("predicted_price")) / s,
                          pl.lit(0.0)).alias("distance"))
    bad = m.filter(pl.col("distance") > bound + TOL)
    worst = float(m["distance"].max()) if m.height else 0.0
    return bad.height, worst, sorted(int(r) for r in bad["row_id"].to_list())


def check(dataset: Path, path: str, bound: float | None) -> dict:
    q = pl.read_parquet(dataset / "quotes.parquet").select(
        "row_id", "chain_id", "underlier", "root", "snapshot_ns", "expiry_days", "strike", "is_call",
        "bid", "ask")
    p = pl.read_csv(path, schema_overrides={"row_id": pl.UInt64}, infer_schema_length=0,
                    null_values="").select(
        "row_id", pl.col("predicted_price").cast(pl.Float64, strict=False), "status")
    d = q.join(p, on="row_id", how="inner").filter(
        pl.col("status").is_in(PRICE_SERVED) & pl.col("predicted_price").is_finite())
    # One price per (chain, side, strike): duplicates must agree.
    g = d.group_by("chain_id", "underlier", "root", "snapshot_ns", "expiry_days", "is_call",
                   "strike").agg(pl.col("predicted_price").min().alias("lo"),
                                 pl.col("predicted_price").max().alias("hi"))
    dup = g.filter(pl.col("hi") - pl.col("lo") > TOL).height
    g = g.with_columns(price=pl.col("lo")).sort("chain_id", "is_call", "strike")
    worst = {"monotone": 0.0, "convex": 0.0, "calendar": 0.0}
    bad = {"monotone": 0, "convex": 0, "calendar": 0}
    for (chain, is_call), s in g.group_by("chain_id", "is_call", maintain_order=True):
        k = s["strike"].to_list()
        v = s["price"].to_list()
        call = int(is_call) == 1
        for i in range(1, len(k)):
            step = (v[i] - v[i - 1]) if call else (v[i - 1] - v[i])
            if step > TOL:
                bad["monotone"] += 1
            worst["monotone"] = max(worst["monotone"], step)
        for i in range(1, len(k) - 1):
            w = (k[i] - k[i - 1]) / (k[i + 1] - k[i - 1])
            chord = v[i - 1] * (1.0 - w) + v[i + 1] * w
            excess = v[i] - chord
            if excess > TOL:
                bad["convex"] += 1
            worst["convex"] = max(worst["convex"], excess)
    cal = g.join(g, on=["underlier", "root", "snapshot_ns", "is_call", "strike"], suffix="_l").filter(
        pl.col("expiry_days") < pl.col("expiry_days_l")).with_columns(
        cross=pl.col("price") - pl.col("price_l"))
    bad["calendar"] = int((cal["cross"] > TOL).sum())
    worst["calendar"] = float(cal["cross"].max()) if cal.height else 0.0
    out = {"predictions": path, "served_rows": d.height, "served_chains": d["chain_id"].n_unique(),
           "duplicate_strike_disagreements": dup, "violations": bad, "worst_excess": worst,
           "calendar_pairs_checked": cal.height, "tolerance": TOL}
    if bound is not None:
        count, far, rows = market_distance(d, bound)
        bad["market_distance"] = count
        worst["market_distance"] = far
        out["market_distance_bound_spreads"] = bound
        out["market_distance_rows"] = rows
    return out


def main() -> None:
    args = sys.argv[1:]
    bound = None
    if args and args[0] == "--market-distance":
        args = args[1:]
        bound = 1.0
        if args:
            try:
                bound = float(args[0])
                args = args[1:]
            except ValueError:
                pass
    dataset = Path(args[0])
    print(json.dumps([check(dataset, path, bound) for path in args[1:]], indent=1))


if __name__ == "__main__":
    main()
