"""nlohmann::json dump(indent) emulation (default ordered-by-std::map objects)."""
import math
from decimal import Decimal

BS = chr(92)
QUOTE = chr(34)


def fmt_float(x):
    if math.isnan(x) or math.isinf(x):
        return 'null'
    if x == 0:
        return '-0.0' if math.copysign(1.0, x) < 0 else '0.0'
    sign = '-' if x < 0 else ''
    t = Decimal(repr(abs(x))).as_tuple()
    digits = ''.join(map(str, t.digits))
    exp = t.exponent
    stripped = digits.rstrip('0')
    exp += len(digits) - len(stripped)
    digits = stripped.lstrip('0') or '0'
    k = len(digits)
    n = k + exp
    min_exp, max_exp = -4, 15
    if k <= n <= max_exp:
        return sign + digits + '0' * (n - k) + '.0'
    if 0 < n <= max_exp:
        return sign + digits[:n] + '.' + digits[n:]
    if min_exp < n <= 0:
        return sign + '0.' + '0' * (-n) + digits
    e = n - 1
    mant = digits[0] + ('.' + digits[1:] if k > 1 else '')
    es = '-' if e < 0 else '+'
    ea = abs(e)
    return sign + mant + 'e' + es + (('0' + str(ea)) if ea < 10 else str(ea))


ESCAPES = {QUOTE: BS + QUOTE, BS: BS + BS, chr(8): BS + 'b', chr(12): BS + 'f',
           chr(10): BS + 'n', chr(13): BS + 'r', chr(9): BS + 't'}


def fmt_str(s):
    out = [QUOTE]
    for ch in s:
        if ch in ESCAPES:
            out.append(ESCAPES[ch])
        elif ord(ch) < 0x20:
            out.append(BS + 'u%04x' % ord(ch))
        else:
            out.append(ch)
    out.append(QUOTE)
    return ''.join(out)


def dump(v, indent=2, level=0):
    pad = ' ' * (indent * (level + 1))
    end = ' ' * (indent * level)
    if v is None:
        return 'null'
    if v is True:
        return 'true'
    if v is False:
        return 'false'
    if isinstance(v, int):
        return str(v)
    if isinstance(v, float):
        return fmt_float(v)
    if isinstance(v, str):
        return fmt_str(v)
    if isinstance(v, list):
        if not v:
            return '[]'
        return '[\n' + ',\n'.join(pad + dump(x, indent, level + 1) for x in v) + '\n' + end + ']'
    if isinstance(v, dict):
        if not v:
            return '{}'
        items = sorted(v.items(), key=lambda kv: kv[0].encode('utf-8'))
        return '{\n' + ',\n'.join(pad + fmt_str(k) + ': ' + dump(x, indent, level + 1)
                                  for k, x in items) + '\n' + end + '}'
    raise TypeError(type(v))


def dump_compact(v):
    if isinstance(v, list):
        return '[' + ','.join(dump_compact(x) for x in v) + ']'
    if isinstance(v, dict):
        items = sorted(v.items(), key=lambda kv: kv[0].encode('utf-8'))
        return '{' + ','.join(fmt_str(k) + ':' + dump_compact(x) for k, x in items) + '}'
    return dump(v)
