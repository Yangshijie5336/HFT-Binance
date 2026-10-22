# Troubleshooting and failure handling

Every capture writes `system.log`, `summary.json` and `verification.json` even when it
fails. Start there: `failure`, `quality_checks` and the last `ws_*` / `*_failed` log
records identify the failing layer.

## HTTP 451 / geographic restriction

Symptom:

```json
{"result":"BLOCKED","http_status":451,"http_blocked":true,
 "failure":"Binance WebSocket HTTP 451: geographic access restriction"}
```

The collector records `capture_blocked_by_exchange`, stops immediately instead of
retrying, persists an empty raw file and writes a BLOCKED summary. It does not use
proxies, VPNs or alternate jurisdictions. Run the local WebSocket integration suite and
record the block as the acceptance outcome.

`403` from a WAF/CDN is reported as `FAIL` with `http_status` and the response body
prefix, because it is not necessarily a legal-access restriction.

## TLS certificate verification failure

Symptom:

```json
{"result":"FAIL","tls_failure":true,
 "failure":"TLS certificate verification failed: certificate verify failed ..."}
```

Checks:

1. OpenSSL trust sources: `--ca-file`, `MARKETPIPE_CA_FILE`, `SSL_CERT_FILE`, the bundled
   `certs/cacert.pem`, OpenSSL default paths, Windows Root store.
2. `system.log` contains `tls_ca_bundle_loaded` with the selected path, or
   `tls_ca_bundle_load_failed` with the OpenSSL error.
3. System clock must be correct; expired certificates cannot be bypassed.
4. Corporate TLS interception requires installing the corporate root CA explicitly via
   `--ca-file`. There is no `--insecure` mode.

Never "fix" this by disabling verification.

## DNS / connection refused

Symptom: repeated `ws_connect_failed` with `consecutive_failures`, then
`ws_max_connect_failures` and `result=FAIL`.

- Verify DNS and `curl -I https://stream.binance.com:9443` from the same host.
- Check firewall/proxy egress; the collector does not read proxy environment variables.
- Backoff is bounded (default 1 s → 15 s, 10 consecutive failures). A refused connection
  always yields a non-zero exit code.

## Idle timeout / disconnect

Beast keep-alive pings are enabled with `--idle-timeout-ms` (default 90 s). The stream
closes if no data or pong arrives within two idle intervals. The collector counts a
disconnect, reconnects with backoff, increments `connection_generations` and records a
`gap_windows` entry. The run result becomes `PARTIAL`, not `PASS`; a reconnect cannot
prove the feed was complete.

## Queue overflow / slow consumer

Symptom:

```json
{"queue_overflow":true,"dropped_events":N,"result":"FAIL",
 "failure":"bounded queue overflow: sample invalid"}
```

The collector does not silently discard frames. It logs `raw_queue_overflow` or
`process_queue_overflow`, marks the sample invalid and stops. Remedies:

- Check whether the disk is the bottleneck (fsync cadence, antivirus, network drive).
- Increase `--queue-capacity` for short bursts, but do not use it to hide sustained
  overload.
- Lower `--fsync-every` only if durability requirements allow; the default is 100.
- A sustained overload is a capacity finding, not a reason to drop data.

## Disk full / write failure / final fsync failure

Symptoms: `persistence_error=true`, `raw_write_failed`, `writer_finalize_failed`,
`raw_finalize_failed`, or `ENOSPC`. Exit code 2.

- Free space; run directories are append-only and never overwritten.
- The collector detects short writes, `fflush`/`fsync` errors and finalization errors.
- On failure the sample is `FAIL`; `report` will also fail its record/hash checks if the
  file is truncated.
- Do not copy a partial run directory into an acceptance report.

## SIGINT / SIGTERM

The handler only sets a flag. Within ~50 ms the network session is cancelled, queues are
closed and drained, raw and derived files are flushed and fsynced, and a summary is
written with `stopped_by_signal=true`, `result=FAIL`, exit code 130.

If the process is force-killed (SIGKILL / `docker kill`), only data already fsynced is
guaranteed. Use `docker stop` (SIGTERM) with a grace period ≥ 30 s.

## Report verification mismatch

`report` returns 2 when:

- `summary.json` says PASS but an artifact hash differs;
- `raw_frames.jsonl` has a bad record, non-increasing sequence or payload hash mismatch;
- the record count differs from `frames_received`/`raw_frames_records`;
- a required artifact is missing.

Do not edit raw files after capture. To verify an archived run, copy the directory
unchanged and run `report --run-dir`.

## Windows DLL not found (0xC0000135)

The build copies `libssl-3-x64.dll` and `libcrypto-3-x64.dll` next to the executables.
If they are missing, ensure `CMAKE_PREFIX_PATH` points at the vcpkg installation that
actually supplied OpenSSL, then reconfigure. Visual Studio can inject a different
`VCPKG_ROOT`; the CMake script derives the DLL directory from the located OpenSSL
configuration as well.

## Docker data disappears

The container writes only under `/app/data`. The provided compose file uses the named
volume `marketpipe-data`, which is initialized with the image directory's ownership and
is writable by UID 10001. If you switch to a host bind mount, make it writable by UID
10001 (or override `user:`); otherwise raw writes fail with a persistence error.
`docker compose down -v` removes the named volume and its captures.

## Synthetic benchmark is not live throughput

Any benchmark output contains `"benchmark_type":"SYNTHETIC_BENCHMARK"` and
`"live_network":false`. Never copy those numbers into a live-capture acceptance claim.
Mode D must come from a real `capture` run's `summary.json`.
