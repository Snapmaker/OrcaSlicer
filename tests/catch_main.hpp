#ifndef CATCH_MAIN
#define CATCH_MAIN

// Catch2 v3 — main() is provided by Catch2::Catch2WithMain (linked via
// test_common → Catch2::Catch2WithMain in tests/CMakeLists.txt).
//
// This header exists as a convenience include for all test entry points.
// The custom VerboseConsoleReporter has been removed because Catch2 v3
// makes ConsoleReporter final; the default console reporter is used instead.
//
// If richer output is needed, use Catch2 v3 listener hooks or pass
// --verbosity high on the command line.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace Catch {
using Matchers::Equals;
} // namespace Catch

namespace Catch::Matchers {
template<typename Target, typename Epsilon>
inline WithinRelMatcher WithinRel(Target target, Epsilon eps)
{
    return WithinRel(static_cast<double>(target), static_cast<double>(eps));
}
} // namespace Catch::Matchers

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using Catch::Matchers::WithinULP;

#endif // CATCH_MAIN
