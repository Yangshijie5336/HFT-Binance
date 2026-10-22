#!/usr/bin/env bash
# Run the 900-second public Binance BTCUSDT acceptance capture.
# HTTP 451 is recorded as BLOCKED and is never bypassed.
set -uo pipefail
BUILD_DIR="${BUILD_DIR:-build}"
OUTPUT_DIR="${OUTPUT_DIR:-data/runs}"
DURATION="${DURATION:-900}"
SYMBOL="${SYMBOL:-BTCUSDT}"
RUN_ID="live${DURATION}s-${SYMBOL}-$(date -u +%Y%m%d)"
"$BUILD_DIR/marketpipe" capture --symbol "$SYMBOL" --duration "$DURATION" \
  --output-dir "$OUTPUT_DIR" --run-id "$RUN_ID"
code=$?
if [[ -f "$OUTPUT_DIR/$RUN_ID/summary.json" ]]; then
  "$BUILD_DIR/marketpipe" report --run-dir "$OUTPUT_DIR/$RUN_ID" || true
  sha256sum "$OUTPUT_DIR/$RUN_ID/raw_frames.jsonl"
fi
exit "$code"
