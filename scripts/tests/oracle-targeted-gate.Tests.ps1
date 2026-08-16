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
}
