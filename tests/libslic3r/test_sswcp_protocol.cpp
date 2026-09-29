#include <catch2/catch_test_macros.hpp>

#include "libslic3r/SSWCPProtocol.hpp"

#include <nlohmann/json.hpp>

#include <utility>
#include <vector>

using json = nlohmann::json;
using namespace Slic3r;

TEST_CASE("SSWCP file flow mappings are aligned and normalized", "[SSWCPProtocol]")
{
    CHECK(SSWCPProtocol::build_filament_volume_types(nullptr, 3) == std::vector<std::string>{"standard", "standard", "standard"});

    const std::vector<int> values{fvtStandard, fvtHighFlow};
    CHECK(SSWCPProtocol::build_filament_volume_types(&values, 4) ==
          std::vector<std::string>{"standard", "high_flow", "standard", "standard"});

    const std::vector<int> unknown{fvtHighFlow, 99};
    CHECK(SSWCPProtocol::build_filament_volume_types(&unknown, 2) == std::vector<std::string>{"high_flow", "standard"});
}

TEST_CASE("SSWCP machine update flow types are optional but validated", "[SSWCPProtocol]")
{
    std::vector<std::string> flows;
    CHECK(SSWCPProtocol::parse_optional_flow_types(json::object(), "nozzle_volume_types", 2, flows) ==
          SSWCPProtocol::OptionalFlowTypesStatus::Missing);

    const json valid = {{"nozzle_volume_types", {"standard", "high_flow"}}};
    CHECK(SSWCPProtocol::parse_optional_flow_types(valid, "nozzle_volume_types", 2, flows) == SSWCPProtocol::OptionalFlowTypesStatus::Valid);
    CHECK(flows == std::vector<std::string>{"standard", "high_flow"});

    for (const json& invalid : std::vector<json>{{{"nozzle_volume_types", "standard"}},
                                                 {{"nozzle_volume_types", {"standard"}}},
                                                 {{"nozzle_volume_types", {"standard", 1}}},
                                                 {{"nozzle_volume_types", {"standard", "tpu_high_flow"}}}}) {
        CHECK(SSWCPProtocol::parse_optional_flow_types(invalid, "nozzle_volume_types", 2, flows) ==
              SSWCPProtocol::OptionalFlowTypesStatus::Invalid);
        CHECK(flows.empty());
    }
}

TEST_CASE("Complete cached slots override direct nozzle data atomically", "[SSWCPProtocol]")
{
    std::vector<std::string> diameters{"0.8"};
    std::vector<std::string> flows{"high_flow"};
    REQUIRE(SSWCPProtocol::select_complete_cached_nozzle_info({{"0.4", "standard"}, {"0.4", "high_flow"}}, diameters, flows));
    CHECK(diameters == std::vector<std::string>{"0.4", "0.4"});
    CHECK(flows == std::vector<std::string>{"standard", "high_flow"});

    diameters = {"0.8"};
    flows     = {"high_flow"};
    CHECK_FALSE(SSWCPProtocol::select_complete_cached_nozzle_info({{"0.4", "standard"}, {"", "high_flow"}}, diameters, flows));
    CHECK(diameters == std::vector<std::string>{"0.8"});
    CHECK(flows == std::vector<std::string>{"high_flow"});

    diameters = {"0.8"};
    flows     = {"high_flow"};
    REQUIRE(SSWCPProtocol::select_complete_cached_nozzle_info({{"0.4", "standard"}, {"0.4", ""}}, diameters, flows));
    CHECK(diameters == std::vector<std::string>{"0.4", "0.4"});
    // Incomplete cached flows must not wipe freshly resolved direct values (see SSWCPProtocol.hpp).
    CHECK(flows == std::vector<std::string>{"high_flow"});
}

TEST_CASE("tpu_high_flow is never emitted in any protocol output", "[SSWCPProtocol]")
{
    // File mapping: any non-fvtHighFlow value (including future enum additions) maps to standard.
    const std::vector<int> with_unknown{fvtHighFlow, 2, 99, fvtStandard};
    const auto             mapped = SSWCPProtocol::build_filament_volume_types(&with_unknown, 4);
    for (const std::string& v : mapped)
        CHECK(v != "tpu_high_flow");

    // Machine update: tpu_high_flow is explicitly rejected as Invalid.
    std::vector<std::string> flows;
    const json               bad_update = {{"nozzle_volume_types", {"tpu_high_flow", "standard"}}};
    CHECK(SSWCPProtocol::parse_optional_flow_types(bad_update, "nozzle_volume_types", 2, flows) ==
          SSWCPProtocol::OptionalFlowTypesStatus::Invalid);
}

TEST_CASE("Machine model normalization handles aliases and preserves empty", "[SSWCPProtocol]")
{
    // Empty must stay empty — never default to a machine type.
    CHECK(SSWCPProtocol::normalize_machine_model("") == "");

    // Known firmware aliases.
    CHECK(SSWCPProtocol::normalize_machine_model("lava") == "Snapmaker U1");
    CHECK(SSWCPProtocol::normalize_machine_model("Snapmaker test") == "Snapmaker U1");

    // Already-normalized values pass through unchanged (idempotent).
    CHECK(SSWCPProtocol::normalize_machine_model("Snapmaker U1") == "Snapmaker U1");
    const auto once = SSWCPProtocol::normalize_machine_model("Snapmaker Artisan");
    CHECK(SSWCPProtocol::normalize_machine_model(once) == once);

    // Unknown models pass through as-is.
    CHECK(SSWCPProtocol::normalize_machine_model("SomeFuturePrinter") == "SomeFuturePrinter");
}
