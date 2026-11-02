#ifndef slic3r_Utils_ColorSpaceConvert_hpp_
#define slic3r_Utils_ColorSpaceConvert_hpp_

// 颜色空间转换的 GUI 层封装。纯数学已抽至引擎层 libslic3r/ColorMath.hpp
// （阶段 B1），本头经包含其向既有使用方转发声明；wxColour 相关包装保留在此。

#include <string>

#include "libslic3r/ColorMath.hpp"

class wxColour;
std::string color_to_string(const wxColour &color);
wxColour    string_to_wxColor(const std::string &str);
#endif /* slic3r_Utils_ColorSpaceConvert_hpp_ */
