// Regression test for SSL_CTX_new() failing inside the vendored paho-mqtt-c.
//
// SSLSocket_createContext() used to report success (rc == 1) with a NULL SSL_CTX, and the caller
// then dereferenced it in SSL_CTX_set_info_callback(), crashing the MQTT receive thread.
//
// SSL_CTX_new() is made to fail without mocks: OPENSSL_CONF points at a configuration that
// activates only OpenSSL's "base" provider, so no TLS cipher can be fetched and the real
// SSL_CTX_new() returns NULL. OpenSSL reads OPENSSL_CONF once per process, so the whole scenario
// runs in a forked child that sets the variable before paho initialises OpenSSL; the Catch2
// runner only inspects the child's exit status and survives a crash.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <mqtt/async_client.h>

#include <openssl/opensslv.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

enum ChildExit : int {
    ConnectFailedCleanly = 0, // the expected outcome: connect() reports an error and the process lives
    ConnectSucceeded     = 10,
    ConnectTimedOut      = 11,
    SetupFailed          = 12,
    ReportPipeFailed     = 13,
    UnexpectedException  = 14,
};

struct ChildResult
{
    bool        exited = false;
    int         exit_code = -1;
    int         term_signal = 0;
    std::string ssl_errors; // what the OpenSSL error callback reported
};

// Runs in the forked child: connect an mqtts:// client to a local listener under a broken OpenSSL.
[[noreturn]] void run_connect(const std::string& conf_path, const std::string& trust_store, int report_fd)
{
    // Catch2's crash handlers are inherited from the runner; the default action keeps the
    // child's death a plain signal that the parent can read from the wait status.
    for (const int sig : {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL})
        signal(sig, SIG_DFL);
    if (!conf_path.empty())
        setenv("OPENSSL_CONF", conf_path.c_str(), 1);

    // A plain listening socket is enough: the kernel completes the TCP handshake from the
    // backlog, and paho goes on to set up TLS on the connected socket.
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;
    socklen_t len        = sizeof(addr);
    if (listener < 0 || bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(listener, 1) != 0 ||
        getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
        _exit(SetupFailed);

    // Same TLS options as MqttClient (src/slic3r/Utils/MQTT.cpp).
    mqtt::ssl_options ssl;
    ssl.set_verify(false);
    ssl.set_enable_server_cert_auth(true);
    ssl.set_ssl_version(MQTT_SSL_VERSION_TLS_1_2);
    if (!trust_store.empty())
        ssl.set_trust_store(trust_store);
    ssl.set_error_handler([report_fd](const std::string& msg) {
        const std::string line = msg + "\n";
        if (write(report_fd, line.data(), line.size()) < 0)
            _exit(ReportPipeFailed);
    });

    mqtt::connect_options opts;
    opts.set_ssl(ssl);
    opts.set_clean_session(true);
    opts.set_automatic_reconnect(false);

    // No exception may leave this function: in the forked child it would unwind into a second copy of the Catch2 runner.
    try {
        mqtt::async_client client("mqtts://127.0.0.1:" + std::to_string(ntohs(addr.sin_port)), "ssl-ctx-failure-test");
        auto tok = client.connect(opts);
        if (!tok->wait_for(std::chrono::seconds(10)))
            _exit(ConnectTimedOut);
        _exit(ConnectSucceeded);
    } catch (const mqtt::exception&) {
        _exit(ConnectFailedCleanly);
    } catch (...) {
        _exit(UnexpectedException);
    }
}

ChildResult run_in_child(const std::string& conf_path, const std::string& trust_store)
{
    int fds[2];
    REQUIRE(pipe(fds) == 0);
    fflush(nullptr);
    const pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        close(fds[0]);
        run_connect(conf_path, trust_store, fds[1]);
    }
    close(fds[1]);

    ChildResult result;
    char        buf[512];
    ssize_t     n;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0)
        result.ssl_errors.append(buf, static_cast<size_t>(n));
    close(fds[0]);

    int status = 0;
    REQUIRE(waitpid(pid, &status, 0) == pid);
    result.exited = WIFEXITED(status);
    if (result.exited)
        result.exit_code = WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        result.term_signal = WTERMSIG(status);
    return result;
}

} // namespace

TEST_CASE("mqtts connect survives SSL_CTX_new() failing", "[MQTT][SSL]")
{
    // The failure is provoked through OpenSSL 3 provider configuration; 1.1.x ignores it.
    if (OPENSSL_VERSION_NUMBER < 0x30000000L)
        SKIP("SSL_CTX_new() can only be made to fail through OpenSSL 3 provider configuration");

    // Only the "base" provider is activated; an explicit providers section stops OpenSSL from
    // loading "default", which is where the TLS ciphers live.
    std::string dir_template = (std::filesystem::temp_directory_path() / "mqtt-ssl-ctx-XXXXXX").string();
    REQUIRE(mkdtemp(dir_template.data()) != nullptr);
    const std::string dir       = dir_template;
    const std::string conf_path = dir + "/openssl.cnf";
    {
        std::ofstream conf(conf_path);
        conf << "openssl_conf = openssl_init\n"
                "[openssl_init]\n"
                "providers = provider_sect\n"
                "[provider_sect]\n"
                "base = base_sect\n"
                "[base_sect]\n"
                "activate = 1\n";
        REQUIRE(conf.good());
    }

    const ChildResult result = run_in_child(conf_path, "");
    std::remove(conf_path.c_str());
    rmdir(dir.c_str());

    INFO("terminating signal: " << result.term_signal);
    INFO("OpenSSL errors reported: " << result.ssl_errors);
    REQUIRE(result.term_signal == 0);
    REQUIRE(result.exited);
    CHECK(result.exit_code == ConnectFailedCleanly);
    // The error queue text must reach the application's callback so it can be logged.
    CHECK_THAT(result.ssl_errors, Catch::Matchers::ContainsSubstring("SSL_CTX_new"));
}

// A context that fails after SSL_CTX_new() (the trust store cannot be read) queues several OpenSSL
// errors. All of them must reach the application's handler: the C++ wrapper's callback has to return a
// positive value, because ERR_print_errors_cb() stops reporting at the first result <= 0.
TEST_CASE("mqtts connect reports every OpenSSL error of a failing trust store", "[MQTT][SSL]")
{
    const ChildResult result = run_in_child("", "/nonexistent/ssl-ctx-failure-test-ca.pem");

    INFO("terminating signal: " << result.term_signal);
    INFO("OpenSSL errors reported: " << result.ssl_errors);
    REQUIRE(result.term_signal == 0);
    REQUIRE(result.exited);
    CHECK(result.exit_code == ConnectFailedCleanly);
    // Each report reads "<thread>:error:<code>:<library>:<function>:<reason>".
    size_t reported_errors = 0;
    for (size_t pos = result.ssl_errors.find(":error:"); pos != std::string::npos; pos = result.ssl_errors.find(":error:", pos + 1))
        ++reported_errors;
    CHECK(reported_errors >= 2);
}
