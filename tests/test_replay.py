import json
from pathlib import Path

import pytest

from marketpipe.replay import replay
from marketpipe.storage import read_jsonl


@pytest.mark.asyncio
async def test_replay_recreates_normalized_events_and_signals(tmp_path: Path):
    raw = tmp_path / "raw_frames.jsonl"
    trades = [
        {"symbol": "BTC/USD", "side": "buy", "price": 100.0, "qty": 2.0,
         "trade_id": 1, "timestamp": "2026-10-22T03:00:00.000000Z"},
        {"symbol": "BTC/USD", "side": "buy", "price": 101.0, "qty": 2.0,
         "trade_id": 2, "timestamp": "2026-10-22T03:00:01.000000Z"},
    ]
    frame = {"channel": "trade", "type": "update", "data": trades}
    raw.write_text(json.dumps({
        "record_type": "raw_websocket_frame", "frame_seq": 1,
        "received_utc": "2026-10-22T03:00:01Z",
        "received_ns": 1_800_000_000_000_000_000,
        "raw_payload": json.dumps(frame),
    }) + "\n", encoding="utf-8")

    out = tmp_path / "replay"
    summary = await replay(raw, out)
    assert summary["result"] == "PASS"
    assert summary["trades_processed"] == 2
    normalized = list(read_jsonl(out / "normalized_events.jsonl"))
    signals = list(read_jsonl(out / "signals.jsonl"))
    assert [row["event"]["trade_id"] for row in normalized] == ["1", "2"]
    assert len(signals) == 2
    assert all(row["execution"] == "DISABLED_DEMO_ONLY" for row in signals)
