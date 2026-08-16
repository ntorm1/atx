#Requires -Modules Pester

$scriptPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'lease-worktree.ps1'
. $scriptPath

Describe 'lease-worktree atomic publication' {
  It 'allows exactly one concurrent process to acquire one slot' {
    $poolRoot = Join-Path $TestDrive 'double-acquire'
    New-Item -ItemType Directory -Path (Join-Path $poolRoot 'pool-1') -Force | Out-Null
    $ownerStart = Get-UtcStamp (Get-Process -Id $PID).StartTime
    $ownerPidForJobs = $PID
    $jobBody = {
      param($Path, $Root, $Run, $DurablePid, $DurableStart)
      try {
        $output = & $Path -Branch 'test/atomic' -RunId $Run -Agent $Run -MaxPool 1 `
          -OwnerPid $DurablePid -OwnerProcessStartedUtc $DurableStart `
          -TestPoolRoot $Root -TestLeaseOnly -TestSelectionDelayMs 150
        [pscustomobject]@{ ok = $true; run_id = $Run; output = ($output -join "`n") }
      } catch {
        [pscustomobject]@{ ok = $false; run_id = $Run; output = $_.Exception.Message }
      }
    }
    $jobs = @(
      Start-Job -ScriptBlock $jobBody -ArgumentList $scriptPath, $poolRoot, 'run-a', $ownerPidForJobs, $ownerStart
      Start-Job -ScriptBlock $jobBody -ArgumentList $scriptPath, $poolRoot, 'run-b', $ownerPidForJobs, $ownerStart
    )
    try {
      $jobs | Wait-Job | Out-Null
      $results = @($jobs | Receive-Job)
    } finally {
      $jobs | Remove-Job -Force -ErrorAction SilentlyContinue
    }

    @($results | Where-Object { $_.ok }).Count | Should Be 1
    @($results | Where-Object { -not $_.ok }).Count | Should Be 1
    ($results | Where-Object { -not $_.ok }).output | Should Match 'pool exhausted|atomic lease publication lost'
    $record = Read-LeaseRecord (Join-Path $poolRoot 'pool-1\.atx-lease')
    Test-CompleteLeaseRecord $record | Should Be $true
    $record.version | Should Be '3'
    $record.owner_kind | Should Be 'process'
    Get-OwnerState $record $poolRoot | Should Be 'alive'
    @(Get-ChildItem (Join-Path $poolRoot 'pool-1') -Filter '.atx-lease.*.tmp').Count | Should Be 0

    & $scriptPath -Release 'pool-1' -RunId $record.run_id -TestPoolRoot $poolRoot -TestLeaseOnly | Out-Null
  }

  It 'fills the first missing numeric pool slot rather than count plus one' {
    $poolRoot = Join-Path $TestDrive 'numeric-gap'
    New-Item -ItemType Directory -Path (Join-Path $poolRoot 'pool-3') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $poolRoot 'pool-4') -Force | Out-Null
    Set-Content -Path (Join-Path $poolRoot 'pool-3\.atx-lease') -Value 'legacy=occupied'
    Set-Content -Path (Join-Path $poolRoot 'pool-4\.atx-lease') -Value 'legacy=occupied'

    $output = & $scriptPath -Branch 'test/gap' -RunId 'gap-run' -Agent 'pester' `
      -HeartbeatId 'gap-run-owner' -MaxPool 4 -TestPoolRoot $poolRoot -TestLeaseOnly
    ($output -join "`n") | Should Match 'pool=pool-1'
    Test-CompleteLeaseRecord (Read-LeaseRecord (Join-Path $poolRoot 'pool-1\.atx-lease')) | Should Be $true
    & $scriptPath -Release 'pool-1' -RunId 'gap-run' -TestPoolRoot $poolRoot -TestLeaseOnly | Out-Null
  }

  It 'reserves a registered missing path while selecting the first true physical gap under concurrency' {
    $poolRoot = Join-Path $TestDrive 'registered-missing-gap'
    $registeredPool = Join-Path $poolRoot 'pool-1'
    $ownerStart = Get-UtcStamp (Get-Process -Id $PID).StartTime
    $ownerPidForJobs = $PID
    $jobBody = {
      param($Path, $Root, $RegisteredPath, $Run, $DurablePid, $DurableStart)
      try {
        $output = & $Path -Branch 'test/registered-gap' -RunId $Run -Agent $Run -MaxPool 2 `
          -OwnerPid $DurablePid -OwnerProcessStartedUtc $DurableStart `
          -TestPoolRoot $Root -TestLeaseOnly -TestSelectionDelayMs 150 `
          -TestRegisteredWorktreePaths @($RegisteredPath)
        [pscustomobject]@{ ok = $true; run_id = $Run; output = ($output -join "`n") }
      } catch {
        [pscustomobject]@{ ok = $false; run_id = $Run; output = $_.Exception.Message }
      }
    }
    $jobs = @(
      Start-Job -ScriptBlock $jobBody -ArgumentList $scriptPath, $poolRoot, $registeredPool, 'gap-a', $ownerPidForJobs, $ownerStart
      Start-Job -ScriptBlock $jobBody -ArgumentList $scriptPath, $poolRoot, $registeredPool, 'gap-b', $ownerPidForJobs, $ownerStart
    )
    try {
      $jobs | Wait-Job | Out-Null
      $results = @($jobs | Receive-Job)
    } finally {
      $jobs | Remove-Job -Force -ErrorAction SilentlyContinue
    }

    @($results | Where-Object { $_.ok }).Count | Should Be 1
    @($results | Where-Object { -not $_.ok }).Count | Should Be 1
    ($results | Where-Object { $_.ok }).output | Should Match 'pool=pool-2'
    ($results | Where-Object { -not $_.ok }).output | Should Match 'pool exhausted'
    Test-Path -LiteralPath $registeredPool | Should Be $false
    $record = Read-LeaseRecord (Join-Path $poolRoot 'pool-2\.atx-lease')
    Test-CompleteLeaseRecord $record | Should Be $true
    & $scriptPath -Release 'pool-2' -RunId $record.run_id -TestPoolRoot $poolRoot -TestLeaseOnly | Out-Null
  }
}

Describe 'lease-worktree durable owner lifecycle' {
  It 'publishes the exact heartbeat identity after keeper readiness' {
    $poolRoot = Join-Path $TestDrive 'heartbeat-output'
    $output = & $scriptPath -Branch 'test/heartbeat-output' -RunId 'heartbeat-output-run' -Agent 'pester' `
      -HeartbeatId 'heartbeat-output-owner' -TestPoolRoot $poolRoot -TestLeaseOnly
    $record = Read-LeaseRecord (Join-Path $poolRoot 'pool-1\.atx-lease')
    ($output -join "`n") | Should Match ([regex]::Escape(
      (' keeper_ready_utc=' + $record.keeper_ready_utc + ' heartbeat_id=' + $record.heartbeat_id)))
    $record.heartbeat_id | Should Be 'heartbeat-output-owner'
    & $scriptPath -Release 'pool-1' -RunId 'heartbeat-output-run' `
      -TestPoolRoot $poolRoot -TestLeaseOnly | Out-Null
  }

  It 'keeps a lease live after the acquisition child exits, then permits reclaim after durable owner death' {
    $poolRoot = Join-Path $TestDrive 'durable-owner'
    New-Item -ItemType Directory -Path (Join-Path $poolRoot 'pool-1') -Force | Out-Null
    $owner = Start-Process powershell -ArgumentList @('-NoProfile', '-Command', 'Start-Sleep -Seconds 60') `
      -PassThru -WindowStyle Hidden
    try {
      $ownerStart = Get-UtcStamp $owner.StartTime
      $child = Start-Process powershell -ArgumentList @(
        '-NoProfile', '-File', $scriptPath,
        '-Branch', 'test/durable', '-RunId', 'durable-run', '-Agent', 'child-launcher',
        '-OwnerPid', [string]$owner.Id, '-OwnerProcessStartedUtc', $ownerStart,
        '-MaxPool', '1', '-TestPoolRoot', $poolRoot, '-TestLeaseOnly'
      ) -PassThru -Wait -WindowStyle Hidden
      $child.ExitCode | Should Be 0

      $record = Read-LeaseRecord (Join-Path $poolRoot 'pool-1\.atx-lease')
      [int]$record.owner_pid | Should Be $owner.Id
      Get-OwnerState $record $poolRoot | Should Be 'alive'
      {
        & $scriptPath -Release 'pool-1' -RunId 'reclaimer' -RecoverStale `
          -TestPoolRoot $poolRoot -TestLeaseOnly
      } | Should Throw 'owner state is alive'

      Stop-Process -Id $owner.Id -Force
      $owner.WaitForExit()
      Get-OwnerState $record $poolRoot | Should Be 'dead'
      & $scriptPath -Release 'pool-1' -RunId 'reclaimer' -RecoverStale `
        -TestPoolRoot $poolRoot -TestLeaseOnly | Out-Null
      Test-Path (Join-Path $poolRoot 'pool-1\.atx-lease') | Should Be $false
    } finally {
      if (-not $owner.HasExited) { Stop-Process -Id $owner.Id -Force -ErrorAction SilentlyContinue }
    }
  }

  It 'rejects silently using the short-lived launcher as production owner' {
    $poolRoot = Join-Path $TestDrive 'launcher-owner'
    $launcherStart = Get-UtcStamp (Get-Process -Id $PID).StartTime
    {
      & $scriptPath -Branch 'test/launcher' -RunId 'launcher-run' -Agent 'pester' `
        -OwnerPid $PID -OwnerProcessStartedUtc $launcherStart `
        -TestPoolRoot $poolRoot -TestLeaseOnly
    } | Should Throw 'short-lived lease launcher'
  }

  It 'keeps foreground work alive beyond timeout, then permits reclaim after keeper death' {
    $poolRoot = Join-Path $TestDrive 'heartbeat-owner'
    & $scriptPath -Branch 'test/heartbeat' -RunId 'heartbeat-run' -Agent 'pester' `
      -HeartbeatId 'heartbeat-run-lane' -HeartbeatTimeoutSeconds 2 `
      -TestPoolRoot $poolRoot -TestLeaseOnly | Out-Null
    $record = Read-LeaseRecord (Join-Path $poolRoot 'pool-1\.atx-lease')
    Get-OwnerState $record $poolRoot | Should Be 'alive'
    Test-ExactProcessAlive ([int]$record.keeper_pid) $record.keeper_process_started_utc | Should Be $true

    # Foreground work exceeds the configured timeout with no explicit pulse. The
    # independent keeper must continuously renew ownership.
    Start-Sleep -Seconds 3
    Get-OwnerState $record $poolRoot | Should Be 'alive'
    {
      & $scriptPath -Release 'pool-1' -RunId 'other-run' -RecoverStale `
        -TestPoolRoot $poolRoot -TestLeaseOnly
    } | Should Throw 'owner state is alive'

    (& $scriptPath -StopKeeper 'pool-1' -RunId 'heartbeat-run' `
      -TestPoolRoot $poolRoot -TestLeaseOnly) | Should Match '^KEEPER_STOPPED'
    Get-OwnerState $record $poolRoot | Should Be 'dead'
    & $scriptPath -Release 'pool-1' -RunId 'other-run' -RecoverStale `
      -TestPoolRoot $poolRoot -TestLeaseOnly | Out-Null
  }
}

Describe 'lease-worktree corruption and frozen-base guards' {
  It 'recovers a stale truncated final record but not a fresh one' {
    $poolRoot = Join-Path $TestDrive 'partial-record'
    $pool = Join-Path $poolRoot 'pool-1'
    New-Item -ItemType Directory -Path $pool -Force | Out-Null
    $leasePath = Join-Path $pool '.atx-lease'
    Set-Content -Path $leasePath -Encoding Ascii -Value @('version=3', 'lease_token=truncated')
    {
      & $scriptPath -Release 'pool-1' -RunId 'recovery-run' -RecoverStale `
        -CorruptRecordGraceSeconds 1 -TestPoolRoot $poolRoot -TestLeaseOnly
    } | Should Throw 'inside the grace period'

    [System.IO.File]::SetLastWriteTimeUtc($leasePath, [DateTime]::UtcNow.AddSeconds(-5))
    (& $scriptPath -Release 'pool-1' -RunId 'recovery-run' -RecoverStale `
      -CorruptRecordGraceSeconds 1 -TestPoolRoot $poolRoot -TestLeaseOnly) | Should Match '^RECOVERED_CORRUPT'
    Test-Path $leasePath | Should Be $false
  }

  It 'rejects an existing branch whose HEAD differs from the frozen base' {
    $poolRoot = Join-Path $TestDrive 'base-mismatch'
    {
      & $scriptPath -Branch 'test/reused' -RunId 'base-run' -Agent 'pester' `
        -HeartbeatId 'base-run-owner' -TestPoolRoot $poolRoot -TestLeaseOnly `
        -TestBaseSha ('0' * 40) -TestExistingBranchSha ('1' * 40)
    } | Should Throw 'use a run-unique branch'
    Test-Path (Join-Path $poolRoot 'pool-1\.atx-lease') | Should Be $false

    & $scriptPath -Branch 'test/reused' -RunId 'base-run' -Agent 'pester' `
      -HeartbeatId 'base-run-owner' -TestPoolRoot $poolRoot -TestLeaseOnly `
      -TestBaseSha ('0' * 40) -TestExistingBranchSha ('0' * 40) | Out-Null
    & $scriptPath -Release 'pool-1' -RunId 'base-run' `
      -TestPoolRoot $poolRoot -TestLeaseOnly | Out-Null
  }
}
