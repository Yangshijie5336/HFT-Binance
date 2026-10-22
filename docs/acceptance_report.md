# 900-second live acceptance report

## Verdict

**PASS** — a real, continuous Binance Spot `btcusdt@trade` WebSocket capture ran for
900.1129 s, persisted all 24,903 received frames, processed all 24,903 trades, verified
the raw file SHA-256, and reported zero queue overflow, zero drops, zero persistence
errors, zero malformed/duplicate/out-of-order events and zero reconnects.

This report is generated from executed artifacts only. Synthetic benchmark numbers are
kept separate and are never used as live-network results.

## Run identity and environment

| Item | Value |
|---|---|
| Git base commit | `76474cf` (working tree extended in this change set) |
| Run id | `live900-BTCUSDT-20261022` |
| Run directory | `data/runs/live900-BTCUSDT-20261022` (not committed) |
| Endpoint | `wss://stream.binance.com:9443/ws/btcusdt@trade` |
| Symbol | `BTCUSDT` |
| TLS | TLSv1.3, `TLS_AES_128_GCM_SHA256`, peer + hostname verification |
| Host | Windows 10/11 x64 workstation (not colocated) |
| Compiler | MSVC 19.51.36260, C++20, Release |
| Build | CMake 4.3.1, Ninja 1.13.2, Boost 1.91, OpenSSL 3.6.2 |

Start/end are the collector's measured wall-clock timestamps:

| Field | UTC |
|---|---|
| Started | `2026-10-22T04:39:34.155114400Z` |
| Finished | `2026-10-22T04:54:34.608986900Z` |
| Target duration | 900 s |
| Observed capture duration | **900.1129 s** |
| Processing/drain duration | 900.4539 s |

## Capture results

| Metric | Value |
|---|---:|
| WebSocket frames received | 24,903 |
| Raw frames journaled | 24,903 |
| Raw frame bytes | 19,881,198 |
| Trade events seen | 24,903 |
| Unique trades processed | 24,903 |
| Signals emitted | 24,903 |
| Signals: BUY / SELL / HOLD | 0 / 0 / 24,903 |
| Duplicate trades | 0 |
| Out-of-order trades | 0 |
| Malformed frames | 0 |
| Server error frames | 0 |
| Disconnects | 0 |
| Reconnect attempts | 0 |
| Connection generations | 1 |
| Queue high-water / capacity | 292 / 65,536 |
| Queue overflow events | 0 |
| Dropped events | 0 |
| Persistence errors | 0 |
| Processing failures | 0 |
| Throughput | 27.67 trades/s |
| Raw disk write rate | 22,087 bytes/s |
| Average / peak unique trades per minute | 1,660 / 3,072 |
| Peak unique trades per second | 462 |
| Average sampled process CPU | 0.309 % |
| Peak sampled RSS | 36,896,768 bytes (35.2 MiB) |

All eleven quality gates in `summary.json` are true: duration ≥ 99%, at least one trade,
raw record count match, zero overflow, zero persistence errors, raw SHA-256 verified,
zero processing failures, no disconnects, no malformed frames, no duplicates/out-of-order,
and TLS verified.

### Why the live window emitted only HOLD

The strategy requires both `|imbalance| ≥ 0.15` and `|return_n| ≥ 1e-4`. Over this
15-minute window the 21-trade rolling return ranged from **-7.346e-05 to +8.081e-05**,
below the 1e-4 momentum threshold, while trade imbalance ranged from -1.0 to +0.615.
The deterministic rule therefore produced `HOLD` for all 24,903 trades. No label was
fabricated to force BUY/SELL; the unit test suite separately proves all three actions.

## Latency (local receive → process, not exchange-to-exchange)

Samples: 4,980 (every fifth trade), microseconds.

| Stage | p50 | p95 | p99 | max |
|---|---:|---:|---:|---:|
| Queue wait | 565.6 | 3,118.2 | 6,201.0 | 11,073.4 |
| Receive to process | 731.9 | 19,401.1 | 22,655.5 | 52,955.0 |
| Receive to signal | 731.8 | 19,401.1 | 22,655.4 | 52,954.9 |

The p95/p99 tail includes local disk fsync and scheduler effects. These are not
exchange-to-exchange latencies.

## Integrity evidence

External `Get-FileHash` and the collector's incremental/read-back hashes agree:

```
raw_frames.jsonl
  records: 24903
  bytes:   19881198
  sha256:  5836675014897976d47654e8d0fde5f765f7ff129d0ecba9c413d5c5a82919b8
```

`verification.json` independently passes:

- `summary_result_pass`, `raw_frames_exist`
- `raw_sha256_matches_summary`
- `raw_records_match_summary`
- `raw_payload_hashes_valid`
- `collector_raw_integrity_verified`
- `artifact_hashes_match`

Artifact hashes (also in `summary.json`):

| Artifact | SHA-256 |
|---|---|
| `raw_frames.jsonl` | `5836675014897976d47654e8d0fde5f765f7ff129d0ecba9c413d5c5a82919b8` |
| `normalized_events.jsonl` | `b5a2e245d3f589d0da7c3f0360d6c20901a78c75835c111672bbde80d8e16bc0` |
| `factors.jsonl` | `3bf57c187d1d43b183e6fc5665154c367ac98bf557b58a32991bd39f383500a1` |
| `signals.jsonl` | `fa77b1f2296b2e35f4629ec343c49277cec928f656237717247702f266cf9ae9` |
| `system.log` | `4e8932968d9a0224b61f82793dee8264a5fae4092b991cdf60bf14b40ee8ae97` |

Derived file sizes: `normalized_events.jsonl` 14,330,103 B; `factors.jsonl` 9,350,992 B;
`signals.jsonl` 5,264,157 B; `system.log` 3,300 B.

The complete raw capture and derived files are intentionally not committed. They remain
under `data/runs/live900-BTCUSDT-20261022/` and are attached to the manual live-capture
CI artifact when that workflow is run. Small evidence copies are under `docs/evidence/`.

## Replay verification

`replay` of the captured raw file produced:

```json
{"result":"PASS","replay":true,"records_read":24903,"trades_processed":24903,
 "duplicates":0,"out_of_order":0,"malformed_frames":0,"execution_enabled":false}
```

Normalized/factor/signal line counts are 24,903 each; live and replay both emit 24,903
HOLD labels. Replay uses the same parser, factors and strategy as live processing.

## A/B/C benchmark (synthetic, not live throughput)

Final binary, 100,000 events per mode, `SYNTHETIC_BENCHMARK`, `live_network:false`:

| Mode | events/s | p50 µs | p95 µs | p99 µs | CPU % | RSS B | errors |
|---|---:|---:|---:|---:|---:|---:|---:|
| A. Parse only | 163,315 | 3.5 | 6.4 | 8.0 | 102.0 | 25,489,408 | 0 |
| B. Full pipeline (dedup+factors+strategy) | 135,678 | 6.4 | 11.5 | 17.6 | 98.7 | 26,112,000 | 0 |
| C. B + raw JSONL persistence | 53,132 | 10.6 | 407.5 | 636.5 | 86.5 | 27,082,752 | 0 |

Mode C wrote 100,000 raw frames (51,363,504 bytes) with SHA-256
`84eb486b8050eb47a1e84cc829e8df53ee4d81f8b0815abe00b54e1fe43642a7`.
Modes A/B emit 99,991 non-HOLD synthetic labels; mode A is parse-only by definition.
CPU above 100% is possible for a multi-core process; it is process CPU time divided by
wall time.

## Tests

`ctest --test-dir build-ninja --output-on-failure`:

```
1/4 marketpipe_help ............... Passed
2/4 marketpipe_unit ............... Passed   (18 cases)
3/4 marketpipe_integration ........ Passed   (14 loopback WebSocket scenarios)
4/4 marketpipe_benchmark_100k ..... Passed
100% tests passed, 0 tests failed out of 4
Total Test time (real) = 20.80 sec
```

The integration suite covers normal streaming, malformed frames, abrupt disconnect and
reconnect generation isolation, duplicate/out-of-order trades, bounded queue overflow,
SIGINT and SIGTERM drain/finalize, HTTP 451 handling, simulated disk-full, final-fsync
failure, tampered raw file detection, replay parity and connection refusal. None of these
tests uses the Binance network. Raw CTest output is in
[`docs/evidence/ctest-last.txt`](evidence/ctest-last.txt).

## Docker and CI status

- Multi-stage Dockerfile, non-root UID 10001, named volume `marketpipe-data`, CTest in the
  build stage, and `STOPSIGNAL SIGTERM` are implemented.
- **Docker was not executed locally** because this Windows host has no Docker engine or
  `docker` CLI. Do not claim a locally verified image. The CI workflow builds the image
  and verifies it starts as non-root.
- GitHub Actions workflows are committed. This session did not produce a GitHub Actions
  run URL; CI evidence must be taken from the actual run once pushed.

## Reproduction

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure

./build/marketpipe capture --symbol BTCUSDT --duration 900 \
  --output-dir data/runs --run-id live900-BTCUSDT-20261022
./build/marketpipe report --run-dir data/runs/live900-BTCUSDT-20261022
sha256sum data/runs/live900-BTCUSDT-20261022/raw_frames.jsonl
./build/marketpipe benchmark --events 100000 --mode all --output-dir data/benchmarks
```

## Limitations

This is one 15-minute observation on one non-colocated Windows workstation. It does not
prove long-term reliability, exchange-side completeness, colocation-grade latency or
profitability. The strategy is a deterministic labeling demo; execution is disabled.
