$ErrorActionPreference = "Stop"

if (-not (Get-Command git -ErrorAction SilentlyContinue)) { throw "git is required" }
if (-not (Get-Command gh -ErrorAction SilentlyContinue)) { throw "GitHub CLI (gh) is required. Install it and run 'gh auth login'." }

# This script must be run from the root of the downloaded MarketPipe project.
if (-not (Test-Path "pyproject.toml") -or -not (Test-Path "src\marketpipe\cli.py")) {
    throw "Run this script from the MarketPipe project root."
}

gh auth status | Out-Host

$forbidden = Get-ChildItem -Force -Recurse -File | Where-Object {
    $_.FullName -notmatch '[\\/](\.git|\.venv|__pycache__)[\\/]' -and
    ($_.Name -in @("api.env", ".env", "execution.token") -or
     $_.Extension -in @(".sqlite", ".sqlite3", ".db", ".pem", ".p12"))
}
if ($forbidden) {
    $forbidden | ForEach-Object { Write-Error "Potential secret/data file blocked from public publish: $($_.FullName)" }
    throw "Remove the listed sensitive/runtime files before publishing."
}

if (-not (Test-Path ".git")) { git init -b main }
git branch -M main
$branch = "main"

git add .gitignore README.md LICENSE pyproject.toml requirements.txt Dockerfile docker-compose.yml Makefile src tests docs scripts .github data/runs/.gitkeep
if ((git status --porcelain).Trim()) {
    git commit -m "Initial MarketPipe real-time market data demo"
}

$origin = git remote get-url origin 2>$null
if ($LASTEXITCODE -eq 0 -and $origin) {
    throw "An origin remote already exists ($origin). Review it manually before publishing."
}
gh repo create marketpipe-hft-demo --public --source . --remote origin --push
Write-Host "Published public repository. Verify the repository URL, Actions CI, and GHCR package visibility in GitHub before claiming release complete."
