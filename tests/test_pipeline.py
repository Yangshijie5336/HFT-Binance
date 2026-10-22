import asyncio
from pathlib import Path

import pytest

from marketpipe.metrics import RuntimeMetrics
from marketpipe.models import TradeEvent
from marketpipe.pipeline import ProcessingPipeline
from marketpipe.storage import DurableJsonlWriter, read_jsonl


@pytest.mark.asyncio
async def test_pipeline_deduplicates_and_emits_all_stages(tmp_path: Path):
    q = asyncio.Queue(maxsize=10)
    writers = [DurableJsonlWriter(tmp_path / name, fsync_every=1)
               for name in ("processed.jsonl", "factors.jsonl", "signals.jsonl")]
    pipeline = ProcessingPipeline(queue=q, metrics=RuntimeMetrics(), processed_writer=writers[0],
                                  factors_writer=writers[1], signals_writer=writers[2], ring_capacity=10)
    task = asyncio.create_task(pipeline.run())
    ts = 1_800_000_000_000_000_000
    event = TradeEvent("BTC/USD", "buy", 100.0, 1.0, "same", "2026-10-22T03:00:00Z", ts,
                       "2026-10-22T03:00:00Z", ts, 1, "update")
    await q.put((event, ts))
    await q.put((event, ts))
    await q.join()
    await pipeline.stop()
    await task
    for writer in writers:
        await writer.close()
    assert len(list(read_jsonl(tmp_path / "processed.jsonl"))) == 1
    assert len(list(read_jsonl(tmp_path / "factors.jsonl"))) == 1
    assert len(list(read_jsonl(tmp_path / "signals.jsonl"))) == 1
    assert pipeline.metrics.counters["duplicates"] == 1
