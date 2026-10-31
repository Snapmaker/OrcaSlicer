#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <iterator>
#include <string>

namespace {

std::string read_flutter_bundle()
{
    std::ifstream bundle(GATEWAY_FLUTTER_BUNDLE, std::ios::binary);
    REQUIRE(bundle.good());
    return std::string{std::istreambuf_iterator<char>{bundle}, {}};
}

} // namespace

TEST_CASE("shipped Flutter bundle does not use removed device bridge commands", "[gateway][flutter]")
{
    const std::string bundle = read_flutter_bundle();
    for (const char* command : {
             "sw_SystemGetDeviceInfo",
             "sw_GetMachineState",
             "sw_SubscribeMachineState",
             "sw_GetMachineObjects",
             "sw_SetSubscribeFilter",
             "sw_StopMachineStateSubscription",
             "sw_GetPrinterInfo",
             "sw_GetMachineSystemInfo",
         })
        CHECK(bundle.find(command) == std::string::npos);
}
