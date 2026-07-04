#pragma once

#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>
#include <nlohmann/json.hpp>
#include <boost/circular_buffer.hpp>

#include <mutex>
#include <atomic>
#include <thread>
#include <map>
#include <string>
#include <sstream>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <memory>

enum class StreamType {
    Candle,
    Depth
};

struct TradeData {
    std::string symbol;
    std::string interval;
    boost::circular_buffer<double> open;
    boost::circular_buffer<double> close;
    boost::circular_buffer<double> high;
    boost::circular_buffer<double> low;
    boost::circular_buffer<double> volume;
    boost::circular_buffer<std::string> timestamp;
    std::mutex mtx;

    TradeData()
        : open(1000), close(1000), high(1000), low(1000), volume(1000), timestamp(1000) {}
};

struct MarketDepth {
    std::map<double, double, std::greater<>> bids;
    std::map<double, double> asks;
    std::mutex mtx;
};

class Streamer {
public:
    Streamer(const std::string& symbol, const std::string& interval, TradeData& trade);
    Streamer(const std::string& symbol, MarketDepth& depth);

    void start();
    void stop();

private:
    using client = websocketpp::client<websocketpp::config::asio_tls_client>;
    client ws_client;

    std::string uri;
    std::string stream;
    TradeData* tradePtr = nullptr;
    MarketDepth* depthPtr = nullptr;
    StreamType streamType;
    std::atomic<bool> running{true};

    void setup_handlers();
    std::shared_ptr<boost::asio::ssl::context> on_tls_init(websocketpp::connection_hdl);
    void on_message(websocketpp::connection_hdl, client::message_ptr msg);
    void on_message_candle(nlohmann::json& j);
    void on_message_depth(nlohmann::json& j);

    void on_open(websocketpp::connection_hdl);
    void on_fail(websocketpp::connection_hdl);
};
