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

long long get_time_timestamp()
{
    auto now = std::chrono::system_clock::now();

    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    return timestamp;
}

std::string get_works_time(const uint64_t& timestamp)
{ 
    uint64_t hours         = timestamp / 3600000;
    uint64_t remaining_ms = timestamp % 3600000;

    uint64_t minutes = remaining_ms / 60000;
    remaining_ms     = remaining_ms % 60000;

    uint64_t seconds = remaining_ms / 1000;
    uint64_t ms      = remaining_ms % 1000;

    char buffer[32] = {0};
    std::snprintf(buffer, sizeof(buffer), "%02llu:%02llu:%02llu.%03llu", hours, minutes, seconds, ms);


    std::string works_time = std::string(buffer);
    
    return works_time;
}