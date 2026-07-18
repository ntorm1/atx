#!/usr/bin/env python3

import csv
import pathlib
import subprocess
import sys
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).parents[1] / "tools" / "build_spy_top50_universe.py"
NS = "http://www.sec.gov/edgar/nport"


def filing(registrant: str = "SPDR S&P 500 ETF TRUST") -> str:
    registrant = registrant.replace("&", "&amp;")
    holdings = []
    for index in range(50):
        holdings.append(
            f"<invstOrSec><name>Name {index:02d}</name><cusip>{index:09d}</cusip>"
            f"<pctVal>{50 - index}.0</pctVal><assetCat>EC</assetCat></invstOrSec>"
        )
    return (
        f'<edgarSubmission xmlns="{NS}"><regName>{registrant}</regName>'
        "<repPdDate>2025-12-31</repPdDate><invstOrSecs>"
        + "".join(holdings)
        + "</invstOrSecs></edgarSubmission>"
    )


class BuildSpyTop50UniverseTest(unittest.TestCase):
    def test_builds_strict_weight_ordered_universe(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            xml = root / "source.xml"
            mapping = root / "map.tsv"
            output = root / "universe.tsv"
            xml.write_text(filing(), encoding="utf-8")
            with mapping.open("w", newline="", encoding="ascii") as stream:
                writer = csv.writer(stream, delimiter="\t", lineterminator="\n")
                writer.writerow(["cusip", "name", "ticker"])
                for index in range(50):
                    writer.writerow([f"{index:09d}", f"Name {index:02d}", f"T{index:02d}"])
            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--xml",
                    str(xml),
                    "--symbol-map",
                    str(mapping),
                    "--out",
                    str(output),
                ],
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            with output.open(newline="", encoding="ascii") as stream:
                rows = list(csv.DictReader(stream, delimiter="\t"))
            self.assertEqual(len(rows), 50)
            self.assertEqual(rows[0]["symbol"], "T00")
            self.assertEqual(rows[-1]["symbol"], "T49")
            self.assertEqual(rows[0]["source"], "SEC_NPORT_0001410368-26-020131")
            self.assertEqual(rows[0]["as_of"], "2025-12-31")

    def test_index_symbol_prepends_index_leg(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            xml = root / "source.xml"
            mapping = root / "map.tsv"
            output = root / "universe.tsv"
            xml.write_text(filing(), encoding="utf-8")
            with mapping.open("w", newline="", encoding="ascii") as stream:
                writer = csv.writer(stream, delimiter="\t", lineterminator="\n")
                writer.writerow(["cusip", "name", "ticker"])
                for index in range(50):
                    writer.writerow([f"{index:09d}", f"Name {index:02d}", f"T{index:02d}"])
            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--xml",
                    str(xml),
                    "--symbol-map",
                    str(mapping),
                    "--out",
                    str(output),
                    "--index-symbol",
                    "SPY",
                ],
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            with output.open(newline="", encoding="ascii") as stream:
                rows = list(csv.DictReader(stream, delimiter="\t"))
            self.assertEqual(len(rows), 51)
            self.assertEqual(rows[0]["symbol"], "SPY")
            self.assertEqual(rows[0]["source"], "INDEX_ETF_SPDR_SPY")
            self.assertEqual(rows[0]["raw_weight"], "100.000000000000")
            self.assertEqual(rows[1]["symbol"], "T00")  # top constituent follows the leg
            self.assertEqual(rows[-1]["symbol"], "T49")

    def test_rejects_wrong_filing_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            xml = root / "source.xml"
            mapping = root / "map.tsv"
            xml.write_text(filing("Wrong Fund"), encoding="utf-8")
            mapping.write_text("cusip\tname\tticker\n", encoding="ascii")
            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--xml",
                    str(xml),
                    "--symbol-map",
                    str(mapping),
                    "--out",
                    str(root / "out.tsv"),
                ],
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("unexpected filing identity", result.stderr)


if __name__ == "__main__":
    unittest.main()
