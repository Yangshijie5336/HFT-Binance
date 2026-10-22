.PHONY: test lint capture replay docker-build

test:
	python -m pytest -q

lint:
	python -m compileall -q src tests

capture:
	python -m marketpipe.cli capture --duration 900 --symbol BTC/USD

replay:
	python -m marketpipe.cli replay --input data/runs/<run-id>/raw_frames.jsonl

docker-build:
	docker build -t marketpipe-hft-demo:0.1.0 .
