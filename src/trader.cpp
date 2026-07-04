#include "trader.hpp"
#include <iostream>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <openssl/hmac.h>

Trader::Trader(const std::string& api_key, const std::string& secret_key, const std::string& symbol)
    : api_key(api_key), secret_key(secret_key), symbol(symbol) {
    // Fix #5: Initialize persistent CURL handle once (avoids TCP+TLS handshake per order)
    curl_handle = curl_easy_init();
    if (curl_handle) {
        curl_easy_setopt(curl_handle, CURLOPT_TCP_KEEPALIVE, 1L);
    }

    // Fix #4: Start background worker thread for async order execution
    worker_thread = std::thread(&Trader::worker_loop, this);
}

Trader::~Trader() {
    // Fix #4: Signal worker thread to stop and wait for it
    running = false;
    cv.notify_one();
    if (worker_thread.joinable()) {
        worker_thread.join();
    }

    // Fix #5: Cleanup persistent CURL handle
    if (curl_handle) {
        curl_easy_cleanup(curl_handle);
        curl_handle = nullptr;
    }
}

void Trader::buy(double quantity, double price) {
    // Fix #4: Enqueue order instead of blocking the caller
    {
        std::lock_guard<std::mutex> lock(queue_mtx);
        order_queue.push({"BUY", quantity, price});
    }
    cv.notify_one();
}

void Trader::sell(double quantity, double price) {
    // Fix #4: Enqueue order instead of blocking the caller
    {
        std::lock_guard<std::mutex> lock(queue_mtx);
        order_queue.push({"SELL", quantity, price});
    }
    cv.notify_one();
}

void Trader::worker_loop() {
    while (running) {
        std::queue<OrderRequest> batch;
        {
            std::unique_lock<std::mutex> lock(queue_mtx);
            cv.wait(lock, [this] { return !order_queue.empty() || !running; });

            // Drain all pending orders into a local batch
            std::swap(batch, order_queue);
        }

        // Execute each order outside the lock
        while (!batch.empty()) {
            auto& req = batch.front();
            place_order(req.side, req.quantity, req.price);
            batch.pop();
        }
    }

    // Drain any remaining orders after shutdown signal
    std::queue<OrderRequest> remaining;
    {
        std::lock_guard<std::mutex> lock(queue_mtx);
        std::swap(remaining, order_queue);
    }
    while (!remaining.empty()) {
        auto& req = remaining.front();
        place_order(req.side, req.quantity, req.price);
        remaining.pop();
    }
}

std::string Trader::hmac_sha256(const std::string& data) {
    unsigned char* digest;
    digest = HMAC(EVP_sha256(), secret_key.c_str(), secret_key.length(),
                  reinterpret_cast<const unsigned char*>(data.c_str()), data.length(), NULL, NULL);

    std::ostringstream oss;
    for (int i = 0; i < 32; i++) {
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)digest[i];
    }
    return oss.str();
}

void Trader::place_order(const std::string& side, double quantity, double price) {
    long timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch()).count();

    std::ostringstream params;
    params << std::fixed << std::setprecision(8);
    params << "symbol=" << symbol
           << "&side=" << side
           << "&type=LIMIT"
           << "&timeInForce=GTC"
           << "&quantity=" << quantity
           << "&price=" << price
           << "&timestamp=" << timestamp;

    std::string signature = hmac_sha256(params.str());
    std::string full_url = base_url + "/api/v3/order?" + params.str() + "&signature=" + signature;

    // Fix #5: Reuse persistent curl_handle instead of init/cleanup per call
    if (curl_handle) {
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, ("X-MBX-APIKEY: " + api_key).c_str());

        curl_easy_setopt(curl_handle, CURLOPT_URL, full_url.c_str());
        curl_easy_setopt(curl_handle, CURLOPT_POST, 1L);
        curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, [](void* contents, size_t size, size_t nmemb, void* userp) -> size_t {
            std::cout << "Binance Response: " << std::string((char*)contents, size * nmemb) << std::endl;
            return size * nmemb;
        });

        CURLcode res = curl_easy_perform(curl_handle);
        if (res != CURLE_OK) {
            std::cerr << "CURL Error: " << curl_easy_strerror(res) << std::endl;
        }

        curl_slist_free_all(headers);
    }
}
