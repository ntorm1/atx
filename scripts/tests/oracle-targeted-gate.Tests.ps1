$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$scriptPath = Join-Path (Split-Path -Parent $here) 'oracle-targeted-gate.ps1'
. $scriptPath

# ── fixtures READ BACK from the definitions they mirror ──────────────────────
#
# Every expectation below used to be a bare literal restated here, and all three
# of them went stale at once (31 vs 57 bench cases, 18 vs 25 convention cases, 8
# vs 48 candidates) because nothing linked a number to the thing it described.
# These read the definition instead, and the cross-checks are deliberately
# ACROSS languages: the gtest ids come from the C++ that ctest actually runs and
# are compared to the PowerShell registry, and the candidate axes come from the
# PowerShell gate and are compared to the C++ grid. Neither side can be widened
# alone without failing here.

# The gtest ids a source file declares, as 'Suite.Case'.
function Get-GtestIdsFromSource([string]$RelativePath) {
  $source = [System.IO.File]::ReadAllText((Join-Path $script:OracleRepoRoot $RelativePath))
  $found = [regex]::Matches($source, '(?m)^TEST(?:_F)?\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)')
  if ($found.Count -eq 0) { throw ('no TEST() macros found in ' + $RelativePath) }
  return @($found | ForEach-Object { $_.Groups[1].Value + '.' + $_.Groups[2].Value })
}
$script:OracleBenchGtestIds = Get-GtestIdsFromSource 'atx-vol\tests\oracle_bench_test.cpp'
$script:OracleConventionGtestIds = Get-GtestIdsFromSource 'atx-vol\tests\oracle_conventions_test.cpp'

# One axis-domain array out of the gate's own `$expectedCandidateIds` block.
# Parsed rather than restated because the gate keeps these three arrays as
# function-local literals, so there is nothing to dot-source.
function Get-OracleGateAxisDomain([string]$Variable) {
  $source = [System.IO.File]::ReadAllText($scriptPath)
  $match = [regex]::Match($source, '(?m)^\s*\$' + $Variable + '\s*=\s*@\(([^)]*)\)')
  if (-not $match.Success) { throw ('scripts/oracle-targeted-gate.ps1 no longer defines $' + $Variable) }
  return @([regex]::Matches($match.Groups[1].Value, "'([a-z0-9_]+)'") | ForEach-Object { $_.Groups[1].Value })
}
$script:GateInputModels = Get-OracleGateAxisDomain 'candidateInputModels'
$script:GateExerciseStyles = Get-OracleGateAxisDomain 'candidateExerciseStyles'
$script:GateTimeDecayMethods = Get-OracleGateAxisDomain 'candidateTimeDecayMethods'
# Crossed in the gate's OWN nesting order (model -> style -> method), so the
# fixture is model-major exactly like a real receipt and the first
# $script:GateFinalistCount entries really are the tied fans of the top two
# input models rather than an arbitrary prefix.
$script:GateCandidateIds = @(foreach ($model in $script:GateInputModels) {
  foreach ($style in $script:GateExerciseStyles) {
    foreach ($method in $script:GateTimeDecayMethods) { $model + '|' + $style + '|' + $method }
  }
})
$script:GateFinalistCount = 2 * $script:GateExerciseStyles.Count * $script:GateTimeDecayMethods.Count

# The AUTHORITY for the grid: the three static_assert-guarded std::array sizes in
# atx-vol/tools/oracle_convention_sweep.cpp (kInputModels / kExerciseStyleRules /
# kTimeDecayMethods). The C++ static_asserts already pin each array to its enum,
# so the declared size is the enum cardinality.
function Get-SweepAxisSize([string]$Source, [string]$Type, [string]$Name) {
  $match = [regex]::Match($Source, 'std::array<\s*' + $Type + '\s*,\s*(\d+)\s*>\s*' + $Name + '\b')
  if (-not $match.Success) { throw ('atx-vol/tools/oracle_convention_sweep.cpp no longer declares ' + $Name) }
  return [int]$match.Groups[1].Value
}
$script:SweepSource = [System.IO.File]::ReadAllText((Join-Path $script:OracleRepoRoot 'atx-vol\tools\oracle_convention_sweep.cpp'))
$script:SweepInputModelCount = Get-SweepAxisSize $script:SweepSource 'InputModel' 'kInputModels'
$script:SweepExerciseStyleCount = Get-SweepAxisSize $script:SweepSource 'ExerciseStyleRule' 'kExerciseStyleRules'
$script:SweepTimeDecayMethodCount = Get-SweepAxisSize $script:SweepSource 'TimeDecayMethod' 'kTimeDecayMethods'

function New-ModeAScorecardJson([string]$Sha, [string]$OmitMetric = '') {
  $cells = [ordered]@{}
  foreach ($metric in @($script:ModeAMetricMap.Keys)) {
    if ($metric -eq $OmitMetric) { continue }
    $cells['a.' + $metric + '.atm.0-7.c'] = [ordered]@{
      n = 3; mae = 0.1; rmse = 0.2; p50 = 0.1; p95 = 0.2; p99 = 0.2; max = 0.2; within_tol_rate = 0.9
    }
  }
  return ([ordered]@{
    iter = 0; git_sha = $Sha; cohort = 'smoke'
    modes = [ordered]@{ a = [ordered]@{
      rows_total = 5; rows_priced = 3; rows_null_sentinel = 1; rows_bad_input = 0; rows_engine_error = 1
      wall_seconds = 0.5; rows_per_second = 6.0
    } }
    tolerances = [ordered]@{ price = 'price'; vol = 'vol'; greeks = 'greeks' }
    cells = $cells
  } | ConvertTo-Json -Depth 8)
}

function New-OracleBenchCtestLines([string[]]$TestIds) {
  $lines = @('Test project C:/atx/build')
  for ($index = 0; $index -lt $TestIds.Count; $index++) {
    $lines += (($index + 1).ToString() + '/' + $TestIds.Count + ' Test #' + (3400 + $index) + ': ' + $TestIds[$index] + ' ........ Passed 0.01 sec')
  }
  $lines += '100% tests passed, 0 tests failed out of ' + $TestIds.Count
  return $lines
}

function New-ConventionSweepJson([string]$Sha, [string]$ProductionDayCount = '', [long]$BaselineCountOverride = 0, [string]$RegressMetric = '', [string[]]$RegressedGreeks = @(), [string]$RegressSymmetricMetric = '', [double]$SymmetricRegressionFraction = 0.0, [switch]$OmitAcceptedRegression, [switch]$DropOneCandidate, [string]$SubstituteCandidateId = '') {
  $map = [ordered]@{
    input_model = 'discrete_forward_pv__rate__sdiv_yield'; forward_formula = 'uprc_exp_rate_t_minus_ddiv'; rate_model = 'continuous_row_rate'; carry_model = 'sdiv_as_yield'; dividend_model = 'discrete_cash_forward'; day_count = 'ACT_365_25'; dte_banding_day_count = 'ACT_365F'
    price_scale = 'per_share'; price_sign = 'positive'; vol_scale = 'decimal_identity'; delta_scale = 'per_unit'; delta_sign = 'positive'; gamma_scale = 'per_unit'; gamma_sign = 'positive'
    theta_basis = 'per_day'; theta_sign = 'positive'; vega_scale = 'per_point'; vega_sign = 'positive'; rho_scale = 'per_point'; rho_sign = 'positive'; phi_scale = 'per_point_squared'; phi_sign = 'positive'
    volga_source = 'volga'; volga_scale = 'per_point_squared'; volga_sign = 'positive'; vanna_source = 'vanna'; vanna_scale = 'per_point'; vanna_sign = 'positive'
    delta_decay_basis = 'per_day'; delta_decay_day_count = 'ACT_365_25'; delta_decay_sign = 'positive'
  }
  $metrics = @($script:ModeAMetricMap.Values | ForEach-Object { [ordered]@{ metric_id = [string]$_; value = 1.0; count = 100; selection_count = 90; unit = if ($_ -eq 'mode_a_price_mae') { 'ticks' } elseif ($_ -eq 'mode_a_vol_mae') { 'bp' } else { 'relative' } } })
  $baseline = @($metrics | ForEach-Object { [ordered]@{ metric_id = $_.metric_id; value = 2.0; count = 100; selection_count = 90; unit = $_.unit } })
  $deltas = @($metrics | ForEach-Object { [ordered]@{ metric_id = $_.metric_id; candidate = 1.0; baseline = 2.0; delta = -1.0; count = 100; unit = $_.unit } })
  # The symmetric-relative trio: same ids, same populations, its own objective.
  # It is the array the no-regression gate and the ratchet baseline run on.
  $symmetric = @($metrics | ForEach-Object { [ordered]@{ metric_id = $_.metric_id; value = 1.0; count = 100; selection_count = 90; unit = $_.unit } })
  $baselineSymmetric = @($metrics | ForEach-Object { [ordered]@{ metric_id = $_.metric_id; value = 2.0; count = 100; selection_count = 90; unit = $_.unit } })
  $symmetricDeltas = @($metrics | ForEach-Object { [ordered]@{ metric_id = $_.metric_id; candidate = 1.0; baseline = 2.0; delta = -1.0; count = 100; unit = $_.unit } })
  # The CLOSED three-axis registry the gate pins, read back out of the gate's own
  # axis literals (see the header block) instead of restated here as an id list
  # that silently narrowed the search the day an axis was added. The finalists
  # are the leading $script:GateFinalistCount entries, which in model-major order
  # are exactly the full tied fans of the top two input models.
  $ids = @($script:GateCandidateIds)
  if ($DropOneCandidate) { $ids = @($ids | Select-Object -First ($ids.Count - 1)) }
  $candidates = @(for ($i = 0; $i -lt $ids.Count; $i++) { [ordered]@{ candidate_id = $ids[$i]; smoke_price_mae_ticks = 1.0 + $i; smoke_count = 50; tune_sample_price_mae_ticks = if ($i -lt $script:GateFinalistCount) { 2.0 + $i } else { 0.0 }; tune_sample_count = if ($i -lt $script:GateFinalistCount) { 20 } else { 0 } } })
  if ($SubstituteCandidateId) { $candidates[0].candidate_id = $SubstituteCandidateId }
  if ($BaselineCountOverride -gt 0) { foreach ($metric in $baseline) { $metric.count = $BaselineCountOverride } }
  # A STANDARD-relative floor worse than its baseline. This is no longer a gate
  # failure: the reported array stays comparable to the charter target but is not
  # the regression criterion, so the gate must let this through.
  if ($RegressMetric) {
    foreach ($metric in $metrics) { if ($metric.metric_id -eq $RegressMetric) { $metric.value = 3.0 } }
    foreach ($delta in $deltas) { if ($delta.metric_id -eq $RegressMetric) { $delta.candidate = 3.0; $delta.delta = 1.0 } }
  }
  # A SYMMETRIC floor worse than its baseline: the BOUNDED no-regression gate.
  # $SymmetricRegressionFraction is the size of the regression as a FRACTION of
  # baseline, so 0.005 is half the 1% bound and 0.02 is twice it. Zero keeps the
  # legacy "far beyond the bound" shape (3.0 against a baseline of 2.0).
  # Every within-bound regression is published in accepted_regressions unless
  # -OmitAcceptedRegression asks for the receipt that hides one.
  $accepted = @()
  if ($RegressSymmetricMetric) {
    $baselineValue = 2.0
    $candidateValue = if ($SymmetricRegressionFraction -gt 0.0) { $baselineValue * (1.0 + $SymmetricRegressionFraction) } else { 3.0 }
    foreach ($metric in $symmetric) { if ($metric.metric_id -eq $RegressSymmetricMetric) { $metric.value = $candidateValue } }
    foreach ($delta in $symmetricDeltas) {
      if ($delta.metric_id -eq $RegressSymmetricMetric) { $delta.candidate = $candidateValue; $delta.delta = $candidateValue - $baselineValue }
    }
    if ($candidateValue -le ($baselineValue * 1.01) -and -not $OmitAcceptedRegression) {
      $accepted = @([ordered]@{
        metric_id = $RegressSymmetricMetric; candidate = $candidateValue; baseline = $baselineValue
        pct_of_baseline = ($candidateValue - $baselineValue) / $baselineValue
      })
    }
  }
  $production = [ordered]@{}
  foreach ($property in $map.GetEnumerator()) { $production[$property.Key] = $property.Value }
  if ($ProductionDayCount) { $production.day_count = $ProductionDayCount }
  return ([ordered]@{
    schema_version = 2; kind = 'convention_sweep'; git_sha = $Sha; cohorts = @('smoke', 'tune'); selection_strategy = 'all_smoke_then_top2_deterministic_tune_sample_then_full_attribution'
    smoke_rows = 40; tune_rows = 60; rows_priced = 100; engine_errors = 0; baseline_conventions = $map; conventions = $map; production_conventions = $production
    metrics = $metrics; baseline_metrics = $baseline; metric_deltas = $deltas
    symmetric_metrics = $symmetric; baseline_symmetric_metrics = $baselineSymmetric; symmetric_metric_deltas = $symmetricDeltas
    accepted_regressions = @($accepted)
    candidate_prices = $candidates
    input_model_regressed_greeks = @($RegressedGreeks); oracle_suspect_candidates = @(); market_evidence_status = 'not_evaluated_no_nbbo_gate'
    diagnostic_speed = [ordered]@{ preset = 'dev'; citable = $false; wall_seconds = 1.0; rows_per_second = 100.0 }
  } | ConvertTo-Json -Depth 20)
}

Describe 'oracle targeted gate production adapter' {
  It 'binds the targeted test gate to the real OracleBench CTest discovery in this worktree' {
    $script:captured = $null
    $result = Invoke-OracleTargetedGate 'mode_a_targeted_tests' {
      param($spec)
      $script:captured = $spec
      [pscustomobject]@{ ExitCode = 0; Lines = @(New-OracleBenchCtestLines @($spec.ExpectedTestIds)) }
    }
    $script:captured.Program | Should Be 'powershell'
    ($script:captured.Arguments -join ' ') | Should Match '-Ctest -R \^OracleBench\.\*\$ --no-tests=error'
    $script:captured.RequiredExecutables[0] | Should Be (Join-Path $script:OracleRepoRoot 'build\bin\atx-vol-tests.exe')
    ($script:captured.PrepareArguments -join ' ') | Should Match 'build atx-vol-tests atx-vol-oracle-bench --parallel 2$'
    $result.schema_version | Should Be 1
    $result.status | Should Be 'PASS'
    $result.command_id | Should Be 'mode_a_targeted_tests'
    $result.gate_kind | Should Be 'ctest'
    # The registry is pinned against the gtest SOURCE, not against itself: a
    # TEST(OracleBench*, ...) added, renamed, or deleted in
    # atx-vol/tests/oracle_bench_test.cpp without the same commit touching
    # $script:OracleBenchTestIds fails here, which is exactly the drift that left
    # this fixture asserting 31 against a 57-case suite.
    (Test-OracleExactStringSet @($script:captured.ExpectedTestIds) $script:OracleBenchGtestIds) | Should Be $true
    $script:captured.ExpectedTestIds.Count | Should Be $script:OracleBenchGtestIds.Count
    $result.tests_executed | Should Be $script:OracleBenchGtestIds.Count
    $result.tests_passed | Should Be $script:OracleBenchGtestIds.Count
    $result.tested_sha | Should Match '^[0-9a-f]{40}$'
    $result.tested_tree | Should Match '^[0-9a-f]{40}$'
    $result.raw_output_sha256 | Should Match '^[0-9a-f]{64}$'
  }

  It 'uses only the real worktree-local bench CLI and ignored aggregate output path' {
    $spec = Get-OracleTargetedGateSpec 'mode_a_smoke'
    $spec.Program | Should Be (Join-Path $script:OracleRepoRoot 'build\bin\atx-vol-oracle-bench.exe')
    ($spec.Arguments -join ' ') | Should Match '--cohort .+smoke\.json --store C:\\atx-cache\\oracle\\spiderrock --out .+build\\oracle-gates\\mode-a-smoke-[0-9a-f]{40}\.json --iter 0 --git-sha [0-9a-f]{40}'
    ($spec.Arguments -join ' ') | Should Not Match '--mode|--aggregate-only|--scorecard'
  }

  It 'fails closed on nonzero, zero tests, and a missing worktree executable' {
    { Invoke-OracleTargetedGate 'mode_a_smoke' { [pscustomobject]@{ ExitCode = 1; Lines = @('failed') } } } | Should Throw
    { Invoke-OracleTargetedGate 'mode_a_targeted_tests' { [pscustomobject]@{ ExitCode = 0; Lines = @('No tests were found!!!') } } } | Should Throw
    $missing = Join-Path $TestDrive 'missing.exe'
    { Assert-OracleGateExecutables ([pscustomobject]@{ RequiredExecutables = @($missing) }) } | Should Throw
  }

  It 'rejects both count drift and same-count test-name substitution' {
    # Exactly one short, derived rather than a literal 30 that only happens to
    # be under the real count: an off-by-one is the count drift most likely to
    # slip through, and it is the one a fixed prefix stops testing as the suite
    # grows.
    $tooFew = @($script:OracleBenchTestIds | Select-Object -First ($script:OracleBenchTestIds.Count - 1))
    { Invoke-OracleTargetedGate 'mode_a_targeted_tests' {
        [pscustomobject]@{ ExitCode = 0; Lines = @(New-OracleBenchCtestLines $tooFew) }
      } } | Should Throw
    $substituted = @($script:OracleBenchTestIds)
    $substituted[0] = 'OracleBenchBands.ReplacementThatMustNotPass'
    { Invoke-OracleTargetedGate 'mode_a_targeted_tests' {
        [pscustomobject]@{ ExitCode = 0; Lines = @(New-OracleBenchCtestLines $substituted) }
      } } | Should Throw
  }

  It 'captures ordinary native stderr without weakening nonzero exit handling' {
    $native = Invoke-OracleNativeProcess 'powershell' @('-NoProfile', '-Command', '[Console]::Error.WriteLine(123); exit 0')
    $native.ExitCode | Should Be 0
    ($native.Lines -join "`n") | Should Match '123'
    $failed = Invoke-OracleNativeProcess 'powershell' @('-NoProfile', '-Command', '[Console]::Error.WriteLine(456); exit 7')
    $failed.ExitCode | Should Be 7
  }

  It 'parses actual scorecard shape and emits all price vol and nine greek metric IDs' {
    $identity = Get-OracleGitIdentity
    $scorecard = New-ModeAScorecardJson $identity.Sha
    $result = Invoke-OracleTargetedGate 'mode_a_smoke' {
      [pscustomobject]@{
        ExitCode = 0
        Lines = @("oracle-bench: cohort 'smoke': 1 partition dir(s) opened, 3 row(s) admitted, 1 null-sentinel + 0 bad-input row(s) skipped", 'oracle-bench: mode a: 3 row(s) priced in 0.500 s (6 rows/s)')
        ScorecardJson = $scorecard
      }
    }
    $result.gate_kind | Should Be 'oracle_bench'
    $result.rows_processed | Should Be 3
    $result.metric_ids.Count | Should Be 11
    (Test-OracleExactStringSet $result.metric_ids (Get-OracleRequiredMetricIds 'mode_a_smoke')) | Should Be $true
    $result.audit_summary | Should Match 'status=PASS rows_processed=3 metric_ids='
    $result.raw_output_sha256 | Should Match '^[0-9a-f]{64}$'
  }

  It 'rejects an incomplete or identity-mismatched actual scorecard' {
    $identity = Get-OracleGitIdentity
    $incomplete = New-ModeAScorecardJson $identity.Sha 'ph'
    { Invoke-OracleTargetedGate 'mode_a_smoke' {
        [pscustomobject]@{ ExitCode = 0; Lines = @('oracle-bench evidence'); ScorecardJson = $incomplete }
      } } | Should Throw
    $wrongIdentity = New-ModeAScorecardJson ('f' * 40)
    { Invoke-OracleTargetedGate 'mode_a_smoke' {
        [pscustomobject]@{ ExitCode = 0; Lines = @('oracle-bench evidence'); ScorecardJson = $wrongIdentity }
      } } | Should Throw
  }

  It 'maps sprint unit gates to real fully-qualified discovered tests with zero-test failure enabled' {
    $american = Get-OracleTargetedGateSpec 'sprint_american_greeks_delta_put'
    ($american.Arguments -join ' ') | Should Match '-R \^AmericanGreeks.Delta_MatchesFd_Put\$ --no-tests=error'
    $adjusted = Get-OracleTargetedGateSpec 'sprint_adjusted_greeks_flat_smile'
    ($adjusted.Arguments -join ' ') | Should Match '-R \^AdjustedGreeks.FlatSmileLeavesDeltaUnchanged\$ --no-tests=error'
  }

  It 'binds Stage 3 to one isolated test target, one closed smoke+tune sweep, and cached exact-SHA floor verification' {
    $identity = Get-OracleGitIdentity
    $tests = Get-OracleTargetedGateSpec 'convention_tests' $identity
    ($tests.PrepareArguments -join ' ') | Should Match 'build atx-vol-oracle-convention-tests --parallel 2$'
    ($tests.Arguments -join ' ') | Should Match '-Ctest -R \^OracleConvention\\\. --no-tests=error'
    # Same source-of-truth pin as the OracleBench registry above.
    (Test-OracleExactStringSet @($tests.ExpectedTestIds) $script:OracleConventionGtestIds) | Should Be $true
    $tests.ExpectedTestIds.Count | Should Be $script:OracleConventionGtestIds.Count
    @($tests.ExpectedTestIds | Where-Object { $_ -notmatch '^OracleConvention\.[A-Za-z0-9_]+$' }).Count | Should Be 0
    $sweep = Get-OracleTargetedGateSpec 'mode_a_smoke_tune' $identity
    ($sweep.PrepareArguments -join ' ') | Should Match 'build atx-vol-oracle-bench --parallel 2$'
    ($sweep.Arguments -join ' ') | Should Match '--convention-sweep --smoke .+smoke\.json --tune .+tune\.json'
    ($sweep.Arguments -join ' ') | Should Not Match 'holdout'
    $floor = Get-OracleTargetedGateSpec 'residual_floor' $identity
    $floor.Kind | Should Be 'oracle_floor_verify'
    $floor.Program | Should Be ''
    $floor.RequiredExecutables.Count | Should Be 0
    $floor.OutputPath | Should Be $sweep.OutputPath
  }

  It 'emits all eleven convention floors, deltas, the complete map, and the whole bounded candidate grid' {
    $identity = Get-OracleGitIdentity
    $result = Invoke-OracleTargetedGate 'mode_a_smoke_tune' {
      [pscustomobject]@{ ExitCode = 0; Lines = @('closed convention sweep completed'); ScorecardJson = (New-ConventionSweepJson $identity.Sha) }
    }
    $result.gate_kind | Should Be 'oracle_convention'
    $result.rows_processed | Should Be 100
    $result.metrics.Count | Should Be 11
    $result.metric_deltas.Count | Should Be 11
    # The whole grid reaches the receipt: the gate must publish every candidate
    # it validated, not a truncated or de-duplicated prefix of them.
    $result.candidate_prices.Count | Should Be $script:GateCandidateIds.Count
    (Test-OracleExactStringSet @($result.candidate_prices.candidate_id) $script:GateCandidateIds) | Should Be $true
    @($result.candidate_prices | Where-Object { [long]$_.tune_sample_count -gt 0 }).Count | Should Be $script:GateFinalistCount
    (Test-OracleConventionMap $result.conventions) | Should Be $true
    (Test-OracleConventionMap $result.production_conventions) | Should Be $true
    $result.metrics[0].selection_count | Should Be 90
    @($result.input_model_regressed_greeks).Count | Should Be 0
  }

  It 'pins the gate candidate registry to the C++ sweep grid it must move with' {
    # The RECORDED TRAP the gate names at $expectedCandidateIds: that id set and
    # the kInputModels / kExerciseStyleRules / kTimeDecayMethods arrays in
    # atx-vol/tools/oracle_convention_sweep.cpp must move in ONE commit, or a
    # gate run spends a whole 12-minute sweep before failing on a registry
    # mismatch. Both sides are read from source, so widening either alone fails
    # here in milliseconds instead.
    $script:GateInputModels.Count | Should Be $script:SweepInputModelCount
    $script:GateExerciseStyles.Count | Should Be $script:SweepExerciseStyleCount
    $script:GateTimeDecayMethods.Count | Should Be $script:SweepTimeDecayMethodCount
    $script:GateCandidateIds.Count | Should Be ($script:SweepInputModelCount * $script:SweepExerciseStyleCount * $script:SweepTimeDecayMethodCount)
    # kFinalistCount = 2 * kTiedArmsPerInputModel: a whole multiple of one tied
    # block, never two candidates overall.
    $script:GateFinalistCount | Should Be (2 * $script:SweepExerciseStyleCount * $script:SweepTimeDecayMethodCount)
    ($script:GateFinalistCount % ($script:SweepExerciseStyleCount * $script:SweepTimeDecayMethodCount)) | Should Be 0
    # candidate_id is '<input_model>|<exercise_style>|<time_decay_method>' —
    # three fields, two separators — and every point of the grid is distinct.
    @($script:GateCandidateIds | Where-Object { @($_.Split('|')).Count -ne 3 }).Count | Should Be 0
    @($script:GateCandidateIds | Select-Object -Unique).Count | Should Be $script:GateCandidateIds.Count
  }

  It 'rejects candidate registry count drift and same-count id substitution' {
    # The happy-path fixture now DERIVES its id list from the gate, so the
    # gate's own registry check would go unexercised without these two. Same
    # pair the OracleBench registry gets: one short, and one the right length
    # with a member the grid does not contain.
    $identity = Get-OracleGitIdentity
    { Invoke-OracleTargetedGate 'mode_a_smoke_tune' {
        [pscustomobject]@{ ExitCode = 0; Lines = @('closed convention sweep completed'); ScorecardJson = (New-ConventionSweepJson $identity.Sha -DropOneCandidate) }
      } } | Should Throw
    { Invoke-OracleTargetedGate 'mode_a_smoke_tune' {
        [pscustomobject]@{ ExitCode = 0; Lines = @('closed convention sweep completed'); ScorecardJson = (New-ConventionSweepJson $identity.Sha -SubstituteCandidateId 'uprc_spot__rate__sdiv_yield|american_all|secant_504') }
      } } | Should Throw
  }

  It 'carries the selected input model greek regressions into the typed receipt' {
    $identity = Get-OracleGitIdentity
    $result = Invoke-OracleTargetedGate 'mode_a_smoke_tune' {
      [pscustomobject]@{ ExitCode = 0; Lines = @('closed convention sweep completed'); ScorecardJson = (New-ConventionSweepJson $identity.Sha '' 0 '' @('mode_a_phi_rel', 'mode_a_delta_decay_rel')) }
    }
    (@($result.input_model_regressed_greeks) -join ',') | Should Be 'mode_a_phi_rel,mode_a_delta_decay_rel'
    # Only the nine relative Greeks may appear, and never twice.
    { Invoke-OracleTargetedGate 'mode_a_smoke_tune' {
        [pscustomobject]@{ ExitCode = 0; Lines = @('closed convention sweep completed'); ScorecardJson = (New-ConventionSweepJson $identity.Sha '' 0 '' @('mode_a_price_mae')) }
      } } | Should Throw
    { Invoke-OracleTargetedGate 'mode_a_smoke_tune' {
        [pscustomobject]@{ ExitCode = 0; Lines = @('closed convention sweep completed'); ScorecardJson = (New-ConventionSweepJson $identity.Sha '' 0 '' @('mode_a_phi_rel', 'mode_a_phi_rel')) }
      } } | Should Throw
  }

  It 'fails closed when a symmetric metric regresses past the published bound' {
    $identity = Get-OracleGitIdentity
    # Equality must still pass: mode_a_vol_mae is structurally 0 on both arms.
    @(Get-OracleMetricRegressions @([pscustomobject]@{ metric_id = 'mode_a_vol_mae'; value = 0.0 }) @([pscustomobject]@{ metric_id = 'mode_a_vol_mae'; value = 0.0 })).Count | Should Be 0
    $offenders = @(Get-OracleMetricRegressions @([pscustomobject]@{ metric_id = 'mode_a_phi_rel'; value = 2.0 }) @([pscustomobject]@{ metric_id = 'mode_a_phi_rel'; value = 1.0 }))
    $offenders.Count | Should Be 1
    $offenders[0] | Should Match 'mode_a_phi_rel candidate=2 baseline=1'
    # Inside the bound is NOT an offender, and exactly ON the bound is inside it,
    # so the rule leaves no unreachable sliver.
    @(Get-OracleMetricRegressions @([pscustomobject]@{ metric_id = 'mode_a_phi_rel'; value = 1.005 }) @([pscustomobject]@{ metric_id = 'mode_a_phi_rel'; value = 1.0 })).Count | Should Be 0
    @(Get-OracleMetricRegressions @([pscustomobject]@{ metric_id = 'mode_a_phi_rel'; value = (1.0 * 1.01) }) @([pscustomobject]@{ metric_id = 'mode_a_phi_rel'; value = 1.0 })).Count | Should Be 0
    @(Get-OracleMetricRegressions @([pscustomobject]@{ metric_id = 'mode_a_phi_rel'; value = 1.02 }) @([pscustomobject]@{ metric_id = 'mode_a_phi_rel'; value = 1.0 })).Count | Should Be 1
    { Invoke-OracleTargetedGate 'mode_a_smoke_tune' {
        [pscustomobject]@{ ExitCode = 0; Lines = @('closed convention sweep completed'); ScorecardJson = (New-ConventionSweepJson $identity.Sha '' 0 '' @() 'mode_a_delta_decay_rel') }
      } } | Should Throw
    # 2% of baseline is twice the bound: still fails closed.
    { Invoke-OracleTargetedGate 'mode_a_smoke_tune' {
        [pscustomobject]@{ ExitCode = 0; Lines = @('closed convention sweep completed'); ScorecardJson = (New-ConventionSweepJson $identity.Sha '' 0 '' @() 'mode_a_delta_decay_rel' 0.02) }
      } } | Should Throw
  }

  It 'passes a within-bound symmetric regression and publishes it' {
    $identity = Get-OracleGitIdentity
    # 0.5% of baseline: inside the 1% bound, so the gate passes — and the
    # permitted loss is published rather than silently absorbed.
    $result = Invoke-OracleTargetedGate 'mode_a_smoke_tune' {
      [pscustomobject]@{ ExitCode = 0; Lines = @('closed convention sweep completed'); ScorecardJson = (New-ConventionSweepJson $identity.Sha '' 0 '' @() 'mode_a_vega_rel' 0.005) }
    }
    @($result.accepted_regressions).Count | Should Be 1
    $result.accepted_regressions[0].metric_id | Should Be 'mode_a_vega_rel'
    [double]$result.accepted_regressions[0].baseline | Should Be 2.0
    [double]$result.accepted_regressions[0].candidate | Should Be 2.01
    # A FRACTION of baseline, never a percentage.
    ([Math]::Abs([double]$result.accepted_regressions[0].pct_of_baseline - 0.005) -lt 1.0e-12) | Should Be $true
    # Nothing regressing means an empty array, never an absent key.
    $clean = Invoke-OracleTargetedGate 'mode_a_smoke_tune' {
      [pscustomobject]@{ ExitCode = 0; Lines = @('closed convention sweep completed'); ScorecardJson = (New-ConventionSweepJson $identity.Sha) }
    }
    @($clean.accepted_regressions).Count | Should Be 0
  }

  It 'fails closed when a within-bound regression is not published' {
    $identity = Get-OracleGitIdentity
    # The rubber-stamp failure mode: regresses inside the bound, publishes [].
    { Invoke-OracleTargetedGate 'mode_a_smoke_tune' {
        [pscustomobject]@{ ExitCode = 0; Lines = @('closed convention sweep completed'); ScorecardJson = (New-ConventionSweepJson $identity.Sha '' 0 '' @() 'mode_a_vega_rel' 0.005 -OmitAcceptedRegression) }
      } } | Should Throw
    # And the other direction: an entry for a metric that did not regress.
    $symmetric = @([pscustomobject]@{ metric_id = 'mode_a_vega_rel'; value = 1.0 })
    $baseline = @([pscustomobject]@{ metric_id = 'mode_a_vega_rel'; value = 2.0 })
    $forged = @([pscustomobject]@{ metric_id = 'mode_a_vega_rel'; candidate = 1.0; baseline = 2.0; pct_of_baseline = -0.5 })
    (Test-OracleAcceptedRegressions @() $symmetric $baseline) | Should Be ''
    (Test-OracleAcceptedRegressions $forged $symmetric $baseline) | Should Match 'not a within-bound symmetric regression'
    # A published entry whose numbers do not match the symmetric arrays.
    $regressed = @([pscustomobject]@{ metric_id = 'mode_a_vega_rel'; value = 2.01 })
    $wrongPct = @([pscustomobject]@{ metric_id = 'mode_a_vega_rel'; candidate = 2.01; baseline = 2.0; pct_of_baseline = 0.5 })
    (Test-OracleAcceptedRegressions $wrongPct $regressed $baseline) | Should Match 'pct_of_baseline'
    (Test-OracleAcceptedRegressions @() $regressed $baseline) | Should Match 'absent from accepted_regressions'
  }

  It 'publishes both floor arrays and gates only on the symmetric one' {
    $identity = Get-OracleGitIdentity
    # A STANDARD-relative metric worse than its baseline must NOT fail the gate:
    # that array pins its denominator on near-zero-oracle rows and would reward
    # the smaller multiplier, contradicting the selector it is meant to guard.
    $result = Invoke-OracleTargetedGate 'mode_a_smoke_tune' {
      [pscustomobject]@{ ExitCode = 0; Lines = @('closed convention sweep completed'); ScorecardJson = (New-ConventionSweepJson $identity.Sha '' 0 'mode_a_theta_rel') }
    }
    $result.symmetric_metrics.Count | Should Be 11
    $result.baseline_symmetric_metrics.Count | Should Be 11
    $result.symmetric_metric_deltas.Count | Should Be 11
    (Test-OracleExactStringSet @($result.symmetric_metrics.metric_id) @($result.metrics.metric_id)) | Should Be $true
    @($result.metrics | Where-Object { $_.metric_id -eq 'mode_a_theta_rel' })[0].value | Should Be 3.0
    @($result.symmetric_metrics | Where-Object { $_.metric_id -eq 'mode_a_theta_rel' })[0].value | Should Be 1.0
  }

  It 'fails closed when the production map differs from the resolved sweep winner' {
    $identity = Get-OracleGitIdentity
    { Invoke-OracleTargetedGate 'mode_a_smoke_tune' {
        [pscustomobject]@{ ExitCode = 0; Lines = @('closed convention sweep completed'); ScorecardJson = (New-ConventionSweepJson $identity.Sha 'ACT_360') }
      } } | Should Throw
  }

  It 'fails closed when the candidate and baseline floors describe different populations' {
    $identity = Get-OracleGitIdentity
    { Invoke-OracleTargetedGate 'mode_a_smoke_tune' {
        [pscustomobject]@{ ExitCode = 0; Lines = @('closed convention sweep completed'); ScorecardJson = (New-ConventionSweepJson $identity.Sha '' 99) }
      } } | Should Throw
  }

  It 'measures the rel-avx2 rate with no pin before iter-000 exists, and pins only afterwards' {
    $identity = Get-OracleGitIdentity
    $measure = Get-OracleTargetedGateSpec 'convention_speed_measure' $identity
    $measure.Kind | Should Be 'oracle_speed'
    $measure.ExpectedFloorPath | Should Be ''
    ($measure.PrepareArguments -join ' ') | Should Match '-Preset rel-avx2 build atx-vol-oracle-bench --parallel 2$'
    ($measure.Arguments -join ' ') | Should Not Match 'holdout'
    $pinned = Get-OracleTargetedGateSpec 'convention_speed' $identity
    $pinned.ExpectedFloorPath | Should Match 'iter-000\.json$'
    $measure.OutputPath | Should Not Be $pinned.OutputPath
    (Get-OracleRequiredMetricIds 'convention_speed_measure') | Should Be @('rel_avx2_rows_per_second')

    $scorecard = ([ordered]@{
      iter = 0; git_sha = $identity.Sha; cohort = 'tune'
      modes = [ordered]@{ a = [ordered]@{ rows_priced = 500; rows_per_second = 1200.0 } }
      tolerances = [ordered]@{ price = 'price' }; cells = [ordered]@{}
    } | ConvertTo-Json -Depth 8)
    $result = Invoke-OracleTargetedGate 'convention_speed_measure' {
      [pscustomobject]@{ ExitCode = 0; Lines = @('rel-avx2 tune measurement'); ScorecardJson = $scorecard }
    }
    $result.gate_kind | Should Be 'oracle_speed'
    $result.speed.value | Should Be 1200.0
    $result.speed.Contains('pin') | Should Be $false
  }
}
