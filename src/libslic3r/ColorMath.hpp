// ColorMath: 颜色空间纯数学（Lab/XYZ/RGB/HSV 变换与 DeltaE 色差）。
// 阶段 B1 自 GUI/Utils/ColorSpaceConvert 抽出至引擎层，
// 消除 libslic3r/FlushVolCalc.cpp 对 GUI 头的包含穿透（全局作用域，保持既有调用方式）。
#ifndef slic3r_ColorMath_hpp_
#define slic3r_ColorMath_hpp_

#include <string>
#include <tuple>

const int CUSTOM_COLOR_COUNT = 16;

std::tuple<int, int, int> rgb_to_yuv(float r, float g, float b);
double PivotRGB(double n);
double PivotXYZ(double n);
void XYZ2RGB(float X, float Y, float Z, float* R, float* G, float* B);
void Lab2XYZ(float L, float a, float b, float* X, float* Y, float* Z);
void Lab2RGB(float L, float a, float b, float* R, float* G, float* B);
void RGB2XYZ(float R, float G, float B, float* X, float* Y, float* Z);
void XYZ2Lab(float X, float Y, float Z, float* L, float* a, float* b);
void RGB2Lab(float R, float G, float B, float* L, float* a, float* b);
void RGB2HSV(float r, float g, float b, float* h, float* s, float* v);

float DeltaE00(float l1, float a1, float b1, float l2, float a2, float b2);
float DeltaE94(float l1, float a1, float b1, float l2, float a2, float b2);
float DeltaE76(float l1, float a1, float b1, float l2, float a2, float b2);

#endif /* slic3r_ColorMath_hpp_ */
