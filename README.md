# Binance USD-M perpetual market maker

C++17 market maker and historical backtester for Binance USDⓈ-M futures. Inventory-aware spread, fee floor, gross cap, reduce-only take profit, and stop logic are preserved as one-way quote planning.

## Endpoints

| Use | Host | When |
|---|---|---|
| Public REST | `https://fapi.binance.com` | Market data only. HTTP 451 is reported and not bypassed. |
| Public WebSocket | `wss://fstream.binance.com/public/stream` | Market data only. Reconnects up to three times inside the run window. |
| Demo REST orders | `https://demo-fapi.binance.com` | Only with `--demo`, after demo `/fapi/v1/ping` returns HTTP 200, and only with `BINANCE_DEMO_API_KEY` / `BINANCE_DEMO_API_SECRET`. |
| Historical files | `https://data.binance.vision/data/futures/um/` | Backtest inputs. A local zip is reused when its SHA256 matches the publisher checksum. |

There is no flag that sends orders to `fapi.binance.com`. `--check` never builds an order request, even if `--demo` is also passed. A non-200 REST response, including HTTP 451, does not switch hosts. Public bookTicker still runs when REST is blocked. Depth snapshots and `exchangeInfo` are requested only after production REST returns HTTP 200; otherwise the operator supplies tick, step, and quantity filters.

Demo credentials are `BINANCE_DEMO_API_KEY` and `BINANCE_DEMO_API_SECRET`. `BINANCE_API_KEY` is ignored. If the demo ping is not HTTP 200, no order URL is built.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DOPENSSL_INCLUDE_DIR=/path/to/openssl/include \
  -DZLIB_INCLUDE_DIR=/path/to/zlib/include
cmake --build build --config Release --parallel
ctest --test-dir build --output-on-failure
```

Link with OpenSSL 3 and zlib. On Windows also link `ws2_32` and `crypt32` (the CMake file does this). Set `SSL_CERT_FILE` if the CA bundle is not at `/etc/ssl/certs/ca-certificates.crt`.

## Run

Public connectivity check. This does not place orders.

```bash
./build/binance_market_maker --check --symbol BTCUSDT
```

Public book and dry quotes for 20 seconds:

```bash
./build/binance_market_maker --symbol BTCUSDT --seconds 20
```

Demo trading, only if the account is eligible and the keys are demo keys:

```bash
BINANCE_DEMO_API_KEY=... BINANCE_DEMO_API_SECRET=... \
  ./build/binance_market_maker --demo --symbol BTCUSDT --seconds 30
```

Historical backtest from official daily files. `--tick`, `--step`, and `--min-qty` are required because this process does not invent exchange filters when `exchangeInfo` is unreachable.

```bash
./build/binance_backtest \
  --symbol BTCUSDT --date 2026-10-19 \
  --tick 0.1 --step 0.001 --min-qty 0.001 --min-notional 50 \
  --budget 1000 --max-net-qty 0.01 --latency-ms 200 \
  --maker-fee 0.0002 --taker-fee 0.0005 \
  --data-dir data --out reports/btcusdt-2026-10-19
```

Reports: `report.md`, `summary.json`, `equity.csv`, `fills.csv`. Data manifests record the publisher URL and SHA256.

## Backtest limits

Fills are simulated. A maker fill requires a real aggTrade to print strictly through the quote after the latency delay, and the fill size cannot exceed that print. `bookDepth` is percentage-bucket depth, not a tick book. Touches at the quote price are counted and not filled. Missing funding files stay missing.
