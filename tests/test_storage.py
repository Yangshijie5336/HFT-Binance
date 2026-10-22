import json
from pathlib import Path

import pytest

from marketpipe.storage import DurableJsonlWriter, read_jsonl


@pytest.mark.asyncio
async def test_writer_persists_jsonl(tmp_path: Path):
    path = tmp_path / "events.jsonl"
    writer = DurableJsonlWriter(path, fsync_every=2)
    await writer.write({"n": 1})
    await writer.write({"n": 2})
    await writer.close()
    records = list(read_jsonl(path))
    assert records == [{"n": 1}, {"n": 2}]


def test_read_jsonl_rejects_corrupt_line(tmp_path: Path):
    path = tmp_path / "broken.jsonl"
    path.write_text('{"ok":1}\nnope\n', encoding="utf-8")
    with pytest.raises(ValueError, match="invalid JSONL"):
        list(read_jsonl(path))
