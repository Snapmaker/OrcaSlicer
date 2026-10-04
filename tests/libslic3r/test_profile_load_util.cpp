#include <catch2/catch.hpp>
#include <atomic>
#include <clocale>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

#ifdef _WIN32
#include <locale.h>
#endif

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

// Header under test lives in the GUI layer but has no GUI/wx dependencies -
// only nlohmann::json + TBB, both available transitively via libslic3r.
#include "../../src/slic3r/GUI/ProfileLoadUtil.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace nlohmann;
using namespace Slic3r::GUI;

// Catch2 v2 assertions are NOT thread-safe. The `work` lambdas here run on TBB
// workers, so they only WRITE their slot (never assert); every check runs on
// the main thread after parallel_load_items / load_section returns (both block
// until the parallel pass completes, and load_section's merge loop runs on the
// calling thread).

TEST_CASE("parallel_load_items fills every slot indexed by n", "[ProfileLoadUtil]")
{
    const int n = 64;
    std::atomic<bool> destroy{false};
    auto slots = parallel_load_items(n, destroy, [](int i, json &slot) {
        slot = i;
    });
    REQUIRE((int) slots.size() == n);
    for (int i = 0; i < n; ++i) {
        REQUIRE(slots[i].is_number_integer());
        REQUIRE(slots[i].get<int>() == i);
    }
}

TEST_CASE("parallel_load_items leaves skipped slots null", "[ProfileLoadUtil]")
{
    const int n = 32;
    std::atomic<bool> destroy{false};
    auto slots = parallel_load_items(n, destroy, [](int i, json &slot) {
        if (i % 2 == 0) return;  // even indices skipped (slot stays null)
        slot = i;
    });
    REQUIRE((int) slots.size() == n);
    for (int i = 0; i < n; ++i) {
        if (i % 2 == 0) {
            REQUIRE(slots[i].is_null());
        } else {
            REQUIRE(slots[i].get<int>() == i);
        }
    }
}

TEST_CASE("parallel_load_items handles an empty range", "[ProfileLoadUtil]")
{
    std::atomic<bool> destroy{false};
    auto slots = parallel_load_items(0, destroy, [](int, json &) {
        FAIL("work must not run for an empty range");
    });
    REQUIRE(slots.empty());
}

TEST_CASE("parallel_load_items with destroy already set writes no slot", "[ProfileLoadUtil]")
{
    // destroy is checked before work() each iteration, so a flag that is already
    // true makes every worker bail without writing. This is the dialog-torn-down
    // teardown path.
    const int n = 16;
    std::atomic<bool> destroy{true};
    auto slots = parallel_load_items(n, destroy, [](int i, json &slot) {
        slot = i;
    });
    REQUIRE((int) slots.size() == n);
    for (int i = 0; i < n; ++i)
        REQUIRE(slots[i].is_null());
}

TEST_CASE("load_section merges non-null slots in list order", "[ProfileLoadUtil]")
{
    const int n = 48;
    std::vector<int> collected;
    std::atomic<bool> destroy{false};
    load_section(n, destroy,
        [](int i, json &slot) { slot = i; },
        [&](json &item) { collected.push_back(item.get<int>()); });

    REQUIRE((int) collected.size() == n);
    for (int i = 0; i < n; ++i)
        REQUIRE(collected[i] == i);
}

TEST_CASE("load_section skips null slots and keeps the rest in order", "[ProfileLoadUtil]")
{
    const int n = 40;
    int expected = 0;
    for (int i = 0; i < n; ++i)
        if (i % 3 != 0) ++expected;

    std::vector<int> collected;
    std::atomic<bool> destroy{false};
    load_section(n, destroy,
        [](int i, json &slot) { if (i % 3 != 0) slot = i; },  // drop multiples of 3
        [&](json &item) { collected.push_back(item.get<int>()); });

    REQUIRE((int) collected.size() == expected);
    for (int v : collected)
        REQUIRE(v % 3 != 0);
    for (size_t i = 1; i < collected.size(); ++i)
        REQUIRE(collected[i] > collected[i - 1]);
}

TEST_CASE("load_section performs no merge when destroy is set", "[ProfileLoadUtil]")
{
    std::vector<int> collected;
    std::atomic<bool> destroy{true};
    load_section(16, destroy,
        [](int i, json &slot) { slot = i; },
        [&](json &item) { collected.push_back(item.get<int>()); });
    REQUIRE(collected.empty());
}

TEST_CASE("load_section handles an empty range", "[ProfileLoadUtil]")
{
    std::vector<int> collected;
    std::atomic<bool> destroy{false};
    load_section(0, destroy,
        [](int, json &) { FAIL("work must not run for an empty range"); },
        [&](json &)      { FAIL("merge must not run for an empty range"); });
    REQUIRE(collected.empty());
}

TEST_CASE("worker pattern: malformed JSON drops only itself, no throw", "[ProfileLoadUtil]")
{
    // Mirrors the no-throw contract used in LoadProfileFamily: parse with the
    // exception-free overload, leave the slot null on failure, and load_section
    // skips it - the rest of the family is unaffected and nothing is thrown.
    const std::vector<std::string> inputs = {"1", "2", "{ broken", "3", "4"};
    std::vector<int> collected;
    std::atomic<bool> destroy{false};
    load_section((int) inputs.size(), destroy,
        [&](int i, json &slot) {
            json j = json::parse(inputs[i], nullptr, false);
            if (j.is_discarded()) return;  // malformed -> drop only this item
            slot = j;
        },
        [&](json &item) { collected.push_back(item.get<int>()); });

    std::vector<int> expected = {1, 2, 3, 4};  // index 2 dropped
    REQUIRE(collected == expected);
}

// ascii_iequals lives as a file-static helper in Config.cpp (Orca #15943 Stage A).
// These cases drive it through load_from_json: mixed-case meta keys must still land
// under the canonical BBL_JSON_KEY_* names, and mixed-case include-dir / template
// meta keys must still resolve the same way as the old boost::iequals path.

namespace {

struct TempTree
{
    boost::filesystem::path root;

    explicit TempTree(const std::string &tag)
        : root(boost::filesystem::temp_directory_path() / "snorca_tests" /
               boost::filesystem::unique_path(tag + "_%%%%%%%%"))
    {
        boost::filesystem::create_directories(root);
    }
    ~TempTree()
    {
        boost::system::error_code ec;
        boost::filesystem::remove_all(root, ec);
    }

    boost::filesystem::path write(const std::string &rel, const std::string &body) const
    {
        const boost::filesystem::path path = root / rel;
        boost::filesystem::create_directories(path.parent_path());
        boost::nowide::ofstream ofs(path.string());
        ofs << body;
        ofs.close();
        return path;
    }
};

int load_json(Slic3r::DynamicPrintConfig &config,
              const boost::filesystem::path &path,
              bool load_inherits_to_config,
              std::map<std::string, std::string> &key_values,
              std::string &reason)
{
    Slic3r::ConfigSubstitutionContext ctxt(Slic3r::ForwardCompatibilitySubstitutionRule::Enable);
    return config.load_from_json(path.string(), ctxt, load_inherits_to_config, key_values, reason);
}

// Install a comma-decimal numeric locale when the host has one, so nested-setter
// restore is observable. active is false when no such locale exists (CI images
// often ship only C/POSIX); the restore assertion then just checks the original
// is_decimal_separator_point() value. Do not generate locales via localedef.
struct CommaNumericLocale
{
    bool        active = false;
    std::string previous;
#ifdef _WIN32
    int previous_threadlocale = 0;
#endif

    CommaNumericLocale()
    {
#ifdef _WIN32
        // Enable per-thread locale before setlocale so a comma locale cannot leak
        // into the process-wide LC_NUMERIC for later tests.
        previous_threadlocale = _configthreadlocale(0);
        _configthreadlocale(_ENABLE_PER_THREAD_LOCALE);
#endif
        const char *cur = std::setlocale(LC_NUMERIC, nullptr);
        previous        = cur ? cur : "C";
#ifdef _WIN32
        static const char *const names[] = {"de-DE", "German", "fr-FR", "French"};
#else
        static const char *const names[] = {"de_DE.UTF-8", "de_DE", "fr_FR.UTF-8", "fr_FR", "nl_NL.UTF-8"};
#endif
        for (const char *name : names) {
            if (std::setlocale(LC_NUMERIC, name) == nullptr)
                continue;
            if (!Slic3r::is_decimal_separator_point()) {
                active = true;
                return;
            }
        }
        std::setlocale(LC_NUMERIC, previous.c_str());
    }
    ~CommaNumericLocale()
    {
        std::setlocale(LC_NUMERIC, previous.c_str());
#ifdef _WIN32
        _configthreadlocale(previous_threadlocale);
#endif
    }
};

} // namespace

TEST_CASE("mixed-case JSON meta keys are recognized as canonical keys", "[Config][ascii_iequals]")
{
    TempTree tree("ascii_iequals_meta");
    const auto path = tree.write("child.json", R"({
    "Type": "filament",
    "Name": "Mixed Case Child",
    "From": "user",
    "INHERITS": "SomeParent",
    "VERSION": "1.2.3",
    "INSTANTIATION": "true",
    "RENAMED_FROM": "Old Name",
    "filament_flow_ratio": ["0.88"]
})");

    Slic3r::DynamicPrintConfig config;
    std::map<std::string, std::string> key_values;
    std::string reason;
    const int ret = load_json(config, path, false, key_values, reason);

    REQUIRE(ret == 0);
    REQUIRE(reason.empty());
    REQUIRE(key_values[BBL_JSON_KEY_NAME] == "Mixed Case Child");
    REQUIRE(key_values[BBL_JSON_KEY_INHERITS] == "SomeParent");
    REQUIRE(key_values[BBL_JSON_KEY_TYPE] == "filament");
    REQUIRE(key_values[BBL_JSON_KEY_FROM] == "user");
    REQUIRE(key_values[BBL_JSON_KEY_VERSION] == "1.2.3");
    REQUIRE(key_values[BBL_JSON_KEY_INSTANTIATION] == "true");
    REQUIRE(key_values[ORCA_JSON_KEY_RENAMED_FROM] == "Old Name");
    REQUIRE(key_values.count("Name") == 0);
    REQUIRE(key_values.count("INHERITS") == 0);
    REQUIRE(config.has("filament_flow_ratio"));
    CHECK(config.opt_serialize("filament_flow_ratio") == "0.88");
}

TEST_CASE("a longer key is not treated as a meta-key prefix", "[Config][ascii_iequals]")
{
    // ascii_iequals must reject "names" vs "name": a prefix match would stash
    // this under BBL_JSON_KEY_NAME and drop it as a setting.
    TempTree tree("ascii_iequals_prefix");
    const auto path = tree.write("child.json", R"({
    "type": "filament",
    "name": "Prefix Child",
    "names": "should-not-match-name",
    "filament_flow_ratio": ["0.91"]
})");

    Slic3r::DynamicPrintConfig config;
    std::map<std::string, std::string> key_values;
    std::string reason;
    const int ret = load_json(config, path, false, key_values, reason);

    REQUIRE(ret == 0);
    REQUIRE(key_values[BBL_JSON_KEY_NAME] == "Prefix Child");
    REQUIRE_FALSE(key_values[BBL_JSON_KEY_NAME] == "should-not-match-name");
}

TEST_CASE("mixed-case include directory and template meta keys still resolve", "[Config][ascii_iequals]")
{
    TempTree tree("ascii_iequals_include");
    tree.write("Filament/template.json", R"({
    "Type": "filament",
    "Name": "test_template",
    "INSTANTIATION": "false",
    "FROM": "system",
    "filament_max_volumetric_speed": ["12", "12"],
    "nozzle_temperature": ["250", "250"],
    "filament_flow_ratio": ["0.98", "0.98"]
})");
    const auto child = tree.write("Filament/Sub/child.json", R"({
    "type": "filament",
    "Name": "child",
    "include": ["template"],
    "filament_flow_ratio": ["0.88", "0.88"]
})");

    Slic3r::DynamicPrintConfig config;
    std::map<std::string, std::string> key_values;
    std::string reason;
    const int ret = load_json(config, child, true, key_values, reason);

    REQUIRE(ret == 0);
    REQUIRE(key_values[BBL_JSON_KEY_NAME] == "child");
    REQUIRE(config.has("filament_max_volumetric_speed"));
    CHECK(config.opt_serialize("filament_max_volumetric_speed") == "12,12");
    REQUIRE(config.has("nozzle_temperature"));
    CHECK(config.opt_serialize("nozzle_temperature") == "250,250");
    REQUIRE(config.has("filament_flow_ratio"));
    CHECK(config.opt_serialize("filament_flow_ratio") == "0.88,0.88");
    // instantiation / from are template-only meta keys, not ConfigOptions.
    REQUIRE(key_values.count(BBL_JSON_KEY_INSTANTIATION) == 0);
    REQUIRE(key_values.count(BBL_JSON_KEY_FROM) == 0);
}

TEST_CASE("nested CNumericLocalesSetter keeps a C decimal point and restores after", "[LocalesUtils]")
{
    STATIC_REQUIRE_FALSE(std::is_copy_constructible<Slic3r::CNumericLocalesSetter>::value);
    STATIC_REQUIRE_FALSE(std::is_copy_assignable<Slic3r::CNumericLocalesSetter>::value);

    CommaNumericLocale comma;
    const bool point_before = Slic3r::is_decimal_separator_point();
    if (!comma.active)
        WARN("no comma-decimal locale installed; restore is checked against the original separator only");
    else
        REQUIRE_FALSE(point_before);

    Slic3r::reset_numeric_locale_setter_counts();
    {
        Slic3r::CNumericLocalesSetter outer;
        REQUIRE(Slic3r::is_decimal_separator_point());
        {
            Slic3r::CNumericLocalesSetter inner;
            REQUIRE(Slic3r::is_decimal_separator_point());
            {
                Slic3r::CNumericLocalesSetter inner2;
                REQUIRE(Slic3r::is_decimal_separator_point());
                REQUIRE(Slic3r::numeric_locale_setter_installs() == 1);
                REQUIRE(Slic3r::numeric_locale_setter_nested_skips() == 2);
            }
            // Inner destructors must not restore the pre-outer locale.
            REQUIRE(Slic3r::is_decimal_separator_point());
        }
        REQUIRE(Slic3r::is_decimal_separator_point());
    }

    REQUIRE(Slic3r::is_decimal_separator_point() == point_before);
    REQUIRE(Slic3r::numeric_locale_setter_installs() == 1);
    REQUIRE(Slic3r::numeric_locale_setter_nested_skips() == 2);
}
