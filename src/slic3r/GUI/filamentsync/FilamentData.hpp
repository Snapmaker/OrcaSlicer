#pragma once

#include <cstdint>
#include <functional>
#include <string>

struct FilamentData
{
    unsigned int m_index   = 0;
    std::string  m_name;
    uint8_t      m_color_r = 0;
    uint8_t      m_color_g = 0;
    uint8_t      m_color_b = 0;
};

using FilamentInfoCallback = std::function<void(const FilamentData& data)>;
