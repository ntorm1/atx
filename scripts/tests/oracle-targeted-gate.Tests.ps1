$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$scriptPath = Join-Path (Split-Path -Parent $here) 'oracle-targeted-gate.ps1'
. $scriptPath

Describe 'oracle targeted gate production adapter' {
  It 'executes the production adapter path and binds ordinary output to typed JSON fields' {
    $script:captured = $null
    $result = Invoke-OracleTargetedGate 'mode_a_targeted_tests' {
      param($spec)
      $script:captured = $spec
      [pscustomobject]@{ ExitCode = 0; Lines = @('Test project C:/atx/build', '100% tests passed, 0 tests failed out of 2') }
    }
    $script:captured.Program | Should Be 'powershell'
    ($script:captured.Arguments -join ' ') | Should Match '-Ctest -R \^mode_a_targeted_tests\$ --no-tests=error'
    $result.schema_version | Should Be 1
    $result.status | Should Be 'PASS'
    $result.command_id | Should Be 'mode_a_targeted_tests'
    $result.gate_kind | Should Be 'ctest'
    $result.tests_executed | Should Be 2
    $result.tests_passed | Should Be 2
    $result.observations | Should Be 2
    $result.raw_output_sha256 | Should Match '^[0-9a-f]{64}$'
    ($result | ConvertTo-Json -Compress) | Should Match '"command_id":"mode_a_targeted_tests"'
  }

  It 'fails closed on a nonzero tool exit or empty evidence' {
    { Invoke-OracleTargetedGate 'mode_a_smoke' { [pscustomobject]@{ ExitCode = 1; Lines = @('failed') } } } | Should Throw
    { Invoke-OracleTargetedGate 'mode_b_smoke_tune' { [pscustomobject]@{ ExitCode = 0; Lines = @() } } } | Should Throw
  }

  It 'rejects exit-zero zero-work ctest and OracleBench output' {
    { Invoke-OracleTargetedGate 'mode_a_targeted_tests' { [pscustomobject]@{ ExitCode = 0; Lines = @('No tests were found!!!') } } } | Should Throw
    $empty = '{"status":"EMPTY","rows_processed":0,"metric_ids":[]}'
    { Invoke-OracleTargetedGate 'mode_a_smoke' { [pscustomobject]@{ ExitCode = 0; Lines = @($empty) } } } | Should Throw
  }

  It 'requires typed positive OracleBench rows and the complete metric set' {
    $typed = [ordered]@{
      status = 'PASS'; rows_processed = 120
      metric_ids = @('mode_a_price_mae', 'mode_a_vol_mae', 'mode_a_delta_rel', 'mode_a_gamma_rel', 'mode_a_theta_rel', 'mode_a_vega_rel')
    } | ConvertTo-Json -Compress
    $result = Invoke-OracleTargetedGate 'mode_a_smoke' { [pscustomobject]@{ ExitCode = 0; Lines = @($typed) } }
    $result.gate_kind | Should Be 'oracle_bench'
    $result.rows_processed | Should Be 120
    $result.metric_ids.Count | Should Be 6
  }

  It 'maps sprint unit gates to real fully-qualified discovered tests with zero-test failure enabled' {
    $american = Get-OracleTargetedGateSpec 'sprint_american_greeks_delta_put'
    ($american.Arguments -join ' ') | Should Match '-R \^AmericanGreeks.Delta_MatchesFd_Put\$ --no-tests=error'
    $adjusted = Get-OracleTargetedGateSpec 'sprint_adjusted_greeks_flat_smile'
    ($adjusted.Arguments -join ' ') | Should Match '-R \^AdjustedGreeks.FlatSmileLeavesDeltaUnchanged\$ --no-tests=error'
  }
}
