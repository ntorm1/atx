"""Determinism and provenance tests for the Wilmott Figure 1 digitizer."""

from __future__ import annotations

import csv
import hashlib
import importlib.util
import json
import pathlib
import struct
import sys
import tempfile
import unittest
import zlib


_PYTHON_ROOT = pathlib.Path(__file__).resolve().parents[1]
_REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
_TOOL = _PYTHON_ROOT / "digitize_spx_wilmott_figure1.py"
_DATA = _REPO_ROOT / "atx-vol" / "tests" / "data" / "spx_wilmott_2019"
_SOURCE = pathlib.Path(r"C:\atx\archive\research\vol\images\figures\figure-01.png")

_spec = importlib.util.spec_from_file_location("digitize_spx_wilmott_figure1", _TOOL)
if _spec is None or _spec.loader is None:
    raise RuntimeError(f"cannot load digitizer from {_TOOL}")
digitizer = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = digitizer
_spec.loader.exec_module(digitizer)


def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa = abs(p - a)
    pb = abs(p - b)
    pc = abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def _filtered_row(raw: bytes, previous: bytes, filter_kind: int) -> bytes:
    filtered = bytearray(len(raw))
    for i, value in enumerate(raw):
        left = raw[i - 3] if i >= 3 else 0
        above = previous[i] if previous else 0
        upper_left = previous[i - 3] if previous and i >= 3 else 0
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
            raise AssertionError("test generated an invalid PNG filter")
        filtered[i] = (value - predictor) & 0xFF
    return bytes(filtered)


def _chunk(kind: bytes, payload: bytes) -> bytes:
    body = kind + payload
    return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body))


def _write_rgb_png(path: pathlib.Path, rows: list[bytes]) -> None:
    width = len(rows[0]) // 3
    filtered = bytearray()
    previous = b""
    for filter_kind, row in enumerate(rows):
        filtered.append(filter_kind)
        filtered.extend(_filtered_row(row, previous, filter_kind))
        previous = row
    header = struct.pack(">IIBBBBB", width, len(rows), 8, 2, 0, 0, 0)
    png = b"\x89PNG\r\n\x1a\n" + _chunk(b"IHDR", header)
    png += _chunk(b"IDAT", zlib.compress(bytes(filtered), level=9)) + _chunk(b"IEND", b"")
    path.write_bytes(png)


class PngReaderTest(unittest.TestCase):
    def test_decodes_every_truecolor_row_filter(self) -> None:
        rows = [
            bytes(((17 * x + 31 * y + channel * 53) & 0xFF) for x in range(4) for channel in range(3))
            for y in range(5)
        ]
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "filters.png"
            _write_rgb_png(path, rows)
            image = digitizer.read_png(path)

        self.assertEqual((image.width, image.height), (4, 5))
        for y, row in enumerate(rows):
            for x in range(4):
                self.assertEqual(image.rgb(x, y), tuple(row[3 * x : 3 * x + 3]))


class CalibrationTest(unittest.TestCase):
    def test_least_squares_calibration_reproduces_tick_anchors(self) -> None:
        calibration = digitizer.AffineCalibration.from_anchors(
            ((10, -1.0), (20, 0.0), (30, 1.0), (40, 2.0))
        )

        self.assertAlmostEqual(calibration.value(25), 0.5, places=15)
        self.assertAlmostEqual(calibration.slope, 0.1, places=15)
        self.assertAlmostEqual(calibration.intercept, -2.0, places=15)
        self.assertLess(calibration.max_anchor_residual, 1.0e-14)

    def test_centerline_gaps_are_linear_and_flagged(self) -> None:
        rows = digitizer.interpolate_centerline({2: 10.0, 5: 16.0}, 2, 5)

        self.assertEqual(rows, [(2, 10.0, False), (3, 12.0, True), (4, 14.0, True), (5, 16.0, False)])


class GoldenArtifactsTest(unittest.TestCase):
    def test_committed_artifacts_are_self_consistent(self) -> None:
        manifest_path = _DATA / "figure1_digitization.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))

        self.assertEqual(manifest["schema_version"], 1)
        self.assertEqual(manifest["source"]["filename"], "figure-01.png")
        self.assertEqual(manifest["source"]["dimensions_px"], [1318, 1139])
        self.assertEqual(
            manifest["source"]["sha256"],
            "edfab349342204eea1952a0ecb78dd00527839b09843451d55a61b6c0676cd4c",
        )
        self.assertEqual(manifest["metrics"]["market_band_contains_fit_rows"], 157)
        self.assertAlmostEqual(
            manifest["metrics"]["market_center_median_abs_fit_gap"], 0.0014412767
        )
        self.assertLess(manifest["metrics"]["market_center_p95_abs_fit_gap"], 0.0075)

        for output in manifest["outputs"]:
            payload = (_DATA / output["filename"]).read_bytes()
            self.assertEqual(hashlib.sha256(payload).hexdigest(), output["sha256"])

        with (_DATA / "figure1_fit.tsv").open(newline="", encoding="utf-8") as stream:
            fit = list(csv.DictReader(stream, delimiter="\t"))
        with (_DATA / "figure1_market.tsv").open(newline="", encoding="utf-8") as stream:
            market = list(csv.DictReader(stream, delimiter="\t"))

        self.assertEqual(
            list(fit[0]), ["ns", "vol", "pixel_x", "pixel_y", "interpolated"]
        )
        self.assertEqual(
            list(market[0]),
            [
                "ns",
                "vol_center",
                "vol_low",
                "vol_high",
                "pixel_x",
                "pixel_center_y",
                "pixel_top_y",
                "pixel_bottom_y",
            ],
        )
        self.assertEqual(len(fit), 1085)
        self.assertGreaterEqual(len(market), 150)
        self.assertLessEqual(len(market), 180)
        self.assertEqual(sum(row["interpolated"] == "1" for row in fit), 107)

        fit_ns = [float(row["ns"]) for row in fit]
        fit_vol = [float(row["vol"]) for row in fit]
        self.assertTrue(all(a < b for a, b in zip(fit_ns, fit_ns[1:])))
        self.assertGreater(min(fit_vol), 0.11)
        self.assertLess(max(fit_vol), 0.57)

        market_ns = [float(row["ns"]) for row in market]
        self.assertTrue(all(a < b for a, b in zip(market_ns, market_ns[1:])))
        for row in market:
            low = float(row["vol_low"])
            center = float(row["vol_center"])
            high = float(row["vol_high"])
            self.assertLessEqual(low, center)
            self.assertLessEqual(center, high)

    @unittest.skipUnless(_SOURCE.exists(), "source research archive is not installed")
    def test_exact_source_regenerates_committed_tsv_bytes(self) -> None:
        source_payload = _SOURCE.read_bytes()
        self.assertEqual(hashlib.sha256(source_payload).hexdigest(), digitizer.SOURCE_SHA256)

        result = digitizer.digitize(digitizer.read_png(_SOURCE))

        self.assertEqual(digitizer._render_fit_tsv(result), (_DATA / "figure1_fit.tsv").read_bytes())
        self.assertEqual(
            digitizer._render_market_tsv(result), (_DATA / "figure1_market.tsv").read_bytes()
        )


if __name__ == "__main__":
    unittest.main()
