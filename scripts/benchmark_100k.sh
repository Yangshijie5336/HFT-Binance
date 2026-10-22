#!/usr/bin/env bash
# Reproducible 100,000-event synthetic benchmark (A/B/C).
set -euo pipefail
BUILD_DIR="${BUILD_DIR:-build}"
OUTPUT_DIR="${OUTPUT_DIR:-data/benchmarks}"
mkdir -p "$OUTPUT_DIR"
"$BUILD_DIR/marketpipe" benchmark --events 100000 --mode all \
  --output-dir "$OUTPUT_DIR" | tee "$OUTPUT_DIR/benchmark-100k.json"
