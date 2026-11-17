#include <catch2/catch_test_macros.hpp>

#include "slic3r/Utils/ConnectionProcessManager.hpp"

#include <boost/filesystem/path.hpp>

#include <string>
#include <string_view>
#include <vector>

using namespace Slic3r::Gateway;

TEST_CASE("ConnectionProcessManager builds the Orca launch arguments", "[gateway][process]")
{
    const std::vector<std::string> arguments = ConnectionProcessManager::build_arguments("zh-CN");
    REQUIRE(arguments == std::vector<std::string>{"--locale=zh-CN", "--orca"});
}

TEST_CASE("ConnectionProcessManager maps shipped Orca languages to gateway locales", "[gateway][process][locale]")
{
    SECTION("Chinese variants retain their script")
    {
        REQUIRE(ConnectionProcessManager::gateway_locale("zh_CN", "CN") == "zh-CN");
        REQUIRE(ConnectionProcessManager::gateway_locale("zh_CN", "US") == "zh-US");
        REQUIRE(ConnectionProcessManager::gateway_locale("zh_TW", "CN") == "zh-Hant-CN");
        REQUIRE(ConnectionProcessManager::gateway_locale("zh_TW", "US") == "zh-Hant-US");
        REQUIRE(ConnectionProcessManager::gateway_locale("zh_HK", "CN") == "zh-Hant-CN");
        REQUIRE(ConnectionProcessManager::gateway_locale("zh_MO", "US") == "zh-Hant-US");
        REQUIRE(ConnectionProcessManager::gateway_locale("zh_SG", "CN") == "zh-CN");
        REQUIRE(ConnectionProcessManager::gateway_locale("zh-Hant-TW", "US") == "zh-Hant-US");
        REQUIRE(ConnectionProcessManager::gateway_locale("zh-Hans-SG", "CN") == "zh-CN");
        REQUIRE(ConnectionProcessManager::gateway_locale("zh_XX", "US") == "zh-US");
    }

    SECTION("Languages in the locale contract use their language subtag")
    {
        REQUIRE(ConnectionProcessManager::gateway_locale("en_US", "CN") == "en-CN");
        REQUIRE(ConnectionProcessManager::gateway_locale("ru_RU", "US") == "ru-US");
        REQUIRE(ConnectionProcessManager::gateway_locale("de_DE", "CN") == "de-CN");
        REQUIRE(ConnectionProcessManager::gateway_locale("fr_FR", "US") == "fr-US");
        REQUIRE(ConnectionProcessManager::gateway_locale("tr_TR", "CN") == "tr-CN");
        REQUIRE(ConnectionProcessManager::gateway_locale("es_ES", "US") == "es-US");
    }

    SECTION("Additional shipped Orca languages remain selectable")
    {
        REQUIRE(ConnectionProcessManager::gateway_locale("ca_ES", "CN") == "ca-CN");
        REQUIRE(ConnectionProcessManager::gateway_locale("cs_CZ", "US") == "cs-US");
        REQUIRE(ConnectionProcessManager::gateway_locale("hu_HU", "CN") == "hu-CN");
        REQUIRE(ConnectionProcessManager::gateway_locale("it_IT", "US") == "it-US");
        REQUIRE(ConnectionProcessManager::gateway_locale("ja_JP", "CN") == "ja-CN");
        REQUIRE(ConnectionProcessManager::gateway_locale("ko_KR", "US") == "ko-US");
        REQUIRE(ConnectionProcessManager::gateway_locale("lt_LT", "CN") == "lt-CN");
        REQUIRE(ConnectionProcessManager::gateway_locale("nl_NL", "US") == "nl-US");
        REQUIRE(ConnectionProcessManager::gateway_locale("pl_PL", "CN") == "pl-CN");
        REQUIRE(ConnectionProcessManager::gateway_locale("pt_BR", "US") == "pt-US");
        REQUIRE(ConnectionProcessManager::gateway_locale("sv_SE", "CN") == "sv-CN");
        REQUIRE(ConnectionProcessManager::gateway_locale("uk_UA", "US") == "uk-US");
    }

    SECTION("Input is normalized and unavailable contract languages fall back to English")
    {
        REQUIRE(ConnectionProcessManager::gateway_locale("ZH_tw", "CN") == "zh-Hant-CN");
        REQUIRE(ConnectionProcessManager::gateway_locale("pt-BR", "Others") == "pt-US");
        REQUIRE(ConnectionProcessManager::gateway_locale("he_IL", "CN") == "en-CN");
        REQUIRE(ConnectionProcessManager::gateway_locale("", "US") == "en-US");
    }
}

TEST_CASE("ConnectionProcessManager selects the platform-specific CLI executable", "[gateway][process]")
{
#if defined(_WIN32)
#if defined(_M_X64) || defined(__x86_64__)
    constexpr std::string_view expected = "snapmaker_control_windows_x64.exe";
#elif defined(_M_ARM64) || defined(__aarch64__)
    constexpr std::string_view expected = "snapmaker_control_windows_arm64.exe";
#else
#error "unsupported Windows architecture for the snapmaker_control CLI"
#endif
#elif defined(__APPLE__)
#if defined(__x86_64__)
    constexpr std::string_view expected = "snapmaker_control_macos_x64";
#elif defined(__arm64__) || defined(__aarch64__)
    constexpr std::string_view expected = "snapmaker_control_macos_arm64";
#else
#error "unsupported macOS architecture for the snapmaker_control CLI"
#endif
#elif defined(__linux__)
#if defined(__x86_64__)
    constexpr std::string_view expected = "snapmaker_control_linux_x64";
#elif defined(__aarch64__) || defined(__arm64__)
    constexpr std::string_view expected = "snapmaker_control_linux_arm64";
#else
#error "unsupported Linux architecture for the snapmaker_control CLI"
#endif
#else
#error "unsupported platform for the snapmaker_control CLI"
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

TEST_CASE("ConnectionProcessManager compares truncated macOS process names", "[gateway][process]")
{
    // The kernel truncates p_comm to 15 visible characters.
    REQUIRE(ConnectionProcessManager::truncated_process_name_matches("snapmaker_contr", "snapmaker_control_macos_arm64"));
    REQUIRE(ConnectionProcessManager::truncated_process_name_matches("snapmaker_conne", "snapmaker_connection_macos_arm64"));
    REQUIRE_FALSE(ConnectionProcessManager::truncated_process_name_matches("snapmaker_conne", "snapmaker_control_macos_arm64"));
    REQUIRE_FALSE(ConnectionProcessManager::truncated_process_name_matches("snapmaker_contr", "snapmaker_connection_macos_arm64"));
    REQUIRE_FALSE(ConnectionProcessManager::truncated_process_name_matches("", "snapmaker_control_macos_arm64"));
    REQUIRE(ConnectionProcessManager::truncated_process_name_matches("short", "short"));
    REQUIRE_FALSE(ConnectionProcessManager::truncated_process_name_matches("shorter", "short"));
}
