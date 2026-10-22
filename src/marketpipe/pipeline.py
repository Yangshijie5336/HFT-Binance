from __future__ import annotations

import asyncio
import time
from collections import OrderedDict, deque
from typing import Any

from .indicators import FactorEngine
from .metrics import RuntimeMetrics
from .models import TradeEvent
from .storage import DurableJsonlWriter
from .strategy import DemoStrategy


class ProcessingPipeline:
    def __init__(self, *, queue: asyncio.Queue, metrics: RuntimeMetrics,
                 processed_writer: DurableJsonlWriter, factors_writer: DurableJsonlWriter,
                 signals_writer: DurableJsonlWriter, ring_capacity: int = 20_000,
                 dedup_capacity: int = 100_000) -> None:
        self.queue = queue
        self.metrics = metrics
        self.processed_writer = processed_writer
        self.factors_writer = factors_writer
        self.signals_writer = signals_writer
        self.ring: deque[TradeEvent] = deque(maxlen=ring_capacity)
        self.seen: OrderedDict[str, None] = OrderedDict()
        self.dedup_capacity = dedup_capacity
        self.factor_engine = FactorEngine()
        self.strategy = DemoStrategy()
        self._stop = asyncio.Event()

    async def stop(self) -> None:
        self._stop.set()

    async def run(self) -> None:
        while not self._stop.is_set() or not self.queue.empty():
            try:
                item, enqueued_ns = await asyncio.wait_for(self.queue.get(), timeout=0.2)
            except TimeoutError:
                continue
            try:
                await self.process_one(item, enqueued_ns)
            except Exception:
                self.metrics.counters["processing_failures"] += 1
                raise
            finally:
                self.queue.task_done()

    async def process_one(self, event: TradeEvent, enqueued_ns: int) -> None:
        if event.event_key in self.seen:
            self.metrics.counters["duplicates"] += 1
            return
        self.seen[event.event_key] = None
        if len(self.seen) > self.dedup_capacity:
            self.seen.popitem(last=False)
        self.ring.append(event)
        minute_key = event.exchange_timestamp[:16]
        second_key = event.exchange_timestamp[:19]
        self.metrics.activity_by_minute[minute_key] += 1
        self.metrics.activity_by_second[second_key] += 1
        process_ns = time.perf_counter_ns()
        self.metrics.counters["trades_processed"] += 1
        self.metrics.queue_wait.add_ns(process_ns - enqueued_ns)
        self.metrics.process_latency.add_ns(process_ns - event.received_ns)

        processed = {
            "stage": "normalized_event",
            "event": event.to_dict(),
            "processed_ns": process_ns,
            "ring_size": len(self.ring),
        }
        await self.processed_writer.write(processed)

        factors = self.factor_engine.update(event)
        factor_record = factors.to_dict() | {"stage": "factor_snapshot", "computed_ns": time.perf_counter_ns()}
        await self.factors_writer.write(factor_record)

        signal = self.strategy.evaluate(factors, created_ns=time.perf_counter_ns())
        await self.signals_writer.write(signal.to_dict())
        self.metrics.signal_latency.add_ns(signal.created_ns - event.received_ns)
        self.metrics.counters["signals_emitted"] += 1
