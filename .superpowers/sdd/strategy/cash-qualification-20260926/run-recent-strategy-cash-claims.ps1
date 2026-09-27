$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath 'C:/atx-wt/pool-2'
if (@(& git status --porcelain).Count -ne 0) { throw 'Rehearsal requires a clean source tree' }
$claimPath = 'atx-impl/strategies/slow_price_volume_24_v1.cash_claims.json'
$claimSha = '257c6d645292b9f9464e6401ed1f9adb5e41a66089fdc4f7e23db0724f6da510'
if ((Get-FileHash -LiteralPath $claimPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $claimSha) {
  throw 'Frozen claim configuration changed'
}
$priorReceipt = Get-Content -LiteralPath 'build-equity/recent-dev-rehearsal-v2-run/receipt.json' -Raw | ConvertFrom-Json
$strategyArguments = @($priorReceipt.command)
$outputFlag = [array]::IndexOf($strategyArguments, '--output')
if ($outputFlag -lt 0) { throw 'Missing original run output binding' }
$strategyArguments[$outputFlag + 1] = 'build-equity/recent-dev-rehearsal-v3'
$strategyArguments += @('--cash-claims', $claimPath, '--cash-claims-sha256', $claimSha)
$bindingArguments = @()
foreach ($binding in $priorReceipt.bindings) { $bindingArguments += @('--bind', $binding.path) }
$bindingArguments += @('--bind', $claimPath)
foreach ($eventName in @('mdco_20200106','wair_20200109','bold_20200115','arql_20200116','thor_20200123')) {
  $bindingArguments += @('--bind', "atx-impl/strategies/evidence/$eventName.research.json")
}
$env:PATH = 'C:/atx-cache/vcpkg_installed/x64-windows/debug/bin;C:/atx-cache/vcpkg_installed/x64-windows/bin;' + $env:PATH
& python scripts/run_bounded_research.py --output build-equity/recent-dev-rehearsal-v3-run --seconds 180 --max-rss-mib 1536 --min-free-mib 768 @bindingArguments -- @strategyArguments
exit $LASTEXITCODE
