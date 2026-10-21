# Binance USD-M perpetual market maker

C++17 market maker and historical backtester for Binance USDⓈ-M futures. Inventory-aware spread, fee floor, gross cap, reduce-only take profit, and stop logic are preserved as one-way quote planning.

This is a research and engineering project. Fills in the backtester are simulated. There is no verified live-trading performance.

## Endpoints

| Use | Host | When |
|---|---|---|
| Public REST | `https://fapi.binance.com` | Market data only. HTTP 451 is reported and not bypassed. |
| Public WebSocket | `wss://fstream.binance.com/public/stream` | Market data only. Reconnects up to three times inside the run window. |
| Demo REST orders | `https://demo-fapi.binance.com` | Only with `--demo`, after demo `/fapi/v1/ping` returns HTTP 200, and only with `BINANCE_DEMO_API_KEY` / `BINANCE_DEMO_API_SECRET`. |
| Historical files | `https://data.binance.vision/data/futures/um/` | Backtest inputs. A local zip is reused when its SHA256 matches the publisher checksum. |

There is no flag that sends orders to `fapi.binance.com`. `--check` never builds an order request, even if `--demo` is also passed. A non-200 REST response, including HTTP 451, does not switch hosts. Public bookTicker still runs when REST is blocked. Depth snapshots and `exchangeInfo` are requested only after production REST returns HTTP 200; otherwise the operator supplies tick, step, and quantity filters.

Demo credentials are `BINANCE_DEMO_API_KEY` and `BINANCE_DEMO_API_SECRET`. `BINANCE_API_KEY` is ignored. If the demo ping is not HTTP 200, no order URL is built.

On shutdown after a demo run, the process attempts to cancel outstanding demo orders for the symbol and reconciles via the open-orders endpoint. If confirmation fails, a warning is printed that exchange-side orders may remain. Local order state is not trusted after API errors or WebSocket reconnects.

## Build

Requires a C++17 compiler, CMake 3.20+, OpenSSL 3, and zlib.

### Ubuntu (matches CI)

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build libssl-dev zlib1g-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### Windows (existing CMake/Ninja setup)

Use a recent Visual Studio or clang toolchain with Ninja, OpenSSL, and zlib available (e.g. via vcpkg or a prebuilt kit). Then:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DOPENSSL_INCLUDE_DIR=C:/path/to/openssl/include ^
  -DZLIB_INCLUDE_DIR=C:/path/to/zlib/include
cmake --build build --config Release --parallel
ctest --test-dir build --output-on-failure
```

On Windows the CMake file also links `ws2_32` and `crypt32`. Set `SSL_CERT_FILE` if the CA bundle is not at the default location.

## Run

### Public market-data check

Does not place orders.

```bash
./build/binance_market_maker --check --symbol BTCUSDT
```

Public book and dry quotes for 20 seconds (still no orders):

```bash
./build/binance_market_maker --symbol BTCUSDT --seconds 20
```

### Demo-order functionality

Only if the account is eligible and the keys are demo keys. Orders go exclusively to the demo endpoint. On exit the process attempts cancel-all + open-orders reconciliation.

```bash
BINANCE_DEMO_API_KEY=... BINANCE_DEMO_API_SECRET=... \
  ./build/binance_market_maker --demo --symbol BTCUSDT --seconds 30
```

### Historical backtest

From official daily files. `--tick`, `--step`, and `--min-qty` are required because this process does not invent exchange filters when `exchangeInfo` is unreachable.

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

Fills are simulated. A maker fill requires a real aggTrade to print strictly through the quote after the latency delay, and the fill size cannot exceed that print. `bookDepth` is percentage-bucket depth, not a tick book. Touches at the quote price are counted and not filled. Missing funding files stay missing. Queue position, latency, fees, and historical depth-data limitations mean results are not exchange executions and do not imply live profitability.
