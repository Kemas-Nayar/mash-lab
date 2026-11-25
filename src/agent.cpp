#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/beast.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;
using boost::property_tree::ptree;

static std::string hostname() {
    char name[256]{};
    if (gethostname(name, sizeof(name) - 1) != 0) return "unknown";
    return name;
}

static std::string make_beacon() {
    ptree p;
    p.put("id", "lab-agent-01");
    p.put("hostname", hostname());
    p.put("platform", "linux-lab");
    std::ostringstream out;
    boost::property_tree::write_json(out, p, false);
    return out.str();
}

static ptree beacon_once(ssl::context& ctx) {
    asio::io_context ioc;
    tcp::resolver resolver(ioc);
    asio::ssl::stream<beast::tcp_stream> stream(ioc, ctx);
    // The destination is intentionally hard-coded to loopback.
    auto const results = resolver.resolve("127.0.0.1", "8443");
    beast::get_lowest_layer(stream).connect(results);
    stream.handshake(ssl::stream_base::client);

    http::request<http::string_body> req{http::verb::post, "/beacon", 11};
    req.set(http::field::host, "127.0.0.1");
    req.set(http::field::user_agent, "Lunyx-Lab-Agent/0.1");
    req.set(http::field::content_type, "application/json");
    req.body() = make_beacon();
    req.prepare_payload();
    http::write(stream, req);

    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);
    if (res.result() != http::status::ok) throw std::runtime_error("controller rejected beacon");
    beast::error_code ec;
    stream.shutdown(ec);

    std::istringstream input(res.body());
    ptree task;
    boost::property_tree::read_json(input, task);
    return task;
}

int main(int argc, char** argv) {
    const std::string ca = argc > 1 ? argv[1] : "certs/server.crt";
    try {
        asio::io_context ioc;
        ssl::context ctx(ssl::context::tls_client);
        ctx.set_default_verify_paths();
        ctx.load_verify_file(ca);
        ctx.set_verify_mode(ssl::verify_peer);
        // Verify the certificate identity as well as its CA chain.
        ctx.set_verify_callback(ssl::host_name_verification("localhost"));

        int interval = 5;
        std::mt19937 rng(std::random_device{}());
        std::uniform_real_distribution<double> jitter(0.8, 1.2);
        std::cout << "mash-lab agent started; controller is fixed at 127.0.0.1:8443\n";
        for (;;) {
            try {
                ptree task = beacon_once(ctx);
                const auto name = task.get<std::string>("task", "idle");
                if (name == "status") {
                    std::cout << "[agent] status: id=lab-agent-01 host=" << hostname() << '\n';
                } else if (name == "interval") {
                    interval = task.get<int>("seconds", interval);
                    std::cout << "[agent] beacon interval changed to " << interval << "s\n";
                } else if (name == "shutdown") {
                    std::cout << "[agent] killswitch received; exiting\n";
                    return 0;
                } else if (name != "idle") {
                    std::cerr << "[agent] ignored unsupported task: " << name << '\n';
                }
            } catch (const std::exception& e) {
                std::cerr << "[agent] beacon failed: " << e.what() << '\n';
            }
            const auto delay = std::chrono::milliseconds(static_cast<int>(interval * 1000 * jitter(rng)));
            std::this_thread::sleep_for(delay);
        }
    } catch (const std::exception& e) {
        std::cerr << "agent: " << e.what() << '\n';
        return 1;
    }
}
