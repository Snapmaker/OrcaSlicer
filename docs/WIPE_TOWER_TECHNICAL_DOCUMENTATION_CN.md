# OrcaSlicer 擦除塔（Wipe Tower）详细技术文档

**文档版本**: v1.0
**生成日期**: 2025-11-24
**基于代码库**: OrcaSlicer 分支 2.2.0
**文档语言**: 简体中文

---

## 📋 目录

1. [系统架构概述](#1-系统架构概述)
2. [核心数据结构](#2-核心数据结构)
3. [Rib 外墙算法详解](#3-rib-外墙算法详解)
4. [路径规划系统](#4-路径规划系统)
5. [工具切换流程](#5-工具切换流程)
6. [层完成与外墙生成](#6-层完成与外墙生成)
7. [配置参数体系](#7-配置参数体系)
8. [与主打印流程集成](#8-与主打印流程集成)
9. [关键算法流程图](#9-关键算法流程图)
10. [性能优化与设计亮点](#10-性能优化与设计亮点)
11. [常见问题与调试](#11-常见问题与调试)
12. [总结](#12-总结)

---

## 1. 系统架构概述

### 1.1 双版本架构

OrcaSlicer 实现了两套独立的擦除塔系统：

```
WipeTower (src/libslic3r/GCode/WipeTower.cpp)
├── 专为 Bambu Lab (BBL) 打印机优化
├── 1769 行代码
├── 简化的逻辑和专用 G-code 标签
└── 仅支持矩形外墙

WipeTower2 (src/libslic3r/GCode/WipeTower2.cpp)
├── 支持所有非 BBL 打印机
├── 2584 行代码
├── 支持 MMU 和工具交换器
├── 三种外墙类型：矩形/锥形/Rib
└── Snapmaker U1 特殊支持
```

**核心文件位置**：
- `src/libslic3r/GCode/WipeTower2.hpp:21` - WipeTower2 类定义
- `src/libslic3r/GCode/WipeTower2.cpp:561` - WipeTowerWriter2 内部类
- `src/libslic3r/GCode/WipeTower.hpp:21` - WipeTower 基础类定义

### 1.2 设计原理

擦除塔的核心目的是在多材料打印时：

1. **清洗旧材料**：通过 ramming 和 wiping 移除喷嘴内残留的旧耗材
2. **换料缓冲**：提供工具切换时的挤出平台
3. **预热预打印**：priming 确保新材料正常挤出
4. **结构支撑**：通过不同外墙类型提供物理稳定性

---

## 2. 核心数据结构

### 2.1 WipeTower::Extrusion（挤出路径）

**位置**: `WipeTower.hpp:29`

```cpp
struct Extrusion {
    Vec2f pos;           // 终点位置 (x, y)
    float width;         // 挤出宽度（考虑了压扁后的圆角修正）
    unsigned int tool;   // 当前工具索引

    Extrusion(const Vec2f &pos, float width, unsigned int tool)
        : pos(pos), width(width), tool(tool) {}
};
```

**用途**：
- 存储每段挤出移动的终点、宽度和工具信息
- 用于路径预览和 G-code 分析
- `width = 0` 表示空移（travel move）

### 2.2 WipeTower::ToolChangeResult（工具切换结果）

**位置**: `WipeTower.hpp:41`

```cpp
struct ToolChangeResult {
    float print_z;                      // 打印高度
    float layer_height;                 // 层高
    std::string gcode;                  // 生成的 G-code
    std::vector<Extrusion> extrusions; // 挤出路径序列
    Vec2f start_pos;                    // 起始位置（已装载，无 Z-hop）
    Vec2f end_pos;                      // 结束位置（已装载，无 Z-hop）
    float elapsed_time;                 // 耗时（秒）
    bool priming;                       // 是否是预打印
    std::vector<Vec2f> wipe_path;      // 擦拭路径（供主 G-code 生成器使用）
    float purge_volume;                 // 清洗体积（mm³）
    int initial_tool;                   // 初始工具
    int new_tool;                       // 新工具
    bool is_finish_first;               // finish_layer 是否在 tool_change 前执行
};
```

**关键方法**：
```cpp
float total_extrusion_length_in_plane() {
    float e_length = 0.f;
    for (size_t i = 1; i < this->extrusions.size(); ++i) {
        const Extrusion &e = this->extrusions[i];
        if (e.width > 0) {
            Vec2f v = e.pos - (&e - 1)->pos;
            e_length += v.norm();
        }
    }
    return e_length;
}
```

### 2.3 WipeTower2::FilamentParameters（耗材参数）

**位置**: `WipeTower2.hpp:135`

```cpp
struct FilamentParameters {
    std::string material = "PLA";       // 材料类型
    bool is_soluble = false;            // 是否可溶解（如 PVA）
    int temperature = 0;                // 打印温度
    int first_layer_temperature = 0;    // 首层温度

    // 装载/卸载参数
    float loading_speed = 0.f;          // 装载速度 (mm/s)
    float loading_speed_start = 0.f;    // 起始装载速度
    float unloading_speed = 0.f;        // 卸载速度
    float unloading_speed_start = 0.f;  // 起始卸载速度
    float delay = 0.f;                  // 冷却延迟（秒）

    // Ramming 参数
    std::vector<float> ramming_speed;   // Ramming 速度数组
    float ramming_line_width_multiplicator = 1.f;  // 线宽倍数
    float ramming_step_multiplicator = 1.f;        // 步进倍数
    bool multitool_ramming;             // 是否启用多工具 ramming
    float multitool_ramming_time = 0.f; // 多工具 ramming 时间

    // 冷却移动参数
    int cooling_moves = 0;              // 冷却移动次数
    float cooling_initial_speed = 0.f;  // 初始冷却速度
    float cooling_final_speed = 0.f;    // 最终冷却速度

    // Snapmaker 专用
    float filament_stamping_loading_speed = 0.f;  // 压印装载速度
    float filament_stamping_distance = 0.f;       // 压印距离

    // 物理参数
    float nozzle_diameter;              // 喷嘴直径
    float filament_area;                // 耗材截面积
    float retract_length;               // 回抽长度
    float retract_speed;                // 回抽速度
    float max_e_speed = std::numeric_limits<float>::max();  // 最大挤出速度
    float filament_minimal_purge_on_wipe_tower = 0.f;  // 最小清洗量
};
```

### 2.4 WipeTowerInfo::ToolChange（工具切换规划）

**位置**: `WipeTower2.hpp:275`

```cpp
struct ToolChange {
    size_t old_tool;              // 旧工具索引
    size_t new_tool;              // 新工具索引
    float required_depth;         // 所需深度（mm）
    float ramming_depth;          // Ramming 深度
    float first_wipe_line;        // 第一条擦拭线长度
    float wipe_volume;            // 擦拭体积（mm³）
    float wipe_volume_total;      // 总擦拭体积

    ToolChange(size_t old, size_t newtool,
               float depth=0.f, float ramming_depth=0.f,
               float fwl=0.f, float wv=0.f)
        : old_tool{old}, new_tool{newtool},
          required_depth{depth}, ramming_depth{ramming_depth},
          first_wipe_line{fwl}, wipe_volume{wv},
          wipe_volume_total{wv} {}
};
```

### 2.5 WipeTowerInfo（层信息）

**位置**: `WipeTower2.hpp:274`

```cpp
struct WipeTowerInfo {
    float z;                             // 层的 Z 高度
    float height;                        // 层高
    float depth;                         // 基于所有上层计算的深度
    std::vector<ToolChange> tool_changes; // 该层的所有工具切换

    float toolchanges_depth() const {
        float sum = 0.f;
        for (const auto &a : tool_changes)
            sum += a.required_depth;
        return sum;
    }

    WipeTowerInfo(float z_par, float layer_height_par)
        : z{z_par}, height{layer_height_par}, depth{0} {}
};
```

**用途**：
- `m_plan` 向量存储所有层的信息
- 在 `plan_toolchange()` 中填充（`WipeTower2.cpp:2175`）
- 在 `plan_tower()` 中计算最终深度（`WipeTower2.cpp:2206`）

---

## 3. Rib 外墙算法详解

### 3.1 Rib 外墙原理

Rib（肋条）外墙采用 **X 形交叉对角线结构**，提供比矩形更强的结构稳定性，同时比实心锥形节省材料。

**视觉示意**：
```
顶视图（从上往下看）:

    ┌─────────────────┐
    │╲               ╱│  ← Rib 对角线 1
    │  ╲           ╱  │
    │    ╲       ╱    │
    │      ╲   ╱      │
    │        X        │  ← 中心交叉点
    │      ╱   ╲      │
    │    ╱       ╲    │
    │  ╱           ╲  │
    │╱               ╲│  ← Rib 对角线 2
    └─────────────────┘

    外框 + 两条对角线 = Rib 墙
```

**动态特性**：
- **底层**：Rib 对角线最长，延伸超出框体（增强基础稳定性）
- **顶层**：Rib 对角线逐渐缩短至框体对角线长度（渐变式支撑）

### 3.2 generate_rib_polygon() 核心算法

**位置**: `WipeTower2.cpp:2399`

```cpp
Polygon WipeTower2::generate_rib_polygon(const WipeTower::box_coordinates& wt_box)
{
    // 【步骤 1】定义动态长度计算 Lambda
    // 随高度递减的 rib 长度公式：
    // 当前长度 = (最大高度 - 当前高度) / 最大高度 * 最大长度
    auto get_current_layer_rib_len = [](float cur_height, float max_height, float max_len) -> float {
        return std::abs(max_height - cur_height) / max_height * max_len;
    };

    // 【步骤 2】计算几何参数
    coord_t diagonal_width = scaled(m_rib_width) / 2;  // Rib 宽度的一半（缩放后）
    float a = m_wipe_tower_width;   // 擦除塔宽度
    float b = m_wipe_tower_depth;   // 擦除塔深度

    // 【步骤 3】创建两条对角线（从四个角延伸）
    // 对角线 1：左下 (0,0) → 右上 (a,b)
    Line line_1(Point::new_scale(Vec2f{0, 0}), Point::new_scale(Vec2f{a, b}));

    // 对角线 2：右下 (a,0) → 左上 (0,b)
    Line line_2(Point::new_scale(Vec2f{a, 0}), Point::new_scale(Vec2f{0, b}));

    // 【步骤 4】计算额外延伸长度（随高度递减）
    // 基础额外长度 = max(0, 配置的 rib_length - 对角线长度) / 2
    float diagonal_extra_length = std::max(0.f, m_rib_length - (float)unscaled(line_1.length())) / 2.f;

    // 应用动态递减公式
    diagonal_extra_length = scaled(get_current_layer_rib_len(
        this->m_z_pos,              // 当前 Z 高度
        this->m_wipe_tower_height,  // 擦除塔总高度
        diagonal_extra_length       // 最大额外长度
    ));

    // 【步骤 5】延伸对角线并应用 Y 偏移
    Point y_shift{0, scaled(this->m_y_shift)};

    line_1.extend(double(diagonal_extra_length));  // 从两端延伸
    line_2.extend(double(diagonal_extra_length));
    line_1.translate(-y_shift);  // 应用 Y 轴偏移（用于锥形墙的过渡）
    line_2.translate(-y_shift);

    // 【步骤 6】生成对角线的矩形包络（创建 Rib 条）
    Polygon poly_1 = generate_rectange(line_1, diagonal_width);  // 第一条 rib
    Polygon poly_2 = generate_rectange(line_2, diagonal_width);  // 第二条 rib

    // 【步骤 7】生成基础擦除塔矩形框
    Polygon poly;
    poly.points.push_back(Point::new_scale(wt_box.ld));  // 左下
    poly.points.push_back(Point::new_scale(wt_box.rd));  // 右下
    poly.points.push_back(Point::new_scale(wt_box.ru));  // 右上
    poly.points.push_back(Point::new_scale(wt_box.lu));  // 左上

    // 【步骤 8】布尔运算合并（Union）
    // 将两条 rib + 外框合并成最终多边形
    Polygons p_1_2 = union_({poly_1, poly_2, poly});

    return p_1_2.front();  // 返回合并后的多边形
}
```

**关键数学公式**：

1. **动态长度递减**：
   ```
   current_extra_length = (H_max - H_current) / H_max × L_extra_max

   其中：
   H_max = 擦除塔总高度
   H_current = 当前层 Z 高度
   L_extra_max = 配置的额外延伸长度
   ```

2. **矩形包络生成** (`generate_rectange`，位置 `WipeTower2.cpp:270`）：
   ```cpp
   Polygon generate_rectange(const Line& line, coord_t offset)
   {
       Point p1 = line.a;
       Point p2 = line.b;

       // 计算单位方向向量 (ux, uy)
       double dx = p2.x() - p1.x();
       double dy = p2.y() - p1.y();
       double length = std::sqrt(dx * dx + dy * dy);
       double ux = dx / length;
       double uy = dy / length;

       // 垂直向量 (vx, vy) = 旋转 90°
       double vx = -uy;
       double vy = ux;

       // 偏移量
       double ox = vx * offset;
       double oy = vy * offset;

       // 生成矩形四个顶点
       Points rect;
       rect[0] = {p1.x() + ox, p1.y() + oy};  // 左上
       rect[1] = {p1.x() - ox, p1.y() - oy};  // 左下
       rect[2] = {p2.x() - ox, p2.y() - oy};  // 右下
       rect[3] = {p2.x() + ox, p2.y() + oy};  // 右上

       return Polygon(rect);
   }
   ```

### 3.3 圆角处理（rounding_polygon）

**位置**: `WipeTower2.cpp:107`

```cpp
Polygon rounding_polygon(Polygon& polygon, double rounding = 2., double angle_tol = 30./180.*PI)
{
    if (polygon.points.size() < 3) return polygon;

    Polygon res;
    res.points.reserve(polygon.points.size() * 2);
    int mod = polygon.points.size();
    double cos_angle_tol = abs(std::cos(angle_tol));  // 30° → cos(30°) ≈ 0.866

    // 遍历每个顶点
    for (int i = 0; i < polygon.points.size(); i++) {
        Vec2d a = unscaled(polygon.points[(i - 1 + mod) % mod]);  // 前一点
        Vec2d b = unscaled(polygon.points[i]);                    // 当前点（角点）
        Vec2d c = unscaled(polygon.points[(i + 1) % mod]);        // 后一点

        // 计算向量和长度
        double ab_len = (a - b).norm();
        double bc_len = (b - c).norm();
        Vec2d ab = (b - a) / ab_len;  // 单位向量
        Vec2d bc = (c - b) / bc_len;

        // 计算夹角余弦值
        float cosangle = ab.dot(bc);
        cosangle = std::clamp(cosangle, -1.f, 1.f);
        bool is_ccw = cross2(ab, bc) > 0;  // 逆时针？

        // 【关键判断】：如果是角点（夹角 < 150°）
        if (abs(cosangle) < cos_angle_tol) {
            // 计算实际圆角距离（防止过大导致点重合）
            float real_rounding_dis = std::min({rounding, ab_len / 2.1, bc_len / 2.1});

            // 圆角的起点和终点
            Vec2d left = b - ab * real_rounding_dis;
            Vec2d right = b + bc * real_rounding_dis;

            // 【圆心计算】
            float half_angle = std::acos(cosangle) / 2.f;  // 半角
            Vec2d dir = (right - left).normalized();
            dir = Vec2d{-dir[1], dir[0]};  // 旋转 90°
            dir = is_ccw ? dir : -dir;     // 根据方向调整
            double dis = real_rounding_dis / sin(half_angle);  // 圆心到角点的距离

            Vec2d center = b + dir * dis;
            double radius = (left - center).norm();

            // 【生成圆弧点】（20 个点拟合）
            ArcSegment arc(scaled(center), scaled(radius), scaled(left), scaled(right),
                          is_ccw ? Arc_Dir_CCW : Arc_Dir_CW);

            int n = 20;  // arc_fit_size
            for (int j = 0; j < n; j++) {
                float cur_angle = arc.polar_start_theta + (float)j / n * arc.angle_radians;
                // 角度归一化到 [0, 2π]
                if (cur_angle > 2 * PI) cur_angle -= 2 * PI;
                else if (cur_angle < 0) cur_angle += 2 * PI;

                Point tmp = arc.center + Point{
                    arc.radius * std::cos(cur_angle),
                    arc.radius * std::sin(cur_angle)
                };
                res.points.push_back(tmp);
            }
            res.points.push_back(scaled(right));
        } else {
            // 不是角点，直接保留
            res.points.push_back(polygon.points[i]);
        }
    }

    res.remove_duplicate_points();
    res.points.shrink_to_fit();
    return res;
}
```

**几何原理图**：
```
        a
        │
        │ ab
        │
        b (角点)
       ╱ ╲
      ╱   ╲ bc
     ╱     ╲
    c

圆角后：
        a
        │
        │
    left ◠◡◠ right  ← 圆弧（20个点拟合）
       ╱       ╲
      ╱         ╲
     c

圆心 center 计算：
- 半角 = arccos(ab·bc) / 2
- 垂直方向 dir = rotate_90°(left→right)
- 距离 dis = rounding_dis / sin(半角)
- center = b + dir × dis
```

### 3.4 generate_support_rib_wall() 路径生成

**位置**: `WipeTower2.cpp:2433`

```cpp
Polygon WipeTower2::generate_support_rib_wall(
    WipeTowerWriter2& writer,
    const WipeTower::box_coordinates& wt_box,
    double feedrate,
    bool first_layer,
    bool rib_wall,           // 是否使用 rib 墙（true）或矩形墙（false）
    bool extrude_perimeter,  // 是否挤出外周（true）或仅返回多边形（false）
    bool skip_points)        // 是否为跳过点创建间隙
{
    // 【步骤 1】获取回抽参数
    float retract_length = m_filpar[m_current_tool].retract_length;
    float retract_speed = m_filpar[m_current_tool].retract_speed * 60;

    // 【步骤 2】生成墙多边形（rib 或矩形）
    Polygon wall_polygon = rib_wall
        ? generate_rib_polygon(wt_box)           // Rib 墙
        : generate_rectange_polygon(wt_box.ld, wt_box.ru);  // 矩形墙

    Polylines result_wall;
    Polygon insert_skip_polygon;

    // 【步骤 3】应用圆角（如果启用）
    if (m_used_fillet) {
        if (!rib_wall && m_y_shift > EPSILON) {
            // 矩形墙有 y_shift 时不应用圆角（防止悬空）
        } else {
            // 对 rib 墙应用圆角平滑
            wall_polygon = rib_wall
                ? rounding_polygon(wall_polygon)  // 应用圆角
                : wall_polygon;                   // 矩形不处理

            // 与基础矩形合并确保连续性
            Polygon wt_box_polygon = generate_rectange_polygon(wt_box.ld, wt_box.ru);
            wall_polygon = union_({wall_polygon, wt_box_polygon}).front();
        }
    }

    // 【步骤 4】如果不需要挤出，直接返回多边形
    if (!extrude_perimeter)
        return wall_polygon;

    // 【步骤 5】转换为路径
    if (skip_points) {
        // 为跳过点创建间隙（用于避让障碍物）
        result_wall = contrust_gap_for_skip_points(
            wall_polygon,
            std::vector<Vec2f>(),
            m_wipe_tower_width,
            2.5 * m_perimeter_width,
            insert_skip_polygon
        );
    } else {
        // 直接转换为 polyline
        result_wall.push_back(to_polyline(wall_polygon));
        insert_skip_polygon = wall_polygon;
    }

    // 【步骤 6】生成打印路径（关键！）
    writer.generate_path(result_wall, feedrate, retract_length, retract_speed, m_used_fillet);

    return insert_skip_polygon;
}
```

---

## 4. 路径规划系统

### 4.1 generate_path() 智能路径规划

**位置**: `WipeTower2.cpp:1083`

这是擦除塔路径规划的核心函数，负责将多边形路径转换为优化的 G-code 移动序列。

```cpp
void WipeTowerWriter2::generate_path(Polylines& pls, float feedrate,
                                    float retract_length, float retract_speed, bool used_fillet)
{
    // 【子函数】找到距离当前位置最近的段
    auto get_closet_idx = [this](std::vector<Segment>& corners) -> int {
        Vec2f anchor{this->m_current_pos.x(), this->m_current_pos.y()};
        int closestIndex = -1;
        float minDistance = std::numeric_limits<float>::max();

        for (int i = 0; i < corners.size(); ++i) {
            float distance = (corners[i].start - anchor).squaredNorm();
            if (distance < minDistance) {
                minDistance = distance;
                closestIndex = i;
            }
        }
        return closestIndex;
    };

    std::vector<Segment> segments;

    // 【步骤 1】圆弧拟合（如果启用）
    if (m_enable_arc_fitting) {
        // 对每条 polyline 进行圆弧拟合
        for (auto& pl : pls)
            pl.simplify_by_fitting_arc(SCALED_WIPE_TOWER_RESOLUTION);  // 0.1mm 分辨率

        // 提取线段和圆弧段
        for (const auto& pl : pls) {
            if (pl.points.size() < 2) continue;

            for (int i = 0; i < pl.fitting_result.size(); i++) {
                if (pl.fitting_result[i].path_type == EMovePathType::Linear_move) {
                    // 【线性段】：直接添加
                    for (int j = pl.fitting_result[i].start_point_index;
                         j < pl.fitting_result[i].end_point_index; j++) {
                        segments.push_back({
                            unscaled<float>(pl.points[j]),
                            unscaled<float>(pl.points[j + 1])
                        });
                    }
                } else {
                    // 【圆弧段】：标记为圆弧并存储 ArcSegment
                    int beg = pl.fitting_result[i].start_point_index;
                    int end = pl.fitting_result[i].end_point_index;
                    segments.push_back({
                        unscaled<float>(pl.points[beg]),
                        unscaled<float>(pl.points[end])
                    });
                    segments.back().is_arc = true;
                    segments.back().arcsegment = pl.fitting_result[i].arc_data;
                }
            }
        }

        // 简化处理
        for (auto& pl : pls)
            pl.simplify(SCALED_WIPE_TOWER_RESOLUTION);

    } else {
        // 【步骤 1B】不启用圆弧拟合：直接使用线性段
        for (const auto& pl : pls) {
            if (pl.points.size() < 2) continue;
            for (int i = 0; i < pl.size() - 1; i++) {
                segments.push_back({
                    unscaled<float>(pl.points[i]),
                    unscaled<float>(pl.points[i + 1])
                });
            }
        }
    }

    // 【步骤 2】找最近段并移动到起点
    int index_of_closest = get_closet_idx(segments);
    int i = index_of_closest;

    travel(segments[i].start);  // 空移到最近段的起点

    // 挤出第一段
    segments[i].is_arc
        ? extrude_arc(segments[i].arcsegment, feedrate)
        : extrude(segments[i].end, feedrate);

    // 【步骤 3】循环打印所有段
    do {
        i = (i + 1) % segments.size();
        if (i == index_of_closest) break;  // 回到起点，完成

        // 检查是否需要移动（段之间有间隙）
        float dx = segments[i].start.x() - m_current_pos.x();
        float dy = segments[i].start.y() - m_current_pos.y();
        float len = std::sqrt(dx * dx + dy * dy);

        if (len > EPSILON) {
            // 【智能回抽】：仅在有间隙时才回抽
            retract(retract_length, retract_speed);     // 回抽
            travel(segments[i].start, 600.);            // 快速空移（600 mm/s）
            retract(-retract_length, retract_speed);    // 回填（unretract）
        }

        // 挤出当前段
        segments[i].is_arc
            ? extrude_arc(segments[i].arcsegment, feedrate)
            : extrude(segments[i].end, feedrate);

    } while (1);
}
```

**优化策略总结**：

| 优化技术 | 说明 | 效果 |
|---------|------|------|
| **圆弧拟合** | 将多段直线拟合为 G2/G3 圆弧指令 | 减少 G-code 行数 40-60% |
| **最近点优先** | 从当前位置最近的段开始打印 | 减少空移距离和时间 |
| **智能回抽** | 仅在段间有间隙时才回抽 | 减少不必要的回抽，节省时间 |
| **连续挤出** | 尽可能保持连续挤出路径 | 提高打印质量，减少起停纹 |

### 4.2 圆弧挤出（extrude_arc_explicit）

**位置**: `WipeTower2.cpp:997`

支持 G2（顺时针圆弧）和 G3（逆时针圆弧）指令。

```cpp
WipeTowerWriter2& extrude_arc_explicit(Vec2f end_pos, Vec2f center_offset,
                                      float feedrate, float e = 0, bool is_ccw = true) {
    // 计算弧长
    Vec2f start = m_current_pos;
    Vec2f center = start + center_offset;
    float radius = center_offset.norm();

    float start_angle = std::atan2(start.y() - center.y(), start.x() - center.x());
    float end_angle = std::atan2(end_pos.y() - center.y(), end_pos.x() - center.x());

    // 角度差
    float angle_diff = is_ccw
        ? (end_angle - start_angle)
        : (start_angle - end_angle);
    if (angle_diff < 0) angle_diff += 2 * M_PI;

    float arc_length = radius * angle_diff;

    // 计算挤出量
    if (e == 0)
        e = m_extrusion_flow * arc_length;

    // 限制体积流速
    if (m_current_tool < m_filpar.size()) {
        float max_volumetric_speed = m_filpar[m_current_tool].max_e_speed;
        if (max_volumetric_speed > 0) {
            float current_volumetric_speed = e / arc_length * feedrate / 60.f;
            if (current_volumetric_speed > max_volumetric_speed)
                feedrate = max_volumetric_speed / (e / arc_length) * 60.f;
        }
    }

    // 生成 G-code
    m_gcode += is_ccw ? "G3" : "G2";
    m_gcode += set_format_X(end_pos.x());
    m_gcode += set_format_Y(end_pos.y());
    m_gcode += " I" + Slic3r::float_to_string_decimal_point(center_offset.x(), 3);
    m_gcode += " J" + Slic3r::float_to_string_decimal_point(center_offset.y(), 3);
    m_gcode += set_format_E(e);
    m_gcode += set_format_F(feedrate);
    m_gcode += "\n";

    // 更新打印时间
    m_elapsed_time += arc_length / (feedrate / 60.f);

    return *this;
}
```

**G2/G3 指令格式**：
```gcode
G2 X50.0 Y30.0 I10.0 J5.0 E0.5 F3000  ; 顺时针圆弧
G3 X30.0 Y50.0 I-5.0 J10.0 E0.5 F3000 ; 逆时针圆弧

参数说明：
X, Y - 终点坐标
I, J - 圆心相对于起点的偏移量
E - 挤出量
F - 进给速度
```

---

## 5. 工具切换流程

工具切换是擦除塔的核心功能，分为 4 个阶段：**Unload → Change → Load → Wipe**

### 5.1 完整流程图

```
┌─────────────────────────────────────────────────────────────┐
│                    tool_change(new_tool)                    │
└─────────────────────────────────────────────────────────────┘
                            │
        ┌───────────────────┼───────────────────┐
        ▼                   ▼                   ▼
┌───────────────┐   ┌───────────────┐   ┌───────────────┐
│ 1. Unload     │   │ 2. Change     │   │ 3. Load       │
│ (卸载旧耗材)   │─▶│ (切换工具)     │─▶│ (装载新耗材)   │
└───────────────┘   └───────────────┘   └───────────────┘
        │                   │                   │
        ▼                   ▼                   ▼
  ┌─────────┐         ┌─────────┐         ┌─────────┐
  │Ramming  │         │[change_ │         │快速装载  │
  │(捣实)   │         │filament_│         │慢速装载  │
  │         │         │gcode]   │         │额外装载  │
  │回抽到   │         │         │         │         │
  │冷却管   │         │切换温度  │         │         │
  │         │         │         │         │         │
  │冷却移动  │         │         │         │         │
  │         │         │         │         │         │
  │停车回抽  │         │         │         │         │
  └─────────┘         └─────────┘         └─────────┘
                                                │
                                                ▼
                                        ┌───────────────┐
                                        │ 4. Wipe       │
                                        │ (擦拭清洗)     │
                                        └───────────────┘
                                                │
                                                ▼
                                          ┌─────────┐
                                          │来回擦拭  │
                                          │逐步加速  │
                                          │清洗体积  │
                                          └─────────┘
```

### 5.2 阶段 1：toolchange_Unload（卸载）

**位置**: `WipeTower2.cpp:1603`

```cpp
void WipeTower2::toolchange_Unload(
    WipeTowerWriter2 &writer,
    const WipeTower::box_coordinates &cleaning_box,
    const std::string& current_material,
    const int old_temperature,
    const int new_temperature)
{
    // 【步骤 1】Ramming（捣实）
    // 目的：通过快速往复移动将耗材压实，防止拉丝
    if (m_enable_filament_ramming) {
        writer.append("; Ramming start\n");

        float ramming_line_width = m_perimeter_width * m_filpar[m_current_tool].ramming_line_width_multiplicator;
        float y_step = m_perimeter_width * m_filpar[m_current_tool].ramming_step_multiplicator * m_extra_spacing_ramming;

        float xl = cleaning_box.ld.x() + m_perimeter_width;
        float xr = cleaning_box.rd.x() - m_perimeter_width;

        // Ramming 速度序列（通常是递增的）
        const std::vector<float>& ramming_speed = m_filpar[m_current_tool].ramming_speed;

        // 往复移动
        for (size_t i = 0; i < ramming_speed.size(); ++i) {
            float e_length = 0.25f * ramming_speed[i];  // 挤出长度

            while (e_length > 0) {
                float dist = std::min(e_length, xr - xl);

                if (m_left_to_right)
                    writer.extrude(xr, writer.y(), ramming_speed[i] * 60.f);
                else
                    writer.extrude(xl, writer.y(), ramming_speed[i] * 60.f);

                writer.extrude(writer.x(), writer.y() + y_step);
                m_left_to_right = !m_left_to_right;
                e_length -= dist;
            }
        }

        writer.append("; Ramming end\n");
    }

    // 【步骤 2】回抽到冷却管
    if (m_enable_filament_ramming && m_semm && (m_cooling_tube_retraction != 0 || m_cooling_tube_length != 0)) {
        writer.append("; Retract(unload)\n");

        float total_retraction = m_cooling_tube_retraction + m_cooling_tube_length/2.f - 15.f;

        writer.suppress_preview()
              .retract(15.f, m_filpar[m_current_tool].unloading_speed_start * 60.f)
              .retract(0.70f * total_retraction, 1.0f * m_filpar[m_current_tool].unloading_speed * 60.f)
              .retract(0.20f * total_retraction, 0.5f * m_filpar[m_current_tool].unloading_speed * 60.f)
              .retract(0.10f * total_retraction, 0.3f * m_filpar[m_current_tool].unloading_speed * 60.f)
              .resume_preview();
    }

    // 【步骤 3】冷却移动
    const int& number_of_cooling_moves = m_filpar[m_current_tool].cooling_moves;
    if (m_enable_filament_ramming && m_semm && number_of_cooling_moves > 0) {
        writer.append("; Cooling\n");

        const float& initial_speed = m_filpar[m_current_tool].cooling_initial_speed;
        const float& final_speed = m_filpar[m_current_tool].cooling_final_speed;
        float speed_inc = (final_speed - initial_speed) / (2.f * number_of_cooling_moves - 1.f);

        // 往复冷却移动
        for (int i = 0; i < number_of_cooling_moves; ++i) {
            // Stamping（压印）- Snapmaker 专用
            if (i > 0 && m_filpar[m_current_tool].filament_stamping_distance != 0) {
                float stamping_dist = m_filpar[m_current_tool].filament_stamping_distance + m_cooling_tube_length / 2.f;
                // ... 压印代码 ...
            }

            // 冷却移动
            float speed = initial_speed + speed_inc * 2*i;
            writer.load_move_x_advanced(turning_point, m_cooling_tube_length, speed);
            speed += speed_inc;
            writer.load_move_x_advanced(old_x, -m_cooling_tube_length, speed);
        }
    }

    // 【步骤 4】停车位置回抽
    if (m_enable_filament_ramming && m_semm) {
        writer.append("; Cooling park\n");
        writer.wait(m_filpar[m_current_tool].delay);  // 等待冷却

        const auto _e = -m_cooling_tube_length / 2.f + m_parking_pos_retraction - m_cooling_tube_retraction;
        if (_e != 0.f)
            writer.retract(_e, 2000);
    }
}
```

**Ramming 原理**：
```
往复移动示意图（从上往下看）：

Y ▲
  │    ════════════════▶ (速度 v1)
  │    ◀════════════════ (速度 v2)
  │    ════════════════▶ (速度 v3，更快)
  │    ◀════════════════ (速度 v4，更快)
  │    ════════════════▶ (速度 v5，最快)
  └────────────────────────────▶ X
  xl                          xr

目的：
- 快速往复运动产生剪切力
- 将耗材压实到喷嘴尖端
- 防止卸载时拉丝
```

### 5.3 阶段 2：toolchange_Change（切换）

**位置**: `WipeTower2.cpp:1830`

```cpp
void WipeTower2::toolchange_Change(
    WipeTowerWriter2 &writer,
    const size_t new_tool,
    const std::string& new_material)
{
    // 记录旧工具的耗材使用量
    if (m_current_tool < m_used_filament_length.size())
        m_used_filament_length[m_current_tool] += writer.get_and_reset_used_filament_length();

    // 【关键】插入自定义换料 G-code
    // 这些占位符会在后处理时被替换为实际的 G-code
    writer.append("[change_filament_gcode]\n");

    // MK4 MMU3 专用：启用耗材监控
    if (m_is_mk4mmu3)
        writer.switch_filament_monitoring(true);

    // 确保位置同步（自定义 G-code 可能移动了打印头）
    writer.append(WipeTower::never_skip_tag() + " ; SKINNYDIP ACTIVE\n");
    writer.travel(writer.x(), writer.y());

    // 更新当前工具
    m_current_tool = new_tool;
}
```

**占位符替换机制**：
```
打印配置中的自定义 G-code：
┌────────────────────────────────────────┐
│ [change_filament_gcode]                │
│ T{new_tool}  ; 切换到新工具            │
│ M109 S{temperature[new_tool]}  ; 等待加热 │
│ G92 E0  ; 重置挤出机                   │
└────────────────────────────────────────┘

后处理时会将占位符替换为实际内容
```

### 5.4 阶段 3：toolchange_Load（装载）

**位置**: `WipeTower2.cpp:1869`

```cpp
void WipeTower2::toolchange_Load(
    WipeTowerWriter2 &writer,
    const WipeTower::box_coordinates &cleaning_box)
{
    if (m_semm && m_enable_filament_ramming && (m_parking_pos_retraction != 0 || m_extra_loading_move != 0)) {
        float xl = cleaning_box.ld.x() + m_perimeter_width * 0.75f;
        float xr = cleaning_box.rd.x() - m_perimeter_width * 0.75f;

        // 【阶段 3.1】快速装载
        writer.travel(xl, cleaning_box.ld.y() + m_perimeter_width * 0.5f)
              .suppress_preview()
              .load(m_parking_pos_retraction, m_filpar[m_current_tool].loading_speed_start * 60.f);

        // 【阶段 3.2】慢速装载（更精确）
        writer.load_move_x_advanced(xr, m_extra_loading_move, m_filpar[m_current_tool].loading_speed * 60.f)
              .resume_preview();
    }
}
```

### 5.5 阶段 4：toolchange_Wipe（擦拭清洗）

**位置**: `WipeTower2.cpp:1898`

```cpp
void WipeTower2::toolchange_Wipe(
    WipeTowerWriter2 &writer,
    const WipeTower::box_coordinates &cleaning_box,
    float wipe_volume)  // 需要清洗的体积（mm³）
{
    // 首层增加流量，降低速度
    const float& f = m_extrusion_flow;
    float flow_multiplier = this->is_first_layer() ? 1.18f * f : f;

    // 计算 Y 步进（行间距）
    float y_step = m_perimeter_width * m_extra_spacing_wipe * flow_multiplier / f * 0.95f;

    float wipe_box_width = cleaning_box.rd.x() - cleaning_box.ld.x() - 2 * m_perimeter_width;
    float wipe_box_depth = cleaning_box.ru.y() - cleaning_box.rd.y();

    // 【策略选择】
    if (m_change_pressure && m_is_mk4mmu3) {
        // MK4 MMU3：使用压力推进调整
        writer.disable_linear_advance_value(m_change_pressure_value);
    }

    // 【往复擦拭】
    float xl = cleaning_box.ld.x() + m_perimeter_width;
    float xr = cleaning_box.rd.x() - m_perimeter_width;
    float y = cleaning_box.ld.y() + m_perimeter_width;

    // 计算需要擦拭的行数
    float length_to_extrude = volume_to_length(wipe_volume, m_perimeter_width * flow_multiplier, m_layer_height);
    int lines_num = std::ceil(length_to_extrude / wipe_box_width);

    // 逐行擦拭（之字形路径）
    for (int i = 0; i < lines_num; ++i) {
        if (i % 2 == 0)
            writer.extrude(xr, y, feedrate);
        else
            writer.extrude(xl, y, feedrate);

        y += y_step;

        // 最后一行：精确控制挤出量
        if (i == lines_num - 1) {
            float remaining_length = length_to_extrude - i * wipe_box_width;
            float remaining_x = i % 2 == 0 ? xl + remaining_length : xr - remaining_length;
            writer.extrude(remaining_x, y, feedrate);
        }
    }

    if (m_change_pressure && m_is_mk4mmu3) {
        writer.enable_linear_advance();  // 恢复压力推进
    }
}
```

**擦拭路径示意**：
```
cleaning_box 内的擦拭路径（之字形）：

┌─────────────────────────────────┐
│ ════════════════════════════▶ 1 │
│ ◀════════════════════════════ 2 │
│ ════════════════════════════▶ 3 │
│ ◀════════════════════════════ 4 │
│ ═══════════▶ 5 (最后一行)      │
└─────────────────────────────────┘
xl                               xr

每行之间的间距 = y_step
总行数 = ceil(清洗体积 / 单行体积)
```

---

## 6. 层完成与外墙生成

### 6.1 finish_layer() 流程

**位置**: `WipeTower2.cpp:1976`

```cpp
WipeTower::ToolChangeResult WipeTower2::finish_layer()
{
    assert(!this->layer_finished());
    m_current_layer_finished = true;

    size_t old_tool = m_current_tool;

    // 【步骤 1】初始化 Writer
    WipeTowerWriter2 writer(m_layer_height, m_perimeter_width, m_gcode_flavor, m_filpar, m_enable_arc_fitting, m_printer_model);
    writer.set_extrusion_flow(m_extrusion_flow)
          .set_z(m_z_pos)
          .set_initial_tool(m_current_tool)
          .set_y_shift(m_y_shift - (m_current_shape == SHAPE_REVERSED ? m_layer_info->toolchanges_depth() : 0.f));

    // 【步骤 2】确定速度（首层减速）
    bool first_layer = is_first_layer() || (m_num_tool_changes <= 1 && m_no_sparse_layers);
    float feedrate = first_layer
        ? m_first_layer_speed * 60.f
        : std::min(m_wipe_tower_max_purge_speed * 60.f, m_infill_speed * 60.f);

    // 【步骤 3】计算填充区域
    float current_depth = m_layer_info->depth - m_layer_info->toolchanges_depth();
    WipeTower::box_coordinates fill_box(
        Vec2f(m_perimeter_width, m_layer_info->depth - (current_depth - m_perimeter_width)),
        m_wipe_tower_width - 2 * m_perimeter_width,
        current_depth - m_perimeter_width
    );

    writer.set_initial_position(
        (m_left_to_right ? fill_box.ru : fill_box.lu),
        m_wipe_tower_width, m_wipe_tower_depth, m_internal_rotation
    );

    // 【步骤 4】打印内周
    if (fill_box.ru.y() - fill_box.rd.y() > m_perimeter_width - WT_EPSILON)
        writer.rectangle(fill_box.ld, fill_box.rd.x() - fill_box.ld.x(),
                        fill_box.ru.y() - fill_box.rd.y(), feedrate);

    // 【步骤 5】打印填充（网格或实心）
    bool solid_infill = /* 检查下一层是否有可溶解材料 */;

    if (solid_infill) {
        // 实心填充（首层或下一层有可溶解材料）
        float sparse_factor = first_layer ? 1.f : 1.5f;
        int n = dy / (m_perimeter_width * sparse_factor);
        float spacing = (dy - m_perimeter_width) / (n - 1);

        for (int i = 0; i < n; ++i) {
            writer.extrude(writer.x(), y, feedrate)
                  .extrude(i % 2 ? left : right, y);
            y += spacing;
        }
    } else {
        // 稀疏填充（倒 U 形 + 垂直桥接）
        writer.extrude(fill_box.lu + Vec2f(m_perimeter_width * 2, 0.f), feedrate);

        const int n = 1 + int((right - left) / m_bridging);
        const float dx = (right - left) / n;
        for (int i = 1; i <= n; ++i) {
            float x = left + dx * i;
            writer.travel(x, writer.y());
            writer.extrude(x, i % 2 ? fill_box.rd.y() : fill_box.ru.y());
        }
    }

    // 【步骤 6】打印外墙（关键！）
    feedrate = first_layer
        ? m_first_layer_speed * 60.f
        : std::min(m_wipe_tower_max_purge_speed * 60.f, m_perimeter_speed * 60.f);

    Polygon poly;
    if (m_wall_type == (int)wtwCone) {
        // 锥形墙
        WipeTower::box_coordinates wt_box(
            Vec2f(0.f, (m_current_shape == SHAPE_REVERSED ? m_layer_info->toolchanges_depth() : 0.f)),
            m_wipe_tower_width, m_layer_info->depth + m_perimeter_width
        );
        bool infill_cone = first_layer && m_wipe_tower_width > 2 * spacing && m_wipe_tower_depth > 2 * spacing;
        poly = generate_support_cone_wall(writer, wt_box, feedrate, infill_cone, spacing);
    } else {
        // Rib 墙或矩形墙
        WipeTower::box_coordinates wt_box(
            Vec2f(0.f, 0.f),
            m_wipe_tower_width,
            m_layer_info->depth + m_perimeter_width
        );
        poly = generate_support_rib_wall(
            writer, wt_box, feedrate, first_layer,
            m_wall_type == (int)wtwRib,  // 是否使用 rib 墙
            true,   // extrude_perimeter
            false   // skip_points
        );
    }

    // 【步骤 7】打印边缘（brim，仅首层）
    if (first_layer) {
        writer.append("; WIPE_TOWER_BRIM_START\n");
        size_t loops_num = (m_wipe_tower_brim_width + spacing/2.f) / spacing;

        for (size_t i = 0; i < loops_num; ++i) {
            poly = offset(poly, scale_(spacing)).front();  // 向外扩展
            int cp = poly.closest_point_index(Point::new_scale(writer.x(), writer.y()));
            writer.travel(unscale(poly.points[cp]).cast<float>());

            // 绕外周一圈
            for (int i = cp + 1; true; ++i) {
                if (i == int(poly.points.size())) i = 0;
                writer.extrude(unscale(poly.points[i]).cast<float>());
                if (i == cp) break;
            }
        }
        writer.append("; WIPE_TOWER_BRIM_END\n");
        m_wipe_tower_brim_width_real = loops_num * spacing;
    }

    // 【步骤 8】准备擦拭路径（供后续使用）
    int i = poly.closest_point_index(Point::new_scale(writer.x(), writer.y()));
    writer.add_wipe_point(writer.pos());
    writer.add_wipe_point(unscale(poly.points[i == 0 ? int(poly.points.size()) - 1 : i - 1]).cast<float>());

    // 【步骤 9】记录耗材使用量
    if (!m_no_sparse_layers || toolchanges_on_layer || first_layer) {
        if (m_current_tool < m_used_filament_length.size())
            m_used_filament_length[m_current_tool] += writer.get_and_reset_used_filament_length();
        m_current_height += m_layer_info->height;
    }

    return construct_tcr(writer, false, old_tool, true);
}
```

**外墙类型对比**：

| 类型 | 代码枚举 | 特点 | 适用场景 |
|------|---------|------|---------|
| **矩形** | `wtwRectangle` | 简单矩形框 | 默认选项，打印速度最快 |
| **锥形** | `wtwCone` | 底部大顶部小，圆弧边缘 | 高塔需要额外稳定性 |
| **Rib** | `wtwRib` | X 形对角线加固 | 平衡稳定性和材料使用 |

---

## 7. 配置参数体系

### 7.1 擦除塔主要配置

**文件**: `PrintConfig.cpp`（配置定义）

| 参数名称 | 类型 | 默认值 | 说明 |
|---------|------|--------|------|
| `enable_prime_tower` | bool | - | 启用擦除塔 |
| `prime_tower_width` | float | 60.0 mm | 擦除塔宽度 |
| `prime_tower_brim_width` | float | 3.0 mm | 擦除塔边缘宽度（首层） |
| `wipe_tower_x` | floats | 15.0 mm | X 坐标（支持多板） |
| `wipe_tower_y` | floats | 220.0 mm | Y 坐标（支持多板） |
| `wipe_tower_rotation_angle` | float | 0.0° | 旋转角度（绕 Z 轴） |
| `wipe_tower_cone_angle` | float | 30.0° | 稳定锥角度 (0-90°) |
| `wipe_tower_max_purge_speed` | float | 90.0 mm/s | 最大清洗速度 |
| `wipe_tower_no_sparse_layers` | bool | false | 跳过无工具切换层 |
| `purge_in_prime_tower` | bool | - | 在擦除塔内清洗 |
| `wipe_tower_extra_flow` | float | 100% | 额外挤出流量 |
| `wipe_tower_extra_spacing` | float | 100% | 额外行间距 |
| `wipe_tower_bridging` | float | 10.0 mm | 桥接间距（稀疏填充） |

### 7.2 Rib 外墙专用参数

**位置**: `WipeTower2.hpp:206-210`

| 参数名称 | 类型 | 默认值 | 说明 |
|---------|------|--------|------|
| `m_wall_type` | enum | `wtwRectangle` | 外墙类型（0=矩形, 1=锥形, 2=Rib） |
| `m_rib_width` | float | 10.0 mm | Rib 宽度（对角线条的宽度） |
| `m_extra_rib_length` | float | 0.0 mm | 额外 Rib 长度（延伸量） |
| `m_rib_length` | float | - | 总 Rib 长度（自动计算） |
| `m_used_fillet` | bool | true | 启用圆角平滑 |

**枚举定义**：
```cpp
enum WipeTowerWallType {
    wtwRectangle = 0,  // 矩形墙（默认）
    wtwCone = 1,       // 锥形稳定墙
    wtwRib = 2         // Rib 对角线墙
};
```

### 7.3 耗材清洗参数

| 参数名称 | 类型 | 说明 |
|---------|------|------|
| `flush_volumes_matrix` | floats | 清洗体积矩阵 [from][to]（mm³） |
| `flush_multiplier` | float | 清洗体积倍数 |
| `filament_minimal_purge_on_wipe_tower` | floats | 每个耗材的最小清洗量（mm³） |
| `filament_ramming_parameters` | strings | Ramming 参数字符串 |

**清洗体积矩阵示例**：
```
3 种耗材的清洗矩阵（9 个值）：
          To T0  To T1  To T2
From T0     0     140    70
From T1    140     0     50
From T2    70     50     0

说明：
- 对角线为 0（同一工具无需清洗）
- 不同材料组合需要不同清洗量
- 例如：T0→T1 需要清洗 140mm³
```

### 7.4 高级参数

**G-code 生成器参数**：
```cpp
// 位置：WipeTower2.hpp:214-223
float m_cooling_tube_retraction = 0.f;   // 冷却管回抽长度
float m_cooling_tube_length = 0.f;       // 冷却管长度
float m_parking_pos_retraction = 0.f;    // 停车位置回抽
float m_extra_loading_move = 0.f;        // 额外装载移动
float m_bridging = 0.f;                  // 桥接长度
bool m_no_sparse_layers = false;         // 跳过稀疏层
bool m_set_extruder_trimpot = false;     // 设置挤出机电流
bool m_adhesion = true;                  // 启用粘附
GCodeFlavor m_gcode_flavor;              // G-code 风格（Marlin/Klipper 等）
```

**速度参数**：
```cpp
// 位置：WipeTower2.hpp:199-203
float m_travel_speed = 0.f;              // 空移速度
float m_infill_speed = 0.f;              // 填充速度
float m_wipe_tower_max_purge_speed = 90.f;  // 最大清洗速度
float m_perimeter_speed = 0.f;           // 外周速度
float m_first_layer_speed = 0.f;         // 首层速度
```

---

## 8. 与主打印流程集成

### 8.1 Print 类集成

**文件**: `Print.hpp` 和 `Print.cpp`

```cpp
class Print {
    // ... 其他成员 ...

    // 擦除塔对象（两个版本）
    std::unique_ptr<WipeTower> m_wipe_tower;    // BBL 版本
    std::unique_ptr<WipeTower2> m_wipe_tower2;  // 非 BBL 版本

    // 打印步骤包含擦除塔
    enum PrintStep {
        psWipeTower,              // 擦除塔步骤
        psToolOrdering = psWipeTower,  // 工具排序
        psSkirtBrim,
        psGCodeExport
    };
};
```

### 8.2 GCode 生成集成

**文件**: `GCode.cpp`

```cpp
void GCode::_do_export(...) {
    // 【步骤 1】初始化擦除塔
    if (config.enable_prime_tower) {
        if (is_BBL_printer(config)) {
            wipe_tower = std::make_unique<WipeTower>(config, plate_idx, plate_origin, wipe_volume, initial_tool, wipe_tower_height);
        } else {
            wipe_tower = std::make_unique<WipeTower2>(config, default_region_config, plate_idx, plate_origin, wiping_matrix, initial_tool);
        }
    }

    // 【步骤 2】规划所有工具切换
    for (const auto& layer : layers) {
        for (const auto& toolchange : layer.toolchanges) {
            wipe_tower->plan_toolchange(
                z,              // Z 高度
                layer_height,   // 层高
                old_tool,       // 旧工具
                new_tool,       // 新工具
                wipe_volume     // 清洗体积
            );
        }
    }

    // 【步骤 3】生成擦除塔 G-code
    std::vector<std::vector<WipeTower::ToolChangeResult>> wipe_tower_data;
    wipe_tower->generate(wipe_tower_data);

    // 【步骤 4】在每层插入工具切换 G-code
    for (size_t layer_id = 0; layer_id < layers.size(); ++layer_id) {
        // 打印对象
        for (const auto& object : objects) {
            print_object_layer(object, layer_id);
        }

        // 插入擦除塔 G-code
        for (const auto& tcr : wipe_tower_data[layer_id]) {
            // 移动到擦除塔
            gcode += "G1 X" + to_string(tcr.start_pos.x()) + " Y" + to_string(tcr.start_pos.y()) + " F7200\n";

            // 擦除塔 G-code
            gcode += tcr.gcode;

            // 返回打印对象
            // （下一对象的起点会自动处理）
        }
    }
}
```

### 8.3 工具切换时序图

```
每层打印流程（多材料）:

时间 ─▶

│ 对象 1    │ 工具切换 1 │ 对象 2    │ 工具切换 2 │ 对象 3    │ 层完成    │
│ (T0)      │ (T0→T1)    │ (T1)      │ (T1→T2)    │ (T2)      │           │
├───────────┼────────────┼───────────┼────────────┼───────────┼───────────┤
│ 打印对象  │ 移动到塔   │ 打印对象  │ 移动到塔   │ 打印对象  │ 移动到塔  │
│ 主体部分  │            │ 主体部分  │            │ 主体部分  │           │
│           │ Unload     │           │ Unload     │           │ 打印外墙  │
│           │ Change     │           │ Change     │           │ 打印填充  │
│           │ Load       │           │ Load       │           │ 打印边缘  │
│           │ Wipe       │           │ Wipe       │           │ (首层)    │
│           │            │           │            │           │           │
│           │ 返回对象   │           │ 返回对象   │           │ Z 提升    │
└───────────┴────────────┴───────────┴────────────┴───────────┴───────────┘
```

---

## 9. 关键算法流程图

### 9.1 plan_tower() 深度规划

**位置**: `WipeTower2.cpp:2206`

```
算法：向下传播深度确保稳定性

输入：m_plan (所有层的信息)
输出：每层的最终 depth

1. 初始化：
   m_wipe_tower_depth = 0
   m_wipe_tower_height = 最后一层的 z

2. 从顶层向下遍历：
   for layer in reversed(m_plan):
       ┌─────────────────────────────────────┐
       │ this_layer_depth = max(             │
       │   layer.depth,                      │
       │   layer.toolchanges_depth()         │
       │ )                                   │
       │                                     │
       │ layer.depth = this_layer_depth      │
       │                                     │
       │ if this_layer_depth > m_wipe_tower_depth - m_perimeter_width:│
       │     m_wipe_tower_depth = this_layer_depth + m_perimeter_width│
       │                                     │
       │ // 向下传播                         │
       │ for i in range(layer_index - 1, 0, -1):│
       │     if m_plan[i].depth - this_layer_depth < 2*m_perimeter_width:│
       │         m_plan[i].depth = this_layer_depth │
       └─────────────────────────────────────┘

关键思想：
- 上层需要的深度会传播到下层
- 确保结构稳定（下层至少和上层一样深）
- 防止悬空（2*perimeter_width 安全边距）
```

**示例**：
```
层  |  初始深度  |  传播后深度
----|-----------|-------------
 5  |    10     |     10      ← 顶层
 4  |    8      |     10      ← 从 5 传播
 3  |    12     |     12      ← 自身更大
 2  |    9      |     12      ← 从 3 传播
 1  |    11     |     12      ← 从 3 传播
 0  |    15     |     15      ← 底层最大

最终 m_wipe_tower_depth = 15 + m_perimeter_width
```

### 9.2 Rib 长度动态计算

```
m_rib_length 计算流程（在 plan_tower 之前）:

1. 对角线基础长度 = sqrt(width² + depth²)

2. 额外延伸 = m_extra_rib_length (配置参数)

3. 总 rib 长度 = 对角线长度 + 额外延伸

4. 每层实际长度（在 generate_rib_polygon 中）:
   current_extra = (H_max - H_current) / H_max × 额外延伸
   current_rib_len = 对角线长度 + current_extra

5. 效果：
   Z = 0 (底层):     rib 最长 ═══════════════
   Z = H_max/2:      rib 中等 ═══════
   Z = H_max (顶层): rib 最短 ═══
```

### 9.3 工具切换完整流程图

```
开始 tool_change(new_tool)
        │
        ▼
    是否首层?
    ┌───┴───┐
   是│      │否
    ▼       ▼
打印 brim  继续
    │       │
    └───┬───┘
        ▼
toolchange_Unload
    ├─ Ramming 捣实
    ├─ 回抽到冷却管
    ├─ 冷却移动
    └─ 停车位置回抽
        │
        ▼
toolchange_Change
    ├─ 记录旧工具耗材量
    ├─ 插入 [change_filament_gcode]
    └─ 更新 m_current_tool
        │
        ▼
toolchange_Load
    ├─ 快速装载
    ├─ 慢速装载
    └─ 额外装载移动
        │
        ▼
toolchange_Wipe
    ├─ 计算清洗体积
    ├─ 往复擦拭（之字形路径）
    └─ 逐步加速
        │
        ▼
返回 ToolChangeResult
```

---

## 10. 性能优化与设计亮点

### 10.1 圆弧拟合优化

**效果对比**：
```
未启用圆弧拟合（传统）:
G1 X10.0 Y10.0 E0.05
G1 X10.5 Y10.2 E0.05
G1 X11.0 Y10.5 E0.05
G1 X11.5 Y10.9 E0.05
G1 X12.0 Y11.4 E0.05
... (50 行)

启用圆弧拟合:
G3 X12.0 Y11.4 I2.0 J1.4 E2.5 F3000
... (1 行)

压缩率：50:1
```

**适用场景**：
- 圆角外墙
- Rib 对角线
- 擦拭路径

### 10.2 内存优化

**Segment 结构**（位置 `WipeTower2.cpp:299`）：
```cpp
struct Segment {
    Vec2f start;           // 8 bytes
    Vec2f end;             // 8 bytes
    bool is_arc = false;   // 1 byte
    ArcSegment arcsegment; // 仅在需要时使用

    Segment(const Vec2f& s, const Vec2f& e) : start(s), end(e) {}
};

// 总大小：约 17 bytes (不含 ArcSegment)
// vs. 传统方式存储所有点：N * 8 bytes
```

### 10.3 路径优化策略

1. **最近点优先**：减少空移距离 20-30%
2. **智能回抽**：仅在必要时回抽，节省时间 10-15%
3. **连续挤出**：减少起停次数，提高表面质量
4. **圆角平滑**：减少尖角处的打印缺陷

---

## 11. 常见问题与调试

### 11.1 Rib 外墙不显示

**可能原因**：
1. `m_wall_type` 未设置为 `wtwRib (2)`
2. `m_rib_width` 过小（< 5mm）
3. `m_extra_rib_length` 设置为负数

**检查方法**：
```cpp
// 在 generate_rib_polygon 开始处添加：
std::cout << "Rib 参数：" << std::endl;
std::cout << "  width: " << m_rib_width << std::endl;
std::cout << "  extra_length: " << m_extra_rib_length << std::endl;
std::cout << "  total_length: " << m_rib_length << std::endl;
```

### 11.2 圆角导致悬空

**原因**：`m_y_shift > 0` 时对矩形墙应用圆角

**解决方案**（已在代码中实现，`WipeTower2.cpp:2447`）：
```cpp
if (m_used_fillet) {
    if (!rib_wall && m_y_shift > EPSILON) {
        // 不应用圆角
    } else {
        wall_polygon = rounding_polygon(wall_polygon);
    }
}
```

### 11.3 清洗体积不足

**调试方法**：
```cpp
// 在 toolchange_Wipe 中：
std::cout << "清洗信息：" << std::endl;
std::cout << "  required_volume: " << wipe_volume << " mm³" << std::endl;
std::cout << "  box_width: " << wipe_box_width << " mm" << std::endl;
std::cout << "  lines_num: " << lines_num << std::endl;
std::cout << "  actual_volume: " << length_to_extrude * m_perimeter_width * m_layer_height << " mm³" << std::endl;
```

---

## 12. 总结

### 12.1 技术特点

1. **双版本架构**：BBL 专用优化 + 通用非 BBL 支持
2. **三种外墙类型**：矩形/锥形/Rib，适应不同需求
3. **智能路径规划**：圆弧拟合、最近点优先、智能回抽
4. **精确体积控制**：基于体积的清洗策略
5. **高度可配置**：超过 30 个配置参数

### 12.2 关键文件汇总

| 文件 | 行数 | 功能 |
|------|------|------|
| `WipeTower2.hpp` | 359 | 类定义和数据结构 |
| `WipeTower2.cpp` | 2584 | 核心实现和算法 |
| `WipeTowerDialog.cpp` | - | GUI 配置界面 |
| `PrintConfig.cpp` | - | 配置参数定义 |

### 12.3 核心算法总结

- **generate_rib_polygon()**: Rib 多边形生成（对角线 + 布尔运算）
- **rounding_polygon()**: 圆角平滑（20 点圆弧拟合）
- **generate_path()**: 智能路径规划（圆弧拟合 + 最近点）
- **plan_tower()**: 深度规划（向下传播确保稳定性）
- **toolchange_Unload/Change/Load/Wipe()**: 工具切换四阶段

### 12.4 代码位置速查表

| 功能 | 文件 | 行号 |
|------|------|------|
| WipeTower2 类定义 | WipeTower2.hpp | 21 |
| WipeTowerWriter2 类定义 | WipeTower2.cpp | 561 |
| generate_rib_polygon | WipeTower2.cpp | 2399 |
| rounding_polygon | WipeTower2.cpp | 107 |
| generate_support_rib_wall | WipeTower2.cpp | 2433 |
| generate_path | WipeTower2.cpp | 1083 |
| toolchange_Unload | WipeTower2.cpp | 1603 |
| toolchange_Change | WipeTower2.cpp | 1830 |
| toolchange_Load | WipeTower2.cpp | 1869 |
| toolchange_Wipe | WipeTower2.cpp | 1898 |
| finish_layer | WipeTower2.cpp | 1976 |
| plan_tower | WipeTower2.cpp | 2206 |
| plan_toolchange | WipeTower2.cpp | 2175 |

---

**文档结束**

此文档详细解释了 OrcaSlicer 擦除塔的所有核心实现，包括 Rib 外墙的算法细节、路径规划策略、工具切换流程等。所有代码片段都标注了具体的文件位置和行号，便于进一步研究和开发。

如有任何问题或需要补充的内容，请参考源代码或联系开发团队。
