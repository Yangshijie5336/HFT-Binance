.PHONY: all configure build test capture report benchmark docker clean

BUILD_DIR ?= build

all: build

configure:
	cmake -S . -B $(BUILD_DIR) -G Ninja -DCMAKE_BUILD_TYPE=Release

build: configure
	cmake --build $(BUILD_DIR)

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

capture: build
	./$(BUILD_DIR)/marketpipe capture --symbol BTCUSDT --duration 900 --output-dir data/runs

report: build
	./$(BUILD_DIR)/marketpipe report --run-dir $(RUN_DIR)

benchmark: build
	./$(BUILD_DIR)/marketpipe benchmark --events 100000 --mode all --output-dir data/benchmarks

docker:
	docker build -t marketpipe-hft-demo:0.3.0 .

clean:
	rm -rf $(BUILD_DIR) data/benchmarks
