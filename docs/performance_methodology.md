# Performance methodology

Live capture metrics use a monotonic clock for duration and processing latency. Raw frame persistence is measured from receipt until the per-frame `flush` and `fsync` complete; receive-to-signal latency ends when the signal record is produced. Percentiles are sampled every fifth event and are reported as `N/A` when no samples exist. Synthetic throughput measurements must be labeled `SYNTHETIC_BENCHMARK` and are never included in live results.

The 900-second acceptance requires a completed continuous Binance Spot `btcusdt@trade` run with valid trades. HTTP 451, DNS, TLS, or connection refusal is recorded as `BLOCKED` with the original error.
