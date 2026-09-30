// Http::follow_redirects(false) and Http::on_body(), which the Home tab's Vendors rely on: a vendor's
// credentials travel in custom headers, and curl hands those to wherever a redirect points, so the
// Vendors engine follows redirects itself (Utils/VendorConnector fetch()); and a downloaded model is
// written to disk as it arrives rather than held in memory.

#include <catch2/catch.hpp>

#include "slic3r/Utils/Http.hpp"

#include <boost/asio.hpp>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace asio = boost::asio;
using asio::ip::tcp;

// Answers one connection per canned response, and records how many it accepted. Give it exactly as
// many answers as the test makes requests: it waits for each.
struct LoopbackServer
{
    asio::io_context         ioc;
    tcp::acceptor            acceptor { ioc };
    std::thread              thread;
    std::vector<std::string> responses;
    std::atomic<int>         served { 0 };
    unsigned short           port { 0 };

    explicit LoopbackServer(std::vector<std::string> answers) : responses(std::move(answers))
    {
        acceptor.open(tcp::v4());
        acceptor.bind(tcp::endpoint(asio::ip::make_address_v4("127.0.0.1"), 0));
        acceptor.listen();
        port   = acceptor.local_endpoint().port();
        thread = std::thread([this]() {
            for (const std::string& answer : responses) {
                boost::system::error_code ec;
                tcp::socket               socket(ioc);
                acceptor.accept(socket, ec);
                if (ec)
                    return;
                std::string request;
                char        buf[4096];
                while (request.find("\r\n\r\n") == std::string::npos) {
                    const size_t n = socket.read_some(asio::buffer(buf), ec);
                    if (ec || n == 0)
                        break;
                    request.append(buf, n);
                }
                ++served;
                asio::write(socket, asio::buffer(answer), ec);
                socket.shutdown(tcp::socket::shutdown_both, ec);
            }
        });
    }
    ~LoopbackServer()
    {
        boost::system::error_code ec;
        acceptor.close(ec);
        if (thread.joinable())
            thread.join();
    }
    std::string url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port) + path; }
};

std::string ok_response(const std::string& body)
{
    return "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\nConnection: close\r\n\r\n" + body;
}

} // namespace

TEST_CASE("http: a redirect is reported, not followed, when asked", "[Http]")
{
    // One answer only: a client that followed the redirect would get no answer and fail.
    LoopbackServer server({"HTTP/1.1 302 Found\r\nLocation: /elsewhere\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"});
    unsigned    status = 0;
    std::string body, headers;
    Slic3r::Http::get(server.url("/start"))
        .follow_redirects(false)
        .on_header_callback([&headers](std::string h) { headers = std::move(h); })
        .on_complete([&](std::string b, unsigned s) { body = std::move(b); status = s; })
        .on_error([&](std::string b, std::string, unsigned s) { body = std::move(b); status = s; })
        .perform_sync();
    CHECK(status == 302);
    CHECK(body.empty());
    CHECK(headers.find("Location: /elsewhere") != std::string::npos);
    CHECK(server.served == 1);
}

TEST_CASE("http: on_body takes the body in pieces", "[Http]")
{
    const std::string payload(300000, 'x');

    SECTION("all of it, and on_complete gets none")
    {
        LoopbackServer server({ok_response(payload)});
        size_t         got = 0;
        unsigned       seen_status = 0;
        std::string    completed = "unset";
        Slic3r::Http::get(server.url("/file"))
            .on_body([&](unsigned s, const char*, size_t n) { seen_status = s; got += n; return true; })
            .on_complete([&](std::string b, unsigned) { completed = std::move(b); })
            .perform_sync();
        CHECK(got == payload.size());
        CHECK(seen_status == 200);
        CHECK(completed.empty());
    }
    SECTION("false stops the transfer")
    {
        LoopbackServer server({ok_response(payload)});
        std::string    error;
        Slic3r::Http::get(server.url("/file"))
            .on_body([](unsigned, const char*, size_t) { return false; })
            .on_error([&error](std::string, std::string e, unsigned) { error = std::move(e); })
            .perform_sync();
        CHECK(error == "The transfer was stopped");
    }
    SECTION("the size limit still holds")
    {
        LoopbackServer server({ok_response(payload)});
        std::string    error;
        size_t         got = 0;
        Slic3r::Http::get(server.url("/file"))
            .size_limit(1000)
            .on_body([&got](unsigned, const char*, size_t n) { got += n; return true; })
            .on_error([&error](std::string, std::string e, unsigned) { error = std::move(e); })
            .perform_sync();
        CHECK(got <= 1000);
        CHECK(error.find("limit") != std::string::npos);
    }
}
