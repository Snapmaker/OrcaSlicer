#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/LocalesUtils.hpp"

#include <clocale>
#include <cstddef>
#include <string>

using namespace Slic3r;

namespace {
// Switches the process LC_NUMERIC to a locale that writes decimals with a comma, the way the GUI
// does for Italian, German, French..., and restores the previous one on scope exit.
class ProcessCommaNumericLocale
{
public:
    bool apply()
    {
        const char *current = std::setlocale(LC_NUMERIC, nullptr);
        m_previous = current ? current : "C";
        for (const char *name : { "de_DE.UTF-8", "it_IT.UTF-8", "fr_FR.UTF-8", "cs_CZ.UTF-8", "de_DE", "it_IT", "German" })
            if (std::setlocale(LC_NUMERIC, name) != nullptr && std::localeconv()->decimal_point[0] == ',')
                return m_applied = true;
        std::setlocale(LC_NUMERIC, m_previous.c_str());
        return false;
    }
    ~ProcessCommaNumericLocale()
    {
        if (m_applied)
            std::setlocale(LC_NUMERIC, m_previous.c_str());
    }

private:
    std::string m_previous;
    bool        m_applied = false;
};
} // namespace

TEST_CASE("strtod_decimal_point parses what strtod parses in the C locale", "[LocalesUtils]")
{
    const auto [text, value, length] = GENERATE(table<const char *, double, size_t>({
        { "5", 5., 1 },
        { "  12.5 7", 12.5, 6 },
        { "\t+3", 3., 3 },
        { "-1.25", -1.25, 5 },
        { "1e2", 100., 3 },
        { ".5", 0.5, 2 },
        { "0,5", 0., 1 },
        { "+-5", 0., 0 },
        { "", 0., 0 },
        { "abc", 0., 0 },
    }));
    INFO(text);
    char *end = nullptr;
    CHECK_THAT(strtod_decimal_point(text, &end), Catch::Matchers::WithinAbs(value, 1e-12));
    CHECK(size_t(end - text) == length);
}

TEST_CASE("strtod_decimal_point reads a decimal point when the process locale uses a comma", "[LocalesUtils]")
{
    // On macOS 13.3 to 14.0 strtod follows the process locale even inside CNumericLocalesSetter (#1249),
    // so the OBJ/MTL parser must not depend on it.
    ProcessCommaNumericLocale comma;
    if (!comma.apply())
        SKIP("no locale with a comma decimal separator is installed");
    const char *text = "0.5 1";
    char *end = nullptr;
    CHECK_THAT(strtod_decimal_point(text, &end), Catch::Matchers::WithinAbs(0.5, 1e-12));
    CHECK(end == text + 3);
}
