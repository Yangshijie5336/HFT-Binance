from __future__ import annotations

import asyncio
import json
import time
from datetime import datetime, timezone
from pathlib import Path

from .metrics import RuntimeMetrics
from .models import parse_trade
from .pipeline import ProcessingPipeline
from .storage import DurableJsonlWriter, read_jsonl


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="microseconds").replace("+00:00", "Z")


async def replay(raw_path: Path, output_dir: Path, *, queue_capacity: int = 50_000,
                 ring_capacity: int = 20_000) -> dict:
    output_dir.mkdir(parents=True, exist_ok=True)
    queue = asyncio.Queue(maxsize=queue_capacity)
    metrics = RuntimeMetrics()
    processed = DurableJsonlWriter(output_dir / "normalized_events.jsonl", fsync_every=100)
    factors = DurableJsonlWriter(output_dir / "factors.jsonl", fsync_every=100)
    signals = DurableJsonlWriter(output_dir / "signals.jsonl", fsync_every=100)
    pipeline = ProcessingPipeline(queue=queue, metrics=metrics, processed_writer=processed,
                                  factors_writer=factors, signals_writer=signals,
                                  ring_capacity=ring_capacity)
    task = asyncio.create_task(pipeline.run())
    for record in read_jsonl(raw_path):
        raw_text = record.get("payload", record.get("raw_payload"))
        if not isinstance(raw_text, str):
            continue
        try:
            payload = json.loads(raw_text)
        except json.JSONDecodeError:
            metrics.counters["malformed_frames"] += 1
            continue
        if payload.get("e") == "trade":
            trades = [payload]
        elif payload.get("channel") == "trade" and isinstance(payload.get("data"), list):
            trades = payload["data"]
        else:
            continue
        for raw_trade in trades:
            if not isinstance(raw_trade, dict):
                continue
            try:
                event = parse_trade(
                    raw_trade,
                    received_utc=record.get("received_at_utc", record["received_utc"]),
                    received_ns=int(record.get("received_monotonic_ns", record["received_ns"])),
                    frame_seq=int(record["frame_seq"]),
                    source_type=str(payload.get("type", payload.get("e", "update"))),
                )
            except (KeyError, TypeError, ValueError):
                metrics.counters["malformed_frames"] += 1
                continue
            metrics.counters["trade_events_seen"] += 1
            await queue.put((event, time.perf_counter_ns()))
    await queue.join()
    await pipeline.stop()
    await task
    for writer in (processed, factors, signals):
        await writer.close()
    summary = {
        "result": "PASS" if metrics.counters["malformed_frames"] == 0 else "PARTIAL",
        "mode": "deterministic_replay",
        "source_raw_file": str(raw_path),
        "trade_events_seen": metrics.counters["trade_events_seen"],
        "trades_processed": metrics.counters["trades_processed"],
        "duplicates": metrics.counters["duplicates"],
        "malformed_frames": metrics.counters["malformed_frames"],
        "signals_emitted": metrics.counters["signals_emitted"],
        "execution_enabled": False,
        "finished_utc": utc_now(),
    }
    with (output_dir / "replay_summary.json").open("w", encoding="utf-8") as stream:
        json.dump(summary, stream, ensure_ascii=False, indent=2)
        stream.write("\n")
        stream.flush()
        import os
        os.fsync(stream.fileno())
    return summary
