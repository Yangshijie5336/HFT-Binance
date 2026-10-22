from __future__ import annotations

from dataclasses import dataclass
from .indicators import FactorSnapshot


@dataclass(frozen=True, slots=True)
class Signal:
    symbol: str
    trade_id: str
    action: str
    reason: str
    score: float
    price: float
    created_ns: int

    def to_dict(self) -> dict:
        return {
            "symbol": self.symbol,
            "trade_id": self.trade_id,
            "action": self.action,
            "reason": self.reason,
            "score": self.score,
            "price": self.price,
            "created_ns": self.created_ns,
            "execution": "DISABLED_DEMO_ONLY",
        }


class DemoStrategy:
    """Deterministic demonstration strategy. It emits signals only, never orders."""

    def __init__(self, *, imbalance_threshold: float = 0.15,
                 return_threshold: float = 0.0001) -> None:
        self.imbalance_threshold = imbalance_threshold
        self.return_threshold = return_threshold

    def evaluate(self, factors: FactorSnapshot, *, created_ns: int) -> Signal:
        imbalance = factors.trade_imbalance
        ret = factors.return_n
        if imbalance >= self.imbalance_threshold and ret >= self.return_threshold:
            action, reason = "BUY_SIGNAL", "positive_trade_imbalance_and_momentum"
        elif imbalance <= -self.imbalance_threshold and ret <= -self.return_threshold:
            action, reason = "SELL_SIGNAL", "negative_trade_imbalance_and_momentum"
        else:
            action, reason = "HOLD", "thresholds_not_aligned"
        score = max(-1.0, min(1.0, imbalance * 0.7 + ret * 1000.0 * 0.3))
        return Signal(
            symbol=factors.symbol,
            trade_id=factors.trade_id,
            action=action,
            reason=reason,
            score=score,
            price=factors.price,
            created_ns=created_ns,
        )
