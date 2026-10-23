# Run the deterministic offline acceptance path twice and compare its outputs.
# This entry point never contacts Binance. It proves that a fixed raw JSONL
# input produces byte-identical normalized events, factors, and signals.
[CmdletBinding()]
param(
  [string]$BuildDir = "build-ninja",
  [string]$OutputDir = "data/reproducible",
  [switch]$SkipBuild,
  [switch]$SkipBenchmark
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $root
$buildPath = (Resolve-Path $BuildDir -ErrorAction SilentlyContinue)
if ($null -eq $buildPath) { $buildPath = Join-Path $root $BuildDir }
$exe = Join-Path $buildPath "marketpipe.exe"
$fixture = Join-Path $root "tests/fixtures/reproducible_raw_frames.jsonl"
$outputPath = Join-Path $root $OutputDir
$replayA = Join-Path $outputPath "replay-a"
$replayB = Join-Path $outputPath "replay-b"
$benchmark = Join-Path $outputPath "benchmark"

function Invoke-Checked([string]$File, [string[]]$Arguments, [string]$Label) {
  Write-Host "[reproducible] $Label"
  & $File @Arguments
  $code = $LASTEXITCODE
  if ($code -ne 0) { throw "$Label failed with exit code $code" }
}

if (-not $SkipBuild) {
  if (-not (Test-Path $buildPath)) {
    Invoke-Checked "cmake" @("-S", ".", "-B", $BuildDir, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release") "configure"
  }
  Invoke-Checked "cmake" @("--build", $BuildDir) "build"
}
if (-not (Test-Path $exe)) { throw "missing executable: $exe" }
if (-not (Test-Path $fixture)) { throw "missing deterministic fixture: $fixture" }

Invoke-Checked "ctest" @("--test-dir", $BuildDir, "--output-on-failure") "CTest"

if (Test-Path $outputPath) {
  Get-ChildItem -LiteralPath $outputPath -Force |
    Where-Object { $_.Name -ne ".gitkeep" } |
    Remove-Item -Recurse -Force
}
New-Item -ItemType Directory -Path $outputPath -Force | Out-Null
Invoke-Checked $exe @("replay", "--input", $fixture, "--output-dir", $replayA) "replay A"
Invoke-Checked $exe @("replay", "--input", $fixture, "--output-dir", $replayB) "replay B"

$compared = @("normalized_events.jsonl", "factors.jsonl", "signals.jsonl")
$hashes = [ordered]@{}
$equal = $true
foreach ($name in $compared) {
  $a = Join-Path $replayA $name
  $b = Join-Path $replayB $name
  if (-not (Test-Path $a) -or -not (Test-Path $b)) { throw "missing replay output: $name" }
  $ha = (Get-FileHash $a -Algorithm SHA256).Hash.ToLowerInvariant()
  $hb = (Get-FileHash $b -Algorithm SHA256).Hash.ToLowerInvariant()
  $same = ($ha -eq $hb)
  if (-not $same) { $equal = $false }
  $hashes[$name] = [ordered]@{ replay_a_sha256 = $ha; replay_b_sha256 = $hb; identical = $same }
}
if (-not $equal) { throw "determinism check failed: replay outputs differ" }

$benchmarkStatus = "SKIPPED"
if (-not $SkipBenchmark) {
  Invoke-Checked $exe @("benchmark", "--events", "100000", "--mode", "all", "--output-dir", $benchmark) "synthetic benchmark"
  $benchmarkStatus = "PASS"
}

$commit = (& git rev-parse HEAD).Trim()
$summary = [ordered]@{
  result = "PASS"
  mode = "REPRODUCIBLE_OFFLINE"
  live_network = $false
  commit = $commit
  fixture = "tests/fixtures/reproducible_raw_frames.jsonl"
  fixture_sha256 = (Get-FileHash $fixture -Algorithm SHA256).Hash.ToLowerInvariant()
  commands = @(
    "cmake --build $BuildDir",
    "ctest --test-dir $BuildDir --output-on-failure",
    "marketpipe replay --input tests/fixtures/reproducible_raw_frames.jsonl --output-dir $OutputDir/replay-a",
    "marketpipe replay --input tests/fixtures/reproducible_raw_frames.jsonl --output-dir $OutputDir/replay-b"
  )
  replay_outputs_identical = $equal
  output_sha256 = $hashes
  benchmark = $benchmarkStatus
  generated_utc = [DateTime]::UtcNow.ToString("o")
}
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $outputPath "summary.json") -Encoding utf8
Write-Host "[reproducible] PASS: $outputPath/summary.json"
exit 0
