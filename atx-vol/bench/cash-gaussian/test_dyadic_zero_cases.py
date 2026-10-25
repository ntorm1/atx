"""Independent exact-rational unit controls; no pricing export or study matrix."""
from fractions import Fraction
from pathlib import Path
import math
import re
import struct
import unittest


def declared_cases():
    maximum = float.fromhex("0x1.fffffffffffffp+1023")
    tiny = float.fromhex("0x0.0000000000001p-1022")
    previous = math.nextafter(maximum, 0.)
    rows = [
        ("cubic", (0., 1., 0., 1., 0., 3.)),
        ("negative_cubic", (0., 1., 0., -1., 0., -3.)),
        ("translated", (1., 2., 0., 1., 0., 3.)),
        ("affine", (0., 1., 0., 1., 1., 1.)),
        ("constant", (0., 1., 2., 2., 0., 0.)),
        ("rounded_zero", (0., 1., 0., 1., 1e-300, 3.)),
        ("rounded_zero_swapped", (0., 1., 0., 1., 3., 1e-300)),
        ("subnormal_width", (0., tiny, 0., tiny, 0., 3.)),
    ]
    for name, a in (("adjacent_high", 1e12), ("adjacent_low", 1e-12), ("adjacent_unit", 1.)):
        b = math.nextafter(a, math.inf)
        rows.append((name, (a, b, a, b, 1., 1.)))
    rows += [
        ("maximum_zero", (0., maximum, 0., maximum, 3., 0.)),
        ("maximum_product", (0., maximum, -maximum, maximum, maximum, maximum)),
        ("signed_zero", (-0., 1., -0., 0., -0., 0.)),
        ("opposite_subnormals", (0., tiny, 0., 0., tiny, -tiny)),
        ("large_offset_zero", (0., 1., maximum, previous, -3*(maximum-previous), 0.)),
    ]
    return rows


def bits(value):
    return struct.unpack(">Q", struct.pack(">d", value))[0]


def exact_numerator(values):
    a, b, v0, v1, m0, m1 = map(Fraction.from_float, values)
    # Independent rational formula, without the native common-integer decoding.
    scaled = (3*(v1-v0)-(b-a)*(m0+m1)) * (1 << 2148)
    if scaled.denominator != 1:
        raise AssertionError("Binary64 identity must be integral at the declared scale")
    n = scaled.numerator
    if n == 0:
        return 0, 0, "0"
    magnitude = abs(n)
    trailing = (magnitude & -magnitude).bit_length()-1
    return (1 if n > 0 else -1), trailing, format(magnitude >> trailing, "x")


PATTERN = re.compile(r'DYADIC_CASE\("([a-z_]+)", ((?:0x[0-9a-f]{16}ULL, ){5}'
                     r'0x[0-9a-f]{16}ULL), (-?\d+), (\d+), "([0-9a-f]+)"\)')


def read_fixture():
    text = Path(__file__).with_name("dyadic_zero_cases.inc").read_text()
    records = []
    for line in text.splitlines():
        if line.startswith("//") or not line:
            continue
        match = PATTERN.fullmatch(line)
        if match is None:
            raise ValueError("Malformed compile-time exact-arithmetic fixture")
        name, operands, sign, trailing, odd = match.groups()
        records.append((name, tuple(int(s[2:-3], 16) for s in operands.split(", ")),
                        int(sign), int(trailing), odd))
    return records


class DyadicFixtureTests(unittest.TestCase):
    def test_all_sixteen_inputs_and_normalized_numerators_match_fraction(self):
        actual = read_fixture()
        expected = [(name, tuple(map(bits, values)), *exact_numerator(values))
                    for name, values in declared_cases()]
        self.assertEqual(len(actual), 16)
        self.assertEqual(actual, expected)

    def test_true_zero_and_rounded_zero_adversary_are_distinct(self):
        cases = dict(declared_cases())
        self.assertEqual(exact_numerator(cases["cubic"]), (0, 0, "0"))
        self.assertEqual(exact_numerator(cases["negative_cubic"]), (0, 0, "0"))
        self.assertEqual(exact_numerator(cases["rounded_zero"])[0], -1)
        self.assertEqual(exact_numerator(cases["rounded_zero_swapped"])[0], -1)

    def test_fixed_width_bound_and_signed_zero_identity(self):
        for _, values in declared_cases():
            sign, trailing, odd = exact_numerator(values)
            self.assertLessEqual(int(odd, 16).bit_length()+trailing, 4200)
            if sign == 0:
                self.assertEqual((trailing, odd), (0, "0"))
        signed = dict(declared_cases())["signed_zero"]
        self.assertEqual(bits(signed[0]), 1 << 63)
        self.assertEqual(bits(signed[2]), 1 << 63)
        self.assertEqual(bits(signed[4]), 1 << 63)


if __name__ == "__main__":
    unittest.main()
