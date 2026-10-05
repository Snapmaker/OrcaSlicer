// The saved-printer table the Bambu LAN list and the FlashForge Device tab share
// (AppConfig::m_local_machines). It holds both vendors' rows, told apart by the product id only
// FlashForge writes (BBLocalMachine::dev_pid).
//
// What this pins: the FlashForge grid used to list every saved Bambu printer as an Offline tile
// and never showed a FlashForge printer the user had added. Nothing here touches a printer, the
// network or the real config file.

#include <catch2/catch.hpp>

#include <algorithm>

#include "libslic3r/AppConfig.hpp"

using namespace Slic3r;

namespace {

bool has_id(const AppConfig::LocalMacInfo& list, const std::string& id)
{
    return std::any_of(list.begin(), list.end(), [&](const AppConfig::MacInfoMap& m) {
        auto it = m.find("dev_id");
        return it != m.end() && it->second == id;
    });
}

} // namespace

TEST_CASE("the FlashForge list leaves out saved Bambu LAN printers", "[FlashForgeDevices]")
{
    AppConfig config;

    // What the Bambu Device tab writes for a LAN-only printer: no product id.
    BBLocalMachine bambu;
    bambu.dev_id       = "01P00A351900006";
    bambu.dev_name     = "Zelda";
    bambu.dev_ip       = "192.168.1.20";
    bambu.printer_type = "C11";
    config.update_local_machine(bambu);

    config.save_bind_machine_to_config("SNFF12345", "Creator 5", "", 25, true, "192.168.1.50", 8898);

    AppConfig::LocalMacInfo list;
    config.get_local_mahcines(list);
    REQUIRE(list.size() == 1);
    CHECK(has_id(list, "SNFF12345"));
    CHECK_FALSE(has_id(list, "01P00A351900006"));

    // The address and port are kept, so the tab can reconnect without a scan.
    CHECK(list[0].at("dev_ip") == "192.168.1.50");
    CHECK(list[0].at("dev_port") == "8898");
    CHECK(list[0].at("dev_pid") == "25");

    // Both rows are still in the shared table; the Bambu side sees its own and skips ours.
    const auto& all = config.get_local_machines();
    CHECK(all.size() == 2);
    CHECK_FALSE(all.at("01P00A351900006").is_flashforge());
    CHECK(all.at("SNFF12345").is_flashforge());
}

TEST_CASE("saving a FlashForge printer again keeps its address unless a new one is given", "[FlashForgeDevices]")
{
    AppConfig config;
    config.save_bind_machine_to_config("SNFF1", "Creator 5", "Bench", 25, true, "10.0.0.7", 8898);

    // The name changes (the printer was renamed); no address is passed.
    config.save_bind_machine_to_config("SNFF1", "Creator 5 Pro", "", 25, false);
    BBLocalMachine saved = config.get_local_machines().at("SNFF1");
    CHECK(saved.dev_name == "Creator 5 Pro");
    CHECK(saved.dev_ip == "10.0.0.7");
    CHECK(saved.dev_port == "8898");
    CHECK(saved.dev_placement == "Bench");

    // A new address replaces it.
    config.save_bind_machine_to_config("SNFF1", "Creator 5 Pro", "", 25, false, "10.0.0.9", 8898);
    CHECK(config.get_local_machines().at("SNFF1").dev_ip == "10.0.0.9");
}

TEST_CASE("a product id of zero is still a FlashForge printer", "[FlashForgeDevices]")
{
    // fnet reports pid 0 for a printer whose model it does not know; the row is still FlashForge's.
    AppConfig config;
    config.save_bind_machine_to_config("SNFF0", "Unknown model", "", 0, true, "10.0.0.8", 8898);
    AppConfig::LocalMacInfo list;
    config.get_local_mahcines(list);
    CHECK(has_id(list, "SNFF0"));
}
