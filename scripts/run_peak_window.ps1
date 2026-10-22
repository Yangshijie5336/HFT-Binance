param(
    [switch]$Immediate,
    [string]$Symbol = "BTCUSDT",
    [int]$DurationSeconds = 900
)
$ErrorActionPreference = "Stop"

# BTC spot trades 24/7. The 13:00-17:00 UTC window is chosen as a practical
# higher-activity observation window around Europe/US overlap, not a guarantee.
if (-not $Immediate) {
    while ($true) {
        $utc = [DateTime]::UtcNow
        if ($utc.Hour -ge 13 -and $utc.Hour -lt 17) { break }
        $next = $utc.Date.AddHours(13)
        if ($utc.Hour -ge 17) { $next = $next.AddDays(1) }
        $seconds = [Math]::Max(1, [int][Math]::Ceiling(($next - $utc).TotalSeconds))
        Write-Host "Waiting until selected UTC activity window: $($next.ToString('o')); seconds=$seconds"
        Start-Sleep -Seconds ([Math]::Min($seconds, 300))
    }
}

$python = Join-Path (Get-Location) ".venv\Scripts\python.exe"
if (-not (Test-Path $python)) { $python = "python" }
Write-Host "Starting live capture: UTC=$([DateTime]::UtcNow.ToString('o')) duration=$DurationSeconds symbol=$Symbol"
& $python -m marketpipe.cli capture --duration $DurationSeconds --symbol $Symbol --output-dir data/runs
exit $LASTEXITCODE
