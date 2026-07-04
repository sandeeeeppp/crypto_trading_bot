#pragma once

#include <string>
#include <mutex>
#include <thread>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <curl/curl.h>

class Trader {
public:
    Trader(const std::string& api_key, const std::string& secret_key, const std::string& symbol);
    ~Trader();

    void buy(double quantity, double price);   // Limit Buy
    void sell(double quantity, double price);  // Limit Sell

private:
    std::string api_key;
    std::string secret_key;
    std::string symbol;
    std::string base_url = "https://testnet.binance.vision";

    // --- Fix #5: Persistent CURL handle for connection reuse ---
    CURL* curl_handle;

    // --- Fix #4: Async order execution (producer-consumer) ---
    struct OrderRequest {
        std::string side;
        double quantity;
        double price;
    };

    std::queue<OrderRequest> order_queue;
    std::mutex queue_mtx;
    std::condition_variable cv;
    std::atomic<bool> running{true};
    std::thread worker_thread;

    void worker_loop();
    void place_order(const std::string& side, double quantity, double price);
    std::string hmac_sha256(const std::string& data);
};
