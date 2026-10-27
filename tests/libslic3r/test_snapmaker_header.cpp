#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "libslic3r/GCode/SnapmakerHeader.hpp"

#include <limits>
#include <memory>
#include <string>

using namespace Slic3r;

TEST_CASE("Snapmaker 2.0 header selection excludes other printers", "[SnapmakerHeader]")
{
    for (const auto model : {"Snapmaker A250", "Snapmaker A350", "Snapmaker A250 Dual", "Snapmaker A350 Dual QS+B Kit"})
        CHECK(SnapmakerHeader::is_snapmaker_2(model));
    for (const auto model : {"", "Snapmaker Artisan", "Snapmaker J1", "Snapmaker U1", "Snapmaker A3500", "Other A350"})
        CHECK_FALSE(SnapmakerHeader::is_snapmaker_2(model));
}

TEST_CASE("Snapmaker line count updates preserve file size and line offsets", "[SnapmakerHeader]")
{
    std::unique_ptr<std::FILE, decltype(&std::fclose)> file(std::tmpfile(), &std::fclose);
    REQUIRE(file != nullptr);
    const std::string original = std::string(SnapmakerHeader::START) + ";Header End\nG28\n";
    REQUIRE(std::fwrite(original.data(), 1, original.size(), file.get()) == original.size());

    const size_t count = GENERATE(size_t(0), size_t(123456), std::numeric_limits<size_t>::max());
    REQUIRE(SnapmakerHeader::update_line_count(file.get(), count));
    REQUIRE(std::fseek(file.get(), 0, SEEK_END) == 0);
    CHECK(std::ftell(file.get()) == static_cast<long>(original.size()));
    REQUIRE(std::fseek(file.get(), 0, SEEK_SET) == 0);
    std::string updated(original.size(), '\0');
    REQUIRE(std::fread(updated.data(), 1, updated.size(), file.get()) == updated.size());

    const size_t offset = sizeof(SnapmakerHeader::LINE_COUNT_PREFIX) - 1;
    CHECK(updated.substr(0, offset) == SnapmakerHeader::LINE_COUNT_PREFIX);
    CHECK(std::stoull(updated.substr(offset, SnapmakerHeader::LINE_COUNT_WIDTH)) == count);
    CHECK(updated.substr(offset + SnapmakerHeader::LINE_COUNT_WIDTH) == original.substr(offset + SnapmakerHeader::LINE_COUNT_WIDTH));
}
