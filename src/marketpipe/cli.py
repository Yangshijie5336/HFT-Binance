from __future__ import annotations

import argparse
import asyncio
import json
import logging
import os
import sys
from datetime import datetime, timezone
from pathlib import Path

from .collector import MarketCollector
from .replay import replay

DEFAULT_WS_URL = os.environ.get("MARKETPIPE_WS_URL", "wss://stream.binance.com:9443/ws/btcusdt@trade")
DEFAULT_SYMBOL = os.environ.get("MARKETPIPE_SYMBOL", "BTCUSDT")
DEFAULT_OUTPUT_DIR = Path(os.environ.get("MARKETPIPE_OUTPUT_DIR", "data/runs"))


class JsonLogFormatter(logging.Formatter):
    def format(self, record: logging.LogRecord) -> str:
        payload = {
            "timestamp": self.formatTime(record, "%Y-%m-%dT%H:%M:%S%z"),
            "level": record.levelname,
            "message": record.getMessage(),
        }
        if record.exc_info:
            payload["exception"] = self.formatException(record.exc_info)
        return json.dumps(payload, ensure_ascii=False, separators=(",", ":"))


class FsyncLogHandler(logging.FileHandler):
    def emit(self, record: logging.LogRecord) -> None:
        super().emit(record)
        self.flush()
        if self.stream:
            os.fsync(self.stream.fileno())


def setup_logging(output_dir: Path) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    logger = logging.getLogger("marketpipe")
    logger.setLevel(logging.INFO)
    logger.handlers.clear()
    formatter = JsonLogFormatter()
    handler = FsyncLogHandler(output_dir / "system.log", encoding="utf-8")
    handler.setFormatter(formatter)
    logger.addHandler(handler)
    console = logging.StreamHandler(sys.stdout)
    console.setFormatter(logging.Formatter("%(asctime)s %(levelname)s %(message)s"))
    logger.addHandler(console)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(prog="marketpipe", description="Durable real-time trade feed demo")
    sub = parser.add_subparsers(dest="command", required=True)
    capture_parser = sub.add_parser("capture", help="capture live public trade events")
    capture_parser.add_argument("--duration", type=float, default=float(os.environ.get("MARKETPIPE_DURATION_SECONDS", "900")))
    capture_parser.add_argument("--symbol", default=DEFAULT_SYMBOL)
    capture_parser.add_argument("--ws-url", default=DEFAULT_WS_URL)
    capture_parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT_DIR)
    capture_parser.add_argument("--queue-capacity", type=int, default=int(os.environ.get("MARKETPIPE_QUEUE_CAPACITY", "50000")))
    capture_parser.add_argument("--ring-capacity", type=int, default=int(os.environ.get("MARKETPIPE_RING_CAPACITY", "20000")))
    replay_parser = sub.add_parser("replay", help="replay a previously captured raw frame JSONL")
    replay_parser.add_argument("--input", type=Path, required=True)
    replay_parser.add_argument("--output-dir", type=Path, default=Path("data/replays") / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
    replay_parser.add_argument("--queue-capacity", type=int, default=50_000)
    replay_parser.add_argument("--ring-capacity", type=int, default=20_000)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if args.command == "capture":
        if args.duration <= 0:
            raise SystemExit("--duration must be positive")
        if args.queue_capacity < 1 or args.ring_capacity < 1:
            raise SystemExit("queue and ring capacities must be positive")
        run_id = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        output_dir = args.output_dir / f"{run_id}_{args.symbol.replace('/', '-') }"
        setup_logging(output_dir)
        collector = MarketCollector(
            ws_url=args.ws_url, symbol=args.symbol, duration_seconds=args.duration,
            output_dir=output_dir, queue_capacity=args.queue_capacity,
            ring_capacity=args.ring_capacity,
        )
        summary = asyncio.run(collector.run())
    else:
        setup_logging(args.output_dir)
        summary = asyncio.run(replay(args.input, args.output_dir,
                                     queue_capacity=args.queue_capacity,
                                     ring_capacity=args.ring_capacity))
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    if summary.get("result") not in {"PASS", "PARTIAL"}:
        raise SystemExit(2)


if __name__ == "__main__":
    main()
