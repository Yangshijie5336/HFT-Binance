"""Synthetic capacity probe; output is never a live-market result."""
import json
import time
from pathlib import Path
from marketpipe.indicators import FactorEngine
from marketpipe.models import TradeEvent
from marketpipe.strategy import DemoStrategy

N = 100_000
engine = FactorEngine()
strategy = DemoStrategy()
start = time.perf_counter_ns()
for i in range(N):
    event = TradeEvent("BTCUSDT", "buy" if i % 2 == 0 else "sell", 67000.0 + i % 10,
                       0.001, str(i), "2026-10-22T03:00:00Z", 1_800_000_000_000_000_000 + i,
                       "2026-10-22T03:00:00Z", time.perf_counter_ns(), i, "synthetic")
    factors = engine.update(event)
    strategy.evaluate(factors, created_ns=time.perf_counter_ns())
elapsed = (time.perf_counter_ns() - start) / 1e9
result = {"result": "PASS", "benchmark_type": "SYNTHETIC_BENCHMARK", "events": N,
          "elapsed_seconds": round(elapsed, 6), "events_per_second": round(N / elapsed, 3),
          "data_source": "synthetic", "execution_enabled": False}
out = Path("data/synthetic_benchmark_summary.json")
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
print(json.dumps(result, indent=2))
