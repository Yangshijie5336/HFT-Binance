# Multi-stage build for the C++20 MarketPipe collector.
FROM debian:bookworm-slim AS build

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       build-essential cmake ninja-build libboost-all-dev libssl-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY cpp ./cpp
COPY certs ./certs
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j"$(nproc)" \
    && ctest --test-dir build --output-on-failure

FROM debian:bookworm-slim AS runtime

RUN apt-get update \
    && apt-get install -y --no-install-recommends libssl3 ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --gid 10001 marketpipe \
    && useradd --uid 10001 --gid 10001 --create-home --shell /usr/sbin/nologin marketpipe

WORKDIR /app
COPY --from=build /src/build/marketpipe /usr/local/bin/marketpipe
COPY --from=build /src/certs /app/certs

RUN mkdir -p /app/data \
    && chown -R marketpipe:marketpipe /app

ENV SSL_CERT_FILE=/app/certs/cacert.pem
VOLUME ["/app/data"]
USER marketpipe
STOPSIGNAL SIGTERM
ENTRYPOINT ["marketpipe"]
CMD ["--help"]
