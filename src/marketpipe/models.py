from __future__ import annotations

from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from typing import Any
from decimal import Decimal, InvalidOperation


def parse_timestamp_ns(value: str) -> int:
    normalized = value[:-1] + "+00:00" if value.endswith("Z") else value
    dt = datetime.fromisoformat(normalized)
    if dt.tzinfo is None:
        raise ValueError("exchange timestamp must include timezone")
    utc_dt = dt.astimezone(timezone.utc)
    epoch = datetime(1970, 1, 1, tzinfo=timezone.utc)
    delta = utc_dt - epoch
    return ((delta.days * 86_400 + delta.seconds) * 1_000_000 + delta.microseconds) * 1_000


@dataclass(frozen=True, slots=True)
class TradeEvent:
    symbol: str
    side: str
    price: float
    qty: float
    trade_id: str
    exchange_timestamp: str
    exchange_ts_ns: int
    received_utc: str
    received_ns: int
    frame_seq: int
    source_type: str
    price_text: str = ""
    qty_text: str = ""

    @property
    def event_key(self) -> str:
        return f"{self.symbol}:{self.trade_id}"

    def to_dict(self) -> dict[str, Any]:
        return asdict(self) | {"event_key": self.event_key}


def parse_trade(raw: dict[str, Any], *, received_utc: str, received_ns: int,
                frame_seq: int, source_type: str) -> TradeEvent:
    # Binance Spot @trade fields are e/E/s/t/p/q/T/m/M. Keep support for the
    # original normalized Kraken-shaped fixture format for backwards compatibility.
    is_binance = "e" in raw or "p" in raw or "m" in raw
    symbol = str(raw.get("s", raw.get("symbol"))).upper()
    if is_binance:
        if raw.get("e") != "trade":
            raise ValueError("unsupported Binance event type")
        side = "sell" if bool(raw["m"]) else "buy"
        price_raw, qty_raw = raw["p"], raw["q"]
        trade_id = str(raw["t"])
        timestamp_ms = int(raw["T"])
        timestamp = datetime.fromtimestamp(timestamp_ms / 1000, tz=timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")
    else:
        side = str(raw["side"]).lower()
        price_raw, qty_raw = raw["price"], raw["qty"]
        trade_id = str(raw["trade_id"])
        timestamp = str(raw["timestamp"])
    if side not in {"buy", "sell"}:
        raise ValueError(f"unsupported trade side: {side}")
    try:
        price_decimal = Decimal(str(price_raw))
        qty_decimal = Decimal(str(qty_raw))
    except (InvalidOperation, ValueError) as exc:
        raise ValueError("price and qty must be numeric") from exc
    if not price_decimal.is_finite() or not qty_decimal.is_finite() or price_decimal <= 0 or qty_decimal <= 0:
        raise ValueError("price and qty must be positive")
    exchange_ts_ns = parse_timestamp_ns(timestamp)
    return TradeEvent(
        symbol=symbol,
        side=side,
        price=float(price_decimal),
        qty=float(qty_decimal),
        trade_id=trade_id,
        exchange_timestamp=timestamp,
        exchange_ts_ns=exchange_ts_ns,
        received_utc=received_utc,
        received_ns=received_ns,
        frame_seq=frame_seq,
        source_type=source_type,
        price_text=str(price_raw),
        qty_text=str(qty_raw),
    )
