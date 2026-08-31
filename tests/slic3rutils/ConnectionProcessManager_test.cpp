#include <catch2/catch.hpp>

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
