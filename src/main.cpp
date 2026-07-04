#include "streamer.hpp"
#include "trader.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <iomanip>
#include <vector>
#include <cmath>
#include <numeric>
#include <fstream>
#include <sstream>
#include <string>
#include <cstdlib>
#include <boost/circular_buffer.hpp>

#ifdef _WIN32
#include <stdlib.h>  // _putenv_s
#endif

// ---------------------------------------------------------------------------
// load_dotenv — reads a .env file (KEY=VALUE pairs) and sets each entry as a
// process-level environment variable.  Lines starting with '#' and blank lines
// are ignored.  Values are not overwritten if already set in the environment.
// ---------------------------------------------------------------------------
static void load_dotenv(const std::string& path = ".env") {
    std::ifstream file(path);
    if (!file.is_open()) {
        // .env is optional — silently continue if not found
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        // Strip leading whitespace
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;  // blank line
        line = line.substr(start);

        if (line[0] == '#') continue;  // comment

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;  // malformed — skip

        std::string key   = line.substr(0, eq);
        std::string value = line.substr(eq + 1);

        // Trim trailing whitespace / CR from key and value
        auto rtrim = [](std::string& s) {
            size_t e = s.find_last_not_of(" \t\r\n");
            if (e != std::string::npos) s = s.substr(0, e + 1);
            else s.clear();
        };
        rtrim(key);
        rtrim(value);

        if (key.empty()) continue;

        // Only set if not already defined in the environment
        if (std::getenv(key.c_str()) == nullptr) {
#ifdef _WIN32
            _putenv_s(key.c_str(), value.c_str());
#else
            setenv(key.c_str(), value.c_str(), 0);
#endif
        }
    }
}

std::string current_timestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t ts = std::chrono::system_clock::to_time_t(now);
    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&ts), "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

void log_trade(std::ofstream& trade_log, const std::string& side, double price, double qty, const std::string& timestamp) {
    trade_log << timestamp << "," << side << "," << price << "," << qty << "\n";
}

struct Position {
    double qty = 0.0;
    double avg_price = 0.0;

    void update(const std::string& side, double trade_qty, double price) {
        if (side == "BUY") {
            double total_cost = avg_price * qty + price * trade_qty;
            qty += trade_qty;
            if (qty > 0.0)
                avg_price = total_cost / qty;
        } else if (side == "SELL") {
            qty -= trade_qty;
            if (qty < 0.0) {
                qty = 0.0;
                avg_price = 0.0;
            }
        }
    }

    double unrealized_pnl(double mark_price) const {
        return (mark_price - avg_price) * qty;
    }

    void print() const {
        std::cout << "Position Qty: " << qty << " Avg Entry: " << avg_price << std::endl;
    }
};

std::string detect_market_signal(const boost::circular_buffer<double>& closes) {
    const size_t N = closes.size();
    if (N < 20) return "neutral";

    double sum_x = 0, sum_y = 0, sum_xy = 0, sum_x2 = 0;
    for (size_t i = 0; i < N; ++i) {
        sum_x += i;
        sum_y += closes[i];
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

    if (score > 5.0) return "very bullish";
    if (score > 1.5) return "bullish";
    if (score < -5.0) return "very bearish";
    if (score < -1.5) return "bearish";
    return "neutral";
}

void place_laddered_orders(const std::string& side, double base_price, int num_orders,
                           double price_step_pct, double order_usdt, Trader& trader, Position& pos, std::ofstream& trade_log) {
    std::string timestamp = current_timestamp();
    for (int i = 0; i < num_orders; ++i) {
        double price = base_price * (side == "BUY" ? (1.0 - i * price_step_pct) : (1.0 + i * price_step_pct));
        double qty = order_usdt / price;
        if (side == "BUY") trader.buy(qty, price);
        else trader.sell(qty, price);
        log_trade(trade_log, side, price, qty, timestamp);
        pos.update(side, qty, price);
        std::cout << "[LADDER " << side << "] Qty: " << qty << " @ " << price << std::endl;
    }
}

void run_market_making_biased(double mid_price, double spread_offset_pct, double order_usdt,
                               Trader& trader, Position& pos, std::ofstream& trade_log) {
    double buy_price = mid_price * (1.0 - spread_offset_pct);
    double sell_price = mid_price * (1.0 + spread_offset_pct);
    double buy_qty = order_usdt / buy_price;
    double sell_qty = order_usdt / sell_price;
    std::string timestamp = current_timestamp();
    trader.buy(buy_qty, buy_price);
    log_trade(trade_log, "BUY", buy_price, buy_qty, timestamp);
    pos.update("BUY", buy_qty, buy_price);
    std::cout << "[MM BUY] Qty: " << buy_qty << " @ " << buy_price << std::endl;
    trader.sell(sell_qty, sell_price);
    log_trade(trade_log, "SELL", sell_price, sell_qty, timestamp);
    pos.update("SELL", sell_qty, sell_price);
    std::cout << "[MM SELL] Qty: " << sell_qty << " @ " << sell_price << std::endl;
}

void strategy_dispatcher(const std::string& signal, MarketDepth& depth,
                         Trader& trader, Position& pos, std::ofstream& trade_log) {
    std::lock_guard<std::mutex> lock(depth.mtx);
    if (depth.bids.empty() || depth.asks.empty()) return;
    double best_bid = depth.bids.begin()->first;
    double best_ask = depth.asks.begin()->first;
    double mid_price = (best_bid + best_ask) / 2.0;

    if (signal == "very bullish") {
        place_laddered_orders("BUY", best_ask, 5, 0.001, 10.0, trader, pos, trade_log);
    } else if (signal == "bullish") {
        run_market_making_biased(mid_price, 0.001, 10.0, trader, pos, trade_log);
    } else if (signal == "neutral") {
        for (int i = -2; i <= 2; ++i) {
            if (i == 0) continue;
            double grid_price = mid_price * (1.0 + i * 0.002);
            double qty = 10.0 / grid_price;
            if (i < 0) {
                trader.buy(qty, grid_price);
                log_trade(trade_log, "BUY", grid_price, qty, current_timestamp());
                pos.update("BUY", qty, grid_price);
            } else {
                trader.sell(qty, grid_price);
                log_trade(trade_log, "SELL", grid_price, qty, current_timestamp());
                pos.update("SELL", qty, grid_price);
            }
            std::cout << "[GRID] " << (i < 0 ? "BUY" : "SELL") << " Qty: " << qty << " @ " << grid_price << std::endl;
        }
    } else if (signal == "bearish" || signal == "very bearish") {
        place_laddered_orders("SELL", best_bid, 5, 0.001, 10.0, trader, pos, trade_log);
    }
}

int main() {
    // Load .env file before reading any environment variables
    load_dotenv();

    const char* api_key_env    = std::getenv("BINANCE_API_KEY");
    const char* secret_key_env = std::getenv("BINANCE_SECRET_KEY");

    if (!api_key_env || !secret_key_env) {
        std::cerr << "Error: BINANCE_API_KEY and BINANCE_SECRET_KEY environment variables must be set." << std::endl;
        return 1;
    }

    std::string symbol     = "btcusdt";
    std::string api_key    = api_key_env;
    std::string secret_key = secret_key_env;

    MarketDepth depth;
    TradeData trade_data;
    Position position;

    Streamer depth_streamer(symbol, depth);
    Streamer candle_streamer(symbol, "1m", trade_data);
    Trader trader(api_key, secret_key, symbol);

    std::thread ws_thread_depth([&]() { depth_streamer.start(); });
    std::thread ws_thread_candle([&]() { candle_streamer.start(); });
    std::this_thread::sleep_for(std::chrono::seconds(3));

    std::string log_filename = "trade_logs/" + symbol + ".csv";
    std::ofstream trade_log(log_filename, std::ios::app);
    if (!trade_log.is_open()) {
        std::cerr << "Failed to open log file: " << log_filename << std::endl;
        return 1;
    }

    while (true) {
        std::string signal;
        {
            std::lock_guard<std::mutex> lock(trade_data.mtx);
            signal = detect_market_signal(trade_data.close);
            std::cout << "Market Signal: " << signal << std::endl;
        }

        strategy_dispatcher(signal, depth, trader, position, trade_log);
        position.print();
        {
            std::lock_guard<std::mutex> lock(depth.mtx);
            if (!depth.bids.empty()) {
                double mark_price = depth.bids.begin()->first;
                std::cout << "Unrealized PnL: " << position.unrealized_pnl(mark_price) << std::endl;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    ws_thread_depth.join();
    ws_thread_candle.join();
    return 0;
}