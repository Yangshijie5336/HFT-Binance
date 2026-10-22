from marketpipe.models import parse_trade


def test_parse_binance_trade_semantics_and_decimal_text():
    event = parse_trade({
        "e": "trade", "E": 1792638000000, "s": "BTCUSDT", "t": 42,
        "p": "67000.12000000", "q": "0.01000000", "T": 1792638000000,
        "m": True, "M": True,
    }, received_utc="2026-10-22T03:00:00Z", received_ns=1,
       frame_seq=3, source_type="trade")
    assert event.symbol == "BTCUSDT"
    assert event.side == "sell"
    assert event.trade_id == "42"
    assert event.price_text == "67000.12000000"
    assert event.qty_text == "0.01000000"
