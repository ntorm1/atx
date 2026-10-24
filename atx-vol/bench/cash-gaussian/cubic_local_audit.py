"""Independent successor local-series checks; historical v1 receipt stays frozen."""
import argparse
from decimal import Decimal, localcontext
from fractions import Fraction
from functools import lru_cache
import hashlib
import json
import math
from pathlib import Path
import sys

import cubic_weights_audit as original

require = original.require
BASELINE_SHA = '70e635b6d7becba2a51d6e9ef71d4e3e7dd5e0b6ee3bf727e4302fefaf0f90bb'
ORIGINAL_READER_SHA = '808813614103ff6bfb199859214ad4bdd3413360d6762bd8b0d1dd652f3b1205'
DECLARATION = '2a078f20cd743367dfba0793c4632e7e30ca19fc'
PRIMITIVE = '8436a3b978a1dae98cb4c5ec988b07001bfffa5c'
LIMITS = dict(original.LIMITS, max_selection_checks=256, max_coefficient_products=262144,
              max_local_functions=64, max_workspace_bytes=1048576,
              max_log_order=16, max_exponential_order=16, coefficient_buffer_entries=514)
LOCAL_ARRAYS = dict(value_conditions=4, derivative_conditions=3,
                    value_remainders=4, derivative_remainders=3)
LOCAL_SCALARS = ('density_remainder', 'relative_value_remainder', 'relative_derivative_remainder')
WORK_COUNTS = ('selection_checks', 'coefficient_products', 'local_functions',
               'workspace_bytes', 'log_order', 'exponential_order')


@lru_cache(maxsize=1)
def pi_control():
    """Fixed rational arctangent series, independent of Boost's pi constant.

    DLMF4.24.3; tan(4*atan(1/5)-atan(1/239))=1 in (0,pi/2).
    Alternating truncation <1e-300, distinct from Decimal rounding error.
    """
    with localcontext() as context:
        context.prec = 270
        def atan_inverse(denominator, terms):
            x = Decimal(1)/denominator
            value = sum((-1)**k*x**(2*k+1)/Decimal(2*k+1) for k in range(terms))
            return value, x**(2*terms+1)/Decimal(2*terms+1)
        a, ea = atan_inverse(5, 224)
        b, eb = atan_inverse(239, 64)
        require(16*ea+4*eb < Decimal('1e-300'), 'bounded independent pi series')
        return 16*a-4*b


@lru_cache(maxsize=4096)
def integral(degree, j, power):
    """Exact Fraction integral: small Bernstein polynomial in w, parity explicit."""
    require(degree in (2, 3) and 0 <= j <= degree and 0 <= power <= 513,
            'bounded rational integral domain')
    coefficients = [0]*(degree+1)
    for left in range(j+1):
        for right in range(degree-j+1):
            coefficients[left+right] += (math.comb(j, left)*math.comb(degree-j, right)*
                                         (-1)**right)
    return Fraction(math.comb(degree, j), 2**degree)*sum(
        (Fraction(value, power+k+1) for k, value in enumerate(coefficients)
         if (power+k) % 2 == 0), Fraction(0))


def _multiply(left, right):
    result = [Decimal(0)]*(len(left)+len(right)-1)
    for i, a in enumerate(left):
        for j, b in enumerate(right):
            result[i+j] += a*b
    return result


def _conditions(coefficients, degree):
    output = []
    for j in range(degree+1):
        terms = []
        for k, coefficient in enumerate(coefficients):
            rational = integral(degree, j, k)
            terms.append(coefficient*Decimal(rational.numerator)/rational.denominator)
        value = sum(terms)
        require(value > 0, 'positive independently reconstructed integral')
        output.append(sum(map(abs, terms))/value)
    return output


def derive(model, digits):
    """250-digit formula/selection reconstruction; no native prices or weights."""
    return _derive(tuple(float(model[k]) for k in original.FIELDS), digits)


@lru_cache(maxsize=64)
def _derive(values, digits):
    require(digits in (100, 200), 'declared precision')
    with localcontext() as context:
        context.prec = 250
        lower, upper, stock, dt, sigma, rate, carry = map(Decimal.from_float, values)
        result = dict(route='original_moments', reason='none', J=0, N=0,
                      products=0, local_functions=0, selection_checks=0, used_log1p=False)
        if lower == 0:
            return dict(result, reason='zero_lower')
        width = upper-lower
        center = lower+width/2
        epsilon = width/(2*center)
        if epsilon > Decimal(1)/16:
            return dict(result, reason='wide_cell')
        variance = sigma*sigma*dt
        sd = variance.sqrt()
        result['used_log1p'] = abs((center-stock)/stock) <= Decimal(1)/2
        z = ((center/stock).ln()-(rate-carry-sigma*sigma/2)*dt)/sd
        a, b = 1+z/sd, 1/(2*variance)
        ell = -(1-epsilon).ln()
        c = abs(a)*ell+b*ell*ell
        result['local_functions'] = 3
        if c > Decimal(1)/8:
            return dict(result, reason='density_variation')
        tau = Decimal(10)**(20-digits)
        selected = None
        # Direct integer powers/factorials independently reconstruct native recurrences.
        for j in range(1, 17):
            log_error = epsilon**(j+1)/((j+1)*(1-epsilon))
            exponent_error = log_error*(abs(a)+b*(2*ell+log_error))
            bound = c+exponent_error
            for n in range(1, 17):
                rf = bound.exp()*(exponent_error+bound**(n+1)/math.factorial(n+1))
                relative_value = rf*c.exp()
                relative_derivative = relative_value*(1+epsilon)/(1-epsilon)
                if relative_value <= tau and relative_derivative <= tau:
                    selected = j, n, rf, relative_value, relative_derivative
                    break
            if selected:
                break
        if not selected:
            return dict(result, reason='order_cap', selection_checks=256, local_functions=20)
        j, n, rf, relative_value, relative_derivative = selected
        logarithm = [Decimal(0)]+[(-1)**(k+1)*epsilon**k/k for k in range(1, j+1)]
        square = _multiply(logarithm, logarithm)
        exponent = [-b*v for v in square]
        for k, v in enumerate(logarithm):
            exponent[k] -= a*v
        # Unlike the C++ recurrence, retain P**n without incremental factorial division.
        power, polynomial = [Decimal(1)], [Decimal(1)]+[Decimal(0)]*(2*j*n)
        for order in range(1, n+1):
            power = _multiply(power, exponent)
            for k, v in enumerate(power):
                polynomial[k] += v/math.factorial(order)
        derivative = _multiply(polynomial, [Decimal(1), epsilon])
        prefactor = (-rate*dt-z*z/2).exp()*width/(sd*center*(2*pi_control()).sqrt())
        degree = 2*j*n
        products = (2*j+2*j*j + sum((2*j*(order-1)+1)*(2*j+1)
                                   for order in range(1, n+1)) + 8*degree+18)
        return dict(result, route='local_series', J=j, N=n, products=products,
                    selection_checks=16*(j-1)+n, local_functions=6+j,
                    RF=rf, relative_value=relative_value, relative_derivative=relative_derivative,
                    value_bound=prefactor*rf/4,
                    derivative_bound=prefactor*(center/stock)*(1+epsilon)*rf/3,
                    value_conditions=_conditions(polynomial, 3),
                    derivative_conditions=_conditions(derivative, 2))


def verify(report, baseline_bytes):
    historical, baseline_rows = _historical(baseline_bytes)
    require(report['schema'] == 'cash-cubic-local-weight-control-v1' and
            report['declaration'] == DECLARATION and report['primitive_source'] == PRIMITIVE and
            report['limits'] == LIMITS, 'fixed successor declaration')
    require(isinstance(report['boost_version'], str) and bool(report['boost_version']),
            'reported Boost version')
    require(all(report[k] is False for k in ('timed', 'full_price_error_bound', 'semigroup_run')),
            'component-only scope')
    sizes = report['scalar_object_bytes']
    require(set(sizes) == {'100', '200'} and
            all(type(x) is int and 0 < x <= 256 for x in sizes.values()) and
            sizes['100'] < sizes['200'], 'bounded compiled scalar object sizes')
    rows = report['rows']
    expected = [(name, digits) for name, _ in original.declared() for digits in (100, 200)]
    require(len(rows) == 24 and [(r['id'], r['requested_digits']) for r in rows] == expected,
            'all24 successor precision/cell requests')
    models = {name: dict(zip(original.FIELDS, values)) for name, values in original.declared()}
    parsed, refused, routes = {}, [], []
    with localcontext() as context:
        context.prec = 250
        for row, baseline in zip(rows, baseline_rows, strict=True):
            model, digits = models[row['id']], row['requested_digits']
            require(type(digits) is int, 'integer requested precision')
            require(set(row['input']) == set(row['input_bits']) == set(original.FIELDS),
                    'input fields')
            for field, value in model.items():
                require(type(row['input'][field]) in (int, float) and
                        original.bits(row['input'][field]) == original.bits(value) and
                        isinstance(row['input_bits'][field], str) and
                        int(row['input_bits'][field]) == original.bits(value),
                        'exact declared input bits')
            derived = derive(model, digits)
            complete = row['status'] == 'success'
            _check_work(row['work'], derived, sizes[str(digits)], digits, complete)
            routes.append(dict(id=row['id'], digits=digits, work=row['work'], status=row['status']))
            if not complete:
                require(row['status'] == 'refused' and row['weights'] is None and
                        row['local'] is None and type(row['error_code']) is int and
                        row['error_code'] in (1, 2, 6) and
                        isinstance(row['error'], str) and bool(row['error']),
                        'typed refusal without partial weights')
                refused.append(dict(id=row['id'], digits=digits, error_code=row['error_code'],
                                    error=row['error'], work=row['work']))
                continue
            require(type(row['error_code']) is int and row['error_code'] == 0 and row['error'] == '',
                    'complete weight outcome')
            weights = row['weights']
            require(set(weights) == {'value', 'derivative', 'scaled_moments', 'mass',
                    'discounted_stock_ratio', 'maximum_condition'}, 'weight fields')
            arrays = {}
            for field, count in (('value', 4), ('derivative', 3), ('scaled_moments', 4)):
                require(isinstance(weights[field], list) and len(weights[field]) == count,
                        'weight denominator')
                arrays[field] = [original.positive_decimal(v) for v in weights[field]]
            mass = original.positive_decimal(weights['mass'])
            mean = original.positive_decimal(weights['discounted_stock_ratio'])
            condition = original.positive_decimal(weights['maximum_condition'])
            # Original raw moments remain bit-for-bit decimal diagnostics on both routes.
            for field in ('scaled_moments', 'mass', 'discounted_stock_ratio'):
                require(weights[field] == baseline['weights'][field], 'unchanged raw moment diagnostics')
            require(mass == arrays['scaled_moments'][0], 'zero moment identity')
            require(abs(sum(arrays['value'])-mass) <= mass*Decimal('1e-25') and
                    abs(sum(arrays['derivative'])-mean) <= mean*Decimal('1e-25'),
                    'partition-of-unity identities')
            local = row['local']
            require(set(local) == set(LOCAL_ARRAYS) | set(LOCAL_SCALARS), 'local metadata fields')
            for field, count in LOCAL_ARRAYS.items():
                require(isinstance(local[field], list) and len(local[field]) == count,
                        'local metadata denominator')
            if derived['route'] == 'original_moments':
                require(weights == baseline['weights'], 'labeled original route bit identity')
                require(all(local[key] == ['']*count for key, count in LOCAL_ARRAYS.items()) and
                        all(local[key] == '' for key in LOCAL_SCALARS),
                        'no series metadata invented on original route')
            else:
                conditions = []
                for field in ('value_conditions', 'derivative_conditions'):
                    for value, control in zip(local[field], derived[field], strict=True):
                        value = original.positive_decimal(value)
                        require(1 <= value <= 4, 'per-weight conditioning admission')
                        _agree(value, control, digits, 'independent condition reconstruction')
                        conditions.append(value)
                require(condition == max(conditions), 'all seven conditions join maximum')
                for field, control in (('density_remainder', derived['RF']),
                        ('relative_value_remainder', derived['relative_value']),
                        ('relative_derivative_remainder', derived['relative_derivative'])):
                    _agree(original.positive_decimal(local[field]), control, digits,
                           'independent truncation reconstruction')
                for field, control in (('value_remainders', derived['value_bound']),
                                       ('derivative_remainders', derived['derivative_bound'])):
                    for value in local[field]:
                        _agree(original.positive_decimal(value), control, digits,
                               'independent absolute remainder prefactor')
            parsed[row['id'], digits] = dict(**arrays, mass=mass, mean=mean, condition=condition)

        comparisons, precision_checks = [], []
        for name, _ in original.declared():
            if (name, 100) not in parsed or (name, 200) not in parsed:
                continue
            first, second = parsed[name, 100], parsed[name, 200]
            relative = [abs(a-b)/abs(b) for field in ('value', 'derivative', 'scaled_moments')
                        for a, b in zip(first[field], second[field], strict=True)]
            precision_checks.append(dict(id=name, maximum_relative_difference=str(max(relative)),
                                         passed=max(relative) <= Decimal('1e-40')))
            quadrature = original.independent_quadrature(models[name])
            for digits in (100, 200):
                native = parsed[name, digits]['value']+parsed[name, digits]['derivative']
                for index, (value, control) in enumerate(zip(native, quadrature['components'], strict=True)):
                    scaled = float(value)/quadrature['scale']
                    difference = abs(scaled-control['value'])
                    tolerance = 5e-11*abs(control['value'])+8*control['estimated_error']
                    comparisons.append(dict(id=name, digits=digits, component=index,
                        normalized_difference=difference, normalized_tolerance=tolerance,
                        quadrature=control, scale=quadrature['scale'], passed=difference <= tolerance))
    return dict(schema='cash-cubic-local-verification-v1', cell_requests=12, precision_outcomes=24,
        baseline_sha256=BASELINE_SHA, historical=historical, routes=routes, refused=refused,
        precision_checks=precision_checks, quadrature_checks=comparisons,
        controls_passed=not refused and len(precision_checks) == 12 and len(comparisons) == 168 and
            all(x['passed'] for x in precision_checks+comparisons),
        stage_a_complete=False, semigroup_run=False, full_price_error_bound=False,
        timing_qualified=False, python=sys.version, executable=sys.executable,
        scipy=original.scipy.__version__)


def _agree(value, control, digits, message):
    require(abs(value-control) <= abs(control)*Decimal(10)**(5-digits), message)


def _check_work(work, derived, size, digits, complete):
    require(set(work) == set(WORK_COUNTS) | {'moments', 'route', 'reason', 'used_log1p'} and
            all(type(work[k]) is int and work[k] >= 0 for k in WORK_COUNTS) and
            type(work['used_log1p']) is bool, 'typed local work fields')
    moments = work['moments']
    require(set(moments) == {'moment_attempts', 'normal_functions', 'polynomial_terms',
                            'decimal_digits'} and
            all(type(v) is int and v >= 0 for v in moments.values()), 'typed moment work fields')
    local_route = derived['route'] == 'local_series'
    expected_workspace = 0 if derived['reason'] == 'zero_lower' else size*(2312 if local_route else 256)
    exact = dict(route=derived['route'], reason=derived['reason'],
        selection_checks=derived['selection_checks'], coefficient_products=derived['products'],
        local_functions=derived['local_functions'], workspace_bytes=expected_workspace,
        log_order=derived['J'], exponential_order=derived['N'], used_log1p=derived['used_log1p'],
        moments=dict(moment_attempts=4, normal_functions=8,
                     polynomial_terms=0 if local_route else 28, decimal_digits=digits))
    if complete:
        require(work == exact, 'complete route and work identities')
        return
    # Failures retain bounded attempted work; no fallback after local selection.
    require(work['route'] in ('not_selected', derived['route']) and
            work['reason'] in ('none', derived['reason']), 'refused route consistent with declared model')
    if work['route'] == 'not_selected':
        require(work['reason'] == 'none' and work['log_order'] == work['exponential_order'] == 0,
                'no selected orders before selection')
    else:
        require(work['reason'] == derived['reason'] and
                work['log_order'] == derived['J'] and work['exponential_order'] == derived['N'],
                'retained selected route and orders')
    for field in ('selection_checks', 'coefficient_products', 'local_functions'):
        require(work[field] <= exact[field], 'bounded attempted local work')
    require(work['workspace_bytes'] in {0, 256*size, expected_workspace} and
            work['workspace_bytes'] <= LIMITS['max_workspace_bytes'] and
            (not work['used_log1p'] or derived['used_log1p']), 'bounded local reservation/branch')
    require(moments['decimal_digits'] in (0, digits) and moments['moment_attempts'] <= 4 and
            moments['normal_functions'] <= 2*moments['moment_attempts'] and
            moments['polynomial_terms'] <= (0 if local_route else 28) and
            moments['polynomial_terms'] % 4 == 0, 'bounded attempted moment work')


@lru_cache(maxsize=1)
def _historical(baseline_bytes):
    require(hashlib.sha256(baseline_bytes).hexdigest() == BASELINE_SHA, 'immutable original24 bytes')
    require(hashlib.sha256(Path(original.__file__).read_bytes()).hexdigest() == ORIGINAL_READER_SHA,
            'immutable original numerical reader')
    baseline = json.loads(baseline_bytes)
    result = original.verify(baseline)
    require([x['id'] for x in result['precision_checks'] if not x['passed']] ==
            ['adjacent-high', 'adjacent-low', 'adjacent-unit'] and
            len(result['quadrature_checks']) == 168 and
            all(x['passed'] for x in result['quadrature_checks']) and not result['refused'],
            'original negative control decisions unchanged')
    return dict(precision_outcomes=24, precision_checks=result['precision_checks'],
                quadrature_checks=168, quadrature_passed=168, controls_passed=False), baseline['rows']


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    require(not args.output.exists(), 'refuse existing verification output')
    data, baseline = args.report.read_bytes(), args.baseline.read_bytes()
    result = verify(json.loads(data), baseline)
    result.update(native_sha256=hashlib.sha256(data).hexdigest(),
                  reader_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  original_reader_sha256=hashlib.sha256(Path(original.__file__).read_bytes()).hexdigest())
    with args.output.open('x', encoding='utf-8') as output:
        output.write(json.dumps(result, indent=2, allow_nan=False)+'\n')
    print(json.dumps({k: v for k, v in result.items()
                      if k not in ('quadrature_checks', 'precision_checks', 'routes')}, indent=2))
