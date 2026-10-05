"""D-COV reader (gate report item 4 (c), as amended for M2i): risk-forecast accuracy of two risk stores written by the
same exe on the same role, atx-risk-v1.1 vs atx-cov-v1, side by side.

usage: python dcov_read.py V11_DIR COVV1_DIR

Reads only:
- each store's manifest.json: `model`, `recipe.eigen_adjustment`, `eigen_adjustment.sessions`, `extended_families`
  (if the manifest carries none, `bias_summary.json`'s `extended_families.unformed_sessions` is read instead), and the
  `container` block;
- each store's bias_summary.json, families random, factor, eigen, minvar, optimized (no book family is formed: no
  --book-weights): status, series_ok, observations, full_sample b_mean / b_median / series_in_band /
  series_in_kurtosis_band, rolling 63 / 252 share_in_band and b_mean, qlike, mrad 63 / 252 mean;
- from bias.csv only the eigen family's columns `family`, `name`, `z`, plus `session` (a timestamp, used only to check
  that both stores score the same observations; never printed).
Never reads or prints `forecast_vol`, `realized_return`, `b63`, `b252` or any NAV / book / return / IC / Sharpe figure.

Per eigen rank k (COV-11: read in both directions): b_k = sample SD of z (ddof 1; the exe's bias_statistic: finite z
only, mean removed), T_k = finite z count, band sqrt(2 / T_k), in band |b - 1| <= band, direction b < 1 - band
"over-forecast / over-corrected", b > 1 + band "under-forecast"; reduction of |b - 1| from v1.1 to atx-cov-v1
(positive = atx-cov-v1 nearer 1). Pooled eigen b over all ranks and without eigen-00, with N and band sqrt(2 / N);
ranks in band (count).
"""
from __future__ import annotations

import csv
import json
import math
import sys
from pathlib import Path

FAMILIES = ("random", "factor", "eigen", "minvar", "optimized")


def bias_statistic(z: list[float]) -> float:
    f = [v for v in z if math.isfinite(v)]
    if len(f) < 2:
        return math.nan
    mean = sum(f) / len(f)
    return math.sqrt(sum((v - mean) * (v - mean) for v in f) / (len(f) - 1))


def eigen_rows(store: Path) -> dict[str, list[tuple[str, float]]]:
    out: dict[str, list[tuple[str, float]]] = {}
    with (store / "bias.csv").open(newline="") as fh:
        for row in csv.DictReader(fh):
            if row["family"] != "eigen":
                continue
            out.setdefault(row["name"], []).append((row["session"], float(row["z"])))
    return out


def fmt(v, w: int = 10) -> str:
    if v is None:
        return f"{'null':>{w}}"
    if isinstance(v, bool) or isinstance(v, str):
        return f"{str(v):>{w}}"
    if isinstance(v, int):
        return f"{v:>{w}d}"
    if isinstance(v, float) and not math.isfinite(v):
        return f"{'nan':>{w}}"
    return f"{v:>{w}.6f}"


def get(d, *path):
    for p in path:
        if not isinstance(d, dict) or p not in d:
            return None
        d = d[p]
    return d


def main(argv: list[str]) -> int:
    v11, cov = Path(argv[0]), Path(argv[1])
    names = ("atx-risk-v1.1", "atx-cov-v1")
    stores = (v11, cov)
    man = [json.loads((s / "manifest.json").read_text(encoding="utf-8")) for s in stores]
    summ = [json.loads((s / "bias_summary.json").read_text(encoding="utf-8")) for s in stores]
    print(f"D-COV read: v1.1 store {v11.as_posix()} | atx-cov-v1 store {cov.as_posix()}")
    print("\n## Manifest blocks")
    for n, m, s in zip(names, man, summ):
        print(f"[{n}] model = {m.get('model')!r}")
        print(f"[{n}] recipe.eigen_adjustment = {json.dumps(get(m, 'recipe', 'eigen_adjustment'), sort_keys=True)}")
        print(f"[{n}] eigen_adjustment.sessions = {json.dumps(get(m, 'eigen_adjustment', 'sessions'), sort_keys=True)}")
        ext = m.get("extended_families")
        if ext is not None:
            print(f"[{n}] manifest extended_families.unformed_sessions = {get(ext, 'unformed_sessions')}")
        else:
            print(f"[{n}] manifest extended_families = null; bias_summary extended_families.unformed_sessions = "
                  f"{get(s, 'extended_families', 'unformed_sessions')}")
        print(f"[{n}] container = {json.dumps(m.get('container'), sort_keys=True)}")
        print(f"[{n}] bias_summary families present = {sorted(get(s, 'families') or {})}")

    print("\n## Families (bias_summary.json), v1.1 | atx-cov-v1")
    rows = [("status", ("status",)), ("series_ok", ("series_ok",)), ("observations", ("observations",)),
            ("full b_mean", ("full_sample", "b_mean")), ("full b_median", ("full_sample", "b_median")),
            ("series_in_band", ("full_sample", "series_in_band")),
            ("series_in_kurtosis_band", ("full_sample", "series_in_kurtosis_band")),
            ("roll63 share_in_band", ("rolling", "63", "share_in_band")),
            ("roll63 b_mean", ("rolling", "63", "b_mean")),
            ("roll252 share_in_band", ("rolling", "252", "share_in_band")),
            ("roll252 b_mean", ("rolling", "252", "b_mean")),
            ("qlike", ("qlike",)), ("mrad63 mean", ("mrad", "63", "mean")), ("mrad252 mean", ("mrad", "252", "mean"))]
    for fam in FAMILIES:
        a, b = get(summ[0], "families", fam), get(summ[1], "families", fam)
        print(f"- {fam}:")
        for label, path in rows:
            va, vb = get(a, *path), get(b, *path)
            print(f"    {label:<26}{fmt(va, 12)} | {fmt(vb, 12)}")
        same_obs = get(a, "observations") == get(b, "observations")
        print(f"    {'same observation count':<26}{str(same_obs):>12}")

    print("\n## Eigen family per rank (bias.csv: family, name, z; session for alignment only)")
    ea, eb = eigen_rows(v11), eigen_rows(cov)
    ranks = sorted(set(ea) | set(eb))
    aligned = all(ea.get(r) is not None and eb.get(r) is not None and
                  [s for s, _ in ea[r]] == [s for s, _ in eb[r]] and
                  [math.isfinite(z) for _, z in ea[r]] == [math.isfinite(z) for _, z in eb[r]] for r in ranks)
    print(f"ranks {len(ranks)}; identical observations (same sessions and finite-z pattern per rank in both): {aligned}")
    print(f"{'rank':<10}{'T':>6}{'band':>10} | {'b v1.1':>10}{'in':>4} {'direction v1.1':<30}| "
          f"{'b cov-v1':>10}{'in':>4} {'direction cov-v1':<30}| {'|b-1| reduction':>16}")

    def direction(b: float, band: float) -> str:
        if not math.isfinite(b):
            return "n/a"
        if b < 1 - band:
            return "below: over-forecast/over-corr."
        if b > 1 + band:
            return "above: under-forecast"
        return "in band"

    in_a = in_b = 0
    pooled = {0: ([], []), 1: ([], [])}  # all ranks / without eigen-00: (v11 z, cov z)
    for r in ranks:
        za = [z for _, z in ea.get(r, [])]
        zb = [z for _, z in eb.get(r, [])]
        ta, tb = sum(math.isfinite(v) for v in za), sum(math.isfinite(v) for v in zb)
        ba, bb = bias_statistic(za), bias_statistic(zb)
        banda = math.sqrt(2 / ta) if ta else math.nan
        bandb = math.sqrt(2 / tb) if tb else math.nan
        ia = math.isfinite(ba) and abs(ba - 1) <= banda
        ib = math.isfinite(bb) and abs(bb - 1) <= bandb
        in_a += ia
        in_b += ib
        red = abs(ba - 1) - abs(bb - 1)
        tcol = f"{ta}" if ta == tb else f"{ta}/{tb}"
        print(f"{r:<10}{tcol:>6}{banda:>10.4f} | {ba:>10.4f}{'y' if ia else 'n':>4} {direction(ba, banda):<30}| "
              f"{bb:>10.4f}{'y' if ib else 'n':>4} {direction(bb, bandb):<30}| {red:>+16.4f}")
        pooled[0][0].extend(za)
        pooled[0][1].extend(zb)
        if r != "eigen-00":
            pooled[1][0].extend(za)
            pooled[1][1].extend(zb)
    print(f"ranks in band: v1.1 {in_a} of {len(ranks)} | atx-cov-v1 {in_b} of {len(ranks)}")
    for key, label in ((0, "pooled, all ranks"), (1, "pooled, without eigen-00")):
        za, zb = pooled[key]
        na, nb = sum(math.isfinite(v) for v in za), sum(math.isfinite(v) for v in zb)
        ba, bb = bias_statistic(za), bias_statistic(zb)
        banda = math.sqrt(2 / na) if na else math.nan
        print(f"{label:<26} N {na if na == nb else f'{na}/{nb}'}  band {banda:.4f} | v1.1 b {ba:.4f} "
              f"({direction(ba, banda)}) | atx-cov-v1 b {bb:.4f} ({direction(bb, banda)}) | |b-1| reduction "
              f"{abs(ba - 1) - abs(bb - 1):+.4f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
