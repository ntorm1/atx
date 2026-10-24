"""Independent positive-integrand checks for the fixed local-weight study only."""
import argparse
from decimal import Decimal, localcontext
import hashlib
import json
import math
from pathlib import Path
import struct
import sys

import scipy
from scipy.integrate import quad


FIELDS = ('lower', 'upper', 'stock', 'dt', 'sigma', 'rate', 'yield')
LIMITS = dict(max_moments=4, max_normal_functions=8,
              max_abs_standardized_endpoint=256, max_abs_log_prefactor=32768,
              max_series_iterations=4096, max_root_iterations=256)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def declared():
    nxt = lambda x: math.nextafter(x, math.inf)
    return [
        ('central', (80., 120., 100., .7, .35, .03, .01)),
        ('zero-lower', (0., 100., 80., .5, .2, -.03, .02)),
        ('adjacent-high', (1e12, nxt(1e12), 1e12, 1., .2, 0., 0.)),
        ('adjacent-low', (1e-12, nxt(1e-12), 1e-12, 1., .2, 0., 0.)),
        ('adjacent-unit', (1., nxt(1.), 1., 1., .2, 0., 0.)),
        ('lower-tail', (1., 2., 100., 1., .25, .01, .03)),
        ('upper-tail', (400., 450., 100., 1., .25, .01, .03)),
        ('positive-near-zero-time', (100., nxt(100.), 100., 1e-16, .2, 0., 0.)),
        ('negative-rate', (40., 180., 100., 3., .4, -.1, .07)),
        ('negative-yield', (70., 140., 100., 2., .5, .02, -.05)),
        ('small-variance', (99.99, 100.01, 100., .01, .001, 0., 0.)),
        ('wide', (1., 1000., 100., 5., 1., 0., 0.)),
    ]


def bits(value):
    return struct.unpack('<Q', struct.pack('<d', value))[0]


def positive_decimal(value):
    require(isinstance(value, str), 'decimal string required')
    parsed = Decimal(value)
    require(parsed.is_finite() and parsed > 0, 'positive finite decimal required')
    return parsed


def independent_quadrature(model):
    """Integrate nonnegative local bases directly, without raw moment subtraction."""
    a, b, stock, dt, sigma, rate, carry = (model[k] for k in FIELDS)
    sd = sigma*math.sqrt(dt)
    drift = (rate-carry-.5*sigma*sigma)*dt
    root_log = .5*math.log(2*math.pi)
    if a > 0:
        rho = (b-a)/a
        log_ratio = math.log1p((a-stock)/stock) if .5 < a/stock < 2 else math.log(a/stock)
        mode = math.expm1(-log_ratio+(rate-carry-1.5*sigma*sigma)*dt)/rho

        def log_density(u):
            change = math.log1p(rho*u)
            z = (log_ratio+change-drift)/sd
            return -rate*dt+math.log(rho)-math.log(sd)-change-.5*z*z-root_log

        ratio = lambda u: (a/stock)*(1+rho*u)
    else:
        mode = (stock/b)*math.exp((rate-carry-1.5*sigma*sigma)*dt)

        def log_density(u):
            if u == 0:
                return -math.inf
            z = (math.log(b/stock)+math.log(u)-drift)/sd
            return -rate*dt-math.log(sd)-math.log(u)-.5*z*z-root_log

        ratio = lambda u: b*u/stock
    peak = log_density(min(1., max(0., mode)))
    require(math.isfinite(peak), 'finite independent density scale')
    scale = math.exp(peak)
    require(math.isfinite(scale) and scale > 0, 'supported independent density scale')
    density = lambda u: math.exp(log_density(u)-peak)
    outputs = []
    for degree, multiplier in ((3, lambda u: 1.), (2, ratio)):
        for j in range(degree+1):
            function = lambda u: (density(u)*math.comb(degree, j)*u**j*
                                   (1-u)**(degree-j)*multiplier(u))
            result = quad(function, 0., 1., epsabs=1e-14, epsrel=3e-13,
                          limit=200, full_output=1)
            require(len(result) == 3, 'independent quadrature convergence')
            value, error, info = result
            require(math.isfinite(value) and value > 0 and math.isfinite(error) and error >= 0,
                    'positive finite quadrature result')
            outputs.append(dict(value=value, estimated_error=error, evaluations=info['neval']))
    return dict(scale=scale, log_scale=peak, components=outputs)


def verify(report):
    require(report['schema'] == 'cash-cubic-weight-control-v1' and
            report['declaration'] == '73dd008e' and report['limits'] == LIMITS,
            'fixed weight declaration')
    require(all(report[k] is False for k in ('timed', 'full_price_error_bound', 'semigroup_run')),
            'component-only scope')
    expected = [(name, digits) for name, _ in declared() for digits in (100, 200)]
    rows = report['rows']
    require(len(rows) == 24 and [(r['id'], r['requested_digits']) for r in rows] == expected,
            'all24 precision/cell requests')
    models = {name: dict(zip(FIELDS, values)) for name, values in declared()}
    parsed, refused = {}, []
    with localcontext() as context:
        context.prec = 250
        for row in rows:
            model, digits = models[row['id']], row['requested_digits']
            require(set(row['input']) == set(row['input_bits']) == set(FIELDS), 'input fields')
            for field, value in model.items():
                require(type(row['input'][field]) in (int, float) and
                        bits(row['input'][field]) == bits(value) and
                        isinstance(row['input_bits'][field], str) and
                        int(row['input_bits'][field]) == bits(value), 'exact declared input bits')
            work = row['work']
            require(set(work) == {'moment_attempts', 'normal_functions', 'polynomial_terms',
                                  'decimal_digits'} and
                    all(type(v) is int and v >= 0 for v in work.values()), 'work fields')
            require(work['decimal_digits'] == digits and work['moment_attempts'] <= 4 and
                    work['normal_functions'] <= 2*work['moment_attempts'] and
                    work['polynomial_terms'] <= 28 and work['polynomial_terms'] % 4 == 0,
                    'bounded actual work')
            if row['status'] == 'refused':
                require(row['weights'] is None and row['error_code'] in (1, 2, 6) and row['error'],
                        'typed refusal without partial weights')
                refused.append((row['id'], digits, row['error']))
                continue
            require(row['status'] == 'success' and row['error_code'] == 0 and row['error'] == '',
                    'complete weight outcome')
            require(work == dict(moment_attempts=4, normal_functions=8, polynomial_terms=28,
                                 decimal_digits=digits), 'complete work identity')
            weights = row['weights']
            require(set(weights) == {'value', 'derivative', 'scaled_moments', 'mass',
                                     'discounted_stock_ratio', 'maximum_condition'}, 'weight fields')
            arrays = {}
            for field, count in (('value', 4), ('derivative', 3), ('scaled_moments', 4)):
                require(len(weights[field]) == count, 'weight denominator')
                arrays[field] = [positive_decimal(v) for v in weights[field]]
            mass = positive_decimal(weights['mass'])
            mean = positive_decimal(weights['discounted_stock_ratio'])
            condition = positive_decimal(weights['maximum_condition'])
            require(Decimal(1) <= condition <= Decimal(10)**(digits-30), 'conditioning admission')
            require(mass == arrays['scaled_moments'][0], 'zero moment identity')
            ratio = Decimal.from_float(model['upper'])/Decimal.from_float(model['stock'])
            require(abs(mean-ratio*arrays['scaled_moments'][1]) <= mean*Decimal(10)**(5-digits),
                    'scaled first moment identity')
            require(abs(sum(arrays['value'])-mass) <= mass*Decimal('1e-25') and
                    abs(sum(arrays['derivative'])-mean) <= mean*Decimal('1e-25'),
                    'partition-of-unity identities')
            parsed[row['id'], digits] = dict(**arrays, mass=mass, mean=mean, condition=condition)

        comparisons, precision_checks = [], []
        for name, _ in declared():
            if (name, 100) not in parsed or (name, 200) not in parsed:
                continue
            first, second = parsed[name, 100], parsed[name, 200]
            relative = [abs(a-b)/abs(b) for field in ('value', 'derivative', 'scaled_moments')
                        for a, b in zip(first[field], second[field], strict=True)]
            precision_checks.append(dict(id=name, maximum_relative_difference=str(max(relative)),
                                         passed=max(relative) <= Decimal('1e-40')))
            quadrature = independent_quadrature(models[name])
            for digits in (100, 200):
                native = parsed[name, digits]['value']+parsed[name, digits]['derivative']
                for index, (value, control) in enumerate(zip(native, quadrature['components'], strict=True)):
                    scaled = float(value)/quadrature['scale']
                    difference = abs(scaled-control['value'])
                    tolerance = 5e-11*abs(control['value'])+8*control['estimated_error']
                    comparisons.append(dict(id=name, digits=digits, component=index,
                        normalized_difference=difference, normalized_tolerance=tolerance,
                        quadrature=control, scale=quadrature['scale'], passed=difference <= tolerance))
    return dict(schema='cash-cubic-weight-verification-v1', cell_requests=12, precision_outcomes=24,
        refused=refused, precision_checks=precision_checks, quadrature_checks=comparisons,
        controls_passed=not refused and len(precision_checks) == 12 and len(comparisons) == 168 and
            all(x['passed'] for x in precision_checks+comparisons),
        stage_a_complete=False, semigroup_run=False, full_price_error_bound=False,
        timing_qualified=False, python=sys.version, executable=sys.executable, scipy=scipy.__version__)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    require(not args.output.exists(), 'refuse existing verification output')
    data = args.report.read_bytes()
    result = verify(json.loads(data))
    result.update(native_sha256=hashlib.sha256(data).hexdigest(),
                  reader_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    args.output.write_text(json.dumps(result, indent=2, allow_nan=False)+'\n', encoding='utf-8')
    print(json.dumps({k: v for k, v in result.items()
                      if k not in ('quadrature_checks', 'precision_checks')}, indent=2))
