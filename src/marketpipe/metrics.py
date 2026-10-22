from __future__ import annotations

import os
import time
from collections import Counter
from dataclasses import dataclass, field
from typing import Any

import psutil


@dataclass
class LatencySamples:
    max_samples: int = 200_000
    every_n: int = 5
    seen: int = 0
    values_us: list[float] = field(default_factory=list)

    def add_ns(self, value_ns: int) -> None:
        self.seen += 1
        if self.seen % self.every_n == 0 and len(self.values_us) < self.max_samples:
            self.values_us.append(max(0, value_ns) / 1_000.0)

    def summary(self) -> dict[str, Any]:
        if not self.values_us:
            return {"samples": 0, "p50_us": None, "p95_us": None, "p99_us": None, "max_us": None}
        ordered = sorted(self.values_us)
        def percentile(p: float) -> float:
            index = min(len(ordered) - 1, max(0, int((len(ordered) - 1) * p)))
            return round(ordered[index], 3)
        return {
            "samples": len(ordered),
            "p50_us": percentile(0.50),
            "p95_us": percentile(0.95),
            "p99_us": percentile(0.99),
            "max_us": round(ordered[-1], 3),
            "sampling": f"every_{self.every_n}_events_capped_at_{self.max_samples}",
        }


@dataclass
class RuntimeMetrics:
    started_monotonic: float = field(default_factory=time.monotonic)
    started_wall_ns: int = field(default_factory=time.time_ns)
    counters: Counter = field(default_factory=Counter)
    queue_high_water: int = 0
    queue_wait: LatencySamples = field(default_factory=LatencySamples)
    process_latency: LatencySamples = field(default_factory=LatencySamples)
    signal_latency: LatencySamples = field(default_factory=LatencySamples)
    cpu_samples: list[float] = field(default_factory=list)
    rss_samples: list[int] = field(default_factory=list)
    activity_by_minute: Counter = field(default_factory=Counter)
    activity_by_second: Counter = field(default_factory=Counter)

    def sample_resources(self) -> None:
        process = psutil.Process(os.getpid())
        self.cpu_samples.append(process.cpu_percent(interval=None))
        self.rss_samples.append(process.memory_info().rss)

    def summary(self, *, raw_writer, processed_writer, factors_writer, signals_writer,
                duration_target: float, queue_capacity: int, ring_capacity: int,
                capture_duration_seconds: float | None = None,
                processing_elapsed_seconds: float | None = None) -> dict[str, Any]:
        processing_elapsed = processing_elapsed_seconds or (time.monotonic() - self.started_monotonic)
        capture_elapsed = capture_duration_seconds if capture_duration_seconds is not None else processing_elapsed
        events = self.counters["trades_processed"]
        minute_counts = dict(sorted(self.activity_by_minute.items()))
        second_counts = list(self.activity_by_second.values())
        return {
            "duration_target_seconds": duration_target,
            "duration_actual_seconds": round(capture_elapsed, 6),
            "processing_elapsed_seconds": round(processing_elapsed, 6),
            "started_wall_ns": self.started_wall_ns,
            "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(self.started_wall_ns / 1e9)),
            "finished_wall_ns": time.time_ns(),
            "activity_by_minute_utc": minute_counts,
            "average_trades_per_minute": round(events / (capture_elapsed / 60), 3) if capture_elapsed > 0 else 0,
            "max_trades_per_minute": max(minute_counts.values(), default=0),
            "max_trades_per_second": max(second_counts, default=0),
            "frames_received": self.counters["frames_received"],
            "trade_events_seen": self.counters["trade_events_seen"],
            "trades_processed": events,
            "duplicates": self.counters["duplicates"],
            "malformed_frames": self.counters["malformed_frames"],
            "reconnects": self.counters["reconnects"],
            "disconnects": self.counters["disconnects"],
            "backpressure_events": self.counters["backpressure_events"],
            "dropped_events": self.counters["dropped_events"],
            "processing_failures": self.counters["processing_failures"],
            "queue_capacity": queue_capacity,
            "queue_high_water": self.queue_high_water,
            "ring_capacity": ring_capacity,
            "raw_frames_bytes": raw_writer.bytes_written,
            "raw_frames_records": raw_writer.records_written,
            "processed_bytes": processed_writer.bytes_written,
            "factor_bytes": factors_writer.bytes_written,
            "signal_bytes": signals_writer.bytes_written,
            "throughput_trades_per_second": round(events / processing_elapsed, 3) if processing_elapsed > 0 else 0,
            "ingress_trades_per_second": round(self.counters["trade_events_seen"] / capture_elapsed, 3) if capture_elapsed > 0 else 0,
            "queue_wait_latency": self.queue_wait.summary(),
            "receive_to_process_latency": self.process_latency.summary(),
            "receive_to_signal_latency": self.signal_latency.summary(),
            "cpu_percent_samples": len(self.cpu_samples),
            "cpu_percent_avg": round(sum(self.cpu_samples) / len(self.cpu_samples), 3) if self.cpu_samples else None,
            "rss_bytes_peak": max(self.rss_samples) if self.rss_samples else None,
            "raw_fsync_policy": "each_websocket_frame",
            "derived_fsync_policy": "every_100_records_plus_shutdown",
            "execution_enabled": False,
        }
