FROM python:3.12-slim AS runtime
ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    PIP_NO_CACHE_DIR=1
WORKDIR /app
RUN useradd --create-home --uid 10001 appuser
COPY pyproject.toml README.md ./
COPY src ./src
RUN pip install --no-cache-dir .
RUN mkdir -p /app/data/runs && chown -R appuser:appuser /app/data
USER appuser
VOLUME ["/app/data"]
ENTRYPOINT ["marketpipe"]
CMD ["capture", "--duration", "900", "--symbol", "BTCUSDT"]
