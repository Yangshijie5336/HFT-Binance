from marketpipe.indicators import FactorEngine
from marketpipe.models import TradeEvent
from marketpipe.strategy import DemoStrategy
import pytest


def make_trade(i, side, price, qty=1.0):
    ts = 1_800_000_000_000_000_000 + i * 1_000_000_000
    return TradeEvent("BTC/USD", side, price, qty, str(i), "2026-10-22T03:00:00Z", ts,
                      "2026-10-22T03:00:00Z", ts, i, "update")


def test_factor_imbalance_and_return():
    engine = FactorEngine(window_seconds=30, return_lookback=3)
    a = engine.update(make_trade(1, "buy", 100.0, 3.0))
    b = engine.update(make_trade(2, "buy", 101.0, 2.0))
    assert a.trade_imbalance == 1.0
    assert b.trade_imbalance == 1.0
    assert b.return_n == pytest.approx(0.01)
    assert b.window_trades == 2


def test_strategy_emits_signal_without_execution():
    engine = FactorEngine()
    factors = engine.update(make_trade(1, "buy", 100.0, 3.0))
    factors = engine.update(make_trade(2, "buy", 101.0, 3.0))
    signal = DemoStrategy().evaluate(factors, created_ns=123)
    assert signal.action == "BUY_SIGNAL"
    assert signal.to_dict()["execution"] == "DISABLED_DEMO_ONLY"
