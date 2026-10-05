#ifndef slic3r_LocalesUtils_hpp_
#define slic3r_LocalesUtils_hpp_

#include <string>
#include <clocale>
#include <iomanip>
#include <cassert>
#include <string_view>

#ifdef __APPLE__
#include <xlocale.h>
#endif

namespace Slic3r {

// RAII wrapper that sets LC_NUMERIC to "C" on construction
// and restores the old value on destruction.
class CNumericLocalesSetter {
public:
    CNumericLocalesSetter();
    ~CNumericLocalesSetter();
    // A copy would restore the locale twice, and count down once more than up.
    CNumericLocalesSetter(const CNumericLocalesSetter &) = delete;
    CNumericLocalesSetter &operator=(const CNumericLocalesSetter &) = delete;

private:
    // Inside another setter on this thread, which does the setting and restoring.
    bool m_nested{false};
#ifdef _WIN32
    std::string m_orig_numeric_locale;
#else
    locale_t m_original_locale;
    locale_t m_new_locale;
#endif

};

// Diagnostics / test-only: this-thread counts of actual setlocale/uselocale vs
// nested skips. Not a production API; Catch2 uses them to prove inner setters
// skipped setlocale. Do not call from slicer paths.
void reset_numeric_locale_setter_counts();
int  numeric_locale_setter_installs();
int  numeric_locale_setter_nested_skips();

// A function to check that current C locale uses decimal point as a separator.
// Intended mostly for asserts.
bool is_decimal_separator_point();


// A substitute for std::to_string that works according to
// C++ locales, not C locale. Meant to be used when we need
// to be sure that decimal point is used as a separator.
// (We use user C locales and "C" C++ locales in most of the code.)
std::string float_to_string_decimal_point(double value, int precision = -1);
//std::string float_to_string_decimal_point(float value,  int precision = -1);
double string_to_double_decimal_point(const std::string_view str, size_t* pos = nullptr);
// Parses like atof in the C locale, skipping leading whitespace and a '+',
// without the C runtime's per-call locale lookup. Like atof it consumes the leading number only
// ("12.5;x" -> 12.5) and returns 0 when the text does not start with a number ("", "abc", "G4 P1").
// Differs from atof only for hex floats, which G-code never contains.
double atof_decimal_point(std::string_view str);

} // namespace Slic3r

#endif // slic3r_LocalesUtils_hpp_
