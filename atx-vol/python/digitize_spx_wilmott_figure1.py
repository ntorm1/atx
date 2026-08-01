#!/usr/bin/env python3
"""Digitize Vola Dynamics' rasterized Wilmott Profile Figure 1.

The source PDF does not contain the underlying numerical series.  This tool
therefore performs a deterministic pixel-space approximation: manually audited
tick anchors calibrate the axes, color predicates isolate the rendered purple
fit and green market marks, and fixed extraction rules turn those pixels into
TSV inputs.  The JSON sidecar records every material choice and the exact source
hash so downstream research cannot mistake the result for original market data.

Only the Python standard library is required.  The small PNG reader intentionally
supports just the non-interlaced, 8-bit truecolor forms needed by the source.
"""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import json
import math
import pathlib
import statistics
import struct
import zlib
from collections.abc import Iterable, Mapping, Sequence


SCHEMA_VERSION = 1
SOURCE_SHA256 = "edfab349342204eea1952a0ecb78dd00527839b09843451d55a61b6c0676cd4c"
SOURCE_DIMENSIONS = (1318, 1139)
SOURCE_DOCUMENT_URL = "https://voladynamics.com/pdf/VolaDynamics_WilmottProfile_Jan2020.pdf"

PLOT_LEFT = 151
PLOT_RIGHT = 1286
PLOT_TOP = 68
PLOT_BOTTOM = 1048
FIT_X_MIN = 201
FIT_X_MAX = 1285

X_ANCHORS: tuple[tuple[int, float], ...] = (
    (201, -10.0),
    (370, -8.0),
    (538, -6.0),
    (707, -4.0),
    (875, -2.0),
    (1044, 0.0),
    (1212, 2.0),
)
Y_ANCHORS: tuple[tuple[int, float], ...] = (
    (1048, 0.0),
    (875, 0.1),
    (701, 0.2),
    (528, 0.3),
    (354, 0.4),
    (181, 0.5),
)


@dataclasses.dataclass(frozen=True)
class RgbImage:
    """Owned row-major RGB raster."""

    width: int
    height: int
    pixels: bytes

    def rgb(self, x: int, y: int) -> tuple[int, int, int]:
        if not (0 <= x < self.width and 0 <= y < self.height):
            raise IndexError(f"pixel ({x}, {y}) outside {self.width}x{self.height} image")
        offset = 3 * (y * self.width + x)
        return (self.pixels[offset], self.pixels[offset + 1], self.pixels[offset + 2])


@dataclasses.dataclass(frozen=True)
class AffineCalibration:
    """Least-squares mapping from raster coordinates to an axis value."""

    anchors: tuple[tuple[int, float], ...]
    slope: float
    intercept: float
    max_anchor_residual: float

    @classmethod
    def from_anchors(cls, anchors: Iterable[tuple[int, float]]) -> "AffineCalibration":
        points = tuple(anchors)
        if len(points) < 2:
            raise ValueError("an affine calibration requires at least two anchors")
        mean_pixel = statistics.fmean(pixel for pixel, _ in points)
        mean_value = statistics.fmean(value for _, value in points)
        denominator = sum((pixel - mean_pixel) ** 2 for pixel, _ in points)
        if denominator == 0.0:
            raise ValueError("calibration pixel anchors must not all coincide")
        slope = sum(
            (pixel - mean_pixel) * (value - mean_value) for pixel, value in points
        ) / denominator
        intercept = mean_value - slope * mean_pixel
        residual = max(abs(slope * pixel + intercept - value) for pixel, value in points)
        return cls(points, slope, intercept, residual)

    def value(self, pixel: float) -> float:
        return self.slope * pixel + self.intercept


@dataclasses.dataclass(frozen=True)
class FitPixel:
    x: int
    y: float
    interpolated: bool


@dataclasses.dataclass(frozen=True)
class MarketPixel:
    x: int
    center_y: float
    top_y: int
    bottom_y: int


@dataclasses.dataclass(frozen=True)
class Digitization:
    fit: tuple[FitPixel, ...]
    market: tuple[MarketPixel, ...]
    x_calibration: AffineCalibration
    y_calibration: AffineCalibration
    purple_pixel_count: int
    green_pixel_count: int


def _paeth(a: int, b: int, c: int) -> int:
    estimate = a + b - c
    distance_a = abs(estimate - a)
    distance_b = abs(estimate - b)
    distance_c = abs(estimate - c)
    if distance_a <= distance_b and distance_a <= distance_c:
        return a
    if distance_b <= distance_c:
        return b
    return c


def _png_chunks(payload: bytes) -> Iterable[tuple[bytes, bytes]]:
    if payload[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("input is not a PNG file")
    offset = 8
    while offset < len(payload):
        if offset + 12 > len(payload):
            raise ValueError("truncated PNG chunk")
        size = struct.unpack_from(">I", payload, offset)[0]
        end = offset + 12 + size
        if end > len(payload):
            raise ValueError("truncated PNG chunk payload")
        kind = payload[offset + 4 : offset + 8]
        data = payload[offset + 8 : offset + 8 + size]
        expected_crc = struct.unpack_from(">I", payload, offset + 8 + size)[0]
        actual_crc = zlib.crc32(kind + data) & 0xFFFFFFFF
        if actual_crc != expected_crc:
            raise ValueError(f"PNG chunk {kind!r} has an invalid CRC")
        yield kind, data
        offset = end
        if kind == b"IEND":
            if offset != len(payload):
                raise ValueError("unexpected bytes after PNG IEND")
            return
    raise ValueError("PNG has no IEND chunk")


def _unfilter_scanlines(
    encoded: bytes, width: int, height: int, channels: int
) -> tuple[bytes, ...]:
    stride = width * channels
    expected_size = height * (stride + 1)
    if len(encoded) != expected_size:
        raise ValueError(
            f"PNG scanline payload has {len(encoded)} bytes; expected {expected_size}"
        )
    rows: list[bytes] = []
    offset = 0
    previous = bytes(stride)
    for _ in range(height):
        filter_kind = encoded[offset]
        source = encoded[offset + 1 : offset + 1 + stride]
        offset += stride + 1
        row = bytearray(stride)
        for i, value in enumerate(source):
            left = row[i - channels] if i >= channels else 0
            above = previous[i]
            upper_left = previous[i - channels] if i >= channels else 0
            if filter_kind == 0:
                predictor = 0
            elif filter_kind == 1:
                predictor = left
            elif filter_kind == 2:
                predictor = above
            elif filter_kind == 3:
                predictor = (left + above) // 2
            elif filter_kind == 4:
                predictor = _paeth(left, above, upper_left)
            else:
                raise ValueError(f"unsupported PNG row filter {filter_kind}")
            row[i] = (value + predictor) & 0xFF
        previous = bytes(row)
        rows.append(previous)
    return tuple(rows)


def read_png(path: pathlib.Path) -> RgbImage:
    """Read an 8-bit, non-interlaced RGB/RGBA PNG and discard any alpha."""

    payload = path.read_bytes()
    header: tuple[int, int, int] | None = None
    compressed = bytearray()
    for kind, data in _png_chunks(payload):
        if kind == b"IHDR":
            if len(data) != 13:
                raise ValueError("PNG IHDR has an invalid size")
            width, height, depth, color_type, compression, filtering, interlace = struct.unpack(
                ">IIBBBBB", data
            )
            if width == 0 or height == 0:
                raise ValueError("PNG dimensions must be positive")
            if depth != 8 or color_type not in (2, 6):
                raise ValueError("only 8-bit truecolor RGB/RGBA PNG files are supported")
            if compression != 0 or filtering != 0 or interlace != 0:
                raise ValueError("compressed/interlaced PNG variants are unsupported")
            header = (width, height, 3 if color_type == 2 else 4)
        elif kind == b"IDAT":
            compressed.extend(data)
    if header is None:
        raise ValueError("PNG has no IHDR chunk")
    width, height, channels = header
    try:
        encoded = zlib.decompress(bytes(compressed))
    except zlib.error as exc:
        raise ValueError(f"PNG IDAT stream cannot be decompressed: {exc}") from exc
    rows = _unfilter_scanlines(encoded, width, height, channels)
    if channels == 3:
        pixels = b"".join(rows)
    else:
        pixels = bytes(channel for row in rows for i, channel in enumerate(row) if i % 4 != 3)
    return RgbImage(width, height, pixels)


def _is_purple(rgb: tuple[int, int, int]) -> bool:
    red, green, blue = rgb
    return red - green >= 12 and blue - green >= 12


def _is_green(rgb: tuple[int, int, int]) -> bool:
    red, green, blue = rgb
    return green - red >= 20 and green - blue >= 10


def interpolate_centerline(
    observed: Mapping[int, float], x_min: int, x_max: int
) -> list[tuple[int, float, bool]]:
    """Fill bounded holes between observed centerline columns with straight lines."""

    if x_min > x_max:
        raise ValueError("centerline x range is inverted")
    known = sorted(x for x in observed if x_min <= x <= x_max)
    if not known or known[0] != x_min or known[-1] != x_max:
        raise ValueError("centerline observations must cover both requested endpoints")
    result: list[tuple[int, float, bool]] = []
    left_index = 0
    for x in range(x_min, x_max + 1):
        if x in observed:
            result.append((x, observed[x], False))
            continue
        while known[left_index + 1] < x:
            left_index += 1
        left_x = known[left_index]
        right_x = known[left_index + 1]
        weight = (x - left_x) / (right_x - left_x)
        value = observed[left_x] + weight * (observed[right_x] - observed[left_x])
        result.append((x, value, True))
    return result


def _fit_minimum_y(x: int) -> int:
    # Keeps the upper-left curve while excluding the purple legend rectangle.
    return max(PLOT_TOP + 1, round(55.0 + 0.5 * (x - 200)))


def _extract_fit(image: RgbImage) -> tuple[tuple[FitPixel, ...], int]:
    observed: dict[int, float] = {}
    matched_pixels = 0
    for x in range(FIT_X_MIN, FIT_X_MAX + 1):
        y_values = [
            y
            for y in range(_fit_minimum_y(x), PLOT_BOTTOM)
            if _is_purple(image.rgb(x, y))
        ]
        matched_pixels += len(y_values)
        if y_values:
            observed[x] = float(statistics.median(y_values))
    rows = interpolate_centerline(observed, FIT_X_MIN, FIT_X_MAX)
    return tuple(FitPixel(x, y, interpolated) for x, y, interpolated in rows), matched_pixels


def _joined_runs(values: Sequence[int], maximum_gap: int = 4) -> list[tuple[int, int]]:
    if not values:
        return []
    runs: list[tuple[int, int]] = []
    start = values[0]
    previous = values[0]
    for value in values[1:]:
        if value <= previous + maximum_gap + 1:
            previous = value
            continue
        runs.append((start, previous))
        start = value
        previous = value
    runs.append((start, previous))
    return runs


def _green_rows(image: RgbImage, x: int) -> list[int]:
    return [y for y in range(PLOT_TOP + 1, PLOT_BOTTOM) if _is_green(image.rgb(x, y))]


def _best_market_span(image: RgbImage, x: int, fit_y: float) -> tuple[int, int] | None:
    runs = _joined_runs(_green_rows(image, x))
    if not runs:
        return None
    containing_fit = [start_end for start_end in runs if start_end[0] - 2 <= fit_y <= start_end[1] + 2]
    candidates = containing_fit if containing_fit else runs
    return max(candidates, key=lambda start_end: start_end[1] - start_end[0])


def _row_strength(image: RgbImage, x: int, y: int) -> int:
    left = max(PLOT_LEFT + 1, x - 3)
    right = min(PLOT_RIGHT - 1, x + 3)
    return sum(_is_green(image.rgb(sample_x, y)) for sample_x in range(left, right + 1))


def _weighted_cluster_center(cluster: Sequence[tuple[int, int]]) -> tuple[float, int]:
    total_strength = sum(strength for _, strength in cluster)
    center = sum(y * strength for y, strength in cluster) / total_strength
    return center, total_strength


def _market_center(
    image: RgbImage, x: int, top_y: int, bottom_y: int, fit_y: float
) -> float:
    strong_rows = [
        (y, strength)
        for y in range(top_y, bottom_y + 1)
        if (strength := _row_strength(image, x, y)) >= 4
    ]
    clusters: list[list[tuple[int, int]]] = []
    for row in strong_rows:
        if not clusters or row[0] > clusters[-1][-1][0] + 1:
            clusters.append([row])
        else:
            clusters[-1].append(row)

    height = bottom_y - top_y
    margin = min(6, max(2, height // 6))
    interior = []
    for cluster in clusters:
        center, strength = _weighted_cluster_center(cluster)
        if top_y + margin < center < bottom_y - margin:
            interior.append((center, strength))
    if interior:
        center, _ = min(interior, key=lambda item: (abs(item[0] - fit_y), -item[1]))
        return center

    green = [y for y in range(top_y, bottom_y + 1) if _is_green(image.rgb(x, y))]
    return float(min(green, key=lambda y: abs(y - fit_y))) if green else fit_y


def _local_peak_indices(scores: Sequence[int], radius: int, minimum: int) -> list[int]:
    peaks: list[int] = []
    for index, score in enumerate(scores):
        if score < minimum:
            continue
        left = max(0, index - radius)
        right = min(len(scores), index + radius + 1)
        window = scores[left:right]
        if score == max(window) and index == left + window.index(score):
            peaks.append(index)
    return peaks


def _extract_market(
    image: RgbImage, fit: Sequence[FitPixel]
) -> tuple[tuple[MarketPixel, ...], int]:
    fit_by_x = {row.x: row.y for row in fit}
    spans = [
        _best_market_span(image, x, fit_by_x[x]) for x in range(FIT_X_MIN, FIT_X_MAX + 1)
    ]
    scores = [0 if span is None else span[1] - span[0] + 1 for span in spans]
    candidates = _local_peak_indices(scores, radius=2, minimum=5)

    rows: list[MarketPixel] = []
    for index in candidates:
        x = FIT_X_MIN + index
        span = spans[index]
        if span is None:
            continue
        top_y, bottom_y = span
        fit_y = fit_by_x[x]
        # Reject the short green legend sample; genuine far-from-fit outliers
        # have long error bars which still cross or approach the fitted curve.
        if bottom_y - top_y < 20 and not top_y - 5 <= fit_y <= bottom_y + 5:
            continue
        center_y = _market_center(image, x, top_y, bottom_y, fit_y)
        center_y = min(float(bottom_y), max(float(top_y), center_y))
        rows.append(MarketPixel(x, center_y, top_y, bottom_y))

    green_pixels = sum(
        _is_green(image.rgb(x, y))
        for y in range(PLOT_TOP + 1, PLOT_BOTTOM)
        for x in range(PLOT_LEFT + 1, PLOT_RIGHT)
    )
    return tuple(rows), green_pixels


def digitize(image: RgbImage) -> Digitization:
    """Extract Figure 1's fitted centerline and approximate market bands."""

    if (image.width, image.height) != SOURCE_DIMENSIONS:
        raise ValueError(
            f"source dimensions are {image.width}x{image.height}; expected "
            f"{SOURCE_DIMENSIONS[0]}x{SOURCE_DIMENSIONS[1]}"
        )
    fit, purple_pixels = _extract_fit(image)
    market, green_pixels = _extract_market(image, fit)
    return Digitization(
        fit=fit,
        market=market,
        x_calibration=AffineCalibration.from_anchors(X_ANCHORS),
        y_calibration=AffineCalibration.from_anchors(Y_ANCHORS),
        purple_pixel_count=purple_pixels,
        green_pixel_count=green_pixels,
    )


def _format_float(value: float) -> str:
    if not math.isfinite(value):
        raise ValueError("artifact contains a non-finite coordinate")
    return f"{value:.9f}"


def _render_fit_tsv(result: Digitization) -> bytes:
    lines = ["ns\tvol\tpixel_x\tpixel_y\tinterpolated"]
    for row in result.fit:
        lines.append(
            "\t".join(
                (
                    _format_float(result.x_calibration.value(row.x)),
                    _format_float(result.y_calibration.value(row.y)),
                    str(row.x),
                    f"{row.y:.6f}",
                    "1" if row.interpolated else "0",
                )
            )
        )
    return ("\n".join(lines) + "\n").encode("utf-8")


def _render_market_tsv(result: Digitization) -> bytes:
    lines = [
        "ns\tvol_center\tvol_low\tvol_high\tpixel_x\tpixel_center_y\tpixel_top_y\tpixel_bottom_y"
    ]
    for row in result.market:
        lines.append(
            "\t".join(
                (
                    _format_float(result.x_calibration.value(row.x)),
                    _format_float(result.y_calibration.value(row.center_y)),
                    _format_float(result.y_calibration.value(row.bottom_y)),
                    _format_float(result.y_calibration.value(row.top_y)),
                    str(row.x),
                    f"{row.center_y:.6f}",
                    str(row.top_y),
                    str(row.bottom_y),
                )
            )
        )
    return ("\n".join(lines) + "\n").encode("utf-8")


def _sha256(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def _axis_manifest(calibration: AffineCalibration) -> dict[str, object]:
    return {
        "anchors": [
            {"pixel": pixel, "value": value} for pixel, value in calibration.anchors
        ],
        "intercept": calibration.intercept,
        "max_anchor_residual": calibration.max_anchor_residual,
        "slope_per_pixel": calibration.slope,
    }


def _range(values: Iterable[float]) -> list[float]:
    materialized = tuple(values)
    return [min(materialized), max(materialized)]


def _nearest_rank(values: Sequence[float], probability: float) -> float:
    ordered = sorted(values)
    index = round(probability * (len(ordered) - 1))
    return ordered[index]


def _manifest(result: Digitization, fit_payload: bytes, market_payload: bytes) -> dict[str, object]:
    fit_ns = [result.x_calibration.value(row.x) for row in result.fit]
    fit_vol = [result.y_calibration.value(row.y) for row in result.fit]
    market_ns = [result.x_calibration.value(row.x) for row in result.market]
    market_vol = [result.y_calibration.value(row.center_y) for row in result.market]
    fit_y_by_x = {row.x: row.y for row in result.fit}
    center_fit_gaps = [
        abs(result.y_calibration.value(row.center_y) - result.y_calibration.value(fit_y_by_x[row.x]))
        for row in result.market
    ]
    bands_containing_fit = sum(
        row.top_y <= fit_y_by_x[row.x] <= row.bottom_y for row in result.market
    )
    return {
        "schema_version": SCHEMA_VERSION,
        "source": {
            "dimensions_px": list(SOURCE_DIMENSIONS),
            "document_url": SOURCE_DOCUMENT_URL,
            "figure": 1,
            "filename": "figure-01.png",
            "sha256": SOURCE_SHA256,
            "title": "SPX 20190826-153000 C13pm: T=0.0678, i=11, chi=0.057, avE5=1.1",
        },
        "coordinate_system": {
            "x": {"name": "normalized_strike", **_axis_manifest(result.x_calibration)},
            "y": {"name": "implied_volatility", **_axis_manifest(result.y_calibration)},
            "plot_bounds_px": {
                "bottom": PLOT_BOTTOM,
                "left": PLOT_LEFT,
                "right": PLOT_RIGHT,
                "top": PLOT_TOP,
            },
        },
        "extraction": {
            "fit": {
                "color_rule": "red-green >= 12 and blue-green >= 12",
                "columns_px": [FIT_X_MIN, FIT_X_MAX],
                "center": "median purple pixel per column; bounded gaps linearly interpolated",
            },
            "market": {
                "color_rule": "green-red >= 20 and green-blue >= 10",
                "band": "joined vertical green run at a local stem/marker peak",
                "center": "interior horizontal marker cluster nearest the fitted centerline",
                "maximum_joined_gap_px": 4,
                "peak_minimum_span_px": 5,
                "peak_radius_px": 2,
            },
        },
        "metrics": {
            "fit_interpolated_columns": sum(row.interpolated for row in result.fit),
            "fit_ns_range": _range(fit_ns),
            "fit_rows": len(result.fit),
            "fit_vol_range": _range(fit_vol),
            "green_mask_pixels": result.green_pixel_count,
            "market_band_contains_fit_rows": bands_containing_fit,
            "market_center_median_abs_fit_gap": statistics.median(center_fit_gaps),
            "market_center_p95_abs_fit_gap": _nearest_rank(center_fit_gaps, 0.95),
            "market_center_rmse_fit_gap": math.sqrt(
                statistics.fmean(gap * gap for gap in center_fit_gaps)
            ),
            "market_center_vol_range": _range(market_vol),
            "market_ns_range": _range(market_ns),
            "market_rows": len(result.market),
            "purple_mask_pixels": result.purple_pixel_count,
            "x_resolution_per_pixel": abs(result.x_calibration.slope),
            "y_resolution_per_pixel": abs(result.y_calibration.slope),
        },
        "outputs": [
            {
                "filename": "figure1_fit.tsv",
                "rows": len(result.fit),
                "schema": ["ns", "vol", "pixel_x", "pixel_y", "interpolated"],
                "sha256": _sha256(fit_payload),
            },
            {
                "filename": "figure1_market.tsv",
                "rows": len(result.market),
                "schema": [
                    "ns",
                    "vol_center",
                    "vol_low",
                    "vol_high",
                    "pixel_x",
                    "pixel_center_y",
                    "pixel_top_y",
                    "pixel_bottom_y",
                ],
                "sha256": _sha256(market_payload),
            },
        ],
        "limitations": [
            "The PNG is a raster rendering; coordinates are digitized approximations, not original quotes.",
            "A one-pixel uncertainty is about 0.01187 NS horizontally and 0.000577 volatility vertically.",
            "Dense overlapping green markers and error bars cannot always be separated into original observations.",
            "Extreme clipped error bars inherit the visible plot boundary, not an unobserved off-plot endpoint.",
            "Normalized strike cannot be converted back to strike without the source forward and ATF volatility.",
        ],
    }


def write_artifacts(result: Digitization, output_dir: pathlib.Path) -> tuple[pathlib.Path, ...]:
    output_dir.mkdir(parents=True, exist_ok=True)
    fit_payload = _render_fit_tsv(result)
    market_payload = _render_market_tsv(result)
    manifest = _manifest(result, fit_payload, market_payload)
    manifest_payload = (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8")

    fit_path = output_dir / "figure1_fit.tsv"
    market_path = output_dir / "figure1_market.tsv"
    manifest_path = output_dir / "figure1_digitization.json"
    fit_path.write_bytes(fit_payload)
    market_path.write_bytes(market_payload)
    manifest_path.write_bytes(manifest_payload)
    return fit_path, market_path, manifest_path


def _default_output_dir() -> pathlib.Path:
    return pathlib.Path(__file__).resolve().parents[1] / "tests" / "data" / "spx_wilmott_2019"


def _parse_args(argv: Sequence[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=pathlib.Path, help="exact extracted figure-01.png")
    parser.add_argument(
        "--output-dir",
        type=pathlib.Path,
        default=_default_output_dir(),
        help="artifact directory (default: atx-vol/tests/data/spx_wilmott_2019)",
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = _parse_args(argv)
    source_payload = args.source.read_bytes()
    source_hash = _sha256(source_payload)
    if source_hash != SOURCE_SHA256:
        raise ValueError(
            f"source SHA256 is {source_hash}; expected exact Figure 1 hash {SOURCE_SHA256}"
        )
    result = digitize(read_png(args.source))
    paths = write_artifacts(result, args.output_dir)
    print(
        f"digitized {len(result.fit)} fit columns and {len(result.market)} market marks; "
        f"interpolated {sum(row.interpolated for row in result.fit)} fit columns"
    )
    for path in paths:
        print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
