#pragma once
#include <fstream>
#include <string>
#include <mutex>
#include <iomanip>
#include <chrono>
#include <filesystem>
#include <stdexcept>

namespace v2 {

/// Thread-safe, persistent CSV trade logger.
///
/// Replaces the V1 log_trade() pattern which opened, wrote, and closed the
/// file on every single call (15 syscalls per 5-order laddered batch).
/// This class opens the file once in the constructor and holds it open for
/// the lifetime of the object (RAII), flushing only on destruction.
///
/// Thread safety: the internal mutex makes it safe to call log() from
/// multiple threads concurrently (e.g., from a future async executor thread).
///
/// CSV format: timestamp,side,price,quantity
///   Example:  2026-05-21T01:00:00Z,BUY,67843.21,0.000147
class LogWriter {
public:
    /// Opens (or appends to) trade_logs/<symbol>.csv.
    /// Creates the trade_logs/ directory if it does not exist.
    /// Writes a CSV header line if the file is new (size == 0 at open time).
    explicit LogWriter(const std::string& symbol) {
        std::filesystem::create_directories("trade_logs");
        const std::string path = "trade_logs/" + symbol + ".csv";
        file_.open(path, std::ios::app);
        if (!file_.is_open()) {
            throw std::runtime_error("Cannot open log file: " + path);
        }
        // Write CSV header only when creating a brand-new file.
        if (file_.tellp() == std::streampos(0)) {
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

    // Non-copyable and non-movable — owns an open file handle.
    LogWriter(const LogWriter&)            = delete;
    LogWriter& operator=(const LogWriter&) = delete;
    LogWriter(LogWriter&&)                 = delete;
    LogWriter& operator=(LogWriter&&)      = delete;

    /// Write one trade record.
    ///
    /// @param side   "BUY" or "SELL"
    /// @param price  Execution price (formatted to 2 decimal places)
    /// @param qty    Executed quantity (formatted to 6 decimal places)
    ///
    /// Note: no flush() is called here — OS write buffering provides
    /// sufficient durability for non-crash scenarios (~4 KB page granularity).
    /// If crash-durability is required (e.g., append-only audit log), call
    /// file_.flush() at the cost of ~1 ms per trade on a spinning disk.
    void log(const std::string& side, double price, double qty) {
        auto now = std::chrono::system_clock::now();
        auto t   = std::chrono::system_clock::to_time_t(now);

        std::lock_guard<std::mutex> lk(mtx_);
        file_ << std::put_time(std::gmtime(&t), "%Y-%m-%dT%H:%M:%SZ")
              << "," << side
              << "," << std::fixed << std::setprecision(2) << price
              << "," << std::fixed << std::setprecision(6) << qty
              << "\n";
    }

private:
    std::ofstream file_;
    std::mutex    mtx_;
};

} // namespace v2
