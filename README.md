# ⚡ Crypto Algo Trading Engine

A **high-performance, event-driven cryptocurrency trading engine** written in **C++17**. It connects to the **Binance WebSocket API** for real-time market data ingestion, computes regime-adaptive signals using OLS regression and momentum scoring, and executes orders asynchronously via the **Binance REST API** — all within a multi-threaded, mutex-guarded producer-consumer pipeline.

> **Testnet-first.** The engine is configured to trade on [Binance Spot Testnet](https://testnet.binance.vision/) by default.

---

## 🏗️ Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                     BINANCE EXCHANGE                             │
│         WSS Streams              REST API (Testnet)              │
└──────┬───────────┬───────────────────▲───────────────────────────┘
       │           │                   │
  kline_1m    depth20@100ms      POST /api/v3/order
       │           │                   │
┌──────▼──┐  ┌─────▼────┐       ┌──────┴──────┐
│ Candle  │  │  Depth   │       │   Trader    │
│ Stream  │  │  Stream  │       │  (Worker    │
│ Thread  │  │  Thread  │       │   Thread)   │
└────┬────┘  └────┬─────┘       └──────▲──────┘
     │            │                    │
     │  std::mutex│ guards             │ Async Order Queue
     │            │                    │ (Producer-Consumer)
┌────▼────────────▼────────────────────┴──────────────────────┐
│                    MAIN THREAD                               │
│                                                              │
│  TradeData (Ring Buffer)    MarketDepth (L2 Order Book)      │
│        │                          │                          │
│        └──── detect_market_signal ┘                          │
│                    │                                         │
│         ┌──────────┼──────────┐                              │
│      Bullish    Neutral    Bearish                            │
│         │          │          │                               │
│     Laddered    Grid      Laddered                           │
│     Buy Orders  Trading   Sell Orders                        │
│                                                              │
│  ──► log_trade() ──► trade_logs/btcusdt.csv                  │
└──────────────────────────────────────────────────────────────┘
```

### Pipeline Flow

1. **Ingestion** — Two streamer threads maintain persistent TLS WebSocket connections to Binance, parsing incoming JSON and writing to shared memory structures.
2. **Analysis** — The main thread reads the ring buffer (candle history) and order book (L2 depth) every ~100ms, computing OLS slope, momentum, and ROC to classify the market regime.
3. **Dispatch** — Based on the signal (`bullish` / `bearish` / `neutral`), the strategy dispatcher fires laddered orders or symmetric grid orders.
4. **Execution** — Orders are pushed into an async queue. A dedicated worker thread signs each request with HMAC-SHA256 and sends it to the Binance REST API via a persistent libcurl connection.

---

## 📊 Performance Benchmarks

| Metric | Value |
|---|---|
| Full pipeline latency (signal → signed order) | **4.9 µs** median |
| Signal compute (OLS + Momentum + ROC, 1K ticks) | **1.8 µs** median |
| Mutex acquisition overhead (3-thread contention) | **128 ns** per op |
| HMAC-SHA256 signing latency | **1.6 µs** per request |
| RAII logger vs V1 (open/close per write) | **10× throughput** gain |
| I/O syscall reduction | **93%** fewer per batch |

> Benchmarked on Windows x64 with MSVC 17.x. See [`benchmark.cpp`](benchmark.cpp) for the full profiling harness.

---

## 📁 Project Structure

```
crypto_Algo_Trading_Bot/
│
├── .env                            # API keys (git-ignored, never committed)
├── .gitignore                      # Excludes builds, secrets, and logs
├── CMakeLists.txt                  # Build system configuration
├── CMakePresets.json               # vcpkg toolchain integration
├── vcpkg.json                      # Dependency manifest
├── vcpkg-configuration.json        # vcpkg registry config
│
├── src/                            # ── Core Engine Source ──
│   ├── main.cpp                    # Orchestrator: strategy dispatcher, signal detection, event loop
│   ├── streamer.cpp / .hpp         # WebSocket ingestion: kline & depth stream handlers
│   ├── trader.cpp / .hpp           # Order execution: async queue, HMAC signing, REST dispatch
│   └── log_writer.cpp              # Persistent RAII trade logger
│
├── include/                        # ── Headers ──
│   ├── credentials.hpp             # .env file parser & key loader
│   ├── log_writer.hpp              # Log writer interface
│   └── tls_context.hpp             # TLS/SSL context factory for WebSocket++
│
├── triplets/                       # ── vcpkg Build Triplets ──
│   └── x64-windows-static-md.cmake # Static linking with dynamic CRT
│
├── trade_logs/                     # ── Runtime Output ──
│   └── btcusdt.csv                 # Executed trade ledger (git-ignored)
│
├── benchmark.cpp                   # Performance profiling harness
├── websocketpp/                    # Bundled WebSocket++ headers
│
│  ── Root-level legacy files (original single-file versions) ──
├── main.cpp                        # Original main (kept for reference)
├── streamer.cpp / .hpp             # Original streamer
└── trader.cpp / .hpp               # Original trader
```

---

## 🔧 Dependencies

| Library | Purpose | Install via vcpkg |
|---|---|---|
| [Boost](https://www.boost.org/) | Asio (async I/O), Circular Buffer, Beast SSL | `boost:x64-windows` |
| [WebSocket++](https://github.com/zaphoyd/websocketpp) | WebSocket client protocol (bundled, header-only) | — |
| [nlohmann/json](https://github.com/nlohmann/json) | JSON parsing for market data streams | `nlohmann-json:x64-windows` |
| [libcurl](https://curl.se/libcurl/) | HTTP client for REST API order execution | `curl:x64-windows` |
| [OpenSSL](https://www.openssl.org/) | HMAC-SHA256 signing + TLS/SSL transport | `openssl:x64-windows` |

---

## 🚀 Build Instructions

### Prerequisites

- **CMake** ≥ 3.21
- **MSVC** (Visual Studio 2022) or **g++** with C++17 support
- **vcpkg** (for dependency management)

### Build with CMake (Recommended)

```bash
# 1. Configure (vcpkg toolchain is auto-injected via CMakePresets.json)
cmake --preset=release

# 2. Build
cmake --build build/release --config Release

# 3. Output binary
./build/release/Release/crypto_bot.exe
```

### Build with g++ (MinGW / MSYS2)

```bash
g++ -std=c++17 -O2 \
    -I./websocketpp -I./include \
    src/main.cpp src/streamer.cpp src/trader.cpp src/log_writer.cpp \
    -o crypto_bot \
    -lboost_system -lcurl -lssl -lcrypto -lws2_32 -lwsock32
```

---

## 🔑 Configuration — API Keys

> **⚠️ Never commit your API keys.** The `.env` file is listed in `.gitignore`.

### Option 1 — `.env` file (Recommended)

Create a `.env` file in the project root:

```ini
BINANCE_API_KEY=your_api_key_here
BINANCE_SECRET_KEY=your_secret_key_here
```

The bot parses `.env` at startup and loads the keys into the process environment. Shell environment variables take precedence over `.env` values.

### Option 2 — Shell environment variables

```powershell
# PowerShell
$env:BINANCE_API_KEY    = "your_api_key_here"
$env:BINANCE_SECRET_KEY = "your_secret_key_here"
```

```bash
# Linux / macOS
export BINANCE_API_KEY="your_api_key_here"
export BINANCE_SECRET_KEY="your_secret_key_here"
```

The bot will print an error and exit if the keys are missing from both sources.

---

## ▶️ Running

```bash
./build/release/Release/crypto_bot.exe   # Windows
./crypto_bot                              # Linux / macOS
```

On startup you will see:
```
Connection opened to: btcusdt@kline_1m
Connection opened to: btcusdt@depth@100ms
Top Bid: 60230.63 Qty: 1.07913
Top Ask: 60230.64 Qty: 5.49790
Market Signal: neutral
[GRID] BUY  Qty: 0.00016660 @ 59995.05
[GRID] SELL Qty: 0.00016568 @ 60356.47
```

Trade logs are appended to `trade_logs/btcusdt.csv`.

---

## 🧠 Trading Strategies

| Signal | Strategy | Description |
|---|---|---|
| **Neutral** | Symmetric Grid | Places laddered BUY orders below mid-price and SELL orders above, capturing the spread |
| **Bullish** | Biased Market-Making + Laddered Buys | Tightens the bid spread and places 5-level ascending buy ladders |
| **Bearish** | Biased Market-Making + Laddered Sells | Tightens the ask spread and places 5-level descending sell ladders |

Signal detection uses a composite score based on:
- **OLS Linear Regression Slope** — trend direction over the candle window
- **Momentum** — rate of price change
- **ROC (Rate of Change)** — percentage change over N periods

---

## 🤝 Contributing

Contributions, strategy ideas, and optimizations are welcome.

- Open an **Issue** for bugs or feature requests
- Fork the repository and submit a **Pull Request**
- See [`benchmark.cpp`](benchmark.cpp) for performance profiling

---

## ⚠️ Disclaimer

This project is for **educational and experimental purposes only**. It is not financial advice. Trading cryptocurrencies involves substantial risk of loss. Always test with the [Binance Testnet](https://testnet.binance.vision/) before deploying with real funds.
