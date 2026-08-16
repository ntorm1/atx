$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$scriptPath = Join-Path (Split-Path -Parent $here) 'oracle-targeted-gate.ps1'
. $scriptPath

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

function New-ConventionSweepJson([string]$Sha, [string]$ProductionDayCount = '', [long]$BaselineCountOverride = 0) {
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
  $ids = @('uprc_spot__rate__sdiv_yield', 'discrete_forward_pv__rate__sdiv_yield', 'discrete_forward_net_carry__rate__sdiv_yield', 'discrete_forward__rate__sdiv_yield', 'discrete_forward__rate_minus_sdiv__zero_carry', 'discrete_forward__zero_rate__zero_carry', 'discrete_forward_pv__rate_minus_sdiv__zero_carry', 'discrete_forward_pv__rate_plus_sdiv__zero_carry')
  $candidates = @(for ($i = 0; $i -lt $ids.Count; $i++) { [ordered]@{ candidate_id = $ids[$i]; smoke_price_mae_ticks = 1.0 + $i; smoke_count = 50; tune_sample_price_mae_ticks = if ($i -lt 2) { 2.0 + $i } else { 0.0 }; tune_sample_count = if ($i -lt 2) { 20 } else { 0 } } })
  if ($BaselineCountOverride -gt 0) { foreach ($metric in $baseline) { $metric.count = $BaselineCountOverride } }
  $production = [ordered]@{}
  foreach ($property in $map.GetEnumerator()) { $production[$property.Key] = $property.Value }
  if ($ProductionDayCount) { $production.day_count = $ProductionDayCount }
  return ([ordered]@{
    schema_version = 2; kind = 'convention_sweep'; git_sha = $Sha; cohorts = @('smoke', 'tune'); selection_strategy = 'all_smoke_then_top2_deterministic_tune_sample_then_full_attribution'
    smoke_rows = 40; tune_rows = 60; rows_priced = 100; engine_errors = 0; baseline_conventions = $map; conventions = $map; production_conventions = $production
    metrics = $metrics; baseline_metrics = $baseline; metric_deltas = $deltas; candidate_prices = $candidates; oracle_suspect_candidates = @(); market_evidence_status = 'not_evaluated_no_nbbo_gate'
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
    $script:captured.ExpectedTestIds.Count | Should Be 31
    $result.tests_executed | Should Be 31
    $result.tests_passed | Should Be 31
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
    $tooFew = @($script:OracleBenchTestIds | Select-Object -First 30)
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
    $tests.ExpectedTestIds.Count | Should Be 13
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

  It 'emits all eleven convention floors, deltas, the complete map, and eight bounded candidates' {
    $identity = Get-OracleGitIdentity
    $result = Invoke-OracleTargetedGate 'mode_a_smoke_tune' {
      [pscustomobject]@{ ExitCode = 0; Lines = @('closed convention sweep completed'); ScorecardJson = (New-ConventionSweepJson $identity.Sha) }
    }
    $result.gate_kind | Should Be 'oracle_convention'
    $result.rows_processed | Should Be 100
    $result.metrics.Count | Should Be 11
    $result.metric_deltas.Count | Should Be 11
    $result.candidate_prices.Count | Should Be 8
    (Test-OracleConventionMap $result.conventions) | Should Be $true
    (Test-OracleConventionMap $result.production_conventions) | Should Be $true
    $result.metrics[0].selection_count | Should Be 90
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
