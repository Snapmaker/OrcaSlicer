#include "LocalesUtils.hpp"

#include <charconv>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <system_error>

#include <fast_float/fast_float.h>

// Defined where the floating point std::to_chars can be called, which with Apple's libc++ runtime is from macOS 13.3.
#if defined(_LIBCPP_VERSION)
    #if defined(_LIBCPP_AVAILABILITY_HAS_TO_CHARS_FLOATING_POINT) && _LIBCPP_AVAILABILITY_HAS_TO_CHARS_FLOATING_POINT
        #define SLIC3R_FLOAT_TO_CHARS
    #endif
#elif defined(_WIN32) || defined(__cpp_lib_to_chars)
    #define SLIC3R_FLOAT_TO_CHARS
#endif


namespace Slic3r {


// How many setters this thread holds, so the ones nested in another can skip
// setlocale, which takes a lock the whole process shares on Windows.
static thread_local int s_numeric_locale_depth         = 0;
static thread_local int s_numeric_locale_installs      = 0;
static thread_local int s_numeric_locale_nested_skips  = 0;

// Diagnostics / test-only counters (see LocalesUtils.hpp). Not used by slicer paths.
void reset_numeric_locale_setter_counts()
{
    s_numeric_locale_installs     = 0;
    s_numeric_locale_nested_skips = 0;
}

int numeric_locale_setter_installs() { return s_numeric_locale_installs; }

int numeric_locale_setter_nested_skips() { return s_numeric_locale_nested_skips; }

CNumericLocalesSetter::CNumericLocalesSetter()
{
    // Nested in another setter on this thread, whose "C" the separator check
    // confirms is still set.
    if (s_numeric_locale_depth > 0 && is_decimal_separator_point()) {
        m_nested = true;
        ++s_numeric_locale_depth;
        ++s_numeric_locale_nested_skips;
        return;
    }
#ifdef _WIN32
    _configthreadlocale(_ENABLE_PER_THREAD_LOCALE);
    m_orig_numeric_locale = std::setlocale(LC_NUMERIC, nullptr);
    std::setlocale(LC_NUMERIC, "C");
#elif __APPLE__
    m_original_locale = uselocale((locale_t) 0);
    m_new_locale      = newlocale(LC_NUMERIC_MASK, "C", m_original_locale);
    uselocale(m_new_locale);
#else // linux / BSD
    m_original_locale = uselocale((locale_t) 0);
    m_new_locale      = duplocale(m_original_locale);
    m_new_locale      = newlocale(LC_NUMERIC_MASK, "C", m_new_locale);
    uselocale(m_new_locale);
#endif
    // Counted last, since the destructor does not run for a constructor that throws.
    ++s_numeric_locale_depth;
    ++s_numeric_locale_installs;
}



CNumericLocalesSetter::~CNumericLocalesSetter()
{
    --s_numeric_locale_depth;
    if (m_nested)
        return;
#ifdef _WIN32
    std::setlocale(LC_NUMERIC, m_orig_numeric_locale.data());
#else
    uselocale(m_original_locale);
    freelocale(m_new_locale);
#endif
}



bool is_decimal_separator_point()
{
    char str[5] = "";
    sprintf(str, "%.1f", 0.5f);
    return str[1] == '.';
}


double string_to_double_decimal_point(const std::string_view str, size_t* pos /* = nullptr*/)
{
    double out;
    size_t p = fast_float::from_chars(str.data(), str.data() + str.size(), out).ptr - str.data();
    if (pos)
        *pos = p;
    return out;
}

double atof_decimal_point(std::string_view str)
{
    size_t i = 0;
    while (i < str.size() && (str[i] == ' ' || (str[i] >= '\t' && str[i] <= '\r')))
        ++i;
    if (i < str.size() && str[i] == '+') {
        ++i;
        if (i < str.size() && str[i] == '-')
            return 0.;
    }
    return string_to_double_decimal_point(str.substr(i));
}

std::string float_to_string_decimal_point(double value, int precision/* = -1*/)
{
    // Every branch prints the same digits in the classic locale as the stream at the end, which takes over when a branch
    // is compiled out or the value is too long for the buffer.
#if defined(SLIC3R_FLOAT_TO_CHARS)
    constexpr size_t SIZE = 20;
    char out[SIZE] = "";
    std::to_chars_result res;
    if (precision >=0)
        res = std::to_chars(out, out+SIZE, value, std::chars_format::fixed, precision);
    else
        res = std::to_chars(out, out+SIZE, value, std::chars_format::general, 6);
    if (res.ec == std::errc())
        return std::string(out, res.ptr - out);
#elif defined(__APPLE__)
    // Formats in the C locale, as libc++'s stream does, switching only this thread's locale for the call.
    static const locale_t c_locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t) 0);
    if (c_locale != (locale_t) 0) {
        constexpr size_t SIZE = 20;
        char           out[SIZE];
        const locale_t previous = uselocale(c_locale);
        const int      length   = precision >= 0 ? snprintf(out, SIZE, "%.*f", precision, value) : snprintf(out, SIZE, "%.*g", 6, value);
        uselocale(previous);
        if (length >= 0 && size_t(length) < SIZE)
            return std::string(out, length);
    }
#endif
    std::stringstream buf;
    if (precision >= 0)
        buf << std::fixed << std::setprecision(precision);
    buf << value;
    return buf.str();
}


} // namespace Slic3r

