// benchmark.cpp — standalone profiling harness for the crypto trading engine

#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <vector>
#include <string>
#include <chrono>
#include <cmath>
#include <numeric>
#include <algorithm>
#include <thread>
#include <mutex>
#include <atomic>
#include <filesystem>
#include <functional>

#include <nlohmann/json.hpp>
#include <boost/circular_buffer.hpp>
#include <openssl/hmac.h>

using Clock = std::chrono::high_resolution_clock;
using ns    = std::chrono::nanoseconds;
using us    = std::chrono::microseconds;
using json  = nlohmann::json;

// ============================================================================
// UTILITY
// ============================================================================
template <typename T> T percentile(std::vector<T>& v, double pct) {
    size_t idx = static_cast<size_t>(pct / 100.0 * (v.size() - 1));
    std::nth_element(v.begin(), v.begin() + idx, v.end());
    return v[idx];
}
template <typename T> double mean_val(const std::vector<T>& v) {
    double sum = 0;
    for (auto& x : v) sum += static_cast<double>(x);
    return sum / v.size();
}

// ============================================================================
// 1. SIGNAL COMPUTE
// ============================================================================
std::string detect_market_signal(const std::vector<double>& closes) {
    const size_t N = closes.size();
    if (N < 20) return "neutral";

    double sum_x = 0, sum_y = 0, sum_xy = 0, sum_x2 = 0;
    for (size_t i = 0; i < N; ++i) {
        sum_x  += i;
        sum_y  += closes[i];
        sum_xy += i * closes[i];
        sum_x2 += i * i;
    }
    double slope = (N * sum_xy - sum_x * sum_y) / std::max((double)(N * sum_x2 - sum_x * sum_x), 1e-8);

    double mean = std::accumulate(closes.begin(), closes.end(), 0.0) / N;
    double sq_sum = std::accumulate(closes.begin(), closes.end(), 0.0, [&](double acc, double val) {
        return acc + (val - mean) * (val - mean);
    });
    double stddev = std::sqrt(sq_sum / N);

    double momentum = closes.back() - closes.front();
    double roc = (closes.back() - closes[N - 10]) / closes[N - 10];

    double score = 0.0;
    score += std::clamp(slope * 100, -2.0, 2.0);
    score += (momentum / mean) * 2.0;
    score += roc * 100.0;
    if (stddev / mean > 0.01) score *= 0.8;

    if (score >  5.0) return "very bullish";
    if (score >  1.5) return "bullish";
    if (score < -5.0) return "very bearish";
    if (score < -1.5) return "bearish";
    return "neutral";
}

// ============================================================================
// 2. HMAC-SHA256 (OpenSSL exactly as in trader.cpp)
// ============================================================================
std::string hmac_sha256_bench(const std::string& key, const std::string& data) {
    unsigned char* digest = HMAC(EVP_sha256(), key.c_str(), key.length(),
                                  reinterpret_cast<const unsigned char*>(data.c_str()), data.length(), NULL, NULL);
    std::ostringstream oss;
    for (int i = 0; i < 32; i++)
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)digest[i];
    return oss.str();
}

// ============================================================================
// 3. LogWriter — RAII persistent logger
// ============================================================================
class LogWriter {
public:
    explicit LogWriter(const std::string& symbol) {
        std::filesystem::create_directories("bench_logs");
        std::string path = "bench_logs/" + symbol + ".csv";
        file_.open(path, std::ios::app);
        if (file_.tellp() == std::streampos(0))
            file_ << "timestamp,side,price,quantity\n";
    }
    ~LogWriter() { if (file_.is_open()) { file_.flush(); file_.close(); } }
    void log(const std::string& side, double price, double qty) {
        auto now = std::chrono::system_clock::now();
        auto t   = std::chrono::system_clock::to_time_t(now);
        std::lock_guard<std::mutex> lk(mtx_);
        file_ << std::put_time(std::gmtime(&t), "%Y-%m-%dT%H:%M:%SZ")
              << "," << side
              << "," << std::fixed << std::setprecision(2) << price
              << "," << std::fixed << std::setprecision(6) << qty << "\n";
    }
private:
    std::ofstream file_;
    std::mutex mtx_;
};
void log_trade_v1(const std::string& symbol, const std::string& side, double price, double qty) {
    std::string filename = "bench_logs/" + symbol + "_v1.csv";
    std::ofstream file(filename, std::ios::app);
    if (file.is_open())
        file << "2026-01-01T00:00:00Z," << side << "," << price << "," << qty << "\n";
}

// ============================================================================
// Mock WebSocket payload
// ============================================================================
static const char* MOCK_WS_PAYLOAD = R"({
  "e":"kline","E":1719500000000,"s":"BTCUSDT",
  "k":{"t":1719500000000,"T":1719500059999,
       "s":"BTCUSDT","i":"1m",
       "o":"67843.21","c":"67851.40","h":"67860.00","l":"67835.50",
       "v":"12.345678","x":true}
})";

// ============================================================================
// BENCHMARKS
// ============================================================================
int main() {
    std::cout << "============================================================\n";
    std::cout << "  CRYPTO TRADING ENGINE — PERFORMANCE BENCHMARK (V2)\n";
    std::cout << "============================================================\n\n";

    const int WARM = 1000;
    const int ITERS = 100000;

    std::vector<double> closes(1000);
    for (int i = 0; i < 1000; ++i)
        closes[i] = 67000.0 + 500.0 * std::sin(i * 0.01) + (i % 7) * 0.5;

    // [1] SIGNAL COMPUTE
    {
        for (int i = 0; i < WARM; ++i) detect_market_signal(closes);
        std::vector<long long> times; times.reserve(ITERS);
        for (int i = 0; i < ITERS; ++i) {
            auto t0 = Clock::now();
            volatile auto sig = detect_market_signal(closes);
            auto t1 = Clock::now();
            times.push_back(std::chrono::duration_cast<ns>(t1 - t0).count());
        }
        std::sort(times.begin(), times.end());
        std::cout << "[1] SIGNAL COMPUTE (OLS+Momentum+ROC) — N=1000 closes\n";
        std::cout << "    Median:  " << times[times.size()/2] << " ns\n";
        std::cout << "    Mean:    " << (long long)mean_val(times) << " ns\n";
        std::cout << "    p99:     " << percentile(times, 99.0) << " ns\n\n";
    }

    // [2] HMAC-SHA256 SIGNING (OpenSSL)
    {
        std::string key = "tH3S3cR3tK3yF0rBiNaNcE1234567890abcdef";
        std::string payload = "symbol=BTCUSDT&side=BUY&type=LIMIT&timeInForce=GTC"
                              "&quantity=0.00014700&price=67843.21000000&timestamp=1719500000000";
        for (int i = 0; i < WARM; ++i) hmac_sha256_bench(key, payload);
        std::vector<long long> times; times.reserve(ITERS);
        for (int i = 0; i < ITERS; ++i) {
            auto t0 = Clock::now();
            volatile auto h = hmac_sha256_bench(key, payload);
            auto t1 = Clock::now();
            times.push_back(std::chrono::duration_cast<ns>(t1 - t0).count());
        }
        std::sort(times.begin(), times.end());
        std::cout << "[2] HMAC-SHA256 SIGNING (OpenSSL) — 115 bytes\n";
        std::cout << "    Median:  " << times[times.size()/2] << " ns\n";
        std::cout << "    Mean:    " << (long long)mean_val(times) << " ns\n";
        std::cout << "    p99:     " << percentile(times, 99.0) << " ns\n\n";
    }

    // [3] MUTEX CONTENTION (Realistic payload: Copy 1000-element circular buffer)
    {
        struct SharedState {
            std::mutex mtx;
            boost::circular_buffer<double> closes{1000};
        } state;
        
        for (int i = 0; i < 1000; ++i) state.closes.push_back(closes[i]);

        const int OPS_PER_THREAD = 100000;
        std::atomic<long long> total_lock_ns{0};
        std::atomic<int>       total_ops{0};

        auto worker = [&](int id) {
            for (int i = 0; i < OPS_PER_THREAD; ++i) {
                auto t0 = Clock::now();
                {
                    std::lock_guard<std::mutex> lk(state.mtx);
                    // REALISTIC CONTENTION: O(N) copy, representing main loop handoff
                    std::vector<double> copy_closes(state.closes.begin(), state.closes.end());
                    volatile double sum = 0; // Prevent optimization out
                    for (auto c : copy_closes) sum += c;
                }
                auto t1 = Clock::now();
                total_lock_ns += std::chrono::duration_cast<ns>(t1 - t0).count();
                total_ops++;
            }
        };

        auto t0 = Clock::now();
        std::thread t1(worker, 0), t2(worker, 1), t3(worker, 2);
        t1.join(); t2.join(); t3.join();
        auto t1_end = Clock::now();

        long long wall_us = std::chrono::duration_cast<us>(t1_end - t0).count();
        double avg_lock_ns = (double)total_lock_ns.load() / total_ops.load();

        std::cout << "[3] MUTEX CONTENTION (1000-element copy) — 3 threads\n";
        std::cout << "    Total ops:        " << total_ops.load() << "\n";
        std::cout << "    Avg hold+wait:    " << (long long)avg_lock_ns << " ns/op\n";
        std::cout << "    Throughput:       " << (long long)(total_ops.load() * 1e6 / wall_us) << " ops/sec\n\n";
    }

    // [4] I/O
    {
        std::filesystem::create_directories("bench_logs");
        std::filesystem::remove("bench_logs/btcusdt.csv");
        std::filesystem::remove("bench_logs/btcusdt_v1.csv");
        const int LOG_ITERS = 10000;
        {
            LogWriter logger("btcusdt");
            auto t0 = Clock::now();
            for (int i = 0; i < LOG_ITERS; ++i) logger.log("BUY", 67843.21, 0.000147);
            auto t1 = Clock::now();
            std::cout << "[4] I/O — RAII LOGGER (V2)\n";
            std::cout << "    Per-write (V2): " << std::chrono::duration_cast<us>(t1 - t0).count() * 1000 / LOG_ITERS << " ns\n";
        }
        {
            auto t0 = Clock::now();
            for (int i = 0; i < LOG_ITERS; ++i) log_trade_v1("btcusdt", "BUY", 67843.21, 0.000147);
            auto t1 = Clock::now();
            std::cout << "    Per-write (V1): " << std::chrono::duration_cast<us>(t1 - t0).count() * 1000 / LOG_ITERS << " ns\n\n";
        }
    }

    // [5] FULL PIPELINE LATENCY (Realistic JSON Parse)
    {
        std::string payload_str(MOCK_WS_PAYLOAD);
        std::string hmac_key = "tH3S3cR3tK3yF0rBiNaNcE1234567890abcdef";
        double best_bid = 67843.21;
        double best_ask = 67851.40;

        std::vector<long long> pipeline_times;
        pipeline_times.reserve(ITERS);
        
        for (int i = 0; i < WARM; ++i) {
            json j = json::parse(payload_str);
            closes.back() = std::stod(j["k"]["c"].get<std::string>());
            detect_market_signal(closes);
            hmac_sha256_bench(hmac_key, "mock_signature");
        }

        for (int i = 0; i < ITERS; ++i) {
            auto t0 = Clock::now();

            // REALISTIC PARSING: parse JSON strings as in streamer.cpp
            json j = json::parse(payload_str);
            const auto& k = j["k"];
            double open_val  = std::stod(k["o"].get<std::string>());
            double close_val = std::stod(k["c"].get<std::string>());
            double high_val  = std::stod(k["h"].get<std::string>());
            double low_val   = std::stod(k["l"].get<std::string>());
            double volume    = std::stod(k["v"].get<std::string>());
            (void)open_val; (void)high_val; (void)low_val; (void)volume;

            closes.back() = close_val;
            std::string signal = detect_market_signal(closes);

            double mid_price = (best_bid + best_ask) / 2.0;
            double qty = 10.0 / mid_price;

            std::ostringstream params;
            params << std::fixed << std::setprecision(8);
            params << "symbol=BTCUSDT&side=BUY&type=LIMIT&timeInForce=GTC"
                   << "&quantity=" << qty
                   << "&price=" << best_ask
                   << "&timestamp=1719500000000";
            std::string signature = hmac_sha256_bench(hmac_key, params.str());

            auto t1 = Clock::now();
            pipeline_times.push_back(std::chrono::duration_cast<ns>(t1 - t0).count());
        }

        std::sort(pipeline_times.begin(), pipeline_times.end());
        std::cout << "[5] FULL PIPELINE (json::parse → stod → signal → dispatch → HMAC)\n";
        std::cout << "    Median:  " << pipeline_times[pipeline_times.size()/2] << " ns  ("
                  << pipeline_times[pipeline_times.size()/2] / 1000 << " us)\n";
        std::cout << "    Mean:    " << (long long)mean_val(pipeline_times) << " ns  ("
                  << (long long)mean_val(pipeline_times) / 1000 << " us)\n";
        std::cout << "    p99:     " << percentile(pipeline_times, 99.0) << " ns\n\n";
    }

    std::filesystem::remove_all("bench_logs");
    std::cout << "============================================================\n";
    return 0;
}
