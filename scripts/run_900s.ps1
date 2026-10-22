# Run the 900-second public Binance BTCUSDT acceptance capture.
# Requires outbound TLS to stream.binance.com:9443. HTTP 451 is recorded as
# BLOCKED; this script never bypasses regional restrictions.
param(
  [string]$BuildDir = "build-ninja",
  [string]$OutputDir = "data/runs",
  [int]$Duration = 900,
  [string]$Symbol = "BTCUSDT"
)
$ErrorActionPreference = "Stop"
$exe = Join-Path $BuildDir "marketpipe.exe"
if (-not (Test-Path $exe)) {
  throw "missing $exe; build first: cmake --build $BuildDir"
}
$runId = "live${Duration}s-$Symbol-" + (Get-Date -Format "yyyyMMdd")
& $exe capture --symbol $Symbol --duration $Duration --output-dir $OutputDir --run-id $runId
$code = $LASTEXITCODE
$runDir = Join-Path $OutputDir $runId
if (Test-Path (Join-Path $runDir "summary.json")) {
  & $exe report --run-dir $runDir
  Get-FileHash (Join-Path $runDir "raw_frames.jsonl") -Algorithm SHA256 |
    Format-List
}
exit $code
