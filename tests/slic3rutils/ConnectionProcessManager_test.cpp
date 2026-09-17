#include <catch2/catch_test_macros.hpp>

#include "slic3r/Utils/ConnectionProcessManager.hpp"

#include <boost/filesystem/path.hpp>

#include <string>
#include <vector>

using namespace Slic3r::Gateway;

TEST_CASE("ConnectionProcessManager builds the Orca launch arguments", "[gateway][process]")
{
    const std::vector<std::string> arguments = ConnectionProcessManager::build_arguments("zh-CN");
    REQUIRE(arguments == std::vector<std::string>{"--locale=zh-CN", "--orca"});
}

TEST_CASE("ConnectionProcessManager selects the platform-specific CLI executable", "[gateway][process]")
{
#if defined(_WIN32)
#if defined(_M_X64) || defined(__x86_64__)
    constexpr std::string_view expected = "snapmaker_connection_windows_x64.exe";
#elif defined(_M_ARM64) || defined(__aarch64__)
    constexpr std::string_view expected = "snapmaker_connection_windows_arm64.exe";
#else
#error "unsupported Windows architecture for the snapmaker_connection CLI"
#endif
#elif defined(__APPLE__)
#if defined(__x86_64__)
    constexpr std::string_view expected = "snapmaker_connection_macos_x64";
#elif defined(__arm64__) || defined(__aarch64__)
    constexpr std::string_view expected = "snapmaker_connection_macos_arm64";
#else
#error "unsupported macOS architecture for the snapmaker_connection CLI"
#endif
#elif defined(__linux__)
#if defined(__x86_64__)
    constexpr std::string_view expected = "snapmaker_connection_linux_x64";
#elif defined(__aarch64__) || defined(__arm64__)
    constexpr std::string_view expected = "snapmaker_connection_linux_arm64";
#else
#error "unsupported Linux architecture for the snapmaker_connection CLI"
#endif
#else
#error "unsupported platform for the snapmaker_connection CLI"
#endif

    REQUIRE(ConnectionProcessManager::cli_executable_name() == expected);
}

TEST_CASE("ConnectionProcessManager parses one PORT frame after noise", "[gateway][process]")
{
    std::uint16_t     port   = 0;
    const std::string output = "starting dart cli\r\nloading locale\r\nPORT:8888\r\n\r\n";
    REQUIRE(ConnectionProcessManager::parse_port_frame(output, port) == ProcessDiscoveryError::None);
    REQUIRE(port == 8888);
}

TEST_CASE("ConnectionProcessManager rejects malformed and duplicate PORT frames", "[gateway][process]")
{
    std::uint16_t port = 0;
    REQUIRE(ConnectionProcessManager::parse_port_frame("PORT:88\r\n", port) == ProcessDiscoveryError::InvalidPortFrame);
    REQUIRE(ConnectionProcessManager::parse_port_frame("PORT:0\r\n\r\n", port) == ProcessDiscoveryError::InvalidPortFrame);
    REQUIRE(ConnectionProcessManager::parse_port_frame("PORT:70000\r\n\r\n", port) == ProcessDiscoveryError::InvalidPortFrame);
    REQUIRE(ConnectionProcessManager::parse_port_frame("PORT:8888\r\n\r\nPORT:8889\r\n\r\n", port) ==
            ProcessDiscoveryError::MultiplePortFrames);
}

TEST_CASE("ConnectionProcessManager uses the injected process runner", "[gateway][process]")
{
    ConnectionProcessManager::Config config;
    config.executable = boost::filesystem::path{"snapmaker_connection.exe"};

    std::vector<std::string> seen_arguments;
    ConnectionProcessManager manager(config, [&](const std::vector<std::string>& arguments) {
        seen_arguments = arguments;
        ConnectionProcessManager::ProcessRunResult result;
        result.stdout_data = "PORT:8080\r\n\r\n";
        return result;
    });

    const auto result = manager.discover_port("en-US");
    REQUIRE(result.error == ProcessDiscoveryError::None);
    REQUIRE(result.port == 8080);
    REQUIRE(seen_arguments == std::vector<std::string>{"--locale=en-US", "--orca"});
}

TEST_CASE("ConnectionProcessManager terminate is a no-op without a tracked child", "[gateway][process]")
{
    ConnectionProcessManager::Config config;
    config.executable = boost::filesystem::path{"snapmaker_connection.exe"};

    ConnectionProcessManager manager(config, [](const std::vector<std::string>&) {
        ConnectionProcessManager::ProcessRunResult result;
        result.stdout_data = "PORT:8080\r\n\r\n";
        return result;
    });

    REQUIRE(manager.discover_port("en-US").error == ProcessDiscoveryError::None);
    // Injected runners bypass the real child tracking, so terminate() must stay safe.
    manager.terminate();
    manager.terminate();
}
