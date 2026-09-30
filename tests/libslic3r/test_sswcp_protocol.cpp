#include <catch2/catch_test_macros.hpp>

#include "libslic3r/SSWCPProtocol.hpp"

#include "nlohmann/json.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

using json = nlohmann::json;
using namespace Slic3r;

TEST_CASE("SSWCP file flow mappings are aligned and normalized", "[SSWCPProtocol]")
{
    CHECK(SSWCPProtocol::build_filament_volume_types(nullptr, 3) ==
          std::vector<std::string>{"standard", "standard", "standard"});

    const std::vector<int> values{nvtStandard, nvtHighFlow};
    CHECK(SSWCPProtocol::build_filament_volume_types(&values, 4) ==
          std::vector<std::string>{"standard", "high_flow", "standard", "standard"});

    const std::vector<int> unknown{nvtHighFlow, 99};
    CHECK(SSWCPProtocol::build_filament_volume_types(&unknown, 2) ==
          std::vector<std::string>{"high_flow", "standard"});
}

TEST_CASE("SSWCP machine update flow types are optional but validated", "[SSWCPProtocol]")
{
    std::vector<std::string> flows;
    CHECK(SSWCPProtocol::parse_optional_flow_types(json::object(), "nozzle_volume_types", 2, flows) ==
          SSWCPProtocol::OptionalFlowTypesStatus::Missing);

    const json valid = {{"nozzle_volume_types", {"standard", "high_flow"}}};
    CHECK(SSWCPProtocol::parse_optional_flow_types(valid, "nozzle_volume_types", 2, flows) ==
          SSWCPProtocol::OptionalFlowTypesStatus::Valid);
    CHECK(flows == std::vector<std::string>{"standard", "high_flow"});

    for (const json &invalid : std::vector<json>{
             {{"nozzle_volume_types", "standard"}},
             {{"nozzle_volume_types", {"standard"}}},
             {{"nozzle_volume_types", {"standard", 1}}},
             {{"nozzle_volume_types", {"standard", "tpu_high_flow"}}}}) {
        CHECK(SSWCPProtocol::parse_optional_flow_types(invalid, "nozzle_volume_types", 2, flows) ==
              SSWCPProtocol::OptionalFlowTypesStatus::Invalid);
        CHECK(flows.empty());
    }
}

TEST_CASE("SSWCP system info preserves supported scalar and array nozzle data", "[SSWCPProtocol]")
{
    const json response = {{"data", {{"system_info", {{"product_info", {
        {"machine_type", "Snapmaker U1"},
        {"device_name", "Workshop"},
        {"nozzle_diameter", {0.2, 0.4, 0.6, 0.8}},
        {"nozzle_volume_type", {"standard", "high_flow", "standard", "high_flow"}}
    }}}}}}};

    MachineInfo info;
    REQUIRE(SSWCPProtocol::parse_machine_info_response(response, info));
    CHECK(info.model == "Snapmaker U1");
    CHECK(info.device_name == "Workshop");
    CHECK(info.nozzle_diameters == std::vector<std::string>{"0.2", "0.4", "0.6", "0.8"});
    CHECK(info.nozzle_volume_types ==
          std::vector<std::string>{"standard", "high_flow", "standard", "high_flow"});

    const json scalar = {{"system_info", {{"product_info", {
        {"nozzle_diameter", 0.8}, {"nozzle_volume_type", "high_flow"}
    }}}}};
    REQUIRE(SSWCPProtocol::parse_machine_info_response(scalar, info));
    CHECK(info.nozzle_diameters == std::vector<std::string>{"0.8"});
    CHECK(info.nozzle_volume_types == std::vector<std::string>{"high_flow"});

    for (const auto &[diameter, expected] : std::vector<std::pair<double, std::string>>{
             {0.2, "0.2"}, {0.4, "0.4"}, {0.6, "0.6"}, {0.8, "0.8"}}) {
        const json single = {{"system_info", {{"product_info", {{"nozzle_diameter", diameter}}}}}};
        REQUIRE(SSWCPProtocol::parse_machine_info_response(single, info));
        CHECK(info.nozzle_diameters == std::vector<std::string>{expected});
    }
}

TEST_CASE("SSWCP system info tolerates unavailable flow data", "[SSWCPProtocol]")
{
    MachineInfo info;
    const json missing = {{"system_info", {{"product_info", {{"machine_type", "Snapmaker U1"}}}}}};
    REQUIRE(SSWCPProtocol::parse_machine_info_response(missing, info));
    CHECK(info.nozzle_volume_types.empty());

    const json invalid = {{"system_info", {{"product_info", {
        {"machine_type", "Snapmaker U1"}, {"nozzle_volume_type", {"standard", "tpu_high_flow"}}
    }}}}};
    REQUIRE(SSWCPProtocol::parse_machine_info_response(invalid, info));
    CHECK(info.nozzle_volume_types.empty());
}

TEST_CASE("Complete cached slots override direct nozzle data atomically", "[SSWCPProtocol]")
{
    std::vector<std::string> diameters{"0.8"};
    std::vector<std::string> flows{"high_flow"};
    REQUIRE(SSWCPProtocol::select_complete_cached_nozzle_info(
        {{"0.4", "standard"}, {"0.4", "high_flow"}}, diameters, flows));
    CHECK(diameters == std::vector<std::string>{"0.4", "0.4"});
    CHECK(flows == std::vector<std::string>{"standard", "high_flow"});

    diameters = {"0.8"};
    flows     = {"high_flow"};
    CHECK_FALSE(SSWCPProtocol::select_complete_cached_nozzle_info(
        {{"0.4", "standard"}, {"", "high_flow"}}, diameters, flows));
    CHECK(diameters == std::vector<std::string>{"0.8"});
    CHECK(flows == std::vector<std::string>{"high_flow"});

    diameters = {"0.8"};
    flows     = {"high_flow"};
    REQUIRE(SSWCPProtocol::select_complete_cached_nozzle_info(
        {{"0.4", "standard"}, {"0.4", ""}}, diameters, flows));
    CHECK(diameters == std::vector<std::string>{"0.4", "0.4"});
    // Incomplete cached flows must not wipe freshly resolved direct values (see SSWCPProtocol.hpp).
    CHECK(flows == std::vector<std::string>{"high_flow"});
}

TEST_CASE("tpu_high_flow is never emitted in any protocol output", "[SSWCPProtocol]")
{
    // File mapping: any non-nvtHighFlow value (including future enum additions) maps to standard.
    const std::vector<int> with_unknown{nvtHighFlow, 2, 99, nvtStandard};
    const auto             mapped = SSWCPProtocol::build_filament_volume_types(&with_unknown, 4);
    for (const std::string &v : mapped)
        CHECK(v != "tpu_high_flow");

    // Machine update: tpu_high_flow is explicitly rejected as Invalid.
    std::vector<std::string> flows;
    const json               bad_update = {{"nozzle_volume_types", {"tpu_high_flow", "standard"}}};
    CHECK(SSWCPProtocol::parse_optional_flow_types(bad_update, "nozzle_volume_types", 2, flows) ==
          SSWCPProtocol::OptionalFlowTypesStatus::Invalid);

    // System info: tpu_high_flow leaves flow unavailable, never stored.
    MachineInfo  info;
    const json   bad_system = {{"system_info", {{"product_info", {
        {"machine_type", "X"}, {"nozzle_volume_type", {"tpu_high_flow", "high_flow"}}
    }}}}};
    REQUIRE(SSWCPProtocol::parse_machine_info_response(bad_system, info));
    CHECK(info.nozzle_volume_types.empty());
    for (const std::string &v : info.nozzle_volume_types)
        CHECK(v != "tpu_high_flow");
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

TEST_CASE("parse_extruder_nozzle_info extracts real-time nozzle data", "[SSWCPProtocol]")
{
    json response = R"({
        "data": {
            "status": {
                "extruder": {
                    "nozzle_diameter": "0.4",
                    "nozzle_volume_type": "standard"
                },
                "extruder1": {
                    "nozzle_diameter": 0.6,
                    "nozzle_volume_type": "high_flow"
                }
            }
        }
    })"_json;

    std::vector<std::string> diameters, flows;
    CHECK(SSWCPProtocol::parse_extruder_nozzle_info(response, diameters, flows));
    REQUIRE(diameters.size() == 2);
    CHECK(diameters[0] == "0.4");
    CHECK(diameters[1] == "0.6");
    REQUIRE(flows.size() == 2);
    CHECK(flows[0] == "standard");
    CHECK(flows[1] == "high_flow");

    // Missing status -> false
    json empty = json::object();
    CHECK(!SSWCPProtocol::parse_extruder_nozzle_info(empty, diameters, flows));

    // Invalid diameter value -> skipped, returns false when none valid
    json bad = json::parse(R"({"data":{"status":{"extruder":{"nozzle_diameter":"0.99"}}}})");
    CHECK(!SSWCPProtocol::parse_extruder_nozzle_info(bad, diameters, flows));

    // Missing flow type defaults to standard
    json no_flow = json::parse(R"({"data":{"status":{"extruder":{"nozzle_diameter":"0.8"}}}})");
    CHECK(SSWCPProtocol::parse_extruder_nozzle_info(no_flow, diameters, flows));
    // Missing nozzle_volume_type means firmware doesn't report flow: leave flows empty
    CHECK(flows.empty());
}

TEST_CASE("parse_extruder_nozzle_info dynamically discovers and orders extruders", "[SSWCPProtocol]")
{
    // Keys in non-sequential order: extruder1 before extruder.
    // Also tests that non-extruder keys (gcode_move, heater_bed) are ignored.
    json response = json::parse(R"({
        "data": {
            "status": {
                "gcode_move": {},
                "extruder1": {"nozzle_diameter": "0.6", "nozzle_volume_type": "high_flow"},
                "extruder": {"nozzle_diameter": "0.4", "nozzle_volume_type": "standard"},
                "heater_bed": {},
                "extruder2": {"nozzle_diameter": "0.8"}
            }
        }
    })");

    std::vector<std::string> diameters, flows;
    CHECK(SSWCPProtocol::parse_extruder_nozzle_info(response, diameters, flows));
    REQUIRE(diameters.size() == 3);
    CHECK(diameters[0] == "0.4");
    CHECK(diameters[1] == "0.6");
    CHECK(diameters[2] == "0.8");
    // Snapmaker Orca fork: upstream clears every flow type as soon as one extruder lacks its own.
    // Here a tool head without a flow type is a Standard one next to heads that state theirs.
    CHECK(flows == std::vector<std::string>{"standard", "high_flow", "standard"});
}

TEST_CASE("parse_extruder_nozzle_info ignores pathological extruder key suffixes", "[SSWCPProtocol]")
{
    // A key with an absurdly long numeric suffix must be skipped, not crash stoul.
    json response = json::parse(R"({
        "data": {
            "status": {
                "extruder99999999999999999": {"nozzle_diameter": "0.4", "nozzle_volume_type": "standard"}
            }
        }
    })");

    std::vector<std::string> diameters, flows;
    CHECK(!SSWCPProtocol::parse_extruder_nozzle_info(response, diameters, flows));
    CHECK(diameters.empty());

    // Sanity: a reasonable multi-digit suffix still works.
    json ok = json::parse(R"({
        "data": {
            "status": {
                "extruder10": {"nozzle_diameter": "0.8", "nozzle_volume_type": "high_flow"}
            }
        }
    })");
    CHECK(SSWCPProtocol::parse_extruder_nozzle_info(ok, diameters, flows));
    CHECK(diameters == std::vector<std::string>{"0.8"});
}

// Snapmaker Orca fork: the protocol speaks NozzleVolumeType instead of upstream's FilamentVolumeType.
TEST_CASE("Wire spelling of nozzle volume types", "[SSWCPProtocol]")
{
    CHECK(std::string(SSWCPProtocol::to_wire(nvtStandard)) == "standard");
    CHECK(std::string(SSWCPProtocol::to_wire(nvtHighFlow)) == "high_flow");
    // The firmware knows two flow types only.
    CHECK(std::string(SSWCPProtocol::to_wire(nvtHybrid)) == "standard");
    CHECK(std::string(SSWCPProtocol::to_wire(nvtTPUHighFlow)) == "standard");

    CHECK(SSWCPProtocol::to_nozzle_volume_type("standard") == nvtStandard);
    CHECK(SSWCPProtocol::to_nozzle_volume_type("high_flow") == nvtHighFlow);
    // The config spelling is not the wire spelling.
    CHECK_FALSE(SSWCPProtocol::to_nozzle_volume_type("High Flow").has_value());
    CHECK_FALSE(SSWCPProtocol::to_nozzle_volume_type("tpu_high_flow").has_value());
    CHECK_FALSE(SSWCPProtocol::to_nozzle_volume_type("").has_value());

    // The integers the plate metadata and the file mapping carry are upstream's fvt* values.
    CHECK(int(nvtStandard) == 0);
    CHECK(int(nvtHighFlow) == 1);
}

// SSWCP::query_machine_info reads the response through parse_machine_info_response: a scalar
// diameter keeps its own value instead of a fixed "0.2".
TEST_CASE("A scalar nozzle diameter reaches the machine info as its own size", "[SSWCPProtocol]")
{
    for (const auto &[diameter, expected] : std::vector<std::pair<json, std::string>>{
             {json(0.2), "0.2"}, {json(0.4), "0.4"}, {json(0.6), "0.6"}, {json(0.8), "0.8"}, {json("0.6"), "0.6"}}) {
        // "data" already unwrapped, as query_machine_info passes it.
        const json response = {{"system_info", {{"product_info", {{"machine_type", "lava"}, {"nozzle_diameter", diameter}}}}}};
        MachineInfo info;
        REQUIRE(SSWCPProtocol::parse_machine_info_response(response, info));
        CHECK(info.nozzle_diameters == std::vector<std::string>{expected});
        CHECK(SSWCPProtocol::normalize_machine_model(info.model) == "Snapmaker U1");
    }

    // Unknown sizes are dropped, in a scalar and inside an array.
    std::vector<std::string> out;
    SSWCPProtocol::parse_nozzle_diameters(json::array({0.4, "0.8", 0.5, "1.0", nullptr, 0.2}), out);
    CHECK(out == std::vector<std::string>{"0.4", "0.8", "0.2"});
    CHECK_FALSE(SSWCPProtocol::append_nozzle_diameter(json(0.5), out));
    CHECK(out.size() == 3);
}

// Snapmaker Orca fork: what field firmware sends for the flow type of a nozzle is not recorded
// anywhere yet. The cases below pin what the parsers accept, with payloads made up for that.

TEST_CASE("A flow type is read whatever its spelling", "[SSWCPProtocol][HighFlow]")
{
    for (const char *text : {"high_flow", "High Flow", "HIGH_FLOW", "high-flow", "HighFlow", " high flow "})
        CHECK(SSWCPProtocol::normalize_flow_type(text) == std::optional<std::string>("high_flow"));
    for (const char *text : {"standard", "Standard", "STANDARD", " standard"})
        CHECK(SSWCPProtocol::normalize_flow_type(text) == std::optional<std::string>("standard"));
    for (const char *text : {"", "tpu_high_flow", "TPU High Flow", "Hybrid", "high", "0", "1"})
        CHECK_FALSE(SSWCPProtocol::normalize_flow_type(text).has_value());

    CHECK(SSWCPProtocol::is_unstated_flow_type(json()));
    CHECK(SSWCPProtocol::is_unstated_flow_type(json("")));
    CHECK(SSWCPProtocol::is_unstated_flow_type(json("  ")));
    CHECK_FALSE(SSWCPProtocol::is_unstated_flow_type(json("standard")));
    CHECK_FALSE(SSWCPProtocol::is_unstated_flow_type(json(0)));
}

TEST_CASE("A tool head without a flow type is a Standard one next to heads that state theirs", "[SSWCPProtocol][HighFlow]")
{
    std::vector<std::string> diameters, flows;

    // Extruder objects: the config spelling, one head without the field, one with null.
    const json objects = R"({"data":{"status":{
        "extruder":  {"nozzle_diameter": 0.4, "nozzle_volume_type": "High Flow"},
        "extruder1": {"nozzle_diameter": 0.4},
        "extruder2": {"nozzle_diameter": 0.4, "nozzle_volume_type": null},
        "extruder3": {"nozzle_diameter": 0.4, "nozzle_volume_type": "STANDARD"}
    }}})"_json;
    REQUIRE(SSWCPProtocol::parse_extruder_nozzle_info(objects, diameters, flows));
    CHECK(diameters == std::vector<std::string>{"0.4", "0.4", "0.4", "0.4"});
    CHECK(flows == std::vector<std::string>{"high_flow", "standard", "standard", "standard"});

    // A type this tree does not know is a Standard nozzle, as upstream has it.
    const json unknown = R"({"status":{
        "extruder":  {"nozzle_diameter": 0.4, "nozzle_volume_type": "tpu_high_flow"},
        "extruder1": {"nozzle_diameter": 0.4, "nozzle_volume_type": "high_flow"}
    }})"_json;
    REQUIRE(SSWCPProtocol::parse_extruder_nozzle_info(unknown, diameters, flows));
    CHECK(flows == std::vector<std::string>{"standard", "high_flow"});

    // Firmware older than High Flow: no head states a type, so none is reported.
    const json silent = R"({"status":{
        "extruder":  {"nozzle_diameter": 0.4},
        "extruder1": {"nozzle_diameter": 0.6, "nozzle_volume_type": ""}
    }})"_json;
    REQUIRE(SSWCPProtocol::parse_extruder_nozzle_info(silent, diameters, flows));
    CHECK(diameters == std::vector<std::string>{"0.4", "0.6"});
    CHECK(flows.empty());

    // System info: array with gaps, the plural field name, a scalar in the config spelling.
    MachineInfo info;
    const json  gaps = {{"system_info", {{"product_info", {
        {"machine_type", "Snapmaker U1"}, {"nozzle_diameter", {0.4, 0.4, 0.4, 0.4}},
        {"nozzle_volume_type", {nullptr, "High Flow", "", "standard"}}}}}}};
    REQUIRE(SSWCPProtocol::parse_machine_info_response(gaps, info));
    CHECK(info.nozzle_volume_types == std::vector<std::string>{"standard", "high_flow", "standard", "standard"});

    const json plural = {{"system_info", {{"product_info", {
        {"machine_type", "Snapmaker U1"}, {"nozzle_volume_types", {"high_flow", "standard"}}}}}}};
    REQUIRE(SSWCPProtocol::parse_machine_info_response(plural, info));
    CHECK(info.nozzle_volume_types == std::vector<std::string>{"high_flow", "standard"});

    const json all_null = {{"system_info", {{"product_info", {
        {"machine_type", "Snapmaker U1"}, {"nozzle_volume_type", {nullptr, nullptr}}}}}}};
    REQUIRE(SSWCPProtocol::parse_machine_info_response(all_null, info));
    CHECK(info.nozzle_volume_types.empty());

    // The update of the device page: gaps are Standard, nothing stated is no flow data.
    const json update = {{"nozzle_volume_types", {"High Flow", nullptr, "", "standard"}}};
    REQUIRE(SSWCPProtocol::parse_optional_flow_types(update, "nozzle_volume_types", 4, flows) ==
            SSWCPProtocol::OptionalFlowTypesStatus::Valid);
    CHECK(flows == std::vector<std::string>{"high_flow", "standard", "standard", "standard"});
    const json nothing = {{"nozzle_volume_types", {nullptr, "", nullptr, ""}}};
    CHECK(SSWCPProtocol::parse_optional_flow_types(nothing, "nozzle_volume_types", 4, flows) ==
          SSWCPProtocol::OptionalFlowTypesStatus::Missing);
    CHECK(flows.empty());
}

TEST_CASE("Machine info is merged from the system info and the extruder objects", "[SSWCPProtocol][HighFlow]")
{
    const json system_info = {{"data", {{"system_info", {{"product_info", {
        {"machine_type", "lava"}, {"device_name", "Workshop"},
        {"nozzle_diameter", {0.4, 0.4, 0.4, 0.4}},
        {"nozzle_volume_type", {"standard", "standard", "standard", "high_flow"}}}}}}}}};
    const json objects = R"({"data":{"status":{
        "extruder":  {"nozzle_diameter": 0.6, "nozzle_volume_type": "standard"},
        "extruder1": {"nozzle_diameter": 0.4, "nozzle_volume_type": "high_flow"},
        "extruder2": {"nozzle_diameter": 0.4, "nozzle_volume_type": "standard"},
        "extruder3": {"nozzle_diameter": 0.4, "nozzle_volume_type": "standard"}
    }}})"_json;
    const json objects_without_flows = R"({"data":{"status":{
        "extruder":  {"nozzle_diameter": 0.6},
        "extruder1": {"nozzle_diameter": 0.4}
    }}})"_json;
    using SSWCPProtocol::ResolveStatus;

    SECTION("no answer at all") {
        const SSWCPProtocol::ResolveResult result = SSWCPProtocol::merge_machine_info(json(), json());
        CHECK(result.status == ResolveStatus::NoResponse);
        CHECK(result.info.model.empty());
    }
    SECTION("the live extruder objects replace the nozzle data of the system info") {
        const SSWCPProtocol::ResolveResult result = SSWCPProtocol::merge_machine_info(system_info, objects);
        CHECK(result.status == ResolveStatus::Complete);
        CHECK(result.info.model == "Snapmaker U1");
        CHECK(result.info.device_name == "Workshop");
        CHECK(result.info.nozzle_diameters == std::vector<std::string>{"0.6", "0.4", "0.4", "0.4"});
        CHECK(result.info.nozzle_volume_types == std::vector<std::string>{"standard", "high_flow", "standard", "standard"});
    }
    SECTION("extruder objects without flow types keep those of the system info") {
        const SSWCPProtocol::ResolveResult result = SSWCPProtocol::merge_machine_info(system_info, objects_without_flows);
        CHECK(result.info.nozzle_diameters == std::vector<std::string>{"0.6", "0.4"});
        CHECK(result.info.nozzle_volume_types == std::vector<std::string>{"standard", "standard", "standard", "high_flow"});
    }
    SECTION("firmware that reports no flow type leaves the list empty, so the tool heads keep the user's choice") {
        const json old_firmware = {{"system_info", {{"product_info", {
            {"machine_type", "Snapmaker U1"}, {"nozzle_diameter", {0.4, 0.4, 0.4, 0.4}}}}}}};
        const SSWCPProtocol::ResolveResult result = SSWCPProtocol::merge_machine_info(old_firmware, objects_without_flows);
        CHECK(result.status == ResolveStatus::Complete);
        CHECK(result.info.nozzle_volume_types.empty());
    }
    SECTION("the identity alone") {
        const json identity = {{"system_info", {{"product_info", {{"machine_type", "Snapmaker U1"}}}}}};
        CHECK(SSWCPProtocol::merge_machine_info(identity, json()).status == ResolveStatus::GotIdentity);
        // Extruder objects without an identity are no answer: the model gates every caller.
        CHECK(SSWCPProtocol::merge_machine_info(json(), objects).status == ResolveStatus::NoResponse);
    }
}

TEST_CASE("The printer preset of a sync is the one of the first High Flow tool head", "[SSWCPProtocol][HighFlow]")
{
    // Without the size predicate: the first High Flow tool head is the reference.
    CHECK(SSWCPProtocol::sync_reference_head({}, 4) == 0);
    CHECK(SSWCPProtocol::sync_reference_head({"standard", "standard"}, 4) == 0);
    CHECK(SSWCPProtocol::sync_reference_head({"standard", "standard", "high_flow", "high_flow"}, 4) == 2);
    // A flow type beyond the reported nozzle sizes names no tool head.
    CHECK(SSWCPProtocol::sync_reference_head({"standard", "standard", "high_flow"}, 2) == 0);
}

TEST_CASE("The printer preset of a sync follows tool head 1 unless its size offers no High Flow while a High Flow head's size does", "[SSWCPProtocol][HighFlow]")
{
    // A vendor whose 0.4, 0.6 and 0.8 mm machine presets declare High Flow and whose 0.2 mm preset does not.
    const auto offers = [](const std::string &diameter) { return diameter == "0.4" || diameter == "0.6" || diameter == "0.8"; };

    // Tool head 1 offers High Flow: the plate stays its size, the 0.6 mm High Flow head follows on it.
    CHECK(SSWCPProtocol::sync_reference_head({"standard", "high_flow", "standard", "standard"}, 4, offers, {"0.4", "0.6", "0.4", "0.4"}) == 0);
    // Tool head 1 offers nothing: the first High Flow head whose size offers.
    CHECK(SSWCPProtocol::sync_reference_head({"high_flow", "high_flow"}, 2, offers, {"0.2", "0.6"}) == 1);
    CHECK(SSWCPProtocol::sync_reference_head({"standard", "high_flow", "standard", "standard"}, 4, offers, {"0.2", "0.6", "0.4", "0.4"}) == 1);
    // A High Flow head at a size that offers nothing is no reference (it is reset with a notice).
    CHECK(SSWCPProtocol::sync_reference_head({"standard", "high_flow"}, 2, offers, {"0.2", "0.2"}) == 0);
    // Nothing reported, or fewer diameters than flow types.
    CHECK(SSWCPProtocol::sync_reference_head({}, 4, offers, {}) == 0);
    CHECK(SSWCPProtocol::sync_reference_head({"high_flow", "high_flow"}, 2, offers, {"0.2"}) == 0);
}

TEST_CASE("A filament is sent with the flow type of the tool head that prints it", "[SSWCPProtocol][HighFlow]")
{
    const std::vector<int> heads{nvtStandard, nvtHighFlow, nvtStandard, nvtHybrid};

    // Five filaments: identity for the first four, the fifth on tool head 2.
    const std::vector<int> types = SSWCPProtocol::filament_volume_types_by_head(heads, {0, 1, 2, 3, 1}, 5);
    CHECK(types == std::vector<int>{nvtStandard, nvtHighFlow, nvtStandard, nvtHybrid, nvtHighFlow});
    CHECK(SSWCPProtocol::build_filament_volume_types(&types, 5) ==
          std::vector<std::string>{"standard", "high_flow", "standard", "standard", "high_flow"});

    // A filament without a head entry, or on a head that does not exist, is Standard.
    CHECK(SSWCPProtocol::filament_volume_types_by_head(heads, {1, 9}, 3) == std::vector<int>{nvtHighFlow, nvtStandard, nvtStandard});
    CHECK(SSWCPProtocol::filament_volume_types_by_head({}, {0, 1}, 2) == std::vector<int>{nvtStandard, nvtStandard});

    CHECK(SSWCPProtocol::build_nozzle_volume_types(heads, 4) == std::vector<std::string>{"standard", "high_flow", "standard", "standard"});
    CHECK(SSWCPProtocol::build_nozzle_volume_types({nvtHighFlow}, 4) == std::vector<std::string>{"high_flow", "standard", "standard", "standard"});
    CHECK(SSWCPProtocol::build_nozzle_volume_types({}, 0).empty());
}

TEST_CASE("The event of sw_FinishFilamentMapping never fails the request", "[SSWCPProtocol][HighFlow]")
{
    using SSWCPProtocol::FinishFilamentMappingEvent;
    CHECK(SSWCPProtocol::parse_finish_filament_mapping_event(json::object()) == FinishFilamentMappingEvent::None);
    CHECK(SSWCPProtocol::parse_finish_filament_mapping_event(json()) == FinishFilamentMappingEvent::None);
    CHECK(SSWCPProtocol::parse_finish_filament_mapping_event({{"event", nullptr}}) == FinishFilamentMappingEvent::None);
    CHECK(SSWCPProtocol::parse_finish_filament_mapping_event({{"event", 0}}) == FinishFilamentMappingEvent::None);
    CHECK(SSWCPProtocol::parse_finish_filament_mapping_event({{"event", 1}}) == FinishFilamentMappingEvent::CustomFlowRegroup);
    CHECK(SSWCPProtocol::parse_finish_filament_mapping_event({{"event", "1"}}) == FinishFilamentMappingEvent::CustomFlowRegroup);
    // Unknown and malformed values close the webview only.
    for (const json &event : std::vector<json>{json(2), json(-1), json(1.5), json("regroup"), json(""), json(true), json::array({1}),
                                               json("99999999999999999999")})
        CHECK(SSWCPProtocol::parse_finish_filament_mapping_event({{"event", event}}) == FinishFilamentMappingEvent::None);
}

TEST_CASE("the pin code answer is read without an exception path", "[SSWCPProtocol]")
{
    json result;
    CHECK(SSWCPProtocol::parse_pin_code_response(R"({"jsonrpc":"2.0","result":{"pin_code":"123456"},"id":7})", result));
    CHECK(result == json{{"pin_code", "123456"}});

    // A scalar result is handed over as it is.
    CHECK(SSWCPProtocol::parse_pin_code_response(R"({"result":"654321"})", result));
    CHECK(result == json("654321"));

    // Not JSON, not an object, no result: false, and the previous result is left alone.
    result = json("kept");
    for (const char *message : {"", "not json", "{\"result\":", "[1,2,3]", "42", R"({"error":"timeout"})"}) {
        CHECK_FALSE(SSWCPProtocol::parse_pin_code_response(message, result));
        CHECK(result == json("kept"));
    }
}
