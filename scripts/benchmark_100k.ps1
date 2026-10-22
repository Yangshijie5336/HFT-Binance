# Reproducible 100,000-event synthetic benchmark (A/B/C).
# The output is explicitly synthetic and is not live network throughput.
param(
  [string]$BuildDir = "build-ninja",
  [string]$OutputDir = "data/benchmarks"
)
$ErrorActionPreference = "Stop"
$exe = Join-Path $BuildDir "marketpipe.exe"
if (-not (Test-Path $exe)) {
  throw "missing $exe; build first: cmake --build $BuildDir"
}
New-Item -ItemType Directory -Force $OutputDir | Out-Null
& $exe benchmark --events 100000 --mode all --output-dir $OutputDir |
  Tee-Object -FilePath (Join-Path $OutputDir "benchmark-100k.json")
