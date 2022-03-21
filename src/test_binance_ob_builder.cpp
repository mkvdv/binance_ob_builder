//------------------------------------------------------------------------------
//
// Based on this examples:
//  https://github.com/boostorg/beast/tree/develop/example/websocket/client
//
//------------------------------------------------------------------------------

#include "not_for_production/root_certificates.hpp"
#include "thirdparty/nlohmann_json.hpp"

#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/signal_set.hpp>

#include <cstdlib>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <queue>
#include <set>
#include <optional>

namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
namespace websocket = beast::websocket; // from <boost/beast/websocket.hpp>
namespace net = boost::asio;            // from <boost/asio.hpp>
namespace ssl = boost::asio::ssl;       // from <boost/asio/ssl.hpp>
using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>
using njson = nlohmann::json;
//------------------------------------------------------------------------------

namespace
{
    // Report a failure
    void fail(beast::error_code ec, char const* what)
    {
        std::cerr << "Boost::Beast error: " << what << ": " << ec.message() << "\n";
    }

    std::atomic_bool g_ws_connected{ false };
    std::atomic_bool g_snp_processed{ false };

    // Dummy(!) storage for orderbook data
    class Orderbook
    {
        using price_t = std::string; // todo Boost::Decimal for both?
        using volume_t = std::string;

        // Json field names, defined by Binance schema
        static constexpr const char* SNP_LAST_UPDATE_ID = "lastUpdateId";
        static constexpr const char* MSG_OUTPUT_TIME = "E";
        static constexpr const char* TRANSCATION_TIME = "T";
        static constexpr const char* FIRST_UPDATE_ID = "U";
        static constexpr const char* FINAL_UPDATE_ID = "u";
        static constexpr const char* FINAL_UPDATE_ID_IN_LAST_STREAM = "pu";
        static constexpr size_t PRICE_IX = 0;
        static constexpr size_t QTY_IX = 1;

        // Helper-class for indexing during json parsing
        struct Direction
        {
            static constexpr size_t BIDS = 0;
            static constexpr size_t ASKS = 1;
            static constexpr std::array<const char*, 2> SNP_JSON_NAMES = { "bids", "asks" };
            static constexpr std::array<const char*, 2> UPD_JSON_NAMES = { "b", "a" };
        };

        std::array<std::map<price_t, volume_t>, 2> ob_storage_; // not for production! set is just for order
        uint64_t snp_last_update_id_{};
        uint64_t msg_output_time_{};
        uint64_t transaction_time_{};
        std::optional<uint64_t> last_final_update_id_{};
        bool any_updates_applied_ = false;

     public:
        void init_from_snapshot(const std::string_view snp_string)
        {
            const auto j = njson::parse(snp_string);
            snp_last_update_id_ = j[SNP_LAST_UPDATE_ID];
            msg_output_time_ = j[MSG_OUTPUT_TIME];
            transaction_time_ = j[TRANSCATION_TIME];

            for (const auto& dir : { Direction::BIDS, Direction::ASKS })
            {
                for (const auto& price_level : j[Direction::SNP_JSON_NAMES[dir]])
                {
                    ob_storage_[dir][price_level[PRICE_IX]] = price_level[QTY_IX];
                }
            }
        }

        void update(const std::string_view update_string)
        {
            const auto j = njson::parse(update_string)["data"];
            if (BOOST_UNLIKELY(!any_updates_applied_))
            {
                if (j[FINAL_UPDATE_ID] < snp_last_update_id_)
                    return;

                if (!(j[FIRST_UPDATE_ID] <= snp_last_update_id_ && j[FINAL_UPDATE_ID] >= snp_last_update_id_))
                {
                    throw std::runtime_error("condition (U <= lastUpdateId AND u >= lastUpdateId) is violated!");
                }
            }
            if (last_final_update_id_ && j[FINAL_UPDATE_ID_IN_LAST_STREAM] != last_final_update_id_.value())
            {
                throw std::runtime_error("TODO: reload snapshot again, stream is broken!");
            }

            msg_output_time_ = j[MSG_OUTPUT_TIME]; // event time
            transaction_time_ = j[TRANSCATION_TIME];
            last_final_update_id_ = j[FINAL_UPDATE_ID];

            for (const auto& dir : { Direction::BIDS, Direction::ASKS })
            {
                for (const auto& price_level : j[Direction::UPD_JSON_NAMES[dir]])
                {
                    auto new_qty = price_level[QTY_IX];
                    if (new_qty == "0.000")
                    {
                        ob_storage_[dir].erase(price_level[PRICE_IX]);
                    }
                    else
                    {
                        ob_storage_[dir][price_level[PRICE_IX]] = new_qty;
                    }
                }
            }
            any_updates_applied_ = true;
        }

        void pretty_print(const size_t depth = 10)
        {
            int rc = std::system("clear");
            if (rc)
            {
                std::cerr << "system(\"clear\") failed" << std::endl;
            }
            std::cout << "\nlast_final_update_id: " << last_final_update_id_.value_or(0)
                      << "\nmsg_output_time:      " << msg_output_time_
                      << "\ntransaction_time:     " << transaction_time_ << std::endl;

            const auto& asks = ob_storage_[Direction::ASKS];
            auto ask_end = asks.begin();
            std::advance(ask_end, std::min(depth, asks.size()));
            for (auto it = asks.begin(); it != ask_end; it++)
            {
                std::cout << "         | " << it->first << " | " << it->second << std::endl;
            }

            const auto& bids = ob_storage_[Direction::BIDS];
            auto bids_it = bids.end();
            std::advance(bids_it, -std::min(depth, bids.size()));
            for (; bids_it != bids.end(); bids_it++)
            {
                std::cout << std::setw(8) << bids_it->second << " | " << bids_it->first << std::endl;
            }
        }
    };

    // Sends a WebSocket message and prints the response
    class WssSession : public std::enable_shared_from_this<WssSession>
    {
        tcp::resolver resolver_;
        websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws_;
        beast::flat_buffer buffer_;
        std::string host_;
        std::string target_;
        std::queue<std::string> unprocessed_messages_;
        Orderbook& ob_;

     private: // functions

        void process_update_message(const std::string_view update_message)
        {
            ob_.update(update_message);
        }

        void process_messages()
        {
            // todo impl
            if (BOOST_LIKELY(g_snp_processed))
            {
                while (!unprocessed_messages_.empty())
                {
                    process_update_message(unprocessed_messages_.front());
                    unprocessed_messages_.pop();
                }
                process_update_message(
                    std::string_view{ static_cast<const char*>(buffer_.cdata().data()), buffer_.cdata().size() });

                ob_.pretty_print();
            }
            else
            {
                unprocessed_messages_.push(beast::buffers_to_string(buffer_.cdata()));
            }
        }

     public:
        // Resolver and socket require an io_context
        explicit WssSession(net::io_context& ioc, ssl::context& ctx, Orderbook& ob)
            : resolver_(net::make_strand(ioc)), ws_(net::make_strand(ioc), ctx), ob_(ob)
        {
        }

        ~WssSession()
        {
            beast::error_code ec;
            ws_.close(websocket::close_code::normal, ec);

            if (ec)
                fail(ec, "~WssSession(): close");
        }

        // Start the asynchronous operation
        void run(
            char const* host,
            char const* port,
            char const* target)
        {
            // Save these for later
            host_ = host;
            target_ = target;

            // Look up the domain name
            resolver_.async_resolve(
                host,
                port,
                beast::bind_front_handler(&WssSession::on_resolve, shared_from_this()));
        }

        void on_resolve(beast::error_code ec, tcp::resolver::results_type results)
        {
            if (ec)
                return fail(ec, "resolve");

            // Set a timeout on the operation
            beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(30));

            // Make the connection on the IP address we get from a lookup
            beast::get_lowest_layer(ws_).async_connect(results,
                beast::bind_front_handler(&WssSession::on_connect, shared_from_this()));
        }

        void on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type ep)
        {
            if (ec)
                return fail(ec, "connect");

            // Set a timeout on the operation
            beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(30));

            // Set SNI Hostname (many hosts need this to handshake successfully)
            if (!SSL_set_tlsext_host_name(ws_.next_layer().native_handle(), host_.c_str()))
            {
                ec = beast::error_code(static_cast<int>(::ERR_get_error()), net::error::get_ssl_category());
                return fail(ec, "connect");
            }

            // Update the host_ string. This will provide the value of the
            // Host HTTP header during the WebSocket handshake.
            // See https://tools.ietf.org/html/rfc7230#section-5.4
            host_ += ':' + std::to_string(uint32_t(ep.port()));

            // Perform the SSL handshake
            ws_.next_layer().async_handshake(ssl::stream_base::client,
                beast::bind_front_handler(&WssSession::on_ssl_handshake, shared_from_this()));
        }

        void on_ssl_handshake(beast::error_code ec)
        {
            if (ec)
                return fail(ec, "ssl_handshake");

            // Turn off the timeout on the tcp_stream, because
            // the websocket stream has its own timeout system.
            beast::get_lowest_layer(ws_).expires_never();

            // Set suggested timeout settings for the websocket
            ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));

            // Set a decorator to change the User-Agent of the handshake
            ws_.set_option(websocket::stream_base::decorator(
                [](websocket::request_type& req) -> void
                {
                    req.set(http::field::user_agent,
                        std::string(BOOST_BEAST_VERSION_STRING) + " websocket-client-async-ssl");
                }));

            // Perform the websocket handshake
            ws_.async_handshake(host_, target_,
                beast::bind_front_handler(&WssSession::on_handshake, shared_from_this()));
        }

        void on_handshake(beast::error_code ec)
        {
            if (ec)
                return fail(ec, "handshake");

            g_ws_connected = true;
            // Read a message into our buffer
            ws_.async_read(buffer_,
                beast::bind_front_handler(&WssSession::on_read, shared_from_this()));
        }

        void on_read(beast::error_code ec, [[maybe_unused]] std::size_t bytes_transferred)
        {
            if (ec)
                return fail(ec, "read");

            // todo add ping-pong support !

            process_messages();
            buffer_.clear();

            ws_.async_read(buffer_,
                beast::bind_front_handler(&WssSession::on_read, shared_from_this()));
        }

    };

    std::string load_snapshot(const char* host, const char* port, const char* target, int version)
    {
        // The io_context is required for all I/O
        net::io_context ioc;

        // The SSL context is required, and holds certificates
        ssl::context ctx(ssl::context::tlsv12_client);

        // This holds the root certificate used for verification
        load_root_certificates(ctx);

        // Verify the remote server's certificate
        ctx.set_verify_mode(ssl::verify_peer);

        // These objects perform our I/O
        tcp::resolver resolver(ioc);
        beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);

        // Set SNI Hostname (many hosts need this to handshake successfully)
        if (!SSL_set_tlsext_host_name(stream.native_handle(), host))
        {
            beast::error_code ec{ static_cast<int>(::ERR_get_error()), net::error::get_ssl_category() };
            throw beast::system_error{ ec };
        }

        // Look up the domain name
        auto const results = resolver.resolve(host, port);

        // Make the connection on the IP address we get from a lookup
        beast::get_lowest_layer(stream).connect(results);

        // Perform the SSL handshake
        stream.handshake(ssl::stream_base::client);

        // Set up an HTTP GET request message
        http::request<http::string_body> req{ http::verb::get, target, version };
        req.set(http::field::host, host);
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);

        // Send the HTTP request to the remote host
        http::write(stream, req);

        // This buffer is used for reading and must be persisted
        beast::flat_buffer buffer;

        // Declare a container to hold the response
        http::response<http::string_body> res;

        // Receive the HTTP response
        http::read(stream, buffer, res);

        // Gracefully close the stream
        beast::error_code ec;
        stream.shutdown(ec);
        if (ec == net::error::eof)
        {
            // Rationale:
            // http://stackoverflow.com/questions/25587403/boost-asio-ssl-async-shutdown-always-finishes-with-an-error
            ec = {};
        }
        if (ec == ssl::error::stream_truncated)
        {
            std::cerr << "Warning: stream truncated" << std::endl;
            // asked here https://github.com/boostorg/beast/issues/824 (and in other places)
            // looks like will be fixed in boost 1.79 https://github.com/boostorg/beast/issues/38
            // by this commit https://github.com/boostorg/beast/commit/094f5ec5cb3be1c3ce2d985564f1f39e9bed74ff
            ec = {};
        }

        if (ec)
            throw beast::system_error{ ec };

        // If we get here then the connection is closed gracefully
        return res.body();
    }

} // anonymous namespace

//------------------------------------------------------------------------------

int main([[maybe_unused]] int argc, [[maybe_unused]]  char** argv)
{
    // TODO: read parameters from config, NOT hardcoded!
    const char* wss_host = "fstream.binance.com";
    const char* wss_port = "443";
    const char* wss_target = "/stream?streams=btcusdt@depth@100ms";

    const int http_version = 11; // HTTP version 1.1
    const char* https_host = "fapi.binance.com";
    const char* https_port = "443";
    const char* https_target = "/fapi/v1/depth?symbol=BTCUSDT&limit=1000";
    const auto wait_ws_interval = std::chrono::milliseconds(250);

    // The io_context is required for all I/O
    net::io_context ioc;

    // Capture SIGINT and SIGTERM to perform a clean shutdown
    net::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait(
        [&](beast::error_code const&, int)
        {
            // Stop the `io_context`. This will cause `run()`
            // to return immediately, eventually destroying the
            // `io_context` and all of the sockets in it.
            ioc.stop();
        });

    std::thread ob_update_thread;

    try
    {
        Orderbook ob;

        // Start listening for updates
        ob_update_thread = std::thread([=, &ioc, &ob]()
        {
            try
            {
                // The SSL context is required, and holds certificates
                ssl::context ctx{ ssl::context::tlsv12_client };

                // This holds the root certificate used for verification
                load_root_certificates(ctx);

                // Launch the asynchronous operation
                std::make_shared<WssSession>(ioc, ctx, ob)->run(wss_host, wss_port, wss_target);

                // Run the I/O service. The call will return when
                // the socket is closed.
                ioc.run();
            }
            catch (const std::exception& e)
            {
                std::cerr << "Error in OB update thread: " << e.what() << std::endl;
            }
        });

        // Download and apply snapshot
        std::cout << "Waiting for ob updates stream..." << std::endl;
        while (!g_ws_connected)
        {
            std::this_thread::sleep_for(wait_ws_interval);
        }

        std::cout << "Loading snapshot..." << std::endl;
        std::string snapshot_json_str = load_snapshot(https_host, https_port, https_target, http_version);
        ob.init_from_snapshot(snapshot_json_str);
        ob.pretty_print();

        // Let ws thread apply updates to orderbook
        g_snp_processed = true;

        ob_update_thread.join();
    }
    catch (std::exception const& e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        if (!ioc.stopped())
            ioc.stop();
        ob_update_thread.join();
        return EXIT_FAILURE;
    }

    std::cout << "Stopped" << std::endl;
    return EXIT_SUCCESS;
}
