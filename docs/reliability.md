# Reliability and recovery

Every WebSocket application frame is journaled as JSONL and fsynced before parsing. Queue capacity is bounded; `put()` applies backpressure and no event is silently discarded. A raw journal remains replayable after a processing failure. Reconnects create a new `connection_generation` and `connection_id`, while the global frame sequence remains monotonic. Unknown gaps during a disconnect are reported as unknown and are not treated as zero loss.

Shutdown drains the processing queue, fsyncs and closes all writers, then writes `summary.json` and `performance_report.md`. A truncated final JSONL line causes replay validation to fail rather than being silently ignored.
