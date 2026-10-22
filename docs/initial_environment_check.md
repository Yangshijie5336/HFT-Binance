# Initial environment check

Date: 2026-10-22.

- Python: 3.13.5 in the current authoring container; project target: Python 3.12+.
- `websockets` available in the authoring container: 16.0.
- `psutil` available in the authoring container: 7.2.2.
- Attempted public WebSocket handshake to `wss://ws.kraken.com/v2`; name resolution failed with `gaierror(-3, 'Temporary failure in name resolution')` before a handshake was established.
- Docker CLI is not installed in the authoring container; Docker image build was not executed here.
- No live market data was collected in this environment. A real 900-second sample and its performance metrics must be captured on a host with working DNS/network access; synthetic tests are not a substitute.

After the initial environment check, the following code-level validations completed:

- Unit/integration tests: 8 passed.
- `compileall`: passed.
- Editable package install and installed CLI help: passed.
- TOML and Docker Compose YAML parsing: passed.
- GitHub workflow files parsed as generic YAML; no remote GitHub Actions schema validation was possible.
- A one-second negative-path capture against a deliberately nonexistent hostname returned exit code 2, recorded the DNS error, generated a summary/report, and correctly reported `FAIL` with zero trades. This was a failure-path test, not live market data.
