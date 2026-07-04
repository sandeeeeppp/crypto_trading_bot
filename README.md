# Crypto Algo Trading Bot

A low-latency, event-driven cryptocurrency trading bot written in **C++17**. It connects to the **Binance WebSocket API** for real-time market data, applies several algorithmic strategies (biased market-making, laddered orders, linear-regression signal detection, and symmetric grid trading), and executes limit orders via the **Binance REST API**.

---

## Features

- **Real-time market data** via Binance WebSocket streams (order-book depth + 1-minute kline/candles)
- **Multiple trading strategies** dispatched dynamically based on a scored market signal:
  - Linear-regression momentum signal (bullish / bearish / neutral classification)
  - Biased market-making with configurable spread
  - Laddered order placement (5-level buy or sell staircase)
  - Symmetric grid trading around the mid-price
- **Thread-safe producer-consumer architecture** — separate threads for each WebSocket stream
- **Position tracking** with average-entry price and unrealised P&L display
- **CSV trade logging** — every executed order is appended to `trade_logs/<SYMBOL>.csv`

---

## Dependencies

| Library | Purpose | Minimum Version |
|---|---|---|
| [Boost](https://www.boost.org/) | ASIO, Circular Buffer, SSL | 1.74 |
| [WebSocket++](https://github.com/zaphoyd/websocketpp) | WebSocket client (header-only, bundled) | 0.8.2 |
| [nlohmann/json](https://github.com/nlohmann/json) | JSON parsing (header-only) | 3.11 |
| [libcurl](https://curl.se/libcurl/) | REST API HTTP client | 7.80 |
| [OpenSSL](https://www.openssl.org/) | HMAC-SHA256 request signing | 1.1+ |

---

## Build Instructions

### Prerequisites (Windows — MSVC or MinGW)

1. Install **Boost** (e.g. via `vcpkg install boost:x64-windows`).
2. Install **libcurl** (e.g. via `vcpkg install curl:x64-windows`).
3. Install **OpenSSL** (e.g. via `vcpkg install openssl:x64-windows`).
4. The `websocketpp/` directory is bundled in the repository (header-only).

### Compile with g++ (MinGW / MSYS2)

```bash
g++ -std=c++17 -O2 \
    -I./websocketpp \
    -Ipath/to/boost/include \
    -Ipath/to/nlohmann \
    main.cpp streamer.cpp trader.cpp \
    -o crypto_bot \
    -Lpath/to/boost/lib -lboost_system \
    -Lpath/to/curl/lib  -lcurl \
    -Lpath/to/openssl/lib -lssl -lcrypto \
    -lws2_32 -lwsock32
```

### Compile with MSVC (Developer Command Prompt)

```bash
cl /std:c++17 /EHsc /O2 \
   /I"./websocketpp" /I"path\to\boost\include" /I"path\to\nlohmann" \
   main.cpp streamer.cpp trader.cpp \
   /link /LIBPATH:"path\to\boost\lib" /LIBPATH:"path\to\curl\lib" /LIBPATH:"path\to\openssl\lib" \
   libcurl.lib libssl.lib libcrypto.lib ws2_32.lib
```

---

## Configuration — API Keys

> **⚠️ Never commit your API keys.** The `.env` file is listed in `.gitignore` and is loaded automatically at startup.

### Option 1 — `.env` file (recommended)

Copy the template and fill in your credentials:

```bash
cp .env.example .env   # or just create .env manually
```

`.env` format:
```ini
BINANCE_API_KEY=YOUR_BINANCE_API_KEY
BINANCE_SECRET_KEY=YOUR_BINANCE_SECRET_KEY
```

The bot parses `.env` at startup, sets the variables into the process environment, and then falls through to `std::getenv`. Entries already present in the shell environment are **not** overwritten, so shell exports always take precedence.

### Option 2 — Shell environment variables (CI / containers)

**Windows (PowerShell)**
```powershell
$env:BINANCE_API_KEY    = "YOUR_BINANCE_API_KEY"
$env:BINANCE_SECRET_KEY = "YOUR_BINANCE_SECRET_KEY"
```

**Windows (Command Prompt)**
```cmd
set BINANCE_API_KEY=YOUR_BINANCE_API_KEY
set BINANCE_SECRET_KEY=YOUR_BINANCE_SECRET_KEY
```

**Linux / macOS**
```bash
export BINANCE_API_KEY="YOUR_BINANCE_API_KEY"
export BINANCE_SECRET_KEY="YOUR_BINANCE_SECRET_KEY"
```

The bot will print an error and exit if neither source provides the required keys.

---

## Running

```bash
# After setting environment variables:
./crypto_bot        # Linux / macOS
crypto_bot.exe      # Windows
```

Trade logs are written to `trade_logs/<SYMBOL>.csv` (e.g. `trade_logs/btcusdt.csv`).

---

## Contributing

Contributions, suggestions, and strategy ideas are welcome.

- Open an **Issue** for bugs or feature requests
- Fork the repository and submit a **Pull Request**
- Discussions about strategy improvements are encouraged

---

## Disclaimer

This project is for **educational and experimental purposes only**. It is not financial advice. Trading cryptocurrencies involves substantial risk of loss. Use at your own discretion and always test with the [Binance Testnet](https://testnet.binance.vision/) before deploying with real funds.
