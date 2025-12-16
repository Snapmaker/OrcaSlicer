#include "bury_point.hpp"
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

std::string get_timestamp_seconds()
{
    auto now = std::chrono::system_clock::now();

    auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

    std::ostringstream oss;
    oss << timestamp;
    auto strTime = oss.str();

    return strTime;
}