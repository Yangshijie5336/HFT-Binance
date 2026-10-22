from __future__ import annotations

import asyncio
import json
import os
import threading
from pathlib import Path
from typing import Any


class DurableJsonlWriter:
    """Append-only JSONL writer. Each line is flushed; fsync runs every N records.

    Use fsync_every=1 for raw frames: once write() returns, that frame has been
    handed to the OS and fsync has completed. Derived streams can batch fsyncs.
    """

    def __init__(self, path: Path, *, fsync_every: int = 1) -> None:
        if fsync_every < 1:
            raise ValueError("fsync_every must be >= 1")
        path.parent.mkdir(parents=True, exist_ok=True)
        self.path = path
        self.fsync_every = fsync_every
        self._file = path.open("ab", buffering=0)
        self._lock = threading.Lock()
        self._records_since_sync = 0
        self.records_written = 0
        self.bytes_written = 0

    def _write_sync(self, record: dict[str, Any], force_sync: bool = False) -> None:
        line = (json.dumps(record, ensure_ascii=False, separators=(",", ":"),
                            allow_nan=False) + "\n").encode("utf-8")
        with self._lock:
            written = self._file.write(line)
            if written != len(line):
                raise OSError(f"short write to {self.path}: {written}/{len(line)}")
            self.records_written += 1
            self.bytes_written += written
            self._records_since_sync += 1
            if force_sync or self._records_since_sync >= self.fsync_every:
                self._file.flush()
                os.fsync(self._file.fileno())
                self._records_since_sync = 0

    async def write(self, record: dict[str, Any], *, force_sync: bool = False) -> None:
        await asyncio.to_thread(self._write_sync, record, force_sync)

    def _close_sync(self) -> None:
        with self._lock:
            if not self._file.closed:
                self._file.flush()
                os.fsync(self._file.fileno())
                self._file.close()

    async def close(self) -> None:
        await asyncio.to_thread(self._close_sync)


def read_jsonl(path: Path):
    with path.open("r", encoding="utf-8") as stream:
        for line_no, line in enumerate(stream, start=1):
            if not line.strip():
                continue
            try:
                yield json.loads(line)
            except json.JSONDecodeError as exc:
                raise ValueError(f"invalid JSONL at {path}:{line_no}: {exc}") from exc
