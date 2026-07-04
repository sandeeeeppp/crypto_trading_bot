#include "streamer.hpp"
#include <iomanip>
#include <algorithm>

using json = nlohmann::json;

Streamer::Streamer(const std::string& symbol, const std::string& interval, TradeData& trade)
    : tradePtr(&trade), depthPtr(nullptr), streamType(StreamType::Candle)
{
    stream = symbol + "@kline_" + interval;
    uri = "wss://stream.binance.com:9443/ws/" + stream;
    setup_handlers();
}

Streamer::Streamer(const std::string& symbol, MarketDepth& depth)
    : tradePtr(nullptr), depthPtr(&depth), streamType(StreamType::Depth)
{
    stream = symbol + "@depth20@100ms";
    uri = "wss://stream.binance.com:9443/ws/" + stream;
    setup_handlers();
}


void Streamer::setup_handlers() {
    ws_client.set_access_channels(websocketpp::log::alevel::none);
    ws_client.clear_access_channels(websocketpp::log::alevel::all);

    ws_client.set_tls_init_handler([this](websocketpp::connection_hdl hdl) {
        return this->on_tls_init(hdl);
    });

    ws_client.set_message_handler([this](websocketpp::connection_hdl hdl, client::message_ptr msg) {
        this->on_message(hdl, msg);
    });

    ws_client.set_open_handler([this](websocketpp::connection_hdl hdl) {
        this->on_open(hdl);
    });

    ws_client.set_fail_handler([this](websocketpp::connection_hdl hdl) {
        this->on_fail(hdl);
    });

    ws_client.init_asio();
}

std::shared_ptr<boost::asio::ssl::context> Streamer::on_tls_init(websocketpp::connection_hdl) {
    auto ctx = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tlsv12_client);
    try {
        ctx->set_options(
            boost::asio::ssl::context::default_workarounds |
            boost::asio::ssl::context::no_sslv2 |
            boost::asio::ssl::context::no_sslv3 |
            boost::asio::ssl::context::single_dh_use
        );
        ctx->set_verify_mode(boost::asio::ssl::verify_none);
        ctx->set_default_verify_paths();
    } catch (const std::exception& e) {
        std::cerr << "TLS init failed: " << e.what() << std::endl;
    }
    return ctx;
}

void Streamer::on_message(websocketpp::connection_hdl, client::message_ptr msg) {
    try {
        json j = json::parse(msg->get_payload());

        if (streamType == StreamType::Candle) {
            on_message_candle(j);
        } else if (streamType == StreamType::Depth) {
            on_message_depth(j);
        }

    } catch (const std::exception& e) {
        std::cerr << "Error parsing JSON: " << e.what() << std::endl;
    }
}

void Streamer::on_message_candle(json& j) {
    if (!tradePtr) return;

    const auto& k = j["k"];
    if (k["x"]) {
        double open   = std::stod(k["o"].get<std::string>());
        double high   = std::stod(k["h"].get<std::string>());
        double low    = std::stod(k["l"].get<std::string>());
        double close  = std::stod(k["c"].get<std::string>());
        double volume = std::stod(k["v"].get<std::string>());

        time_t rawtime = static_cast<time_t>(k["t"].get<long long>() / 1000);
        std::stringstream ts;
        ts << std::put_time(std::gmtime(&rawtime), "%Y-%m-%d %H:%M:%S");

        std::lock_guard<std::mutex> lock(tradePtr->mtx);
        tradePtr->open.push_back(open);
        tradePtr->high.push_back(high);
        tradePtr->low.push_back(low);
        tradePtr->close.push_back(close);
        tradePtr->volume.push_back(volume);
        tradePtr->timestamp.push_back(ts.str());

        std::cout << "CANDLE: O: " << open << "\nC: " << close << "\nH: " << high
                  << "\nL: " << low << "\nV: " << volume << "\nT: " << ts.str() << std::endl;
    }
}

void Streamer::on_message_depth(json& j) {
    if (!depthPtr) return;

    const auto& bids = j.contains("bids") ? j["bids"] : j["b"];
    const auto& asks = j.contains("asks") ? j["asks"] : j["a"];

    std::lock_guard<std::mutex> lock(depthPtr->mtx);

    depthPtr->bids.clear();
    depthPtr->asks.clear();

    for (const auto& level : bids) {
        double price = std::stod(level[0].get<std::string>());
        double qty   = std::stod(level[1].get<std::string>());
        if (qty > 0.0)
            depthPtr->bids[price] = qty;
    }

    for (const auto& level : asks) {
        double price = std::stod(level[0].get<std::string>());
        double qty   = std::stod(level[1].get<std::string>());
        if (qty > 0.0)
            depthPtr->asks[price] = qty;
    }

    std::cout << std::fixed << std::setprecision(8);
    if (!depthPtr->bids.empty()) {
        std::cout << "Top Bid: " << depthPtr->bids.begin()->first
                  << " Qty: " << depthPtr->bids.begin()->second << std::endl;
    }
    if (!depthPtr->asks.empty()) {
        std::cout << "Top Ask: " << depthPtr->asks.begin()->first
                  << " Qty: " << depthPtr->asks.begin()->second << std::endl;
    }
}

void Streamer::on_open(websocketpp::connection_hdl) {
    std::cout << "Connection opened to: " << stream << std::endl;
}

void Streamer::on_fail(websocketpp::connection_hdl hdl) {
    client::connection_ptr con = ws_client.get_con_from_hdl(hdl);
    std::cerr << "Connection failed: " << con->get_ec().message() << std::endl;
}

void Streamer::start() {
    int backoff_sec = 1;
    constexpr int max_backoff_sec = 30;

    while (running.load()) {
        // Re-initialize ASIO internals for a fresh connection attempt
        ws_client.reset();
        
        websocketpp::lib::error_code ec;
        client::connection_ptr con = ws_client.get_connection(uri, ec);
        if (ec) {
            std::cerr << "Get connection error: " << ec.message() << std::endl;
        } else {
            ws_client.connect(con);
            ws_client.run();  // blocks until disconnect
        }

        if (!running.load()) break;

        std::cerr << "[Streamer] WebSocket disconnected (" << stream
                  << "). Reconnecting in " << backoff_sec << "s..." << std::endl;

        // Sleep with periodic checks so stop() can break out quickly
        for (int i = 0; i < backoff_sec * 10 && running.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        backoff_sec = std::min(backoff_sec * 2, max_backoff_sec);
    }
}

void Streamer::stop() {
    running.store(false);
}
