# Worktree pool lease manager. Windows PowerShell 5.1 compatible; ASCII only.
#
# Production leases require an explicit durable owner contract:
#   -OwnerPid <pid> -OwnerProcessStartedUtc <utc>, or -HeartbeatId <unique-id>.
# The short-lived PowerShell process running this script is never an implicit
# lease owner. Lease records are fully written to a unique temp file and then
# atomically published by rename; a truncated final record is fail-closed and
# can be explicitly recovered only after a corruption grace period.

param(
  [string]$Branch,
  [string]$Base = 'main',
  [string]$Agent = $env:USERNAME,
  [string]$RunId,
  [int]$OwnerPid = 0,
  [string]$OwnerProcessStartedUtc,
  [string]$HeartbeatId,
  [int]$HeartbeatTimeoutSeconds = 600,
  [string]$Pulse,
  [switch]$Shared,
  [string]$Release,
  [switch]$RecoverStale,
  [string]$RecoveryBaseSha,
  [switch]$Status,
  [int]$MaxPool = 20,
  [int]$CorruptRecordGraceSeconds = 30,
  [string]$TestPoolRoot,
  [switch]$TestLeaseOnly,
  [string]$TestBaseSha,
  [string]$TestExistingBranchSha,
  [int]$TestSelectionDelayMs = 0
)
$ErrorActionPreference = 'Stop'

function Get-UtcStamp([datetime]$Value) {
  return $Value.ToUniversalTime().ToString('o', [Globalization.CultureInfo]::InvariantCulture)
}

function Get-CurrentProcessStartUtc {
  return Get-UtcStamp (Get-Process -Id $PID -ErrorAction Stop).StartTime
}

function Assert-RecordValue([string]$Name, [string]$Value) {
  if ([string]::IsNullOrWhiteSpace($Value)) { throw ($Name + ' must not be empty') }
  if ($Value -match "[`r`n=]") { throw ($Name + ' contains an invalid character') }
}

function Assert-BranchHeadMatchesBase([string]$ExistingSha, [string]$BaseSha) {
  if ($ExistingSha -ne $BaseSha) {
    throw ('existing branch HEAD ' + $ExistingSha + ' does not equal frozen base ' +
      $BaseSha + '; use a run-unique branch')
  }
}

function New-ProcessOwnerFields(
  [int]$DurablePid,
  [string]$DurableStartedUtc,
  [switch]$AllowLauncher
) {
  if ($DurablePid -le 0) { throw 'OwnerPid must identify a durable process' }
  Assert-RecordValue 'OwnerProcessStartedUtc' $DurableStartedUtc
  if (-not $AllowLauncher -and $DurablePid -eq $PID) {
    throw 'OwnerPid is the short-lived lease launcher; supply a durable owner or HeartbeatId'
  }
  try {
    $process = Get-Process -Id $DurablePid -ErrorAction Stop
  } catch {
    throw ('owner process is not alive: ' + $DurablePid)
  }
  $actualStart = Get-UtcStamp $process.StartTime
  if ($actualStart -ne $DurableStartedUtc) {
    throw ('owner PID/start mismatch for PID ' + $DurablePid)
  }
  return [ordered]@{
    owner_kind = 'process'
    owner_host = [Environment]::MachineName
    owner_pid = [string]$DurablePid
    owner_process_started_utc = $DurableStartedUtc
  }
}

function Assert-HeartbeatId([string]$Id) {
  if ($Id -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$') {
    throw 'HeartbeatId must be 1-128 safe filename characters'
  }
}

function Get-HeartbeatPath([string]$PoolRoot, [string]$Id) {
  Assert-HeartbeatId $Id
  return Join-Path (Join-Path $PoolRoot '.atx-heartbeats') ($Id + '.heartbeat')
}

function New-HeartbeatOwnerFields([string]$Id, [int]$TimeoutSeconds) {
  Assert-HeartbeatId $Id
  if ($TimeoutSeconds -lt 30) { throw 'HeartbeatTimeoutSeconds must be at least 30' }
  return [ordered]@{
    owner_kind = 'heartbeat'
    heartbeat_id = $Id
    heartbeat_timeout_seconds = [string]$TimeoutSeconds
  }
}

function New-LeaseFields(
  [string]$LeaseRunId,
  [string]$LeaseAgent,
  [string]$LeaseBranch,
  [string]$BaseRef,
  [string]$BaseSha,
  $OwnerFields
) {
  Assert-RecordValue 'RunId' $LeaseRunId
  Assert-RecordValue 'Agent' $LeaseAgent
  Assert-RecordValue 'Branch' $LeaseBranch
  Assert-RecordValue 'Base' $BaseRef
  Assert-RecordValue 'BaseSha' $BaseSha
  $fields = [ordered]@{
    version = '3'
    lease_token = [guid]::NewGuid().ToString('N')
    run_id = $LeaseRunId
    agent = $LeaseAgent
    branch = $LeaseBranch
    base_ref = $BaseRef
    base_sha = $BaseSha
    acquired_utc = Get-UtcStamp (Get-Date)
  }
  foreach ($entry in $OwnerFields.GetEnumerator()) { $fields[$entry.Key] = $entry.Value }
  return $fields
}

function ConvertTo-RecordLines($Fields) {
  $lines = New-Object System.Collections.Generic.List[string]
  foreach ($entry in $Fields.GetEnumerator()) {
    Assert-RecordValue ([string]$entry.Key) ([string]$entry.Value)
    $lines.Add(([string]$entry.Key + '=' + [string]$entry.Value))
  }
  return $lines.ToArray()
}

function Write-CompleteRecord([string]$Path, $Fields) {
  $stream = $null
  $writer = $null
  try {
    $stream = New-Object System.IO.FileStream(
      $Path,
      [System.IO.FileMode]::CreateNew,
      [System.IO.FileAccess]::Write,
      [System.IO.FileShare]::None
    )
    $writer = New-Object System.IO.StreamWriter($stream, (New-Object System.Text.ASCIIEncoding))
    foreach ($line in (ConvertTo-RecordLines $Fields)) { $writer.WriteLine($line) }
    $writer.Flush()
    $stream.Flush()
  } finally {
    if ($writer) { $writer.Dispose() }
    elseif ($stream) { $stream.Dispose() }
  }
}

function Publish-AtomicRecord([string]$Path, $Fields) {
  $temp = $Path + '.' + $Fields.lease_token + '.tmp'
  try {
    Write-CompleteRecord $temp $Fields
    # Same-volume rename publishes a complete record atomically and refuses to
    # overwrite an existing lease won by another process.
    [System.IO.File]::Move($temp, $Path)
    return $true
  } catch [System.IO.IOException] {
    return $false
  } finally {
    if ([System.IO.File]::Exists($temp)) {
      try { [System.IO.File]::Delete($temp) } catch { }
    }
  }
}

function Try-NewClaimRecord([string]$Path, $Fields) {
  try {
    Write-CompleteRecord $Path $Fields
    return $true
  } catch [System.IO.IOException] {
    return $false
  }
}

function Read-LeaseRecord([string]$Path) {
  if (-not [System.IO.File]::Exists($Path)) { return $null }
  $record = @{}
  foreach ($line in [System.IO.File]::ReadAllLines($Path)) {
    $split = $line.IndexOf('=')
    if ($split -gt 0) {
      $record[$line.Substring(0, $split)] = $line.Substring($split + 1)
    }
  }
  return $record
}

function Test-CompleteLeaseRecord($Record) {
  if (-not $Record -or $Record.version -ne '3') { return $false }
  foreach ($field in @('lease_token', 'run_id', 'agent', 'branch', 'base_sha', 'acquired_utc', 'owner_kind')) {
    if ([string]::IsNullOrWhiteSpace([string]$Record[$field])) { return $false }
  }
  if ($Record.base_sha -notmatch '^[0-9a-fA-F]{40}$') { return $false }
  if ($Record.owner_kind -eq 'process') {
    return -not [string]::IsNullOrWhiteSpace([string]$Record.owner_host) -and
      -not [string]::IsNullOrWhiteSpace([string]$Record.owner_pid) -and
      -not [string]::IsNullOrWhiteSpace([string]$Record.owner_process_started_utc)
  }
  if ($Record.owner_kind -eq 'heartbeat') {
    return -not [string]::IsNullOrWhiteSpace([string]$Record.heartbeat_id) -and
      -not [string]::IsNullOrWhiteSpace([string]$Record.heartbeat_timeout_seconds)
  }
  return $false
}

function Get-OwnerState($Record, [string]$PoolRoot) {
  if (-not (Test-CompleteLeaseRecord $Record)) { return 'unknown' }
  if ($Record.owner_kind -eq 'heartbeat') {
    $timeout = 0
    if (-not [int]::TryParse([string]$Record.heartbeat_timeout_seconds, [ref]$timeout)) { return 'unknown' }
    try { $path = Get-HeartbeatPath $PoolRoot ([string]$Record.heartbeat_id) } catch { return 'unknown' }
    if (-not [System.IO.File]::Exists($path)) { return 'dead' }
    $age = [DateTime]::UtcNow - [System.IO.File]::GetLastWriteTimeUtc($path)
    if ($age.TotalSeconds -le $timeout) { return 'alive' }
    return 'dead'
  }
  if ($Record.owner_host -ne [Environment]::MachineName) { return 'foreign' }
  $durablePid = 0
  if (-not [int]::TryParse([string]$Record.owner_pid, [ref]$durablePid)) { return 'unknown' }
  try {
    $process = Get-Process -Id $durablePid -ErrorAction Stop
    if ((Get-UtcStamp $process.StartTime) -eq [string]$Record.owner_process_started_utc) {
      return 'alive'
    }
    return 'dead'
  } catch {
    return 'dead'
  }
}

function Test-RecordOldEnough([string]$Path, [int]$GraceSeconds) {
  if (-not [System.IO.File]::Exists($Path)) { return $false }
  $age = [DateTime]::UtcNow - [System.IO.File]::GetLastWriteTimeUtc($Path)
  return $age.TotalSeconds -ge $GraceSeconds
}

function Remove-RecordByRename([string]$Path, [string]$ExpectedToken, [string]$Reason) {
  $record = Read-LeaseRecord $Path
  if (-not $record -or $record.lease_token -ne $ExpectedToken) {
    throw ('lease identity changed before ' + $Reason + ': ' + $Path)
  }
  $tombstone = $Path + '.' + $Reason + '.' + $ExpectedToken
  [System.IO.File]::Move($Path, $tombstone)
  [System.IO.File]::Delete($tombstone)
}

function Remove-CorruptRecordByRename([string]$Path, [string]$Reason) {
  $tombstone = $Path + '.' + $Reason + '.' + [guid]::NewGuid().ToString('N')
  [System.IO.File]::Move($Path, $tombstone)
  [System.IO.File]::Delete($tombstone)
}

function Get-PoolTrees([string]$PoolRoot) {
  return @(Get-ChildItem $PoolRoot -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^pool-([0-9]+)$' } |
    Sort-Object { [int]($_.Name.Substring(5)) })
}

function Get-LeasePath([string]$Worktree) { return Join-Path $Worktree '.atx-lease' }

function Enter-SelectionMutex([string]$PoolRoot, [string]$MutexRunId) {
  $path = Join-Path $PoolRoot '.atx-pool-acquire.lock'
  $deadline = (Get-Date).AddSeconds(20)
  while ((Get-Date) -lt $deadline) {
    $internalOwner = New-ProcessOwnerFields $PID (Get-CurrentProcessStartUtc) -AllowLauncher
    $fields = New-LeaseFields $MutexRunId 'pool-selector' 'POOL_SELECTION' 'none' ('0' * 40) $internalOwner
    if (Try-NewClaimRecord $path $fields) {
      return [pscustomobject]@{ Path = $path; Token = $fields.lease_token }
    }
    $existing = Read-LeaseRecord $path
    if (Test-CompleteLeaseRecord $existing) {
      if ((Get-OwnerState $existing $PoolRoot) -eq 'dead') {
        try { Remove-RecordByRename $path $existing.lease_token 'stale' } catch { }
      } else {
        Start-Sleep -Milliseconds 50
      }
    } elseif (Test-RecordOldEnough $path 5) {
      try { Remove-CorruptRecordByRename $path 'corrupt-stale' } catch { }
    } else {
      Start-Sleep -Milliseconds 50
    }
  }
  throw ('timed out waiting for atomic pool-selection lock: ' + $path)
}

function Exit-SelectionMutex($Mutex) {
  if ($Mutex) { Remove-RecordByRename $Mutex.Path $Mutex.Token 'released' }
}

function Find-FreePool([string]$PoolRoot) {
  foreach ($tree in (Get-PoolTrees $PoolRoot)) {
    if (-not [System.IO.File]::Exists((Get-LeasePath $tree.FullName))) { return $tree }
  }
  return $null
}

function Find-MissingPoolNumber([string]$PoolRoot, [int]$PoolLimit) {
  $used = @{}
  foreach ($tree in (Get-PoolTrees $PoolRoot)) { $used[[int]$tree.Name.Substring(5)] = $true }
  for ($number = 1; $number -le $PoolLimit; $number++) {
    if (-not $used.ContainsKey($number)) { return $number }
  }
  return 0
}

function Remove-HeartbeatForRecord($Record, [string]$PoolRoot) {
  if ($Record.owner_kind -ne 'heartbeat') { return }
  $path = Get-HeartbeatPath $PoolRoot ([string]$Record.heartbeat_id)
  if ([System.IO.File]::Exists($path)) { [System.IO.File]::Delete($path) }
}

# Pester imports safety helpers without running command mode.
if ($MyInvocation.InvocationName -eq '.') { return }

if ($MaxPool -lt 1) { throw 'MaxPool must be at least 1' }
if ($CorruptRecordGraceSeconds -lt 1) { throw 'CorruptRecordGraceSeconds must be at least 1' }
if ($TestLeaseOnly -and -not $TestPoolRoot) { throw 'TestLeaseOnly requires TestPoolRoot' }
if ($TestPoolRoot -and -not $TestLeaseOnly) { throw 'TestPoolRoot is valid only with TestLeaseOnly' }
if ($TestSelectionDelayMs -gt 0 -and -not $TestLeaseOnly) { throw 'TestSelectionDelayMs is test-only' }
if (($TestBaseSha -or $TestExistingBranchSha) -and -not $TestLeaseOnly) { throw 'test SHA seams require TestLeaseOnly' }
if ($RecoverStale -and -not $Release) { throw 'RecoverStale is valid only with Release' }

$repoRoot = $null
if ($TestPoolRoot) {
  $wtRoot = [System.IO.Path]::GetFullPath($TestPoolRoot)
} else {
  $repoRoot = (git rev-parse --show-toplevel).Trim()
  if ($LASTEXITCODE -ne 0 -or -not $repoRoot) { throw 'not inside a git repository' }
  $gitCommonDir = (git rev-parse --path-format=absolute --git-common-dir).Trim()
  if ($LASTEXITCODE -ne 0 -or -not $gitCommonDir) { throw 'cannot resolve git common directory' }
  $primaryRoot = Split-Path $gitCommonDir -Parent
  $wtRoot = Join-Path (Split-Path $primaryRoot -Parent) 'atx-wt'
}
New-Item -ItemType Directory -Force $wtRoot | Out-Null

if ($Status) {
  $trees = @(Get-PoolTrees $wtRoot)
  if ($trees.Count -eq 0) {
    Write-Host ('pool empty (' + $wtRoot + '); lease with a durable owner contract')
    return
  }
  foreach ($tree in $trees) {
    $leasePath = Get-LeasePath $tree.FullName
    $record = Read-LeaseRecord $leasePath
    if (Test-CompleteLeaseRecord $record) {
      $state = 'LEASED run_id=' + $record.run_id + ' | agent=' + $record.agent +
        ' | owner_kind=' + $record.owner_kind + ' | owner=' + (Get-OwnerState $record $wtRoot) +
        ' | branch=' + $record.branch + ' | acquired=' + $record.acquired_utc
    } elseif ($record) {
      $state = 'LEASED CORRUPT/LEGACY (explicit investigation required)'
    } else {
      $state = 'free'
    }
    if ($TestLeaseOnly) {
      $branchName = if ($record) { $record.branch } else { 'HEAD' }
    } else {
      $branchName = (git -C $tree.FullName rev-parse --abbrev-ref HEAD 2>$null)
    }
    $warm = if (Test-Path (Join-Path $tree.FullName 'build\build.ninja')) { 'warm' } else { 'cold' }
    Write-Host ($tree.Name + '  [' + $warm + ']  branch=' + $branchName + '  ' + $state)
  }
  return
}

if ($Pulse) {
  Assert-RecordValue 'RunId' $RunId
  if ($Pulse -notmatch '^pool-[0-9]+$') { throw 'Pulse must name one pool-N slot' }
  $leasePath = Get-LeasePath (Join-Path $wtRoot $Pulse)
  $record = Read-LeaseRecord $leasePath
  if (-not (Test-CompleteLeaseRecord $record)) { throw 'cannot pulse an incomplete lease record' }
  if ($record.run_id -ne $RunId) { throw 'run_id mismatch; refusing heartbeat pulse' }
  if ($record.owner_kind -ne 'heartbeat') { throw 'lease does not use heartbeat ownership' }
  $heartbeatPath = Get-HeartbeatPath $wtRoot ([string]$record.heartbeat_id)
  if (-not [System.IO.File]::Exists($heartbeatPath)) { throw 'heartbeat file is missing' }
  [System.IO.File]::SetLastWriteTimeUtc($heartbeatPath, [DateTime]::UtcNow)
  Write-Output ('PULSED pool=' + $Pulse + ' run_id=' + $RunId + ' heartbeat_id=' + $record.heartbeat_id)
  return
}

if ($Release) {
  Assert-RecordValue 'RunId' $RunId
  if ($Release -notmatch '^pool-[0-9]+$') { throw 'Release must name one pool-N slot' }
  $worktree = Join-Path $wtRoot $Release
  if (-not (Test-Path -LiteralPath $worktree -PathType Container)) { throw ('no such pool tree: ' + $worktree) }
  $leasePath = Get-LeasePath $worktree
  $record = Read-LeaseRecord $leasePath

  if (-not (Test-CompleteLeaseRecord $record)) {
    if (-not $RecoverStale) { throw ('incomplete lease record; explicit stale recovery required: ' + $leasePath) }
    if (-not (Test-RecordOldEnough $leasePath $CorruptRecordGraceSeconds)) {
      throw 'refusing corrupt-record recovery inside the grace period'
    }
    if (-not $TestLeaseOnly) {
      if ($RecoveryBaseSha -notmatch '^[0-9a-fA-F]{40}$') {
        throw 'corrupt production recovery requires exact -RecoveryBaseSha'
      }
      $dirty = @(git -C $worktree status --porcelain) | Where-Object { $_ -notmatch '\.atx-lease($|\.)' }
      if ($dirty) { throw ('refusing corrupt release of dirty tree:' + "`n" + ($dirty -join "`n")) }
      git -C $worktree switch --detach $RecoveryBaseSha | Out-Null
      if ($LASTEXITCODE -ne 0) { throw 'git detach failed during corrupt-record recovery' }
    }
    Remove-CorruptRecordByRename $leasePath 'corrupt-recovered'
    Write-Output ('RECOVERED_CORRUPT pool=' + $Release + ' by_run_id=' + $RunId)
    return
  }

  if ($record.run_id -ne $RunId) {
    if (-not $RecoverStale) { throw ('run_id mismatch: lease belongs to ' + $record.run_id + '; refusing release') }
    $ownerState = Get-OwnerState $record $wtRoot
    if ($ownerState -ne 'dead') {
      throw ('refusing stale recovery: owner state is ' + $ownerState + ' (durable owner guard)')
    }
  }
  if (-not $TestLeaseOnly) {
    $dirty = @(git -C $worktree status --porcelain) | Where-Object { $_ -notmatch '\.atx-lease($|\.)' }
    if ($dirty) { throw ('refusing to release ' + $Release + ': tree is dirty:' + "`n" + ($dirty -join "`n")) }
    git -C $worktree switch --detach $record.base_sha | Out-Null
    if ($LASTEXITCODE -ne 0) { throw ('git switch --detach failed in ' + $worktree) }
  }
  Remove-RecordByRename $leasePath $record.lease_token 'released'
  Remove-HeartbeatForRecord $record $wtRoot
  Write-Output ('RELEASED pool=' + $Release + ' run_id=' + $record.run_id)
  return
}

Assert-RecordValue 'Branch' $Branch
Assert-RecordValue 'RunId' $RunId
if (($OwnerPid -gt 0 -or $OwnerProcessStartedUtc) -and $HeartbeatId) {
  throw 'choose exactly one durable owner contract: process or heartbeat'
}
if ($HeartbeatId) {
  $ownerFields = New-HeartbeatOwnerFields $HeartbeatId $HeartbeatTimeoutSeconds
} elseif ($OwnerPid -gt 0 -and $OwnerProcessStartedUtc) {
  $ownerFields = New-ProcessOwnerFields $OwnerPid $OwnerProcessStartedUtc
} else {
  throw 'production lease requires explicit OwnerPid+OwnerProcessStartedUtc or HeartbeatId'
}

if ($TestLeaseOnly) {
  $baseSha = if ($TestBaseSha) { $TestBaseSha } else { '0' * 40 }
  if ($baseSha -notmatch '^[0-9a-fA-F]{40}$') { throw 'TestBaseSha must be a full SHA' }
  if ($TestExistingBranchSha) {
    if ($TestExistingBranchSha -notmatch '^[0-9a-fA-F]{40}$') { throw 'TestExistingBranchSha must be a full SHA' }
    Assert-BranchHeadMatchesBase $TestExistingBranchSha $baseSha
  }
} else {
  $baseSha = (git -C $repoRoot rev-parse ($Base + '^{commit}')).Trim()
  if ($LASTEXITCODE -ne 0 -or $baseSha -notmatch '^[0-9a-fA-F]{40}$') {
    throw ('cannot resolve base ref to a commit: ' + $Base)
  }
  git -C $repoRoot show-ref --verify --quiet ('refs/heads/' + $Branch)
  if ($LASTEXITCODE -eq 0) {
    $existingSha = (git -C $repoRoot rev-parse ('refs/heads/' + $Branch + '^{commit}')).Trim()
    Assert-BranchHeadMatchesBase $existingSha $baseSha
  }
}

$heartbeatPath = $null
$heartbeatCreated = $false
if ($HeartbeatId) {
  $heartbeatPath = Get-HeartbeatPath $wtRoot $HeartbeatId
  New-Item -ItemType Directory -Force (Split-Path $heartbeatPath -Parent) | Out-Null
  $heartbeatFields = [ordered]@{
    version = '1'
    lease_token = [guid]::NewGuid().ToString('N')
    run_id = $RunId
    heartbeat_id = $HeartbeatId
    created_utc = Get-UtcStamp (Get-Date)
  }
  if (-not (Publish-AtomicRecord $heartbeatPath $heartbeatFields)) {
    throw ('HeartbeatId already exists; use a run-unique id: ' + $HeartbeatId)
  }
  $heartbeatCreated = $true
}

$fields = New-LeaseFields $RunId $Agent $Branch $Base $baseSha $ownerFields
$mutex = $null
$claimed = $false
try {
  $mutex = Enter-SelectionMutex $wtRoot $RunId
  if ($TestSelectionDelayMs -gt 0) { Start-Sleep -Milliseconds $TestSelectionDelayMs }
  $free = Find-FreePool $wtRoot
  if ($free) {
    $worktree = $free.FullName
    $poolName = $free.Name
  } else {
    $number = Find-MissingPoolNumber $wtRoot $MaxPool
    if ($number -eq 0) { throw ('pool exhausted (' + $MaxPool + ' slots, all leased)') }
    $poolName = 'pool-' + $number
    $worktree = Join-Path $wtRoot $poolName
    if ($TestLeaseOnly) {
      New-Item -ItemType Directory -Path $worktree -ErrorAction Stop | Out-Null
    } else {
      Write-Host ('growing pool: git worktree add --detach ' + $worktree + ' ' + $baseSha) -ForegroundColor Cyan
      git worktree add --detach $worktree $baseSha
      if ($LASTEXITCODE -ne 0) { throw 'git worktree add failed' }
    }
  }
  $leasePath = Get-LeasePath $worktree
  if (-not (Publish-AtomicRecord $leasePath $fields)) {
    throw ('atomic lease publication lost for ' + $poolName)
  }
  $claimed = $true
} finally {
  if ($mutex) { Exit-SelectionMutex $mutex }
  if (-not $claimed -and $heartbeatCreated -and [System.IO.File]::Exists($heartbeatPath)) {
    [System.IO.File]::Delete($heartbeatPath)
  }
}

if (-not $claimed) { throw 'lease acquisition failed before a slot was claimed' }
if ($TestLeaseOnly) {
  Write-Output ('LEASED pool=' + $poolName + ' path=' + $worktree + ' run_id=' + $RunId +
    ' token=' + $fields.lease_token + ' owner_kind=' + $fields.owner_kind)
  return
}

# Existing branches were proven equal to baseSha before acquisition. Verify again
# after switch so a mutable branch can never bypass the frozen-base contract.
git -C $repoRoot show-ref --verify --quiet ('refs/heads/' + $Branch)
if ($LASTEXITCODE -eq 0) { git -C $worktree switch $Branch }
else { git -C $worktree switch -c $Branch $baseSha }
if ($LASTEXITCODE -ne 0) { throw ('git switch failed; lease remains held: ' + $worktree) }
$checkedOutSha = (git -C $worktree rev-parse HEAD).Trim()
Assert-BranchHeadMatchesBase $checkedOutSha $baseSha

git -C $worktree submodule update --init --recursive
if ($LASTEXITCODE -ne 0) { throw ('submodule update failed; lease remains held: ' + $worktree) }

$preset = if ($Shared) { 'dev-shared' } else { 'dev' }
if (-not (Test-Path (Join-Path $worktree 'build\build.ninja'))) {
  Write-Host ('cold tree: configuring preset ' + $preset + ' (one-time for this slot)') -ForegroundColor Cyan
  Push-Location $worktree
  try { & (Join-Path $worktree 'scripts\atx-build.ps1') configure -Preset $preset }
  finally { Pop-Location }
  if ($LASTEXITCODE -ne 0) { throw ('configure failed; lease remains held: ' + $worktree) }
} elseif ($Shared) {
  Write-Host 'NOTE: tree already configured; -Shared ignored.' -ForegroundColor Yellow
}
if ($HeartbeatId) { [System.IO.File]::SetLastWriteTimeUtc($heartbeatPath, [DateTime]::UtcNow) }

Write-Output ('LEASED pool=' + $poolName + ' path=' + $worktree + ' branch=' + $Branch +
  ' base_sha=' + $baseSha + ' run_id=' + $RunId + ' owner_kind=' + $fields.owner_kind)
if ($HeartbeatId) {
  Write-Output ('PULSE with: powershell scripts\lease-worktree.ps1 -Pulse ' + $poolName + ' -RunId ' + $RunId)
}
Write-Output ('RELEASE with: powershell scripts\lease-worktree.ps1 -Release ' + $poolName + ' -RunId ' + $RunId)
