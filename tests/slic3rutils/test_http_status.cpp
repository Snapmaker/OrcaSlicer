#include <catch2/catch_test_macros.hpp>

// Snapmaker Orca: Slic3r::Http reports every final status outside 200-299 through on_error.
// Before the change a final 1xx or 3xx (a 304, a redirect without a Location header) called
// neither callback, so a caller that waits for one of the two never finished.
#include "slic3r/Utils/Http.hpp"

#include <chrono>
#include <string>
#include <thread>
#include <utility>

#include <boost/asio.hpp>

using namespace Slic3r;

namespace {

// Answers the first connection on an ephemeral loopback port with a canned response and closes it.
class CannedServer
{
public:
    explicit CannedServer(std::string response)
        : m_acceptor(m_io, boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0))
        , m_response(std::move(response))
    {
        m_port   = m_acceptor.local_endpoint().port();
        m_thread = std::thread([this]() { serve(); });
    }

    ~CannedServer()
    {
        m_io.stop();
        if (m_thread.joinable())
            m_thread.join();
    }

    std::string url() const { return "http://127.0.0.1:" + std::to_string(m_port) + "/status"; }

private:
    void serve()
    {
        boost::asio::ip::tcp::socket socket(m_io);
        m_acceptor.async_accept(socket, [this, &socket](const boost::system::error_code& ec) {
            if (ec)
                return;
            boost::system::error_code ignored;
            boost::asio::streambuf    request;
            boost::asio::read_until(socket, request, "\r\n\r\n", ignored);
            boost::asio::write(socket, boost::asio::buffer(m_response), ignored);
            socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
            socket.close(ignored);
        });
        // Bounded: a client that never connects must not hang the suite.
        m_io.run_for(std::chrono::seconds(20));
    }

    boost::asio::io_context        m_io;
    boost::asio::ip::tcp::acceptor m_acceptor;
    std::string                    m_response;
    unsigned short                 m_port = 0;
    std::thread                    m_thread;
};

struct Outcome
{
    int         completed       = 0;
    int         failed          = 0;
    unsigned    complete_status = 0;
    unsigned    error_status    = 0;
    std::string body;
    std::string error;
};

Outcome fetch(const std::string& url)
{
    Outcome outcome;
    Http    http = Http::get(url);
    http.timeout_connect(5)
        .timeout_max(15)
        .on_complete([&outcome](std::string body, unsigned status) {
            ++outcome.completed;
            outcome.complete_status = status;
            outcome.body            = std::move(body);
        })
        .on_error([&outcome](std::string body, std::string error, unsigned status) {
            ++outcome.failed;
            outcome.error_status = status;
            outcome.body         = std::move(body);
            outcome.error        = std::move(error);
        })
        .perform_sync();
    return outcome;
}

} // namespace

TEST_CASE("A 200 answer calls on_complete only", "[HttpStatus]")
{
    CannedServer server("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok");
    const Outcome outcome = fetch(server.url());

    CHECK(outcome.completed == 1);
    CHECK(outcome.failed == 0);
    CHECK(outcome.complete_status == 200u);
    CHECK(outcome.body == "ok");
}

TEST_CASE("A final 304 calls on_error with the status", "[HttpStatus]")
{
    CannedServer server("HTTP/1.1 304 Not Modified\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    const Outcome outcome = fetch(server.url());

    CHECK(outcome.completed == 0);
    CHECK(outcome.failed == 1);
    CHECK(outcome.error_status == 304u);
    // The transfer itself succeeded, so there is no curl error text.
    CHECK(outcome.error.empty());
}

TEST_CASE("A 302 without a Location header calls on_error with the status", "[HttpStatus]")
{
    // Redirects are followed, but this one names no target, so 302 is the final status.
    CannedServer server("HTTP/1.1 302 Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    const Outcome outcome = fetch(server.url());

    CHECK(outcome.completed == 0);
    CHECK(outcome.failed == 1);
    CHECK(outcome.error_status == 302u);
    CHECK(outcome.error.empty());
}
