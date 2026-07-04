cmake_minimum_required(VERSION 3.27)
project(crypto_bot_v2 VERSION 2.0.0 LANGUAGES CXX)

# ── C++ Standard ──────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── vcpkg Toolchain ───────────────────────────────────────────────────────────
# The toolchain file is injected via CMakePresets.json (CMAKE_TOOLCHAIN_FILE).
# It must be set before project() in practice; CMake presets handle this.

# ── Dependencies via vcpkg ────────────────────────────────────────────────────
find_package(Boost 1.84 REQUIRED COMPONENTS system random)
find_package(OpenSSL 3.2 REQUIRED)
find_package(nlohmann_json 3.11 REQUIRED)
find_package(CURL REQUIRED)           # V1-bridge: trader.cpp uses libcurl; removed in Phase 4

# ── MSVC-specific flags ───────────────────────────────────────────────────────
if(MSVC)
    # /W4  : High warning level
    # /WX  : Warnings as errors (enforced in new code)
    # /wd4996: Suppress deprecated POSIX name warnings (OpenSSL internals use them)
    # /MP  : Parallel compilation (one CL.exe per core)
    add_compile_options(/W4 /wd4996 /MP)  # /WX removed: V1 legacy code has warnings; restore in Phase 4

    add_compile_definitions(
        BOOST_ASIO_HAS_IOCP          # Explicitly enable the IOCP backend
        BOOST_ASIO_DISABLE_EPOLL     # Not applicable on Windows; defense-in-depth
        _WIN32_WINNT=0x0A00          # Target Windows 10+
        WIN32_LEAN_AND_MEAN          # Exclude rarely-used Windows headers
        NOMINMAX                     # Prevent Windows.h from defining min/max macros
        BOOST_DATE_TIME_NO_LIB       # No auto-link of date_time (unused here)
        OPENSSL_NO_DEPRECATED        # Enforce modern OpenSSL API only
    )
endif()

# ── Main Executable ───────────────────────────────────────────────────────────
# Phase 5 (current): sources live at the project root.
# Phases 3–4 will migrate them into src/ and add new translation units.
add_executable(crypto_bot
    src/main.cpp
    src/streamer.cpp
    src/trader.cpp
    src/log_writer.cpp       # Phase 5 (TD-8 fix) — persistent ofstream logger
    # src/credentials.cpp    # Phase 5: header-only (inline), no .cpp needed
    # src/order_book.cpp     # Phase 3
    # src/spsc_queue.cpp     # Phase 3 (mostly header)
    # src/ws_session.cpp     # Phase 4
    # src/order_executor.cpp # Phase 4
)

target_include_directories(crypto_bot PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${Boost_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/websocketpp    # V1-bridge: local WebSocket++ headers; removed in Phase 4
)

target_link_libraries(crypto_bot PRIVATE
    Boost::system
    Boost::random     # V1-bridge: websocketpp/random.hpp needs the compiled random_device
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    CURL::libcurl     # V1-bridge: trader.cpp uses libcurl; removed in Phase 4
    ws2_32            # Winsock2 — required for all network I/O on Windows
    crypt32           # Windows CryptoAPI — required for certificate store access
    Secur32           # Windows Security Support Provider Interface
)

# ── Benchmarks ────────────────────────────────────────────────────────────────
# Uncomment once benchmarks/ directory is populated in Phase 3.
# find_package(benchmark REQUIRED)
# add_subdirectory(benchmarks)

# ── Tests ─────────────────────────────────────────────────────────────────────
enable_testing()
# add_subdirectory(tests)   # Uncomment once tests/ is populated in Phase 3.

add_executable(benchmark_run benchmark.cpp)
target_include_directories(benchmark_run PRIVATE ${CMAKE_SOURCE_DIR}/include ${Boost_INCLUDE_DIRS})
target_link_libraries(benchmark_run PRIVATE OpenSSL::Crypto nlohmann_json::nlohmann_json crypt32 ws2_32 Secur32)
