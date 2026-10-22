FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ cmake ninja-build && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY cpp ./cpp
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build

FROM debian:bookworm-slim
RUN useradd --create-home --uid 10001 appuser
WORKDIR /app
COPY --from=build /src/build/marketpipe /usr/local/bin/marketpipe
RUN mkdir -p /app/data && chown -R appuser:appuser /app
USER appuser
VOLUME ["/app/data"]
ENTRYPOINT ["marketpipe"]
CMD ["--help"]
