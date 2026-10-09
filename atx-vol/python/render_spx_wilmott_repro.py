#!/usr/bin/env python3
"""Render and score the atx-vol SPX Wilmott Figure 1 reproduction.

The input is the mixed-section CSV written by ``spx_wilmott_repro``.  This
tool is intentionally offline and keeps its PNG geometry and JSON encoding
stable so that a reproduction can be compared across fitter changes.
"""

from __future__ import annotations

import argparse
import bisect
import csv
import dataclasses
import datetime as dt
import hashlib
import io
import json
import math
import pathlib
import statistics
from collections.abc import Iterable, Sequence
from zoneinfo import ZoneInfo

import matplotlib

matplotlib.use("Agg")
from matplotlib.backends.backend_agg import FigureCanvasAgg  # noqa: E402
from matplotlib.figure import Figure  # noqa: E402
from matplotlib.ticker import AutoMinorLocator, MultipleLocator  # noqa: E402


WIDTH_PX = 1318
HEIGHT_PX = 1139
DPI = 100
DEFAULT_X_LIMITS = (-10.6, 2.9)
DEFAULT_Y_LIMITS = (0.0, 0.565)
FIT_COLOR = "#8d5ab5"
MARKET_COLOR = "#159c70"


@dataclasses.dataclass(frozen=True)
class CurvePoint:
    ns: float
    iv: float


@dataclasses.dataclass(frozen=True)
class QuotePoint:
    ns: float
    strike: float
    side: str
    market_iv: float
    bid_iv: float
    ask_iv: float
    fitted_iv: float
    residual_iv: float


@dataclasses.dataclass(frozen=True)
class VendorPoint:
    ns: float
    iv: float


@dataclasses.dataclass
class ReproData:
    source: str
    meta: dict[str, str]
    summaries: list[dict[str, str]]
    curves: dict[str, list[CurvePoint]]
    quotes: dict[str, list[QuotePoint]]


def _csv_fields(text: str) -> list[str]:
    return next(csv.reader([text], skipinitialspace=True))


def _section_header(line: str, marker: str) -> list[str]:
    header = line[len(marker) :].lstrip(" ,\t")
    return _csv_fields(header) if header else []


def _finite_float(value: str, *, field: str, line_number: int) -> float:
    try:
        parsed = float(value)
    except (TypeError, ValueError) as error:
        raise ValueError(f"line {line_number}: invalid {field}: {value!r}") from error
    if not math.isfinite(parsed):
        raise ValueError(f"line {line_number}: non-finite {field}: {value!r}")
    return parsed


def _row_dict(header: Sequence[str], line: str, line_number: int) -> dict[str, str]:
    values = _csv_fields(line)
    if len(values) != len(header):
        raise ValueError(
            f"line {line_number}: expected {len(header)} fields, found {len(values)}"
        )
    return dict(zip(header, values, strict=True))


def _sorted_curve(points: Iterable[CurvePoint]) -> list[CurvePoint]:
    """Sort points and deterministically average duplicate abscissas."""

    grouped: dict[float, list[float]] = {}
    for point in points:
        grouped.setdefault(point.ns, []).append(point.iv)
    return [
        CurvePoint(ns, statistics.fmean(grouped[ns]))
        for ns in sorted(grouped)
    ]


def parse_repro_text(text: str, *, source: str = "<memory>") -> ReproData:
    """Parse the ``#META/#SUMMARY/#CURVE/#QUOTES`` mixed CSV format."""

    meta: dict[str, str] = {}
    summaries: list[dict[str, str]] = []
    curves: dict[str, list[CurvePoint]] = {}
    quotes: dict[str, list[QuotePoint]] = {}
    section = ""
    header: list[str] = []

    for line_number, raw_line in enumerate(text.splitlines(), start=1):
        line = raw_line.strip().lstrip("\ufeff")
        if not line:
            continue
        if line.startswith("#META"):
            section = ""
            header = []
            payload = line[len("#META") :].lstrip(" ,\t")
            for item in _csv_fields(payload):
                if "=" not in item:
                    raise ValueError(f"line {line_number}: malformed metadata field {item!r}")
                key, value = item.split("=", maxsplit=1)
                meta[key.strip()] = value.strip()
            continue
        matched_section = next(
            (name for name in ("SUMMARY", "CURVE", "QUOTES") if line.startswith(f"#{name}")),
            None,
        )
        if matched_section is not None:
            section = matched_section
            header = _section_header(line, f"#{matched_section}")
            if not header:
                raise ValueError(f"line {line_number}: {matched_section} header is empty")
            continue
        if line.startswith("#"):
            continue
        if not section or not header:
            raise ValueError(f"line {line_number}: data appears before a section header")

        row = _row_dict(header, line, line_number)
        family = row.get("family", "").strip()
        if not family:
            raise ValueError(f"line {line_number}: family is empty")
        if section == "SUMMARY":
            summaries.append(row)
        elif section == "CURVE":
            curves.setdefault(family, []).append(
                CurvePoint(
                    _finite_float(row.get("z", row.get("ns", "")), field="z", line_number=line_number),
                    _finite_float(row.get("fitted_iv", row.get("vol", "")), field="fitted_iv", line_number=line_number),
                )
            )
        elif section == "QUOTES":
            market_iv = _finite_float(row.get("market_iv", ""), field="market_iv", line_number=line_number)
            fitted_iv = _finite_float(row.get("fitted_iv", ""), field="fitted_iv", line_number=line_number)
            residual_text = row.get("residual_iv", "")
            quotes.setdefault(family, []).append(
                QuotePoint(
                    ns=_finite_float(row.get("z", row.get("ns", "")), field="z", line_number=line_number),
                    strike=_finite_float(row.get("strike", ""), field="strike", line_number=line_number),
                    side=row.get("side", "").strip(),
                    market_iv=market_iv,
                    bid_iv=_finite_float(row.get("bid_iv", ""), field="bid_iv", line_number=line_number),
                    ask_iv=_finite_float(row.get("ask_iv", ""), field="ask_iv", line_number=line_number),
                    fitted_iv=fitted_iv,
                    residual_iv=(
                        _finite_float(residual_text, field="residual_iv", line_number=line_number)
                        if residual_text
                        else fitted_iv - market_iv
                    ),
                )
            )

    if not curves:
        raise ValueError(f"{source}: no fitted curve rows found")
    curves = {family: _sorted_curve(points) for family, points in curves.items()}
    return ReproData(source, meta, summaries, curves, quotes)


def parse_repro(path: pathlib.Path) -> ReproData:
    return parse_repro_text(path.read_text(encoding="utf-8-sig"), source=str(path))


def _family_lookup(families: Iterable[str], requested: str) -> str | None:
    folded = requested.casefold()
    return next((family for family in families if family.casefold() == folded), None)


def select_family(data: ReproData, requested: str | None = None) -> str:
    """Choose an explicit family or the lowest reported/derived quote RMSE."""

    families = sorted(data.curves)
    if requested:
        selected = _family_lookup(families, requested)
        if selected is None:
            raise ValueError(
                f"unknown family {requested!r}; available: {', '.join(families)}"
            )
        return selected

    recommended = data.meta.get("recommended_family", "").strip()
    if recommended:
        selected = _family_lookup(families, recommended)
        if selected is not None:
            return selected

    scores: dict[str, float] = {}
    for row in data.summaries:
        family = _family_lookup(families, row.get("family", ""))
        rmse_text = row.get("rmse_iv", row.get("rmse", ""))
        if family is None or not rmse_text:
            continue
        try:
            rmse = float(rmse_text)
        except ValueError:
            continue
        if math.isfinite(rmse) and rmse >= 0.0:
            scores[family] = rmse
    if not scores:
        for family, family_quotes in data.quotes.items():
            if family in data.curves and family_quotes:
                scores[family] = math.sqrt(
                    statistics.fmean(quote.residual_iv**2 for quote in family_quotes)
                )
    if scores:
        return min(scores, key=lambda family: (scores[family], family != "ConvexDense", family))
    convex_dense = _family_lookup(families, "ConvexDense")
    return convex_dense if convex_dense is not None else families[0]


def parse_vendor_text(text: str, *, source: str = "<memory>") -> list[VendorPoint]:
    """Read digitized vendor points from a TSV or CSV with ns/z and vol/iv."""

    meaningful = [line for line in text.splitlines() if line.strip() and not line.lstrip().startswith("#")]
    if not meaningful:
        raise ValueError(f"{source}: target curve is empty")
    delimiter = "\t" if "\t" in meaningful[0] else ","
    reader = csv.DictReader(io.StringIO("\n".join(meaningful)), delimiter=delimiter)
    if reader.fieldnames is None:
        raise ValueError(f"{source}: target header is missing")
    ns_key = next((key for key in ("ns", "z", "normalized_strike") if key in reader.fieldnames), None)
    iv_key = next((key for key in ("vol", "iv", "fitted_iv") if key in reader.fieldnames), None)
    if ns_key is None or iv_key is None:
        raise ValueError(f"{source}: target requires ns/z and vol/iv columns")
    points = [
        CurvePoint(
            _finite_float(row[ns_key], field=ns_key, line_number=index),
            _finite_float(row[iv_key], field=iv_key, line_number=index),
        )
        for index, row in enumerate(reader, start=2)
    ]
    return [VendorPoint(point.ns, point.iv) for point in _sorted_curve(points)]


def parse_vendor(path: pathlib.Path) -> list[VendorPoint]:
    return parse_vendor_text(path.read_text(encoding="utf-8-sig"), source=str(path))


def _interpolate(points: Sequence[CurvePoint], x: float) -> float:
    xs = [point.ns for point in points]
    index = bisect.bisect_left(xs, x)
    if index < len(points) and points[index].ns == x:
        return points[index].iv
    if index == 0 or index == len(points):
        raise ValueError(f"{x} is outside the interpolation domain")
    left = points[index - 1]
    right = points[index]
    weight = (x - left.ns) / (right.ns - left.ns)
    return left.iv + weight * (right.iv - left.iv)


def _percentile(values: Sequence[float], quantile: float) -> float:
    ordered = sorted(values)
    position = (len(ordered) - 1) * quantile
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def compare_to_vendor(
    curve: Sequence[CurvePoint], vendor: Sequence[VendorPoint]
) -> dict[str, object]:
    """Compare a fitted curve to vendor points on their common NS domain."""

    if len(curve) < 2:
        raise ValueError("fitted curve requires at least two points")
    if not vendor:
        raise ValueError("vendor curve requires at least one point")
    lower = max(curve[0].ns, vendor[0].ns)
    upper = min(curve[-1].ns, vendor[-1].ns)
    common = [point for point in vendor if lower <= point.ns <= upper]
    if not common:
        raise ValueError("fitted and vendor curves have no common domain")
    signed = [_interpolate(curve, point.ns) - point.iv for point in common]
    absolute = [abs(error) for error in signed]
    return {
        "n_common": len(common),
        "common_domain_ns": [common[0].ns, common[-1].ns],
        "rmse_iv": math.sqrt(statistics.fmean(error**2 for error in signed)),
        "max_abs_iv": max(absolute),
        "p50_abs_iv": _percentile(absolute, 0.50),
        "p95_abs_iv": _percentile(absolute, 0.95),
        "mean_signed_iv": statistics.fmean(signed),
    }


def _historical_stamp(snapshot: str) -> str:
    try:
        instant = dt.datetime.fromisoformat(snapshot.replace("Z", "+00:00"))
        if instant.tzinfo is None:
            instant = instant.replace(tzinfo=dt.timezone.utc)
        return instant.astimezone(ZoneInfo("America/New_York")).strftime("%Y%m%d-%H%M%S")
    except (ValueError, KeyError):
        return snapshot.replace("-", "").replace(":", "")


def _float_meta(meta: dict[str, str], key: str, fallback: float) -> float:
    try:
        value = float(meta.get(key, ""))
    except ValueError:
        return fallback
    return value if math.isfinite(value) else fallback


def _vendor_comparison_with_support(
    data: ReproData, curve: Sequence[CurvePoint], vendor: Sequence[VendorPoint]
) -> dict[str, object]:
    comparison = compare_to_vendor(curve, vendor)
    lower = _float_meta(data.meta, "visual_z_min", math.nan)
    upper = _float_meta(data.meta, "visual_z_max", math.nan)
    supported = [point for point in vendor if lower <= point.ns <= upper]
    comparison["supported_domain"] = (
        compare_to_vendor(curve, supported)
        if math.isfinite(lower) and math.isfinite(upper) and lower < upper and supported
        else None
    )
    return comparison


def _write_json(path: pathlib.Path, payload: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(payload, indent=2, sort_keys=True, ensure_ascii=True) + "\n",
        encoding="utf-8",
        newline="\n",
    )


def _render_png(
    output_path: pathlib.Path,
    data: ReproData,
    family: str,
    vendor: Sequence[VendorPoint] | None,
    *,
    overlay_vendor: bool,
) -> None:
    curve = data.curves[family]
    quotes = sorted(data.quotes.get(family, []), key=lambda point: (point.ns, point.side))
    matplotlib.rcParams.update(
        {
            "font.family": "DejaVu Sans",
            "font.size": 15,
            "axes.linewidth": 1.0,
            "path.simplify": False,
        }
    )
    figure = Figure(figsize=(WIDTH_PX / DPI, HEIGHT_PX / DPI), dpi=DPI, facecolor="white")
    FigureCanvasAgg(figure)
    # Match the reconstructed vendor figure's approximately 151/68/1285/1047
    # plot rectangle in the 1318 x 1139 canvas.
    axes = figure.add_axes((151 / WIDTH_PX, 92 / HEIGHT_PX, 1134 / WIDTH_PX, 979 / HEIGHT_PX))

    if quotes:
        ns = [point.ns for point in quotes]
        iv = [point.market_iv for point in quotes]
        lower = [max(0.0, point.market_iv - min(point.bid_iv, point.ask_iv)) for point in quotes]
        upper = [max(0.0, max(point.bid_iv, point.ask_iv) - point.market_iv) for point in quotes]
        axes.errorbar(
            ns,
            iv,
            yerr=[lower, upper],
            fmt="x",
            markersize=3.0,
            markeredgewidth=0.7,
            elinewidth=0.65,
            capsize=2.0,
            capthick=0.65,
            color=MARKET_COLOR,
            alpha=0.86,
            label="Mkt",
            zorder=3,
        )
    axes.plot(
        [point.ns for point in curve],
        [point.iv for point in curve],
        color=FIT_COLOR,
        linewidth=1.25,
        label="Fit",
        zorder=4,
    )
    if overlay_vendor and vendor:
        axes.plot(
            [point.ns for point in vendor],
            [point.iv for point in vendor],
            color="#6b7280",
            linewidth=1.0,
            linestyle=(0, (4, 3)),
            alpha=0.55,
            label="Vendor",
            zorder=2,
        )

    axes.set_xlim(*DEFAULT_X_LIMITS)
    axes.set_ylim(*DEFAULT_Y_LIMITS)
    axes.set_xticks([-10, -8, -6, -4, -2, 0, 2])
    axes.yaxis.set_major_locator(MultipleLocator(0.1))
    axes.xaxis.set_minor_locator(AutoMinorLocator(5))
    axes.yaxis.set_minor_locator(AutoMinorLocator(5))
    axes.tick_params(which="major", direction="in", top=True, right=True, length=5, width=0.9)
    axes.tick_params(which="minor", direction="in", top=True, right=True, length=2.5, width=0.7)
    axes.grid(which="major", color="#d8d8d8", linestyle=":", linewidth=0.45, alpha=0.52)
    axes.axvline(0.0, color="#4d4d4d", linewidth=0.9, zorder=1)
    axes.set_xlabel("NS", labelpad=18)
    expiry = data.meta.get("expiry", "").replace("-", "")
    axes.set_ylabel(f"Vol  T = {expiry}" if expiry else "Vol", labelpad=28)
    snapshot = data.meta.get("snapshot", "2019-08-26T19:30:00Z")
    maturity = _float_meta(data.meta, "T", 0.0678)
    observations = data.meta.get("observations", str(len(quotes)))
    axes.set_title(
        f"SPX {_historical_stamp(snapshot)}  {family}:  T={maturity:.4f}, n={observations}",
        fontsize=17,
        pad=16,
    )
    legend = axes.legend(
        loc="upper right",
        bbox_to_anchor=(0.98, 0.96),
        frameon=True,
        fancybox=False,
        borderpad=0.15,
        handlelength=2.8,
        labelspacing=0.15,
    )
    legend.get_frame().set_edgecolor(FIT_COLOR)
    legend.get_frame().set_linewidth(0.8)
    legend.get_frame().set_alpha(0.92)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(
        output_path,
        format="png",
        dpi=DPI,
        facecolor="white",
        metadata={"Software": "atx-vol SPX Wilmott renderer"},
    )


def render_reproduction(
    repro_path: pathlib.Path,
    output_path: pathlib.Path,
    *,
    metrics_path: pathlib.Path | None = None,
    vendor_path: pathlib.Path | None = None,
    family: str | None = None,
    overlay_vendor: bool = False,
) -> dict[str, object]:
    """Render a reproduction and write its deterministic metrics sidecar."""

    repro_path = pathlib.Path(repro_path)
    output_path = pathlib.Path(output_path)
    data = parse_repro(repro_path)
    selected = select_family(data, family)
    vendor = parse_vendor(pathlib.Path(vendor_path)) if vendor_path is not None else None
    _render_png(output_path, data, selected, vendor, overlay_vendor=overlay_vendor)

    quote_residuals = [quote.residual_iv for quote in data.quotes.get(selected, [])]
    sidecar: dict[str, object] = {
        "schema_version": 1,
        "selected_family": selected,
        "meta": data.meta,
        "fit": {
            "n_curve_points": len(data.curves[selected]),
            "n_quotes": len(quote_residuals),
            "quote_rmse_iv": (
                math.sqrt(statistics.fmean(error**2 for error in quote_residuals))
                if quote_residuals
                else None
            ),
            "quote_max_abs_iv": max(map(abs, quote_residuals)) if quote_residuals else None,
        },
        "inputs": {
            "repro_path": str(repro_path),
            "repro_sha256": hashlib.sha256(repro_path.read_bytes()).hexdigest(),
            "vendor_path": str(vendor_path) if vendor_path is not None else None,
            "vendor_sha256": (
                hashlib.sha256(pathlib.Path(vendor_path).read_bytes()).hexdigest()
                if vendor_path is not None
                else None
            ),
        },
        "vendor_comparison": (
            _vendor_comparison_with_support(data, data.curves[selected], vendor)
            if vendor
            else None
        ),
    }
    if metrics_path is None:
        metrics_path = output_path.with_suffix(".metrics.json")
    _write_json(pathlib.Path(metrics_path), sidecar)
    return sidecar


def _argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repro_csv", type=pathlib.Path, help="spx_wilmott_repro mixed CSV")
    parser.add_argument("output_png", type=pathlib.Path, help="destination 1318x1139 PNG")
    parser.add_argument("--metrics", type=pathlib.Path, help="metrics JSON destination")
    parser.add_argument("--vendor", type=pathlib.Path, help="digitized vendor fit TSV/CSV")
    parser.add_argument("--family", help="curve family (default: best quote/summary RMSE)")
    parser.add_argument(
        "--overlay-vendor",
        action="store_true",
        help="draw the digitized vendor curve as a faint dashed overlay",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _argument_parser().parse_args(argv)
    try:
        result = render_reproduction(
            args.repro_csv,
            args.output_png,
            metrics_path=args.metrics,
            vendor_path=args.vendor,
            family=args.family,
            overlay_vendor=args.overlay_vendor,
        )
    except (OSError, ValueError) as error:
        raise SystemExit(f"render failed: {error}") from error
    print(
        json.dumps(
            {
                "output_png": str(args.output_png),
                "selected_family": result["selected_family"],
                "vendor_comparison": result["vendor_comparison"],
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
