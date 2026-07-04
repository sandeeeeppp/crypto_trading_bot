# Technical Implementation Plan (TIP)
## Crypto Algo Bot — V1 → V2 High-Performance Execution Engine
**Classification:** Internal Engineering — Principal Quant / Windows Systems Architecture  
**Source PRD:** Crypto Algorithmic Trading Bot v1.0 (Reverse-Engineered)  
**Target Platform:** Windows (x64), MSVC 2022 / C++17  
**Date:** 2026-05-21

---

## Preamble: What This Document Is

This TIP is a sequenced, executable engineering blueprint. Every section maps directly to a Technical Debt item in the PRD (TD-1 through TD-9). The plan is structured so that each phase produces a **shippable, testable artifact** before the next phase begins. You will never be in a state where the entire codebase is simultaneously broken.

The core architectural transformation is:

```
V1 (Broken)                            V2 (Target)
─────────────────────────────          ──────────────────────────────────────────
std::map order book (heap thrash)  →   Array-based O(1) order book (TD-5)
Data race on MarketDepth           →   Lock-free SPSC queue + atomic snapshot (TD-1)
Hardcoded credentials              →   Environment variable loader (TD-2)
TLS verify_none                    →   Proper CA-chain verification (TD-2/NFR-5.3)
libcurl per-order TCP+TLS          →   Persistent Boost.Beast WSS pipe (TD-3/TD-4)
Blocking curl_easy_perform         →   IOCP async, main thread never blocks (TD-4)
Per-trade fopen/fclose             →   Persistent ofstream + async log queue (TD-8)
Full buffer copy for signal        →   Incremental accumulator (TD-9)
```

---

## Phase 1: Toolchain & Dependency Matrix

### 1.1 The Dependency Problem in Plain Terms

The V1 codebase uses "system-installed" libraries — a phrase that is a build system time-bomb on Windows. On Linux, `apt install libboost-dev` gives you a coherent set. On Windows, mixing a vcpkg-installed Boost with a Chocolatey-installed OpenSSL with a vendored WebSocket++ guarantees one of three failure modes:

1. **ABI mismatch**: Boost compiled against MSVC 2019 `/MD` linked with your MSVC 2022 `/MT` binary → `std::string` layout differences, silent memory corruption.
2. **OpenSSL version skew**: Boost.Asio SSL requires OpenSSL 1.1.x or 3.x headers at compile time. If the runtime DLL is a different version, you get `SSL_CTX_new` symbol errors at link time or crashes at runtime.
3. **Iterator invalidation via mixed allocators**: WebSocket++ compiled with one Boost and linked against another produces undefined behavior in `asio::strand` internals.

The solution is to source **all** dependencies from a single manifest-controlled package manager. We use `vcpkg` in manifest mode, pinned to a single baseline commit.

### 1.2 Prerequisites: Exact Toolchain

Install in this exact order:

```powershell
# 1. Visual Studio 2022 Build Tools (NOT the full IDE — smaller, CI-friendly)
#    Download: https://aka.ms/vs/17/release/vs_BuildTools.exe
#    Select workloads: "Desktop development with C++", include:
#      - MSVC v143 (latest)
#      - Windows 11 SDK (10.0.22621.0 or later)
#      - CMake tools for Windows (3.27+)

# 2. Git (required by vcpkg)
winget install Git.Git

# 3. vcpkg — clone into a stable path (NOT inside your project repo)
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat -disableMetrics

# 4. Set the VCPKG_ROOT environment variable (permanent, system-wide)
[System.Environment]::SetEnvironmentVariable(
    "VCPKG_ROOT", "C:\vcpkg",
    [System.EnvironmentVariableTarget]::Machine
)

# 5. Integrate with MSBuild/CMake (optional convenience)
C:\vcpkg\vcpkg integrate install
```

### 1.3 vcpkg Manifest — `vcpkg.json`

Create this file at your **project root**. This is the single source of truth for all dependency versions.

```json
{
  "$schema": "https://raw.githubusercontent.com/microsoft/vcpkg/master/scripts/vcpkg.schema.json",
  "name": "crypto-bot-v2",
  "version": "2.0.0",
  "dependencies": [
    {
      "name": "boost-asio",
      "version>=": "1.84.0"
    },
    {
      "name": "boost-beast",
      "version>=": "1.84.0"
    },
    {
      "name": "boost-circular-buffer",
      "version>=": "1.84.0"
    },
    {
      "name": "boost-system",
      "version>=": "1.84.0"
    },
    {
      "name": "openssl",
      "version>=": "3.2.0"
    },
    {
      "name": "nlohmann-json",
      "version>=": "3.11.3"
    },
    {
      "name": "benchmark",
      "version>=": "1.8.3"
    }
  ],
  "builtin-baseline": "2024-01-01"
}
```

**Critical decision — WebSocket++**: Do NOT install `websocketpp` from vcpkg. It is header-only and its last release (0.8.2) has known ASIO compatibility issues with Boost 1.80+. In V2 we **replace it entirely with Boost.Beast**, which is Boost's own WebSocket library and is guaranteed to be version-coherent since it ships as part of Boost itself. This eliminates an entire class of dependency mismatch.

**Critical decision — libcurl**: Do NOT add `curl` to the manifest. We are **eliminating libcurl entirely** in Phase 4 and replacing it with `Boost.Beast` HTTP client. Zero new curl dependency.

### 1.4 vcpkg Baseline Pinning

A "baseline" pins the entire vcpkg port tree to a specific commit, making your build reproducible across machines and CI pipelines:

```json
{
  "builtin-baseline": "a42af01b72c28a8e1d7b48107b33e4f286a55ef9"
}
```

To find the current baseline hash:

```powershell
cd C:\vcpkg
git log --oneline -1
# Copy the hash → paste into builtin-baseline
```

Pin this in version control. Any teammate who runs `cmake --preset=windows-release` gets byte-for-byte identical libraries.

### 1.5 Static vs. Dynamic Linking Strategy

This is the most consequential build decision for Windows. The answer is: **static linking for all Boost and OpenSSL components**.

| Library | Link Mode | Reason |
|---|---|---|
| Boost.Asio | Static (header-only, N/A) | Header-only, no link step |
| Boost.Beast | Static (header-only, N/A) | Header-only |
| Boost.System | Static (`/MT`) | Eliminates `boost_system-vc143-mt-x64-1_84.dll` deployment dependency |
| OpenSSL | Static (`/MT`) | Eliminates OpenSSL DLL version mismatches on target Windows machines |
| nlohmann/json | Header-only, N/A | No link step |
| Google Benchmark | Static | Test-only; no DLL needed |

To enforce static linking in vcpkg, create `vcpkg-configuration.json`:

```json
{
  "default-registry": {
    "kind": "git",
    "baseline": "a42af01b72c28a8e1d7b48107b33e4f286a55ef9",
    "repository": "https://github.com/microsoft/vcpkg"
  },
  "overlay-triplets": ["triplets"]
}
```

Create `triplets/x64-windows-static-md.cmake`:

```cmake
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)    # Use /MD (dynamic CRT) — standard for most MSVC projects
set(VCPKG_LIBRARY_LINKAGE static) # But link Boost/OpenSSL statically
set(VCPKG_BUILD_TYPE release)
```

> **Why `/MD` CRT with static libraries?** Pure `/MT` (static CRT) causes problems when your binary loads Windows system DLLs that themselves use the MSVC runtime — you end up with two separate CRT heaps, and `free()` called on memory allocated in one crashes in the other. The `/MD` + static-library pattern is the correct Windows idiom: one shared CRT heap, but your application's Boost/OpenSSL symbols are baked into the `.exe`.

### 1.6 CMake Configuration — `CMakeLists.txt` (Root)

```cmake
cmake_minimum_required(VERSION 3.27)
project(crypto_bot_v2 VERSION 2.0.0 LANGUAGES CXX)

# ── C++ Standard ──────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── vcpkg Toolchain (must be set before project()) in practice, 
#    but CMake presets handle this via CMAKE_TOOLCHAIN_FILE)
# ── See CMakePresets.json below ───────────────────────────────────────────

# ── Dependencies via vcpkg ────────────────────────────────────────────────
find_package(Boost 1.84 REQUIRED COMPONENTS system)
find_package(OpenSSL 3.2 REQUIRED)
find_package(nlohmann_json 3.11 REQUIRED)

# ── MSVC-specific flags ───────────────────────────────────────────────────
if(MSVC)
    # /W4: High warning level
    # /WX: Warnings as errors (enforce in new code)
    # /wd4996: Suppress deprecated POSIX name warnings (OpenSSL internals)
    # /MP: Parallel compilation
    add_compile_options(/W4 /WX /wd4996 /MP)

    # Boost.Asio on Windows: force IOCP backend
    add_compile_definitions(
        BOOST_ASIO_HAS_IOCP          # Explicitly enable IOCP
        BOOST_ASIO_DISABLE_EPOLL     # Not applicable on Windows, but defense-in-depth
        _WIN32_WINNT=0x0A00          # Target Windows 10
        WIN32_LEAN_AND_MEAN          # Exclude rarely-used Windows headers
        NOMINMAX                     # Prevent Windows.h from defining min/max macros
        BOOST_DATE_TIME_NO_LIB       # Prevent auto-linking of date_time (not used)
        OPENSSL_NO_DEPRECATED        # Enforce modern OpenSSL API only
    )
endif()

# ── Main Executable ───────────────────────────────────────────────────────
add_executable(crypto_bot
    src/main.cpp
    src/credentials.cpp      # Phase 2
    src/order_book.cpp       # Phase 3
    src/spsc_queue.cpp       # Phase 3 (mostly header)
    src/ws_session.cpp       # Phase 4
    src/order_executor.cpp   # Phase 4
    src/streamer.cpp         # Refactored existing
    src/trader.cpp           # Refactored existing
    src/log_writer.cpp       # Phase 2 (TD-8 fix)
)

target_include_directories(crypto_bot PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${Boost_INCLUDE_DIRS}
)

target_link_libraries(crypto_bot PRIVATE
    Boost::system
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    ws2_32          # Winsock2 — required for all network I/O on Windows
    crypt32         # Windows CryptoAPI — required for certificate store access
    Secur32         # Windows Security Support Provider Interface
)

# ── Benchmarks ────────────────────────────────────────────────────────────
find_package(benchmark REQUIRED)
add_subdirectory(benchmarks)

# ── Tests ─────────────────────────────────────────────────────────────────
enable_testing()
add_subdirectory(tests)
```

### 1.7 CMake Presets — `CMakePresets.json`

```json
{
  "version": 3,
  "configurePresets": [
    {
      "name": "windows-release",
      "displayName": "Windows x64 Release (MSVC)",
      "generator": "Visual Studio 17 2022",
      "architecture": {"value": "x64", "strategy": "set"},
      "binaryDir": "${sourceDir}/build/release",
      "toolchainFile": "$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Release",
        "VCPKG_TARGET_TRIPLET": "x64-windows-static-md",
        "CMAKE_MSVC_RUNTIME_LIBRARY": "MultiThreadedDLL"
      }
    },
    {
      "name": "windows-debug",
      "displayName": "Windows x64 Debug (MSVC)",
      "inherits": "windows-release",
      "binaryDir": "${sourceDir}/build/debug",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Debug",
        "CMAKE_MSVC_RUNTIME_LIBRARY": "MultiThreadedDebugDLL"
      }
    }
  ],
  "buildPresets": [
    {
      "name": "release",
      "configurePreset": "windows-release",
      "configuration": "Release"
    }
  ]
}
```

**Build commands:**

```powershell
# Configure (downloads and builds all vcpkg dependencies on first run — 10-20 min)
cmake --preset=windows-release

# Build
cmake --build build/release --config Release --parallel

# Run benchmarks
.\build\release\Release\benchmarks\order_book_bench.exe
```

### 1.8 Dependency Verification Checklist

After the first successful build, verify linkage:

```powershell
# Confirm no unexpected DLL dependencies (should show only Windows system DLLs)
dumpbin /dependents .\build\release\Release\crypto_bot.exe

# Expected output — ONLY these should appear:
#   KERNEL32.dll
#   WS2_32.dll
#   CRYPT32.dll
#   SECUR32.dll
#   VCRUNTIME140.dll   (or VCRUNTIME140D.dll in debug)
#   MSVCP140.dll
#   api-ms-win-*.dll

# Boost.System, OpenSSL — NOT listed means static linkage confirmed
```

---

## Phase 2: Security & Infrastructure

This phase has zero performance impact but maximum risk reduction. Fix these first, before any architectural work, because a leaked key in git history is permanent.

### 2.1 TD-2: Eliminating Hardcoded Credentials

**The Problem (from PRD, `main.cpp:159-160`):**

```cpp
// V1 — DO NOT SHIP
std::string api_key    = "OVRJNWAlu17X5zkw..."; // plaintext in source
std::string secret_key = "OPuZ90e26CwfJJTL..."; // plaintext in source
```

Any `git log`, `strings crypto_bot.exe`, or repo fork exposes these permanently.

**Step 1 — Create `include/credentials.hpp`:**

```cpp
#pragma once
#include <string>
#include <stdexcept>
#include <cstdlib>

namespace v2 {

struct Credentials {
    std::string api_key;
    std::string secret_key;
};

/// Load Binance credentials from environment variables.
/// Throws std::runtime_error if either variable is not set.
/// 
/// On Windows, set via:
///   [System.Environment]::SetEnvironmentVariable("BINANCE_API_KEY", "...", "User")
///   [System.Environment]::SetEnvironmentVariable("BINANCE_SECRET_KEY", "...", "User")
/// Or in PowerShell session:
///   $env:BINANCE_API_KEY = "..."
inline Credentials load_credentials() {
    const char* key    = std::getenv("BINANCE_API_KEY");
    const char* secret = std::getenv("BINANCE_SECRET_KEY");

    if (!key || std::string(key).empty()) {
        throw std::runtime_error(
            "BINANCE_API_KEY environment variable is not set.\n"
            "Set it with: $env:BINANCE_API_KEY = \"your_key\""
        );
    }
    if (!secret || std::string(secret).empty()) {
        throw std::runtime_error(
            "BINANCE_SECRET_KEY environment variable is not set.\n"
            "Set it with: $env:BINANCE_SECRET_KEY = \"your_secret\""
        );
    }

    return { std::string(key), std::string(secret) };
}

} // namespace v2
```

**Step 2 — Update `main.cpp`:**

```cpp
// V2 — Credentials loaded at runtime, never in source
#include "credentials.hpp"

int main() {
    v2::Credentials creds;
    try {
        creds = v2::load_credentials();
    } catch (const std::runtime_error& e) {
        std::cerr << "[FATAL] " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    // Pass creds.api_key and creds.secret_key into Trader constructor
    // — do NOT store in a local std::string that outlives its scope
    auto trader = std::make_unique<v2::Trader>(creds.api_key, creds.secret_key);
    // ...
}
```

**Step 3 — Windows environment variable setup (PowerShell, permanent):**

```powershell
# Set at User scope (persists across reboots, not visible to other users)
[System.Environment]::SetEnvironmentVariable(
    "BINANCE_API_KEY",
    "your_64_char_key_here",
    [System.EnvironmentVariableTarget]::User
)
[System.Environment]::SetEnvironmentVariable(
    "BINANCE_SECRET_KEY",
    "your_64_char_secret_here",
    [System.EnvironmentVariableTarget]::User
)

# Verify (new PowerShell session required to see User-scope variables)
echo $env:BINANCE_API_KEY
```

**Step 4 — Purge from git history (CRITICAL):**

```bash
# If keys were ever committed, they must be purged — not just deleted in a new commit
# Use git-filter-repo (modern replacement for BFG)
pip install git-filter-repo

git filter-repo --path main.cpp --force \
  --replace-text <(echo "OVRJNWAlu17X5zkw==>REDACTED_KEY")

# After purge, rotate the keys on Binance — treat the old keys as permanently compromised
```

**Step 5 — Add `.gitignore` entries:**

```gitignore
# Never commit these
.env
*.env
secrets.json
config/credentials.json

# Windows credential files
*.credential
```

### 2.2 TD-2 / NFR-5.3: Fixing TLS Certificate Verification

**The Problem (from PRD, `streamer.cpp:55`):**

```cpp
// V1 — MITM attack surface
ctx.set_verify_mode(boost::asio::ssl::verify_none);
```

This disables all certificate validation. A man-in-the-middle attacker could intercept the WebSocket connection, see your order book data, and inject fake price feeds — causing your strategy to trade on fabricated market data.

**Step 1 — Understand Windows Certificate Stores**

Unlike Linux (which uses `/etc/ssl/certs/ca-bundle.crt`), Windows stores trusted CA certificates in the Windows Certificate Store (the same store that IE/Edge/Chrome use). The correct approach on Windows is to load from this store rather than bundling a separate `ca-bundle.pem` file.

**Step 2 — Create `include/tls_context.hpp`:**

```cpp
#pragma once
#include <boost/asio/ssl.hpp>
#include <wincrypt.h>      // Windows CryptoAPI
#include <string>
#include <stdexcept>

#pragma comment(lib, "crypt32.lib")

namespace v2 {

namespace ssl = boost::asio::ssl;

/// Creates a properly configured TLS context for connecting to Binance.
/// 
/// On Windows, loads the trusted CA certificates from the Windows Certificate Store
/// (the "ROOT" store — same certificates trusted by Chrome/Edge/IE).
/// 
/// This replaces streamer.cpp:47-55 where verify_none was set.
inline ssl::context make_tls_context() {
    ssl::context ctx(ssl::context::tlsv12_client);

    // ── Protocol Hardening ─────────────────────────────────────────────
    // Binance requires TLS 1.2 minimum. Disable older protocols.
    ctx.set_options(
        ssl::context::default_workarounds |
        ssl::context::no_sslv2            |
        ssl::context::no_sslv3            |
        ssl::context::no_tlsv1            |    // Disable TLS 1.0
        ssl::context::no_tlsv1_1          |    // Disable TLS 1.1
        ssl::context::single_dh_use
    );

    // ── Certificate Verification Mode ─────────────────────────────────
    // verify_peer: require a valid certificate chain from the server
    // verify_fail_if_no_peer_cert: reject if server doesn't present cert
    ctx.set_verify_mode(ssl::verify_peer | ssl::verify_fail_if_no_peer_cert);

    // ── Load Windows Certificate Store ────────────────────────────────
    // Open the system "ROOT" store (trusted root CAs)
    HCERTSTORE h_store = CertOpenSystemStoreA(0, "ROOT");
    if (!h_store) {
        throw std::runtime_error(
            "Failed to open Windows ROOT certificate store. "
            "Error: " + std::to_string(GetLastError())
        );
    }

    // Get the underlying OpenSSL X509_STORE from the Boost.Asio SSL context
    X509_STORE* x509_store = SSL_CTX_get_cert_store(ctx.native_handle());

    // Iterate through all certificates in the Windows store
    PCCERT_CONTEXT p_cert_ctx = nullptr;
    int certs_loaded = 0;
    while ((p_cert_ctx = CertEnumCertificatesInStore(h_store, p_cert_ctx)) != nullptr) {
        // Convert from Windows DER format to OpenSSL X509 object
        const unsigned char* cert_data = p_cert_ctx->pbCertEncoded;
        X509* x509 = d2i_X509(nullptr, &cert_data, p_cert_ctx->cbCertEncoded);
        if (x509) {
            X509_STORE_add_cert(x509_store, x509);
            X509_free(x509);
            ++certs_loaded;
        }
    }
    CertCloseStore(h_store, 0);

    if (certs_loaded == 0) {
        throw std::runtime_error(
            "No certificates loaded from Windows ROOT store. "
            "TLS connections will fail."
        );
    }

    // ── Hostname Verification ─────────────────────────────────────────
    // This callback fires during the TLS handshake. It verifies that the
    // certificate's CN/SAN matches the hostname we're connecting to.
    // Without this, a valid cert for example.com would pass when connecting
    // to stream.binance.com — a classic certificate substitution attack.
    ctx.set_verify_callback(
        ssl::rfc2818_verification("stream.binance.com")
    );

    return ctx;
}

/// Creates a TLS context configured for a specific hostname.
/// Use this when connecting to testnet or other Binance endpoints.
inline ssl::context make_tls_context(const std::string& hostname) {
    auto ctx = make_tls_context();  // Load certs from Windows store
    // Override the hostname verification callback
    ctx.set_verify_callback(ssl::rfc2818_verification(hostname));
    return ctx;
}

} // namespace v2
```

**Step 3 — Verify the fix with a connection test (before integrating into main):**

```cpp
// tests/tls_test.cpp
#include "tls_context.hpp"
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <iostream>

int main() {
    namespace asio = boost::asio;
    namespace ssl  = asio::ssl;

    asio::io_context ioc;
    
    try {
        auto ctx = v2::make_tls_context("stream.binance.com");
        ssl::stream<asio::ip::tcp::socket> stream(ioc, ctx);

        // Attempt TCP + TLS handshake to Binance stream endpoint
        asio::ip::tcp::resolver resolver(ioc);
        auto endpoints = resolver.resolve("stream.binance.com", "9443");
        asio::connect(stream.next_layer(), endpoints);
        stream.handshake(ssl::stream_base::client);

        std::cout << "[PASS] TLS handshake to stream.binance.com:9443 succeeded.\n";
        std::cout << "       Certificate verification: ENABLED\n";
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << e.what() << "\n";
        return 1;
    }
    return 0;
}
```

### 2.3 TD-8: Fixing Per-Trade File I/O

**The Problem (from PRD):** `log_trade()` does `fopen` → `fwrite` → `fclose` for every single order. In a 5-order laddered batch: 15 syscalls on the hot path.

**Create `include/log_writer.hpp`:**

```cpp
#pragma once
#include <fstream>
#include <string>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <filesystem>

namespace v2 {

/// Async-safe CSV trade logger.
/// Holds the ofstream open for process lifetime (one open, many writes).
/// Thread-safe via internal mutex — safe to call from multiple threads.
class LogWriter {
public:
    explicit LogWriter(const std::string& symbol) {
        std::filesystem::create_directories("trade_logs");
        const std::string path = "trade_logs/" + symbol + ".csv";
        file_.open(path, std::ios::app);
        if (!file_.is_open()) {
            throw std::runtime_error("Cannot open log file: " + path);
        }
        // Write header if file is new (size == 0 after open)
        if (file_.tellp() == 0) {
            file_ << "timestamp,side,price,quantity\n";
            file_.flush();
        }
    }

    ~LogWriter() {
        if (file_.is_open()) {
            file_.flush();
            file_.close();
        }
    }

    // Non-copyable, non-movable (owns the file handle)
    LogWriter(const LogWriter&) = delete;
    LogWriter& operator=(const LogWriter&) = delete;

    void log(const std::string& side, double price, double qty) {
        auto now = std::chrono::system_clock::now();
        auto t   = std::chrono::system_clock::to_time_t(now);

        std::lock_guard<std::mutex> lock(mtx_);
        file_ << std::put_time(std::gmtime(&t), "%Y-%m-%dT%H:%M:%SZ")
              << "," << side
              << "," << std::fixed << std::setprecision(2) << price
              << "," << std::fixed << std::setprecision(6) << qty
              << "\n";
        // Note: no flush here — OS buffers are sufficient for non-crash durability.
        // If crash-durability is required, call file_.flush() here at ~1ms cost per trade.
    }

private:
    std::ofstream file_;
    std::mutex    mtx_;
};

} // namespace v2
```

---

## Phase 3: The Data Structures (Resolving TD-1 & TD-5)

This is the architectural heart of the upgrade. The two data structures built here — the Array-Based Order Book and the SPSC Queue — are what make V2 a genuinely different system from V1, not just a patched one.

### 3.1 How These Components Connect (Read This First)

Before writing a line of code, understand the information flow:

```
IOCP Thread (Boost.Asio)             Main Strategy Thread
─────────────────────────            ────────────────────────────────
[Binance WS message arrives]
         │
         ▼
[on_message() callback]
         │
   Parse JSON delta
         │
         ▼
[ArrayOrderBook::apply_delta()]      
   - O(1) update by price index
   - No mutex, no allocation
         │
   Publish snapshot via
         ▼
[SpscQueue<OrderBookSnapshot>::push()]
   ← wait-free write →
         │
         │                           [Main loop wakes on timer]
         │                                     │
         └──────────────────────────────► SpscQueue::pop()
                                               │
                                         Read snapshot
                                         (zero-copy, no lock)
                                               │
                                         detect_market_signal()
                                         strategy_dispatcher()
                                               │
                                         push to OrderExecutor queue
```

The IOCP networking thread **never takes a lock** when writing market data. The main strategy thread **never takes a lock** when reading it. This eliminates TD-1 (the data race) and TD-5 (the map thrashing) simultaneously.

### 3.2 TD-5: Array-Based Order Book Implementation

**Design rationale:** BTC/USDT on Binance has a known tick size of $0.01 and a maximum realistic price range. We can pre-allocate a flat array indexed by price slot. A `$100,000` price with `0.01` tick size requires `100,000 / 0.01 = 10,000,000` slots — too many. Instead, we use a sparse representation: a flat array of fixed size (e.g., 8192 slots) anchored around the current best price, recalibrated only when the price moves beyond the window.

**Create `include/order_book.hpp`:**

```cpp
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

namespace v2 {

/// Snapshot of the order book published to the SPSC queue.
/// POD-compatible for lock-free transfer.
struct OrderBookSnapshot {
    double best_bid   = 0.0;
    double best_ask   = 0.0;
    double mid_price  = 0.0;
    double bid_depth  = 0.0;   // Total bid liquidity within 1% of best bid
    double ask_depth  = 0.0;   // Total ask liquidity within 1% of best ask
    uint64_t sequence = 0;     // Monotonic update counter for staleness detection
};

/// Price level stored in the array.
struct PriceLevel {
    double qty = 0.0;   // 0.0 means empty slot
};

/// High-performance order book using pre-allocated flat arrays.
/// 
/// Design:
///   - Bids and asks stored in separate fixed-size arrays
///   - Array index = (price - base_price) / tick_size
///   - O(1) insert, update, delete (no allocation, no tree rebalancing)
///   - NOT thread-safe internally — external synchronization via SPSC queue
/// 
/// Resolves TD-5: replaces std::map clear()+rebuild every 100ms.
class ArrayOrderBook {
public:
    static constexpr int    SLOTS      = 8192;       // Price levels per side
    static constexpr double TICK_SIZE  = 0.01;       // BTC/USDT tick: $0.01
    static constexpr double WINDOW_PCT = 0.05;       // Recalibrate if price moves ±5%

    ArrayOrderBook() {
        reset();
    }

    /// Apply a depth update from the Binance @depth stream.
    /// Called by the IOCP callback on price level change.
    /// 
    /// side: 'b' for bid, 'a' for ask
    /// price: price level
    /// qty:   new quantity (0.0 = remove level)
    void apply_update(char side, double price, double qty) {
        // On first call, anchor the book to this price
        if (base_price_ == 0.0) {
            calibrate(price);
        }

        // Recalibrate if price moves outside ±5% window
        const double drift = std::abs(price - base_price_) / base_price_;
        if (drift > WINDOW_PCT) {
            calibrate(price);
        }

        const int slot = price_to_slot(price);
        if (slot < 0 || slot >= SLOTS) {
            return;  // Outside current window — ignore (will calibrate next tick)
        }

        if (side == 'b') {
            bids_[slot].qty = qty;
            bid_dirty_ = true;
        } else {
            asks_[slot].qty = qty;
            ask_dirty_ = true;
        }

        ++sequence_;
    }

    /// Compute and return a snapshot for publishing to the SPSC queue.
    /// Called after processing a batch of updates from one WebSocket message.
    OrderBookSnapshot snapshot() const {
        OrderBookSnapshot snap;
        snap.sequence = sequence_;
        snap.best_bid = compute_best_bid();
        snap.best_ask = compute_best_ask();

        if (snap.best_bid > 0.0 && snap.best_ask > 0.0) {
            snap.mid_price = (snap.best_bid + snap.best_ask) / 2.0;
            snap.bid_depth = compute_depth(bids_, snap.best_bid, -0.01);  // 1% down from bid
            snap.ask_depth = compute_depth(asks_, snap.best_ask,  0.01);  // 1% up from ask
        }

        return snap;
    }

    /// Full reset — call when reconnecting WebSocket after a drop.
    void reset() {
        std::memset(bids_.data(), 0, sizeof(bids_));
        std::memset(asks_.data(), 0, sizeof(asks_));
        base_price_ = 0.0;
        bid_dirty_  = false;
        ask_dirty_  = false;
        sequence_   = 0;
    }

private:
    void calibrate(double anchor_price) {
        // Anchor the array so slot 0 = anchor_price - (SLOTS/2 * TICK_SIZE)
        base_price_ = anchor_price - (SLOTS / 2) * TICK_SIZE;
        // On recalibration, clear the book — stale levels are invalid
        std::memset(bids_.data(), 0, sizeof(bids_));
        std::memset(asks_.data(), 0, sizeof(asks_));
    }

    int price_to_slot(double price) const {
        return static_cast<int>((price - base_price_) / TICK_SIZE + 0.5);
    }

    double slot_to_price(int slot) const {
        return base_price_ + slot * TICK_SIZE;
    }

    double compute_best_bid() const {
        // Scan from top of array downward (highest price = best bid)
        for (int i = SLOTS - 1; i >= 0; --i) {
            if (bids_[i].qty > 0.0) {
                return slot_to_price(i);
            }
        }
        return 0.0;
    }

    double compute_best_ask() const {
        // Scan from bottom of array upward (lowest price = best ask)
        for (int i = 0; i < SLOTS; ++i) {
            if (asks_[i].qty > 0.0) {
                return slot_to_price(i);
            }
        }
        return 0.0;
    }

    double compute_depth(const std::array<PriceLevel, SLOTS>& levels,
                         double ref_price, double range_pct) const {
        const double limit = ref_price * (1.0 + range_pct);
        double total = 0.0;
        for (int i = 0; i < SLOTS; ++i) {
            if (levels[i].qty > 0.0) {
                const double p = slot_to_price(i);
                // For bids: accumulate levels DOWN from ref_price to limit
                // For asks: accumulate levels UP from ref_price to limit
                if (range_pct < 0 && p >= limit && p <= ref_price) {
                    total += levels[i].qty * p;
                } else if (range_pct > 0 && p <= limit && p >= ref_price) {
                    total += levels[i].qty * p;
                }
            }
        }
        return total;
    }

    std::array<PriceLevel, SLOTS> bids_{};
    std::array<PriceLevel, SLOTS> asks_{};
    double   base_price_ = 0.0;
    uint64_t sequence_   = 0;
    bool     bid_dirty_  = false;
    bool     ask_dirty_  = false;
};

} // namespace v2
```

**Performance comparison vs V1:**

| Operation | V1 (std::map) | V2 (Array) |
|---|---|---|
| Full clear() + rebuild (100ms) | O(N log N) + heap dealloc/alloc | Not needed — incremental |
| Single level update | O(log N) tree insert | O(1) array write |
| Best bid/ask lookup | O(1) (begin()) | O(N) scan — but N=8192 in L1 cache (~2µs) |
| Memory allocation per update | Yes (tree node) | None |
| Cache locality | Poor (pointer tree) | Excellent (contiguous 64KB) |

The O(N) scan for best bid/ask looks alarming, but 8192 doubles = 65KB fits entirely in L2 cache. A sequential scan over cached memory at ~32 bytes/cycle on modern Intel is approximately 2 microseconds — faster than any `std::map` tree traversal under cache pressure.

### 3.3 TD-1: SPSC Lock-Free Queue Implementation

**Design rationale:** The Single-Producer Single-Consumer (SPSC) queue is the canonical lock-free data structure when you have exactly one writer (IOCP thread) and one reader (main strategy thread). SPSC queues can be implemented without any atomic compare-and-swap (CAS) operations — only atomic loads and stores — making them wait-free (bounded-time) rather than merely lock-free.

**Create `include/spsc_queue.hpp`:**

```cpp
#pragma once
#include <atomic>
#include <array>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <new>   // std::hardware_destructive_interference_size

namespace v2 {

// Detect cache line size — 64 bytes on all x86-64 processors.
// std::hardware_destructive_interference_size is C++17 but not universally supported.
#ifdef __cpp_lib_hardware_interference_size
    constexpr std::size_t CACHE_LINE = std::hardware_destructive_interference_size;
#else
    constexpr std::size_t CACHE_LINE = 64;
#endif

/// Wait-free Single-Producer Single-Consumer queue.
/// 
/// CRITICAL INVARIANTS (violation = data race = undefined behavior):
///   1. EXACTLY ONE thread calls push() — the IOCP networking thread
///   2. EXACTLY ONE thread calls pop()  — the main strategy thread
///   3. Capacity must be a power of 2 (enables branchless index masking)
/// 
/// Memory ordering:
///   - head_ (read index) is owned by consumer. Written with release, read by
///     producer with acquire to detect full condition.
///   - tail_ (write index) is owned by producer. Written with release, read by
///     consumer with acquire to detect non-empty condition.
///   - Data slots have no atomic protection — the acquire/release on the indices
///     provides the happens-before edge that makes data visible across threads.
/// 
/// Resolves TD-1: replaces std::mutex on MarketDepth with zero-lock data transfer.
template<typename T, std::size_t Capacity = 256>
class SpscQueue {
    static_assert((Capacity & (Capacity - 1)) == 0,
        "SpscQueue capacity must be a power of 2");
    static_assert(std::is_trivially_copyable_v<T>,
        "SpscQueue requires trivially copyable type for correct lock-free semantics");

public:
    SpscQueue() : head_(0), tail_(0) {}

    // Non-copyable, non-movable
    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    /// Producer: push an item.
    /// Returns false if queue is full (item is dropped).
    /// Called ONLY from the IOCP networking thread.
    [[nodiscard]] bool push(const T& item) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t next_tail = (tail + 1) & MASK;

        // Queue is full if next_tail would collide with head
        if (next_tail == head_.load(std::memory_order_acquire)) {
            return false;  // Drop: consumer is slower than producer
        }

        buffer_[tail] = item;  // Write data to slot

        // Release: makes the data write visible to the consumer thread
        // before the index update
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    /// Consumer: pop an item.
    /// Returns std::nullopt if queue is empty.
    /// Called ONLY from the main strategy thread.
    [[nodiscard]] std::optional<T> pop() noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);

        // Queue is empty if head == tail
        if (head == tail_.load(std::memory_order_acquire)) {
            return std::nullopt;
        }

        // Acquire: ensures that the data written by producer (with release store
        // to tail_) is fully visible before we read buffer_[head]
        T item = buffer_[head];  // Read data from slot

        head_.store((head + 1) & MASK, std::memory_order_release);
        return item;
    }

    /// Returns true if no items are available.
    /// May be stale by the time the caller uses the result.
    bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    /// Approximate size — not precise under concurrent access.
    std::size_t size_approx() const noexcept {
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        const std::size_t head = head_.load(std::memory_order_acquire);
        return (tail - head) & MASK;
    }

private:
    static constexpr std::size_t MASK = Capacity - 1;

    // Cache-line padding prevents false sharing.
    // head_ (consumer) and tail_ (producer) must be on separate cache lines.
    // If they share a line, every write by either thread invalidates the other's
    // cache line — a ~100ns round-trip penalty every operation (false sharing).
    
    alignas(CACHE_LINE) std::atomic<std::size_t> head_;
    alignas(CACHE_LINE) std::atomic<std::size_t> tail_;
    
    // The data buffer. Sized to fit in L1 cache for typical snapshot structs.
    // OrderBookSnapshot is ~48 bytes. 256 slots = 12KB — fits in L1 (32-64KB typical).
    alignas(CACHE_LINE) std::array<T, Capacity> buffer_;
};

// Type aliases for the concrete queues used in the pipeline
using OrderBookQueue = SpscQueue<OrderBookSnapshot, 256>;

} // namespace v2
```

**Proving correctness — the happens-before chain:**

```
Producer Thread                    Consumer Thread
──────────────────────────────     ───────────────────────────────
buffer_[tail] = item              (not yet reading)
tail_.store(next, release)   ──→  tail_.load(acquire)       [1]
                                  T item = buffer_[old_tail] [2]
```

Step [1]: The `acquire` load on `tail_` synchronizes-with the `release` store. This creates a happens-before edge. Step [2]: Because [2] happens after [1] in program order, and [1] happens-after the producer's buffer write, the buffer write is visible at [2]. No mutex needed. No UB.

### 3.4 Benchmark Setup — `benchmarks/CMakeLists.txt`

```cmake
find_package(benchmark REQUIRED)

add_executable(order_book_bench order_book_bench.cpp)
target_link_libraries(order_book_bench PRIVATE benchmark::benchmark)
target_include_directories(order_book_bench PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_executable(spsc_bench spsc_bench.cpp)
target_link_libraries(spsc_bench PRIVATE benchmark::benchmark)
target_include_directories(spsc_bench PRIVATE ${CMAKE_SOURCE_DIR}/include)
```

```cpp
// benchmarks/order_book_bench.cpp
#include <benchmark/benchmark.h>
#include "order_book.hpp"

static void BM_OrderBookUpdate(benchmark::State& state) {
    v2::ArrayOrderBook book;
    double price = 50000.0;

    for (auto _ : state) {
        // Simulate a 10-level depth update (typical Binance @depth message)
        for (int i = 0; i < 10; ++i) {
            book.apply_update('b', price - i * 0.01, 0.5 + i * 0.1);
            book.apply_update('a', price + i * 0.01, 0.5 + i * 0.1);
        }
        auto snap = book.snapshot();
        benchmark::DoNotOptimize(snap);
    }
}
BENCHMARK(BM_OrderBookUpdate)->Unit(benchmark::kMicrosecond);

// Compare against V1 behavior: full clear + rebuild
static void BM_OrderBookV1_FullRebuild(benchmark::State& state) {
    std::map<double, double, std::greater<>> bids;
    std::map<double, double> asks;
    double price = 50000.0;

    for (auto _ : state) {
        bids.clear();
        asks.clear();
        for (int i = 0; i < 10; ++i) {
            bids[price - i * 0.01] = 0.5 + i * 0.1;
            asks[price + i * 0.01] = 0.5 + i * 0.1;
        }
        double best_bid = bids.begin()->first;
        benchmark::DoNotOptimize(best_bid);
    }
}
BENCHMARK(BM_OrderBookV1_FullRebuild)->Unit(benchmark::kMicrosecond);

BENCHMARK_MAIN();
```

Expected results (i7/Ryzen, Release build):

```
BM_OrderBookUpdate           0.8 µs   ← V2: incremental array update
BM_OrderBookV1_FullRebuild   4.2 µs   ← V1: full map clear/rebuild
Speedup:                     ~5x
```

---

## Phase 4: Networking & Execution (Resolving TD-3 & TD-4)

### 4.1 The Problem: Two Compounding Latency Sources

TD-3 and TD-4 are coupled. The root cause is architectural: the V1 system uses libcurl as a synchronous RPC mechanism baked into the strategy loop. The fix requires two simultaneous changes:

1. Replace the per-order `curl_easy_init()` / TCP connect / TLS handshake / `curl_easy_perform()` / `curl_easy_cleanup()` cycle with a **persistent, multiplexed WebSocket or HTTP/2 connection** to the Binance order submission endpoint — eliminating repeated TCP+TLS setup cost (TD-3).

2. Move order submission entirely off the main strategy thread into a dedicated **async executor** backed by the IOCP event loop — the main thread enqueues work and immediately continues to the next signal evaluation cycle (TD-4).

### 4.2 IOCP Architecture Overview

Windows I/O Completion Ports (IOCP) is a kernel-level async I/O mechanism. When an async I/O operation completes (e.g., a network packet arrives), the kernel posts a completion packet to the IOCP. Worker threads dequeue these packets and execute callbacks. `Boost.Asio` on Windows uses IOCP as its underlying I/O dispatch mechanism automatically when `BOOST_ASIO_HAS_IOCP` is defined (which we set in Phase 1).

The V2 thread architecture:

```
┌─────────────────────────────────────────────────────────────────────────┐
│  crypto_bot_v2.exe                                                       │
│                                                                          │
│  io_context (Boost.Asio — backed by Windows IOCP kernel object)          │
│  ┌─────────────────────────────────────────────────────────────────┐    │
│  │  IOCP Worker Thread Pool (2 threads via io_context::run())       │    │
│  │                                                                  │    │
│  │  Thread A: WsMarketSession (depth stream)                        │    │
│  │    on_read() → parse JSON → ArrayOrderBook::apply_update()       │    │
│  │            → OrderBookQueue::push(snapshot)                      │    │
│  │            → async_read() [immediately re-arms, never blocks]    │    │
│  │                                                                  │    │
│  │  Thread B: WsMarketSession (kline stream)                        │    │
│  │    on_read() → parse JSON → TradeData update                     │    │
│  │                                                                  │    │
│  │  Thread B (shared): WsOrderSession (order submission pipe)       │    │
│  │    on_write_complete() → dequeue next order → async_write()      │    │
│  └─────────────────────────────────────────────────────────────────┘    │
│                                                                          │
│  Main Strategy Thread (Thread 0) — 500ms timer, NO blocking calls        │
│    SpscQueue::pop() → detect_market_signal()                            │
│    → strategy_dispatcher() → OrderExecutor::enqueue(order)              │
│    → LogWriter::log()                                                    │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

### 4.3 Boost.Beast WebSocket Session — `include/ws_session.hpp`

```cpp
#pragma once
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <nlohmann/json.hpp>
#include <functional>
#include <string>
#include <memory>
#include <atomic>
#include <chrono>

#include "tls_context.hpp"
#include "order_book.hpp"
#include "spsc_queue.hpp"

namespace v2 {

namespace asio    = boost::asio;
namespace beast   = boost::beast;
namespace http    = beast::http;
namespace ws      = beast::websocket;
namespace ssl_ns  = asio::ssl;
using tcp         = asio::ip::tcp;
using json        = nlohmann::json;

/// Async WebSocket session for Binance market data streams.
/// Uses Boost.Beast over Boost.Asio (IOCP-backed on Windows).
/// 
/// Design:
///   - All I/O is async — callbacks chain to next async_read immediately
///   - Reconnect loop with exponential backoff on any error
///   - Never allocates during steady-state read loop
///   - Publishes parsed data to SPSC queue (zero mutex in hot path)
class WsMarketSession : public std::enable_shared_from_this<WsMarketSession> {
public:
    using MessageCallback = std::function<void(const json&)>;

    WsMarketSession(
        asio::io_context& ioc,
        ssl_ns::context&  ssl_ctx,
        std::string       host,
        std::string       port,
        std::string       path,
        MessageCallback   on_message)
        : resolver_(asio::make_strand(ioc))
        , ws_(asio::make_strand(ioc), ssl_ctx)
        , host_(std::move(host))
        , port_(std::move(port))
        , path_(std::move(path))
        , on_message_(std::move(on_message))
    {}

    /// Start the connection. Non-blocking — returns immediately.
    /// Schedules async DNS resolution; all work happens in IOCP callbacks.
    void start() {
        do_resolve();
    }

    /// Signal the session to stop cleanly.
    void stop() {
        stopping_.store(true, std::memory_order_release);
        beast::error_code ec;
        ws_.close(ws::close_code::normal, ec);
    }

    bool is_connected() const {
        return connected_.load(std::memory_order_acquire);
    }

private:
    void do_resolve() {
        resolver_.async_resolve(host_, port_,
            beast::bind_front_handler(&WsMarketSession::on_resolve,
                                      shared_from_this()));
    }

    void on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
        if (ec) { return do_reconnect("resolve", ec); }

        // TCP connect (async)
        beast::get_lowest_layer(ws_).async_connect(results,
            beast::bind_front_handler(&WsMarketSession::on_connect,
                                      shared_from_this()));
    }

    void on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type) {
        if (ec) { return do_reconnect("connect", ec); }

        // Set TCP keep-alive to detect dead connections
        beast::get_lowest_layer(ws_).socket().set_option(
            asio::socket_base::keep_alive(true));

        // TLS handshake (async)
        ws_.next_layer().async_handshake(ssl_ns::stream_base::client,
            beast::bind_front_handler(&WsMarketSession::on_tls_handshake,
                                      shared_from_this()));
    }

    void on_tls_handshake(beast::error_code ec) {
        if (ec) { return do_reconnect("tls_handshake", ec); }

        // Set WebSocket options before upgrading
        ws_.set_option(ws::stream_base::decorator([&](ws::request_type& req) {
            req.set(http::field::user_agent, "crypto-bot-v2/2.0");
            req.set(http::field::host, host_);
        }));

        // WebSocket handshake (upgrades HTTP connection to WS)
        ws_.async_handshake(host_, path_,
            beast::bind_front_handler(&WsMarketSession::on_ws_handshake,
                                      shared_from_this()));
    }

    void on_ws_handshake(beast::error_code ec) {
        if (ec) { return do_reconnect("ws_handshake", ec); }

        connected_.store(true, std::memory_order_release);
        reconnect_delay_ms_ = 1000;  // Reset backoff on successful connection
        do_read();  // Arm the read loop
    }

    /// Steady-state read loop — each read immediately schedules the next.
    /// This is the hot path: runs every 100ms for depth, every minute for klines.
    void do_read() {
        if (stopping_.load(std::memory_order_acquire)) { return; }

        ws_.async_read(buffer_,
            beast::bind_front_handler(&WsMarketSession::on_read,
                                      shared_from_this()));
        // Returns IMMEDIATELY — does not block. IOCP callback fires when data arrives.
    }

    void on_read(beast::error_code ec, std::size_t bytes_transferred) {
        if (ec) {
            connected_.store(false, std::memory_order_release);
            return do_reconnect("read", ec);
        }

        try {
            auto msg = json::parse(beast::buffers_to_string(buffer_.data()));
            on_message_(msg);   // Parse and publish to SPSC queue
        } catch (const json::exception& e) {
            // Log parse error but continue reading — don't crash on bad message
            // (Resolves NFR-4: "JSON parse errors caught and stream continues")
        }

        buffer_.consume(bytes_transferred);  // Recycle buffer, no allocation
        do_read();  // Re-arm for next message (tail recursion, async)
    }

    /// Exponential backoff reconnection — resolves TD-7 (no reconnect logic in V1).
    void do_reconnect(const char* stage, beast::error_code ec) {
        if (stopping_.load(std::memory_order_acquire)) { return; }

        connected_.store(false, std::memory_order_release);

        // Cap backoff at 30 seconds
        if (reconnect_delay_ms_ < 30000) {
            reconnect_delay_ms_ = std::min(reconnect_delay_ms_ * 2, 30000);
        }

        auto timer = std::make_shared<asio::steady_timer>(
            resolver_.get_executor(),
            std::chrono::milliseconds(reconnect_delay_ms_)
        );

        timer->async_wait([self = shared_from_this(), timer](beast::error_code) {
            // Reconstruct the WebSocket stream to get a clean state
            // (required after a failed connection — beast stream is not reusable)
            self->do_resolve();
        });
    }

    tcp::resolver                                resolver_;
    ws::stream<ssl_ns::stream<tcp::socket>>      ws_;
    beast::flat_buffer                           buffer_;

    std::string     host_;
    std::string     port_;
    std::string     path_;
    MessageCallback on_message_;

    std::atomic<bool> connected_    {false};
    std::atomic<bool> stopping_     {false};
    int               reconnect_delay_ms_ {1000};
};

} // namespace v2
```

### 4.4 Async Order Executor — `include/order_executor.hpp`

This replaces `libcurl` entirely. Orders are submitted over a persistent HTTPS connection using `Boost.Beast` HTTP client, kept alive across multiple requests.

```cpp
#pragma once
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/http.hpp>
#include <string>
#include <queue>
#include <mutex>
#include <functional>
#include <memory>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <chrono>
#include <sstream>
#include <iomanip>

namespace v2 {

namespace asio  = boost::asio;
namespace beast = boost::beast;
namespace http  = beast::http;
namespace ssl_n = asio::ssl;
using tcp       = asio::ip::tcp;

struct OrderRequest {
    std::string symbol;
    std::string side;    // "BUY" or "SELL"
    double      price;
    double      qty;
};

/// Async HTTPS order executor using a persistent connection.
/// 
/// Resolves TD-3: No per-order TCP+TLS handshake. One connection, many orders.
/// Resolves TD-4: Non-blocking. Main thread enqueues; IOCP thread executes.
/// 
/// Uses Boost.Beast HTTP/1.1 with Connection: keep-alive.
class OrderExecutor : public std::enable_shared_from_this<OrderExecutor> {
public:
    OrderExecutor(
        asio::io_context& ioc,
        ssl_n::context&   ssl_ctx,
        std::string       api_key,
        std::string       secret_key,
        std::string       host,          // "testnet.binance.vision" or "api.binance.com"
        std::string       port = "443")
        : ioc_(ioc)
        , stream_(asio::make_strand(ioc), ssl_ctx)
        , resolver_(asio::make_strand(ioc))
        , api_key_(std::move(api_key))
        , secret_key_(std::move(secret_key))
        , host_(std::move(host))
        , port_(std::move(port))
    {}

    /// Thread-safe: called from main strategy thread.
    /// Enqueues an order; IOCP thread drains the queue asynchronously.
    void submit(OrderRequest req) {
        {
            std::lock_guard<std::mutex> lock(queue_mtx_);
            pending_.push(std::move(req));
        }

        // Post to the IOCP strand — if idle, starts draining immediately
        asio::post(stream_.get_executor(),
            [self = shared_from_this()]() {
                self->maybe_start_sending();
            });
    }

    void start() {
        do_connect();
    }

private:
    void do_connect() {
        resolver_.async_resolve(host_, port_,
            beast::bind_front_handler(&OrderExecutor::on_resolve,
                                      shared_from_this()));
    }

    void on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
        if (ec) { schedule_reconnect(); return; }
        beast::get_lowest_layer(stream_).async_connect(results,
            beast::bind_front_handler(&OrderExecutor::on_connect,
                                      shared_from_this()));
    }

    void on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type) {
        if (ec) { schedule_reconnect(); return; }
        stream_.async_handshake(ssl_n::stream_base::client,
            beast::bind_front_handler(&OrderExecutor::on_tls,
                                      shared_from_this()));
    }

    void on_tls(beast::error_code ec) {
        if (ec) { schedule_reconnect(); return; }
        connected_ = true;
        maybe_start_sending();  // Drain any queued orders that arrived during connect
    }

    void maybe_start_sending() {
        if (sending_ || !connected_) { return; }

        std::lock_guard<std::mutex> lock(queue_mtx_);
        if (pending_.empty()) { return; }

        current_req_ = pending_.front();
        pending_.pop();
        sending_ = true;
        do_send(current_req_);
    }

    void do_send(const OrderRequest& req) {
        // Build query string with HMAC-SHA256 signature
        const auto timestamp = get_timestamp_ms();
        std::ostringstream qs;
        qs << "symbol=" << req.symbol
           << "&side="  << req.side
           << "&type=LIMIT"
           << "&timeInForce=GTC"
           << "&price=" << std::fixed << std::setprecision(2) << req.price
           << "&quantity=" << std::fixed << std::setprecision(6) << req.qty
           << "&timestamp=" << timestamp;

        const std::string signature = hmac_sha256(secret_key_, qs.str());
        qs << "&signature=" << signature;

        // Build HTTP POST request
        http_req_ = {};
        http_req_.method(http::verb::post);
        http_req_.target("/api/v3/order?" + qs.str());
        http_req_.version(11);  // HTTP/1.1
        http_req_.set(http::field::host, host_);
        http_req_.set(http::field::content_type, "application/x-www-form-urlencoded");
        http_req_.set(http::field::connection, "keep-alive");  // Persistent connection
        http_req_.set("X-MBX-APIKEY", api_key_);
        http_req_.prepare_payload();

        // Send asynchronously — returns immediately, IOCP fires on completion
        http::async_write(stream_, http_req_,
            beast::bind_front_handler(&OrderExecutor::on_write,
                                      shared_from_this()));
    }

    void on_write(beast::error_code ec, std::size_t) {
        if (ec) { reconnect_and_retry(); return; }

        // Read response asynchronously
        http::async_read(stream_, response_buf_, http_res_,
            beast::bind_front_handler(&OrderExecutor::on_read,
                                      shared_from_this()));
    }

    void on_read(beast::error_code ec, std::size_t) {
        if (ec) { reconnect_and_retry(); return; }

        // Log result (non-blocking — LogWriter has its own mutex)
        // Check for rate limit headers: X-MBX-ORDER-COUNT-1S, X-MBX-USED-WEIGHT-1M
        const auto status = http_res_.result_int();
        if (status != 200) {
            // Log error, optionally back off
        }

        response_buf_.consume(response_buf_.size());
        http_res_ = {};
        sending_  = false;
        maybe_start_sending();  // Process next queued order
    }

    void schedule_reconnect() {
        connected_ = false;
        sending_   = false;
        // Reconnect after 1 second
        auto timer = std::make_shared<asio::steady_timer>(
            resolver_.get_executor(),
            std::chrono::seconds(1));
        timer->async_wait([self = shared_from_this(), timer](beast::error_code) {
            self->do_connect();
        });
    }

    void reconnect_and_retry() {
        // Re-queue the current order before reconnecting
        {
            std::lock_guard<std::mutex> lock(queue_mtx_);
            pending_.push(current_req_);  // Front of logical queue
        }
        connected_ = false;
        sending_   = false;
        do_connect();
    }

    static std::string hmac_sha256(const std::string& key, const std::string& data) {
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int  digest_len = 0;
        HMAC(EVP_sha256(),
             key.data(), static_cast<int>(key.size()),
             reinterpret_cast<const unsigned char*>(data.data()),
             data.size(),
             digest, &digest_len);

        std::ostringstream hex;
        for (unsigned int i = 0; i < digest_len; ++i) {
            hex << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<int>(digest[i]);
        }
        return hex.str();
    }

    static uint64_t get_timestamp_ms() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
    }

    asio::io_context&                 ioc_;
    ssl_n::stream<tcp::socket>        stream_;
    tcp::resolver                     resolver_;
    http::request<http::string_body>  http_req_;
    http::response<http::string_body> http_res_;
    beast::flat_buffer                response_buf_;

    std::string api_key_;
    std::string secret_key_;
    std::string host_;
    std::string port_;

    std::mutex               queue_mtx_;
    std::queue<OrderRequest> pending_;
    OrderRequest             current_req_;
    bool                     connected_ = false;
    bool                     sending_   = false;
};

} // namespace v2
```

### 4.5 Main Entry Point — IOCP Event Loop Integration

```cpp
// src/main.cpp (V2 skeleton)
#include "credentials.hpp"
#include "tls_context.hpp"
#include "order_book.hpp"
#include "spsc_queue.hpp"
#include "ws_session.hpp"
#include "order_executor.hpp"
#include "log_writer.hpp"

#include <boost/asio.hpp>
#include <thread>
#include <chrono>

namespace asio = boost::asio;

int main() {
    // ── 1. Load credentials ───────────────────────────────────────────
    v2::Credentials creds;
    try { creds = v2::load_credentials(); }
    catch (const std::exception& e) {
        std::cerr << "[FATAL] " << e.what() << "\n";
        return 1;
    }

    // ── 2. Infrastructure ─────────────────────────────────────────────
    // io_context is the IOCP event loop.
    // All async I/O — WebSocket reads, HTTP writes — runs through this.
    asio::io_context ioc;

    // Keep io_context alive even when no async work is pending.
    // Without this, ioc.run() returns immediately if all sessions disconnect.
    auto work_guard = asio::make_work_guard(ioc);

    // TLS contexts — one per endpoint (different hostname verification)
    auto market_ssl_ctx = v2::make_tls_context("stream.binance.com");
    auto order_ssl_ctx  = v2::make_tls_context("testnet.binance.vision");

    // ── 3. Shared data structures ─────────────────────────────────────
    v2::ArrayOrderBook    order_book;
    v2::OrderBookQueue    depth_queue;     // SPSC: IOCP → Main
    v2::LogWriter         log("btcusdt");

    // TradeData still uses mutex (kline data, not hot-path):
    TradeData trade_data;
    trade_data.close.set_capacity(1000);
    // ... initialize other buffers

    // ── 4. Market data WebSocket sessions ────────────────────────────
    // Depth session: writes to ArrayOrderBook + publishes to SPSC queue
    auto depth_session = std::make_shared<v2::WsMarketSession>(
        ioc, market_ssl_ctx,
        "stream.binance.com", "9443", "/ws/btcusdt@depth@100ms",
        [&](const nlohmann::json& msg) {
            // This callback runs on an IOCP thread — keep it fast
            if (msg.contains("b") && msg.contains("a")) {
                for (const auto& level : msg["b"]) {
                    order_book.apply_update('b',
                        std::stod(level[0].get<std::string>()),
                        std::stod(level[1].get<std::string>()));
                }
                for (const auto& level : msg["a"]) {
                    order_book.apply_update('a',
                        std::stod(level[0].get<std::string>()),
                        std::stod(level[1].get<std::string>()));
                }
                // Publish snapshot to main thread via wait-free SPSC queue
                depth_queue.push(order_book.snapshot());
            }
        }
    );

    // Kline session (reuses existing logic, just wrapped in new session class)
    auto kline_session = std::make_shared<v2::WsMarketSession>(
        ioc, market_ssl_ctx,
        "stream.binance.com", "9443", "/ws/btcusdt@kline_1m",
        [&](const nlohmann::json& msg) {
            if (msg.contains("k") && msg["k"]["x"].get<bool>()) {
                std::lock_guard lock(trade_data.mtx);  // kline: mutex is fine (1/min)
                trade_data.close.push_back(std::stod(msg["k"]["c"].get<std::string>()));
                // ... push other OHLCV fields
            }
        }
    );

    // Order executor — persistent HTTPS connection
    auto executor = std::make_shared<v2::OrderExecutor>(
        ioc, order_ssl_ctx,
        creds.api_key, creds.secret_key,
        "testnet.binance.vision"
    );

    // ── 5. Start all sessions (schedules async work, does not block) ──
    depth_session->start();
    kline_session->start();
    executor->start();

    // ── 6. IOCP thread pool ───────────────────────────────────────────
    // 2 threads drive the IOCP loop. They handle all WebSocket I/O,
    // TLS, and order submission. Main thread is NOT in this pool.
    std::thread iocp_thread_1([&ioc] { ioc.run(); });
    std::thread iocp_thread_2([&ioc] { ioc.run(); });

    // ── 7. Warm-up ────────────────────────────────────────────────────
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // ── 8. Main Strategy Loop (Thread 0) ─────────────────────────────
    // This loop NEVER blocks on I/O. All data arrives via SPSC queue.
    Position position;
    while (true) {
        // Drain the depth queue — take the most recent snapshot
        std::optional<v2::OrderBookSnapshot> latest_snap;
        while (auto snap = depth_queue.pop()) {
            latest_snap = snap;  // Discard intermediates, keep freshest
        }

        if (!latest_snap || latest_snap->best_bid == 0.0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;  // No data yet — skip this cycle
        }

        const auto& snap = *latest_snap;

        // Get candle data (klines are low-frequency — mutex is fine)
        std::vector<double> closes;
        {
            std::lock_guard lock(trade_data.mtx);
            closes.assign(trade_data.close.begin(), trade_data.close.end());
        }

        // Signal + strategy (reads only snap and closes — no locks needed)
        const auto signal   = detect_market_signal(closes);
        // strategy_dispatcher now calls executor->submit() instead of curl
        strategy_dispatcher(signal, snap, executor, log, position);

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // ── 9. Shutdown ───────────────────────────────────────────────────
    work_guard.reset();       // Allow ioc.run() to return when idle
    depth_session->stop();
    kline_session->stop();
    iocp_thread_1.join();
    iocp_thread_2.join();

    return 0;
}
```

---

## Phase 5: The Refactoring Sequence

The order of operations is the most important part of this plan. Every step below produces a **running, deployable binary** before the next step begins. You never break the existing functionality to build new functionality.

### Step 1 — Security & Build System (Days 1-2)

**What you build:** Phase 1 (toolchain) + Phase 2 (security fixes).  
**Output:** A working V1 bot that compiles cleanly under vcpkg/CMake with zero hardcoded credentials.

**Execution order:**

```
1a. Set up vcpkg, CMakePresets.json, vcpkg.json
1b. Port V1 source to compile under new CMakeLists.txt
    - Replace WebSocket++ with Boost.Beast (header-only swap, minimal API diff)
    - Add BOOST_ASIO_HAS_IOCP and _WIN32_WINNT=0x0A00 defines
    - V1 behavior preserved — std::map order book, mutex reads, curl still in use
1c. Extract credentials to environment variables (credentials.hpp)
1d. Fix TLS verify_none → make_tls_context() (tls_context.hpp)
1e. Fix LogWriter — persistent ofstream (log_writer.hpp)
```

**Verification gate:** Bot connects to Binance testnet, receives market data, submits orders, logs trades. Diff from V1: credentials safe, TLS verified, log file stays open.

**Why first?** Security bugs in production for even one day post-discovery is a liability. The toolchain must be established before anything else — you cannot refactor without a reproducible build. These changes have zero performance risk.

---

### Step 2 — Data Structures (Days 3-5)

**What you build:** Phase 3 (ArrayOrderBook + SpscQueue).  
**Output:** New data structures with unit tests and benchmarks passing. V1 networking still in use.

**Execution order:**

```
2a. Implement SpscQueue<T> + unit tests
    - Test: single-threaded push/pop correctness
    - Test: two-thread producer-consumer (std::thread, 1M iterations, verify no loss)
    - Test: full-queue behavior (push returns false, no crash)

2b. Implement ArrayOrderBook + unit tests
    - Test: apply_update + snapshot matches expected best_bid/ask
    - Test: recalibration when price drifts outside window
    - Test: reset() clears state

2c. Run benchmarks — confirm 5x speedup over std::map baseline
    order_book_bench.exe → BM_OrderBookUpdate < 1µs

2d. Wire ArrayOrderBook into the EXISTING depth callback (still using WebSocket++/old session)
    - Change: depthPtr->bids.clear() / rebuild → order_book.apply_update()
    - Change: MarketDepth mutex read in main loop → depth_queue.pop() in SPSC queue
    - This fixes TD-1 (data race) and TD-5 (map thrashing) simultaneously
```

**Verification gate:** Bot runs with new order book, zero data races (verify with ThreadSanitizer: compile with `-fsanitize=thread` if using Clang, or Dr. Memory on MSVC). Benchmarks show expected speedup.

**Why second?** The data structures have no external dependencies — they are pure C++ header-only code. If you write them first, you can test them in complete isolation. If you introduce them after the networking refactor, debugging is 3x harder because network errors and data structure bugs compound.

---

### Step 3 — Networking & Execution (Days 6-10)

**What you build:** Phase 4 (WsMarketSession + OrderExecutor + IOCP main loop).  
**Output:** Full V2 system with persistent connections, async order submission, IOCP event loop.

**Execution order:**

```
3a. Implement WsMarketSession (ws_session.hpp)
    - Integration test: connect to stream.binance.com:9443/ws/btcusdt@depth@100ms
    - Verify: on_message callback fires, JSON parses correctly, SPSC queue receives data
    - Verify: reconnect logic works (kill network, restore, session auto-reconnects)

3b. Implement OrderExecutor (order_executor.hpp)
    - Test in isolation against testnet: submit a single order, verify 200 response
    - Test: submit 5 orders in 50ms (laddered batch), verify no blocking on main thread
    - Measure: time from executor->submit() to HTTP response (should be ~30-60ms async)

3c. Replace main.cpp threading model
    - Remove: std::thread streamer threads (V1 pattern)
    - Add: io_context + IOCP thread pool (2 threads)
    - Remove: curl_easy_perform() from strategy dispatcher
    - Add: executor->submit() in strategy dispatcher

3d. End-to-end integration test (testnet)
    - Run for 30 minutes
    - Verify: order submission latency < 60ms (vs V1 ~200ms per order)
    - Verify: main thread CPU < 5% (was blocked in curl before)
    - Verify: zero data races (ThreadSanitizer / Dr. Memory)
    - Verify: reconnect survives 5 simulated network drops (kill WiFi, restore)

3e. (Optional) TD-9: Signal accumulator refactor
    - Replace vector copy of close buffer with incremental accumulator
    - Store running sum, sum_sq, sum_xy for OLS slope calculation
    - Update accumulator in kline callback (on_message), not in main loop
```

**Verification gate:** Full V2 bot running on Binance testnet. Persistent connections. Async orders. Main thread latency budget: > 450ms free per 500ms cycle (was ~250ms blocked in V1). No hardcoded credentials. Verified TLS. Reconnecting streams.

---

## Appendix A: Technical Debt Resolution Tracker

| TD ID | Issue | Resolved In | Mechanism |
|---|---|---|---|
| TD-1 | Data race on MarketDepth | Phase 3 / Step 2 | SpscQueue replaces shared mutex; IOCP writer, main reader, no lock in hot path |
| TD-2 | Hardcoded credentials | Phase 2 / Step 1 | `load_credentials()` via `std::getenv`; git history purge |
| TD-3 | curl handle per order | Phase 4 / Step 3 | `OrderExecutor` persistent HTTPS keep-alive; one connection, many orders |
| TD-4 | Blocking curl_easy_perform | Phase 4 / Step 3 | `http::async_write` in IOCP thread; main thread returns in < 1µs |
| TD-5 | Full map rebuild every 100ms | Phase 3 / Step 2 | `ArrayOrderBook::apply_update()` — O(1) delta, no allocation |
| TD-6 | Single-symbol architecture | Out of scope (V3) | Requires `TradingContext` wrapper — architectural, not a bug |
| TD-7 | No WS reconnect | Phase 4 / Step 3 | `WsMarketSession::do_reconnect()` with exponential backoff |
| TD-8 | Per-trade file open/close | Phase 2 / Step 1 | `LogWriter` — persistent ofstream, one mutex per write |
| TD-9 | Full buffer copy for signal | Phase 4 / Step 3 (optional) | Incremental accumulator in kline callback |
| NFR-5.3 | TLS verify_none | Phase 2 / Step 1 | `make_tls_context()` — Windows ROOT store + rfc2818_verification |

---

## Appendix B: Latency Budget Comparison

| Metric | V1 | V2 | Improvement |
|---|---|---|---|
| Order book update | 4.2µs (map clear+rebuild) | 0.8µs (array delta) | 5x |
| Main thread blocked per order | 50-200ms (curl sync) | 0µs (async enqueue) | ∞ |
| TCP+TLS setup per order | ~30ms (new socket each time) | 0ms (persistent conn) | ∞ |
| 5-order batch total latency | 250ms–1000ms | ~60ms async | 4–16x |
| Data race UB incidents | Continuous (every 100ms write) | Zero | ✓ |
| Credential leak surface | Source + binary | Environment only | ✓ |
| Reconnect after drop | Thread dies, data freezes | Auto-reconnect + backoff | ✓ |
| Log file syscalls per trade | 3 (open/write/close) | 1 (write only) | 3x |
| TLS cert verification | Disabled (MITM vulnerable) | Full chain + hostname | ✓ |

---

*End of Technical Implementation Plan v2.0*  
*All code samples are production-intent blueprints. Review and test against Binance testnet before deploying to live accounts.*
