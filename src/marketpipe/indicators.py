from __future__ import annotations

from collections import deque
from dataclasses import dataclass

from .models import TradeEvent


@dataclass(frozen=True, slots=True)
class FactorSnapshot:
    symbol: str
    trade_id: str
    price: float
    qty: float
    trade_imbalance: float
    return_n: float
    trade_rate_10s: float
    avg_trade_size: float
    window_trades: int
    exchange_ts_ns: int
    received_ns: int

    def to_dict(self) -> dict:
        return {
            "symbol": self.symbol,
            "trade_id": self.trade_id,
            "price": self.price,
            "qty": self.qty,
            "trade_imbalance": self.trade_imbalance,
            "return_n": self.return_n,
            "trade_rate_10s": self.trade_rate_10s,
            "avg_trade_size": self.avg_trade_size,
            "window_trades": self.window_trades,
            "exchange_ts_ns": self.exchange_ts_ns,
            "received_ns": self.received_ns,
        }


class FactorEngine:
    """Rolling trade-flow factors; windows use exchange timestamps for replay parity."""

    def __init__(self, *, window_seconds: float = 30.0, return_lookback: int = 20) -> None:
        self.window_ns = int(window_seconds * 1_000_000_000)
        self.return_lookback = return_lookback
        self.window: deque[tuple[int, TradeEvent]] = deque()
        self.ten_second_times: deque[int] = deque()
        self.recent_prices: deque[float] = deque(maxlen=return_lookback + 1)
        self.last_event_ts_ns = 0
        self.buy_qty = 0.0
        self.sell_qty = 0.0

    def update(self, event: TradeEvent) -> FactorSnapshot:
        # Use local receive time as the window clock so out-of-order exchange timestamps
        # cannot leave stale elements trapped behind newer deque entries. The original
        # exchange timestamp is still preserved on every event and factor record.
        event_ts = max(event.received_ns, self.last_event_ts_ns)
        self.last_event_ts_ns = event_ts
        self.window.append((event_ts, event))
        if event.side == "buy":
            self.buy_qty += event.qty
        else:
            self.sell_qty += event.qty
        cutoff = event_ts - self.window_ns
        while self.window and self.window[0][0] < cutoff:
            _, expired = self.window.popleft()
            if expired.side == "buy":
                self.buy_qty -= expired.qty
            else:
                self.sell_qty -= expired.qty

        self.ten_second_times.append(event_ts)
        cutoff_10s = event_ts - 10_000_000_000
        while self.ten_second_times and self.ten_second_times[0] < cutoff_10s:
            self.ten_second_times.popleft()

        self.recent_prices.append(event.price)
        total_qty = self.buy_qty + self.sell_qty
        imbalance = (self.buy_qty - self.sell_qty) / total_qty if total_qty > 0 else 0.0
        prices = list(self.recent_prices)
        ret = prices[-1] / prices[0] - 1.0 if len(prices) >= 2 and prices[0] else 0.0
        count_10s = len(self.ten_second_times)
        avg_size = total_qty / len(self.window) if self.window else 0.0
        return FactorSnapshot(
            symbol=event.symbol,
            trade_id=event.trade_id,
            price=event.price,
            qty=event.qty,
            trade_imbalance=imbalance,
            return_n=ret,
            trade_rate_10s=count_10s / 10.0,
            avg_trade_size=avg_size,
            window_trades=len(self.window),
            exchange_ts_ns=event.exchange_ts_ns,
            received_ns=event.received_ns,
        )
