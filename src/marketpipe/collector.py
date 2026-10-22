from __future__ import annotations

import asyncio
import json
import logging
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import websockets
import uuid
import hashlib

from .metrics import RuntimeMetrics
from .models import parse_trade
from .pipeline import ProcessingPipeline
from .report import artifact_hashes, write_report
from .storage import DurableJsonlWriter

LOGGER = logging.getLogger("marketpipe")


class FatalCaptureError(RuntimeError):
    """A durability failure that must stop capture instead of reconnecting."""


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="microseconds").replace("+00:00", "Z")


class MarketCollector:
    def __init__(self, *, ws_url: str, symbol: str, duration_seconds: float,
                 output_dir: Path, queue_capacity: int = 50_000,
                 ring_capacity: int = 20_000, reconnect_max_seconds: float = 15.0) -> None:
        self.ws_url = ws_url
        self.symbol = symbol
        self.duration_seconds = duration_seconds
        self.output_dir = output_dir
        self.queue = asyncio.Queue(maxsize=queue_capacity)
        self.metrics = RuntimeMetrics()
        self.raw_writer = DurableJsonlWriter(output_dir / "raw_frames.jsonl", fsync_every=1)
        self.processed_writer = DurableJsonlWriter(output_dir / "normalized_events.jsonl", fsync_every=100)
        self.factors_writer = DurableJsonlWriter(output_dir / "factors.jsonl", fsync_every=100)
        self.signals_writer = DurableJsonlWriter(output_dir / "signals.jsonl", fsync_every=100)
        self.pipeline = ProcessingPipeline(
            queue=self.queue, metrics=self.metrics,
            processed_writer=self.processed_writer,
            factors_writer=self.factors_writer,
            signals_writer=self.signals_writer,
            ring_capacity=ring_capacity,
        )
        self.reconnect_max_seconds = reconnect_max_seconds
        self._frame_seq = 0
        self._deadline = 0.0
        self._resource_task: asyncio.Task | None = None
        self._failure: str | None = None
        self._capture_duration_seconds = 0.0
        self.run_id = output_dir.name
        self.connection_generation = 0
        self.connection_id = ""

    async def _record_raw_frame(self, raw_text: str, received_ns: int, received_utc: str) -> dict[str, Any] | None:
        self._frame_seq += 1
        record: dict[str, Any] = {
            "record_type": "raw_websocket_frame",
            "run_id": self.run_id,
            "connection_id": self.connection_id,
            "connection_generation": self.connection_generation,
            "frame_seq": self._frame_seq,
            "received_at_utc": received_utc,
            "received_monotonic_ns": received_ns,
            "received_utc": received_utc,
            "received_ns": received_ns,
            "payload": raw_text,
            "raw_payload": raw_text,
            "payload_sha256": hashlib.sha256(raw_text.encode("utf-8")).hexdigest(),
            "payload_bytes": len(raw_text.encode("utf-8")),
        }
        # The exact wire text is durable before the frame is parsed or queued.
        try:
            await self.raw_writer.write(record, force_sync=True)
        except OSError as exc:
            raise FatalCaptureError(f"raw journal write/fsync failed: {exc}") from exc
        self.metrics.counters["frames_received"] += 1
        try:
            payload = json.loads(raw_text)
            return payload if isinstance(payload, dict) else None
        except json.JSONDecodeError:
            self.metrics.counters["malformed_frames"] += 1
            LOGGER.exception("malformed websocket JSON frame_seq=%s", self._frame_seq)
            return None

    async def _handle_message(self, raw_text: str) -> None:
        received_ns = time.perf_counter_ns()
        received_utc = utc_now()
        payload = await self._record_raw_frame(raw_text, received_ns, received_utc)
        if payload is None:
            return
        if payload.get("e") == "trade":
            trades = [payload]
        elif payload.get("channel") == "trade" and isinstance(payload.get("data"), list):
            trades = payload["data"]
        else:
            if payload.get("success") is False:
                LOGGER.error("subscription/server error payload=%s", payload)
            return
        source_type = str(payload.get("type", payload.get("e", "update")))
        for raw_trade in trades:
            if not isinstance(raw_trade, dict):
                self.metrics.counters["malformed_frames"] += 1
                continue
            if str(raw_trade.get("s", raw_trade.get("symbol", ""))).upper() != self.symbol.upper():
                continue
            try:
                event = parse_trade(
                    raw_trade, received_utc=received_utc, received_ns=received_ns,
                    frame_seq=self._frame_seq, source_type=source_type,
                )
            except (KeyError, TypeError, ValueError) as exc:
                self.metrics.counters["malformed_frames"] += 1
                LOGGER.warning("invalid trade frame_seq=%s error=%s", self._frame_seq, exc)
                continue
            self.metrics.counters["trade_events_seen"] += 1
            if self.queue.full():
                self.metrics.counters["backpressure_events"] += 1
            await self.queue.put((event, time.time_ns()))
            self.metrics.queue_high_water = max(self.metrics.queue_high_water, self.queue.qsize())

    async def _consume_connection(self) -> None:
        backoff = 1.0
        while time.monotonic() < self._deadline:
            try:
                self.connection_generation += 1
                self.connection_id = f"{self.run_id}-conn-{self.connection_generation}-{uuid.uuid4().hex[:8]}"
                remaining = max(0.1, self._deadline - time.monotonic())
                async with websockets.connect(
                    self.ws_url, open_timeout=min(10.0, remaining),
                    close_timeout=min(3.0, remaining),
                    ping_interval=20, ping_timeout=20, max_size=8 * 1024 * 1024,
                ) as websocket:
                    if "binance" in self.ws_url or "stream.binance.com" in self.ws_url:
                        request = {"method": "SUBSCRIBE", "params": [f"{self.symbol.lower()}@trade"], "id": 1}
                    else:
                        request = {"method": "subscribe", "params": {"channel": "trade", "symbol": [self.symbol], "snapshot": True}, "req_id": 1}
                    await websocket.send(json.dumps(request, separators=(",", ":")))
                    LOGGER.info("websocket connected; subscribe sent symbol=%s", self.symbol)
                    backoff = 1.0
                    while time.monotonic() < self._deadline:
                        timeout = min(2.0, max(0.1, self._deadline - time.monotonic()))
                        try:
                            raw = await asyncio.wait_for(websocket.recv(), timeout=timeout)
                        except TimeoutError:
                            continue
                        if isinstance(raw, bytes):
                            raw = raw.decode("utf-8", errors="replace")
                        await self._handle_message(raw)
                if time.monotonic() < self._deadline:
                    self.metrics.counters["disconnects"] += 1
                    LOGGER.warning("websocket closed before duration deadline")
            except asyncio.CancelledError:
                raise
            except FatalCaptureError as exc:
                self._failure = f"{type(exc).__name__}: {exc}"
                LOGGER.exception("fatal capture persistence failure; stopping capture")
                break
            except Exception as exc:
                self.metrics.counters["disconnects"] += 1
                status = getattr(getattr(exc, "response", None), "status_code", None)
                if status == 451:
                    self._failure = "Binance WebSocket HTTP 451: geographic access restriction"
                LOGGER.warning("websocket failure type=%s error=%s", type(exc).__name__, exc)
            if time.monotonic() < self._deadline:
                self.metrics.counters["reconnects"] += 1
                await asyncio.sleep(min(backoff, self.reconnect_max_seconds))
                backoff = min(backoff * 2.0, self.reconnect_max_seconds)

    async def _sample_resources(self) -> None:
        while time.monotonic() < self._deadline:
            self.metrics.sample_resources()
            await asyncio.sleep(1.0)

    async def run(self) -> dict[str, Any]:
        self.output_dir.mkdir(parents=True, exist_ok=True)
        capture_started = time.monotonic()
        self._deadline = capture_started + self.duration_seconds
        LOGGER.info("capture start symbol=%s duration_seconds=%s endpoint=%s", self.symbol,
                    self.duration_seconds, self.ws_url)
        pipeline_task = asyncio.create_task(self.pipeline.run(), name="marketpipe-pipeline")
        collector_task = asyncio.create_task(self._consume_connection(), name="marketpipe-collector")
        resource_task = asyncio.create_task(self._sample_resources(), name="marketpipe-resource-sampler")
        pipeline_failed = False
        try:
            done, _ = await asyncio.wait(
                {collector_task, pipeline_task}, return_when=asyncio.FIRST_COMPLETED
            )
            if pipeline_task in done:
                pipeline_error = pipeline_task.exception() if not pipeline_task.cancelled() else None
                if pipeline_error is not None:
                    pipeline_failed = True
                    self._failure = f"pipeline failure: {type(pipeline_error).__name__}: {pipeline_error}"
                    LOGGER.error("processing pipeline failed; stopping capture type=%s error=%s",
                                 type(pipeline_error).__name__, pipeline_error)
                    if not collector_task.done():
                        collector_task.cancel()
                elif not collector_task.done():
                    pipeline_failed = True
                    self._failure = "processing pipeline stopped unexpectedly"
                    collector_task.cancel()
            if not collector_task.done():
                try:
                    await collector_task
                except asyncio.CancelledError:
                    if not pipeline_failed:
                        raise
            elif not collector_task.cancelled():
                await collector_task
        except asyncio.CancelledError:
            self._failure = self._failure or "capture cancelled"
            raise
        except Exception as exc:
            self._failure = f"{type(exc).__name__}: {exc}"
            LOGGER.exception("collector failed")
        finally:
            self._capture_duration_seconds = max(0.0, time.monotonic() - capture_started)
            if pipeline_task.done() and not pipeline_task.cancelled() and pipeline_task.exception() is not None:
                pipeline_failed = True
            if pipeline_failed:
                # The raw journal remains the source of truth; report any queued events
                # that could not reach the derived pipeline instead of hanging on join().
                self.metrics.counters["dropped_events"] += self.queue.qsize() + 1
                while not self.queue.empty():
                    try:
                        self.queue.get_nowait()
                        self.queue.task_done()
                    except asyncio.QueueEmpty:
                        break
            else:
                await self.queue.join()
                await self.pipeline.stop()
                if not pipeline_task.done():
                    await pipeline_task
            if not collector_task.done():
                collector_task.cancel()
                try:
                    await collector_task
                except asyncio.CancelledError:
                    pass
            resource_task.cancel()
            try:
                await resource_task
            except asyncio.CancelledError:
                pass
            for writer in (self.raw_writer, self.processed_writer, self.factors_writer, self.signals_writer):
                await writer.close()

        processing_elapsed = time.monotonic() - capture_started
        summary = self.metrics.summary(
            raw_writer=self.raw_writer, processed_writer=self.processed_writer,
            factors_writer=self.factors_writer, signals_writer=self.signals_writer,
            duration_target=self.duration_seconds, queue_capacity=self.queue.maxsize,
            ring_capacity=self.pipeline.ring.maxlen or 0,
            capture_duration_seconds=self._capture_duration_seconds,
            processing_elapsed_seconds=processing_elapsed,
        )
        duration_ok = summary["duration_actual_seconds"] >= self.duration_seconds * 0.99
        data_ok = summary["trades_processed"] > 0 and summary["raw_frames_records"] > 0
        no_loss = summary["dropped_events"] == 0 and self.metrics.counters["processing_failures"] == 0
        blocked = self._failure is not None and ("HTTP 451" in self._failure or "geographic" in self._failure.lower())
        if blocked:
            result = "BLOCKED"
        elif self._failure is not None or not duration_ok or not data_ok or not no_loss:
            result = "FAIL"
        elif summary["disconnects"] > 0 or summary["malformed_frames"] > 0:
            result = "PARTIAL"
        else:
            result = "PASS"
        summary.update({
            "symbol": self.symbol,
            "websocket_url": self.ws_url,
            "failure": self._failure,
            "quality_checks": {
                "duration_at_least_99_percent": duration_ok,
                "at_least_one_trade_processed": data_ok,
                "zero_dropped_events": no_loss,
                "zero_processing_failures": self.metrics.counters["processing_failures"] == 0,
                "no_disconnects": summary["disconnects"] == 0,
                "no_malformed_frames": summary["malformed_frames"] == 0,
            },
            "result": result,
        })
        # Summary itself is durable before process exit.
        summary_path = self.output_dir / "summary.json"
        LOGGER.info("capture finished result=%s trades=%s", summary["result"], summary["trades_processed"])
        summary["artifact_sha256"] = artifact_hashes(self.output_dir)
        with summary_path.open("w", encoding="utf-8") as stream:
            json.dump(summary, stream, ensure_ascii=False, indent=2, allow_nan=False)
            stream.write("\n")
            stream.flush()
            import os
            os.fsync(stream.fileno())
        write_report(self.output_dir, summary)
        return summary
