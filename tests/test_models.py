from marketpipe.models import parse_timestamp_ns, parse_trade


def test_parse_timestamp_z():
    assert parse_timestamp_ns("2026-10-22T03:00:00.000000Z") > 0


def test_parse_trade_validates_and_normalizes():
    trade = parse_trade({
        "symbol": "BTC/USD", "side": "buy", "price": 67000.1, "qty": 0.01,
        "trade_id": 123, "timestamp": "2026-10-22T03:00:00.000000Z",
    }, received_utc="2026-10-22T03:00:00Z", received_ns=1_800_000_000_000_000_000,
       frame_seq=7, source_type="update")
    assert trade.event_key == "BTC/USD:123"
    assert trade.side == "buy"
    assert trade.frame_seq == 7
