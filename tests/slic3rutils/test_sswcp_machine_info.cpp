#include <catch2/catch_test_macros.hpp>

// The helpers live in libslic3r/SSWCPProtocol (one implementation, shared with the High Flow
// protocol); these cases keep the device identity rules the GUI relies on.
#include "libslic3r/SSWCPProtocol.hpp"

using namespace Slic3r::SSWCPProtocol;
using nlohmann::json;

TEST_CASE("A scalar nozzle diameter keeps its value", "[SSWCP][MachineInfo]")
{
    // Single-nozzle firmware reports one number instead of an array; every size keeps its value.
    for (const auto &[value, text] : std::vector<std::pair<double, std::string>>{
             {0.2, "0.2"}, {0.4, "0.4"}, {0.6, "0.6"}, {0.8, "0.8"}}) {
        std::vector<std::string> out;
        parse_nozzle_diameters(json(value), out);
        REQUIRE(out.size() == 1);
        CHECK(out.front() == text);
    }

    std::vector<std::string> out;
    parse_nozzle_diameters(json("0.6"), out);
    CHECK(out == std::vector<std::string>{"0.6"});
}

TEST_CASE("A nozzle diameter array mixes numbers and strings and drops unknown sizes", "[SSWCP][MachineInfo]")
{
    std::vector<std::string> out;
    parse_nozzle_diameters(json::array({0.4, "0.8", 0.5, "1.0", nullptr, 0.2}), out);
    CHECK(out == std::vector<std::string>{"0.4", "0.8", "0.2"});

    out.clear();
    CHECK_FALSE(append_nozzle_diameter(json(0.5), out));
    CHECK_FALSE(append_nozzle_diameter(json::object(), out));
    CHECK(out.empty());
}

TEST_CASE("Firmware aliases name a Snapmaker U1", "[SSWCP][MachineInfo]")
{
    CHECK(normalize_machine_model("lava") == "Snapmaker U1");
    CHECK(normalize_machine_model("Snapmaker test") == "Snapmaker U1");
    CHECK(normalize_machine_model("Snapmaker U1") == "Snapmaker U1");
    CHECK(normalize_machine_model("Snapmaker J1") == "Snapmaker J1");
    // Never defaults to a machine type: callers gate on the result.
    CHECK(normalize_machine_model("").empty());
    CHECK(normalize_machine_model(normalize_machine_model("lava")) == "Snapmaker U1");
}
