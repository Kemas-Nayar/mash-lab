#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;
using boost::property_tree::ptree;

static std::mutex queue_mutex;
static std::queue<ptree> tasks;
static std::atomic<bool> killed{false};

static std::string json_string(const ptree& tree) {
    std::ostringstream out;
    boost::property_tree::write_json(out, tree, false);
    return out.str();
}

static ptree make_task(const std::string& name, int seconds = 0) {
    ptree p;
    p.put("task", name);
    if (name == "interval") p.put("seconds", seconds);
    return p;
}

static void handle_connection(tcp::socket socket, ssl::context& ctx) {
    try {
        asio::ssl::stream<beast::tcp_stream> stream(beast::tcp_stream(std::move(socket)), ctx);
        stream.handshake(ssl::stream_base::server);
        beast::flat_buffer buffer;
        http::request<http::string_body> req;
        http::read(stream, buffer, req);

        http::response<http::string_body> res{http::status::ok, req.version()};
        res.set(http::field::server, "Lunyx-Lab/0.1");
        res.set(http::field::content_type, "application/json");
        res.keep_alive(false);

        if (req.method() != http::verb::post || req.target() != "/beacon") {
            res.result(http::status::not_found);
            res.body() = R"({"error":"not found"})";
        } else {
            try {
                std::istringstream input(req.body());
                ptree beacon;
                boost::property_tree::read_json(input, beacon);
                const auto id = beacon.get<std::string>("id", "unknown");
                const auto host = beacon.get<std::string>("hostname", "unknown");
                std::cout << "[beacon] " << id << " from " << host << '\n';

                ptree response;
                if (killed.load()) {
                    response = make_task("shutdown");
                } else {
                    std::lock_guard<std::mutex> lock(queue_mutex);
                    if (!tasks.empty()) {
                        response = tasks.front();
                        tasks.pop();
                    } else {
                        response = make_task("idle");
                    }
                }
                res.body() = json_string(response);
            } catch (const std::exception&) {
                res.result(http::status::bad_request);
                res.body() = R"({"error":"invalid beacon"})";
            }
        }
        res.prepare_payload();
        http::write(stream, res);
        beast::error_code ec;
        stream.shutdown(ec);
    } catch (const std::exception& e) {
        std::cerr << "[connection] " << e.what() << '\n';
    }
}

int main(int argc, char** argv) {
    const std::string cert = argc > 1 ? argv[1] : "certs/server.crt";
    const std::string key = argc > 2 ? argv[2] : "certs/server.key";
    try {
        asio::io_context ioc;
        ssl::context ctx(ssl::context::tls_server);
        ctx.set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 | ssl::context::no_sslv3);
        ctx.use_certificate_chain_file(cert);
        ctx.use_private_key_file(key, ssl::context::pem);

        // Deliberately bind only to IPv4 loopback. No LAN listener exists.
        tcp::acceptor acceptor(ioc, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 8443));
        acceptor.non_blocking(true);
        std::cout << "mash-lab controller listening at https://127.0.0.1:8443\n"
                  << "commands: queue status | queue interval <seconds> | kill | quit\n";

        std::atomic<bool> running{true};
        std::thread console([&] {
            std::string line;
            while (running && std::getline(std::cin, line)) {
                if (line == "queue status") {
                    std::lock_guard<std::mutex> lock(queue_mutex);
                    tasks.push(make_task("status"));
                    std::cout << "[controller] queued status\n";
                } else if (line.rfind("queue interval ", 0) == 0) {
                    try {
                        int seconds = std::stoi(line.substr(15));
                        if (seconds < 2 || seconds > 3600) throw std::out_of_range("interval");
                        std::lock_guard<std::mutex> lock(queue_mutex);
                        tasks.push(make_task("interval", seconds));
                        std::cout << "[controller] queued interval " << seconds << "s\n";
                    } catch (...) { std::cout << "usage: queue interval <2..3600>\n"; }
                } else if (line == "kill") {
                    killed.store(true);
                    std::cout << "[controller] killswitch armed; agents stop at next beacon\n";
                } else if (line == "quit") {
                    running = false;
                    killed.store(true);
                    boost::system::error_code ignored;
                    acceptor.cancel(ignored);
                    acceptor.close(ignored);
                    break;
                } else {
                    std::cout << "commands: queue status | queue interval <seconds> | kill | quit\n";
                }
            }
        });

        while (running) {
            tcp::socket socket(ioc);
            beast::error_code ec;
            acceptor.accept(socket, ec);
            if (ec) {
                if (!running) break;
                if (ec == asio::error::would_block || ec == asio::error::try_again) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                }
                std::cerr << "[accept] " << ec.message() << '\n';
                continue;
            }
            std::thread(handle_connection, std::move(socket), std::ref(ctx)).detach();
        }
        running = false;
        if (console.joinable()) console.join();
    } catch (const std::exception& e) {
        std::cerr << "controller: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
