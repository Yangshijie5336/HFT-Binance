from __future__ import annotations

import hashlib
import json
from pathlib import Path
from typing import Any


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def artifact_hashes(output_dir: Path) -> dict[str, str]:
    names = (
        "raw_frames.jsonl", "normalized_events.jsonl", "factors.jsonl",
        "signals.jsonl", "system.log",
    )
    return {name: sha256_file(output_dir / name) for name in names if (output_dir / name).exists()}


def render_performance_report(summary: dict[str, Any]) -> str:
    def latency_row(name: str, value: dict[str, Any]) -> str:
        if not value or value.get("samples", 0) == 0:
            return f"| {name} | N/A | N/A | N/A | 0 |"
        return (f"| {name} | {value.get('p50_us')} | {value.get('p95_us')} | "
                f"{value.get('p99_us')} | {value.get('samples')} |")

    activity = summary.get("activity_by_minute_utc", {})
    activity_table = "\n".join(f"| {minute}Z | {count} |" for minute, count in activity.items())
    if not activity_table:
        activity_table = "| No processed trade events | 0 |"
    hashes = summary.get("artifact_sha256", {})
    hash_lines = "\n".join(f"- `{name}`: `{value}`" for name, value in hashes.items()) or "- Not available"
    checks = summary.get("quality_checks", {})
    check_lines = "\n".join(f"- `{name}`: **{'PASS' if value else 'FAIL'}**" for name, value in checks.items())
    return f"""# MarketPipe 15-minute live-feed performance report

## Result

- **Run status:** `{summary.get('result', 'UNKNOWN')}`
- **Symbol:** `{summary.get('symbol', 'unknown')}`
- **Source:** `{summary.get('websocket_url', 'unknown')}`
- **Start UTC:** `{summary.get('started_utc', 'unknown')}`
- **Target duration:** {summary.get('duration_target_seconds')} seconds
- **Observed capture duration:** {summary.get('duration_actual_seconds')} seconds
- **Processing/drain duration:** {summary.get('processing_elapsed_seconds')} seconds
- **Failure:** `{summary.get('failure')}`

## Event and storage counters

| Metric | Value |
|---|---:|
| WebSocket frames journaled | {summary.get('raw_frames_records', 0)} |
| Raw frame bytes | {summary.get('raw_frames_bytes', 0)} |
| Trade events seen | {summary.get('trade_events_seen', 0)} |
| Unique trades processed | {summary.get('trades_processed', 0)} |
| Duplicate trades | {summary.get('duplicates', 0)} |
| Malformed frames/events | {summary.get('malformed_frames', 0)} |
| Disconnects | {summary.get('disconnects', 0)} |
| Reconnect attempts | {summary.get('reconnects', 0)} |
| Backpressure events | {summary.get('backpressure_events', 0)} |
| Dropped events | {summary.get('dropped_events', 0)} |
| Processing failures | {summary.get('processing_failures', 0)} |
| Queue high-water mark | {summary.get('queue_high_water', 0)} / {summary.get('queue_capacity', 0)} |
| Ring capacity | {summary.get('ring_capacity', 0)} |
| Processed throughput | {summary.get('throughput_trades_per_second', 0)} trades/s |
| Observed ingress rate | {summary.get('ingress_trades_per_second', 0)} trade events/s |
| Average unique trades/minute | {summary.get('average_trades_per_minute', 0)} |
| Peak unique trades/minute | {summary.get('max_trades_per_minute', 0)} |
| Peak unique trades/second | {summary.get('max_trades_per_second', 0)} |
| Average sampled process CPU | {summary.get('cpu_percent_avg')}% |
| Peak sampled RSS | {summary.get('rss_bytes_peak')} bytes |

## Latency (sampled; microseconds)

Latency samples are taken every fifth processed event, capped at 200,000 samples. They include local filesystem and Python scheduling effects and are not exchange-to-exchange latency.

| Stage | p50 µs | p95 µs | p99 µs | Samples |
|---|---:|---:|---:|---:|
{latency_row('Queue wait', summary.get('queue_wait_latency', {}))}
{latency_row('Receive to process', summary.get('receive_to_process_latency', {}))}
{latency_row('Receive to signal', summary.get('receive_to_signal_latency', {}))}

## Activity by exchange timestamp minute (UTC)

| UTC minute | Unique processed trades |
|---|---:|
{activity_table}

## Quality gates

{check_lines or '- No quality checks available'}

## Artifact integrity (SHA-256)

{hash_lines}

## Persistence and interpretation

- Raw WebSocket frames are written to `raw_frames.jsonl` and `fsync`ed before parsing or queueing.
- Derived event/factor/signal JSONL files are flushed per record and `fsync`ed every 100 records plus shutdown.
- `system.log` is flushed and `fsync`ed per record.
- A full raw frame is preserved, including snapshots, subscription acknowledgements, status messages, and any frame containing multiple trade events.
- A `PASS` means this run met the software's duration/data/no-drop/disconnect checks. It does not establish long-term production reliability or trading profitability.
- This is a single-process Python market-data demo, not a colocated low-latency HFT engine. The strategy emits labels only; execution is disabled.
"""


def write_report(output_dir: Path, summary: dict[str, Any]) -> Path:
    path = output_dir / "performance_report.md"
    content = render_performance_report(summary)
    with path.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write(content)
        stream.flush()
        import os
        os.fsync(stream.fileno())
    return path
